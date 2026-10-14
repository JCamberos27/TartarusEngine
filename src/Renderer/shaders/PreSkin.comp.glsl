#version 460 core
// Skins one sub-mesh's vertices once per frame (Model::PreSkin). Every pass that draws the model afterwards - the sun
// cascades, spot and point faces, the SSAO pre-pass, the camera views - reads these instead of blending up to eight
// bone matrices per vertex again in its own vertex shader.
layout(local_size_x = 64) in;

layout(std430, binding = 1) readonly buffer BoneBlock { mat4 uBones[]; };     // this model's palette (bone ring)
layout(std430, binding = 25) readonly buffer SourceBlock { uint uSrc[]; };    // the mesh's ModelVertex buffer
layout(std430, binding = 26) writeonly buffer SkinnedBlock { uint uOut[]; };  // 5 words per vertex, see SkinnedVertex.glsl

uniform int uCount;   // vertices in this sub-mesh
uniform int uOutBase; // its first vertex in the skinned buffer
// ModelVertex's layout, in 32-bit words (offsetof / 4), so this never drifts from the C++ struct.
uniform int uStride;
uniform int uOffNormal;
uniform int uOffTangent;
uniform int uOffBones;
uniform int uOffWeights;

#include "SkinnedVertex.glsl"

vec3 Load3(uint w) { return vec3(uintBitsToFloat(uSrc[w]), uintBitsToFloat(uSrc[w + 1u]), uintBitsToFloat(uSrc[w + 2u])); }

void main() {
    uint v = gl_GlobalInvocationID.x;
    if (v >= uint(uCount)) return;
    uint s = v * uint(uStride);
    vec4 pos = vec4(Load3(s), 1.0);
    vec3 nrm = Load3(s + uint(uOffNormal));
    vec3 tan = Load3(s + uint(uOffTangent));

    // The same blend ModelVertex.glsl / ShadowDepth.vert.glsl do per pass (#98: never OOB, unweighted = identity).
    mat4 skinMat = mat4(0.0);
    float totalWeight = 0.0;
    for (uint i = 0u; i < 8u; ++i) {
        int b = int(uSrc[s + uint(uOffBones) + i]);
        if (b >= 0) {
            float w = uintBitsToFloat(uSrc[s + uint(uOffWeights) + i]);
            skinMat += uBones[clamp(b, 0, uBones.length() - 1)] * w;
            totalWeight += w;
        }
    }
    if (totalWeight <= 0.0001) skinMat = mat4(1.0);

    pos = skinMat * pos;
    nrm = mat3(skinMat) * nrm;
    tan = mat3(skinMat) * tan;

    uint o = (uint(uOutBase) + v) * kSkinnedWords;
    uOut[o]      = floatBitsToUint(pos.x);
    uOut[o + 1u] = floatBitsToUint(pos.y);
    uOut[o + 2u] = floatBitsToUint(pos.z);
    uOut[o + 3u] = PackUnitOct(nrm);
    uOut[o + 4u] = PackUnitOct(tan);
}
