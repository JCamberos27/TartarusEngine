#version 460 core
// Shell fur (FurRenderer): one instance per shell. Shell i of uShellCount sits at height h = (i + 1) / count of the
// fur's length out along the skinned normal, its tip end drooping along the surface toward the ground with h^2.
// The skinning is ModelVertex.glsl's: this frame's pre-skinned vertex (Model::PreSkin) or the bone palette.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4 aWeights;
layout (location = 8) in ivec4 aBoneIDs2;
layout (location = 9) in vec4 aWeights2;

uniform mat4 uModel;
uniform mat4 uNormalMatrix;
uniform mat4 uView;
uniform mat4 uProj;
uniform int uUseSkinning; // 0 none, 1 the palette, 2 pre-skinned (see ModelVertex.glsl)
layout(std430, binding = 1) readonly buffer BoneBlock { mat4 uBones[]; };
#include "SkinnedVertex.glsl"
layout(std430, binding = 26) readonly buffer SkinnedBlock { uint uSkinned[]; };
uniform int uSkinBase;

uniform int   uShellCount;
uniform float uFurLength;  // metres at the tips, where the mask is white
uniform float uFurGravity; // droop at the tips, as a share of the length

out vec3 vWorldPos;
out vec3 vNormal;   // the skin's normal
out vec3 vStrand;   // the hair's direction at this height (Kajiya-Kay's tangent)
out vec2 vUV;
out float vShellH;  // 0 at the skin, 1 at the longest tips

void main() {
    vec4 localPos = vec4(aPos, 1.0);
    vec3 localNormal = aNormal;
    if (uUseSkinning == 2) {
        uint s = uint(gl_VertexID + uSkinBase) * kSkinnedWords;
        localPos = vec4(uintBitsToFloat(uSkinned[s]), uintBitsToFloat(uSkinned[s + 1u]), uintBitsToFloat(uSkinned[s + 2u]), 1.0);
        localNormal = UnpackUnitOct(uSkinned[s + 3u]);
    } else if (uUseSkinning == 1) {
        int ids[8] = int[8](aBoneIDs.x, aBoneIDs.y, aBoneIDs.z, aBoneIDs.w, aBoneIDs2.x, aBoneIDs2.y, aBoneIDs2.z, aBoneIDs2.w);
        float ws[8] = float[8](aWeights.x, aWeights.y, aWeights.z, aWeights.w, aWeights2.x, aWeights2.y, aWeights2.z, aWeights2.w);
        mat4 skin = mat4(0.0);
        float total = 0.0;
        for (int i = 0; i < 8; ++i)
            if (ids[i] >= 0) { skin += uBones[clamp(ids[i], 0, uBones.length() - 1)] * ws[i]; total += ws[i]; }
        if (total <= 0.0001) skin = mat4(1.0);
        localPos = skin * localPos;
        localNormal = mat3(skin) * aNormal;
    }

    vec3 world = (uModel * localPos).xyz;
    vec3 N = normalize(mat3(uNormalMatrix) * localNormal);
    float h = float(gl_InstanceID + 1) / float(max(uShellCount, 1));
    // Gravity along the surface only: pulled straight down, the hair under the body would sink into it.
    vec3 down = vec3(0.0, -1.0, 0.0);
    vec3 slide = down - N * dot(down, N);
    vec3 offset = N * (h * uFurLength) + slide * (uFurGravity * uFurLength * h * h);
    vWorldPos = world + offset;
    vNormal = N;
    // d(offset)/dh: the strand's direction at this height.
    vStrand = normalize(N + slide * (2.0 * uFurGravity * h));
    vUV = aUV;
    vShellH = h;
    gl_Position = uProj * uView * vec4(vWorldPos, 1.0);
}
