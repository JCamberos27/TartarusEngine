#version 460 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4 aWeights;
layout (location = 8) in ivec4 aBoneIDs2; // influences 5-8 (MAX_BONE_INFLUENCE 8)
layout (location = 9) in vec4 aWeights2;
layout (location = 6) in float aTangentSign;
layout (location = 7) in vec4 aColor; // #113 vertex colour (white when the mesh has none)

uniform mat4 uModel;
uniform mat4 uNormalMatrix; // mat3 inverse-transpose of uModel in a mat4 (loader has no mat3fv)
// Instanced draws (InstanceBatch.h): instance i takes its transform and normal matrix from the per-frame buffer at
// entry uInstanceBase + i (two mat4s each) instead of uModel / uNormalMatrix.
uniform int uInstanced;
uniform int uInstanceBase;
layout(std430, binding = 27) readonly buffer InstanceBlock { mat4 uInstances[]; };
uniform mat4 uView;
uniform mat4 uProj;
// 0 = not skinned, 1 = skin here from the bone palette, 2 = already skinned this frame (Model::PreSkin): read the
// vertex from the skinned buffer at gl_VertexID + uSkinBase.
uniform int uUseSkinning;
// Bone palette as an std430 SSBO (binding 1), uploaded per skinned draw (#104). MAX_BONES=512
// entries (#113); the index is clamped to the buffer's length so a bad index can't read past it.
layout(std430, binding = 1) readonly buffer BoneBlock { mat4 uBones[]; };
#include "SkinnedVertex.glsl"
#include "FoliageWind.glsl"
layout(std430, binding = 26) readonly buffer SkinnedBlock { uint uSkinned[]; };
uniform int uSkinBase;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out mat3 vTBN;
out vec4 vColor;
// The vertex before skinning, in the mesh's own (bind-pose) space: blood splats on a body or a gun are pinned
// there, so they move and deform with it.
out vec3 vBindPos;
out vec3 vBindNormal;
// The player's own body in the camera's view (PlayerBodyTag): skin weighted to these palette bones isn't
// drawn - vHidden carries how much of the vertex they move, and the fragment shader drops > 0.5.
uniform int uHideBoneCount;
uniform int uHideBones[8];
out float vHidden;
// The player's outfit (PlayerBodyTag bone masks, a bit per palette bone): 1 = drop what the masked bones
// mostly move (the camera's world pass: around the head, and the sleeves while they're in the view-model
// pass), 2 = draw only that (the view-model pass's sleeves). Both cut at 0.5, so split halves meet.
uniform int uBoneMaskMode;
uniform int uBoneMask[16]; // bit patterns
// An outfit piece's vertices covered by clothing (SkinHideBuffer): one bit per vertex of the model, the
// sub-mesh's first at uHideVertBase. Not drawn in any camera pass.
layout(std430, binding = 20) readonly buffer HideVertBlock { uint uHideVertBits[]; };
uniform int uHideVerts;
uniform int uHideVertBase;
// The player's collar (PlayerBodyTag::CollarVerts): the same layout, dropped in the camera's own view only.
layout(std430, binding = 21) readonly buffer CollarVertBlock { uint uCollarVertBits[]; };
uniform int uCollarVerts;
// An outfit piece's layer (OutfitLayerTag): metres nearer the camera it's drawn, along each vertex's own view
// ray - the depth changes, the picture doesn't - so cloth wins the depth test against the layer it lies on.
uniform float uLayerPull;

// TAA motion vectors (TemporalAA.h). uTaaVelocity 1: this view writes velocity, so carry this frame's and last
// frame's clip positions without the projection jitter. Last frame = the same object transform (no previous
// transforms are kept) under last frame's camera, with last frame's wind - so camera motion and foliage wind
// both reproject exactly; skinned or moving objects fall back to the resolve's colour clipping.
uniform int  uTaaVelocity;
uniform mat4 uPrevViewProj; // last frame, unjittered
uniform vec2 uTaaJitter;    // this frame's jitter in NDC (subtracted from gl_Position)
out vec4 vCurClip;
out vec4 vPrevClip;

