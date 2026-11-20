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

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform int uUseSkinning;
// #103 — bones live in the binding-1 SSBO (#104) that Model::UploadBoneMatrices fills and binds
// on every Draw(); this used to declare the old never-uploaded uniform array, so a skinned model
// with a clip playing collapsed to a point and got no selection outline.
layout(std430, binding = 1) readonly buffer BoneBlock { mat4 uBones[]; };
#include "Skin.glsl" // SkinPalette
// uUseSkinning 2: skinned once this frame already (Model::PreSkin).
#include "SkinnedVertex.glsl"
layout(std430, binding = 26) readonly buffer SkinnedBlock { uint uSkinned[]; };
uniform int uSkinBase;

void main() {
    // Flat silhouette for the selection/hover mask (SelectionOutline): position only, no
    // normal push, so no per-vertex normal matrix.
    vec4 localPos = vec4(aPos, 1.0);
    if (uUseSkinning == 2) {
        uint s = uint(gl_VertexID - gl_BaseVertex + uSkinBase) * kSkinnedWords;
        localPos = vec4(uintBitsToFloat(uSkinned[s]), uintBitsToFloat(uSkinned[s + 1u]), uintBitsToFloat(uSkinned[s + 2u]), 1.0);
    } else if (uUseSkinning == 1) {
        localPos = SkinPalette() * localPos;
    }
    gl_Position = uProj * uView * (uModel * localPos);
}
