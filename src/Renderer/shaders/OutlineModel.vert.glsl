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
uniform float uThickness;

void main() {
    vec4 localPos = vec4(aPos, 1.0);
    vec3 localNormal = aNormal;

    if (uUseSkinning == 1) {
        int boneIds[8] = int[8](aBoneIDs.x, aBoneIDs.y, aBoneIDs.z, aBoneIDs.w, aBoneIDs2.x, aBoneIDs2.y, aBoneIDs2.z, aBoneIDs2.w);
        float boneWeights[8] = float[8](aWeights.x, aWeights.y, aWeights.z, aWeights.w, aWeights2.x, aWeights2.y, aWeights2.z, aWeights2.w);
        mat4 skinMat = mat4(0.0);
        float totalWeight = 0.0;
        for (int i = 0; i < 8; ++i) {
            if (boneIds[i] >= 0) {
                skinMat += uBones[clamp(boneIds[i], 0, uBones.length() - 1)] * boneWeights[i]; // clamp as ModelVertex (#98)
                totalWeight += boneWeights[i];
            }
        }
        if (totalWeight <= 0.0001) skinMat = mat4(1.0);
        localPos = skinMat * localPos;
        localNormal = mat3(skinMat) * aNormal;
    }

    vec4 world = uModel * localPos;
    vec3 worldNormal = normalize(mat3(transpose(inverse(uModel))) * localNormal);
    world.xyz += worldNormal * uThickness;
    gl_Position = uProj * uView * world;
}