void main() {
    vBindPos = aPos;
    vBindNormal = aNormal;
    vec4 localPos = vec4(aPos, 1.0);
    vec3 localNormal = aNormal;
    vec3 localTangent = aTangent;

    vHidden = 0.0;
    if (uHideVerts == 1) {
        int id = gl_VertexID - gl_BaseVertex + uHideVertBase;
        uint word = uHideVertBits[clamp(id >> 5, 0, uHideVertBits.length() - 1)];
        if (((word >> uint(id & 31)) & 1u) != 0u) vHidden = 1.0;
    }
    if (uCollarVerts == 1) {
        int id = gl_VertexID - gl_BaseVertex + uHideVertBase;
        uint word = uCollarVertBits[clamp(id >> 5, 0, uCollarVertBits.length() - 1)];
        if (((word >> uint(id & 31)) & 1u) != 0u) vHidden = 1.0;
    }
    if (uUseSkinning != 0) {
        int boneIds[8] = int[8](aBoneIDs.x, aBoneIDs.y, aBoneIDs.z, aBoneIDs.w, aBoneIDs2.x, aBoneIDs2.y, aBoneIDs2.z, aBoneIDs2.w);
        float boneWeights[8] = float[8](aWeights.x, aWeights.y, aWeights.z, aWeights.w, aWeights2.x, aWeights2.y, aWeights2.z, aWeights2.w);
        for (int i = 0; i < 8; ++i)
            for (int k = 0; k < uHideBoneCount; ++k)
                if (boneIds[i] >= 0 && boneIds[i] == uHideBones[k]) vHidden += boneWeights[i];
        if (uBoneMaskMode != 0) {
            float masked = 0.0;
            for (int i = 0; i < 8; ++i) {
                int b = boneIds[i];
                if (b >= 0 && b < 512 && ((uint(uBoneMask[b >> 5]) >> uint(b & 31)) & 1u) != 0u) masked += boneWeights[i];
            }
            vHidden += uBoneMaskMode == 1 ? masked : 1.0 - masked;
        }
        if (uUseSkinning == 2) {
            uint s = uint(gl_VertexID - gl_BaseVertex + uSkinBase) * kSkinnedWords;
            localPos = vec4(uintBitsToFloat(uSkinned[s]), uintBitsToFloat(uSkinned[s + 1u]), uintBitsToFloat(uSkinned[s + 2u]), 1.0);
            localNormal = UnpackUnitOct(uSkinned[s + 3u]);
            localTangent = UnpackUnitOct(uSkinned[s + 4u]);
        } else {
            mat4 skinMat = mat4(0.0);
            float totalWeight = 0.0;
            for (int i = 0; i < 8; ++i) {
                if (boneIds[i] >= 0) {
                    skinMat += uBones[clamp(boneIds[i], 0, uBones.length() - 1)] * boneWeights[i]; // never OOB (#98)
                    totalWeight += boneWeights[i];
                }
            }
            if (totalWeight <= 0.0001) {
                skinMat = mat4(1.0);
            }
            localPos = skinMat * localPos;
            localNormal = mat3(skinMat) * aNormal;
            localTangent = mat3(skinMat) * aTangent;
        }
    }

    mat4 model = uModel;
    mat3 normalMat = mat3(uNormalMatrix); // inverse-transpose of uModel, computed once on the CPU (#104)
    if (uInstanced == 1) {
        int at = (uInstanceBase + gl_BaseInstance + gl_InstanceID) * 2; // gl_BaseInstance: the multi-draw command's group
        model = uInstances[at];
        normalMat = mat3(uInstances[at + 1]);
    }
    precise vec4 world = model * localPos;
    world.xyz += FoliageWindOffset(localPos.xyz, aColor, model[3].xyz); // opt-in; zero unless uFoliage
    vWorldPos = world.xyz;

    vNormal = normalize(normalMat * localNormal);
    // Tangents transform with the model matrix's linear part directly (not the
    // inverse-transpose used for normals) — using normalMat here would skew tangents
    // under non-uniform scale.
    vec3 T = normalize(mat3(model) * localTangent);
    T = normalize(T - dot(T, vNormal) * vNormal);
    vec3 B = cross(vNormal, T) * aTangentSign;
    vTBN = mat3(T, B, vNormal);

    vUV = aUV;
    vColor = aColor;
    vec4 viewPos = uView * world;
    if (uLayerPull > 0.0) {
        if (uProj[3][3] == 1.0) viewPos.z += uLayerPull; // orthographic: the view ray is -Z
        else {
            float d = length(viewPos.xyz);
            viewPos.xyz *= max(d - uLayerPull, 0.5 * d) / max(d, 1e-6);
        }
    }
    gl_Position = uProj * viewPos;
    if (uTaaVelocity == 1) {
        vCurClip = gl_Position;
        vCurClip.xy -= uTaaJitter * gl_Position.w;
        vec4 prevWorld = model * localPos;
        prevWorld.xyz += FoliageWindOffsetAt(localPos.xyz, aColor, model[3].xyz, uPrevWindTime);
        vPrevClip = uPrevViewProj * prevWorld;
    } else {
        vCurClip = vec4(0.0, 0.0, 0.0, 1.0);
        vPrevClip = vec4(0.0, 0.0, 0.0, 1.0);
    }
}
