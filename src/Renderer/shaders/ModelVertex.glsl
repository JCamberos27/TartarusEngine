#version 460 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4 aWeights;
layout (location = 6) in float aTangentSign;
layout (location = 7) in vec4 aColor; // #113 vertex colour (white when the mesh has none)

uniform mat4 uModel;
uniform mat4 uNormalMatrix; // mat3 inverse-transpose of uModel in a mat4 (loader has no mat3fv)
uniform mat4 uView;
uniform mat4 uProj;
uniform int uUseSkinning;
// Bone palette as an std430 SSBO (binding 1), uploaded per skinned draw (#104). MAX_BONES=512
// entries (#113); the index is clamped to the buffer's length so a bad index can't read past it.
layout(std430, binding = 1) readonly buffer BoneBlock { mat4 uBones[]; };

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out mat3 vTBN;
out vec4 vColor;
// The player's own body in the camera's view (PlayerBodyTag): skin weighted to these palette bones isn't
// drawn - vHidden carries how much of the vertex they move, and the fragment shader drops > 0.5.
uniform int uHideBoneCount;
uniform int uHideBones[8];
out float vHidden;
// An outfit's sleeves while the player's arms are in the view-model pass (PlayerBodyTag): 1 = the world
// pass drops what the sleeve bones (a bit per palette bone) mostly move, 2 = the view-model pass draws
// only that. Both cut at the same 0.5, so the two halves meet without a gap.
uniform int uSleeveMode;
uniform int uSleeveBones[16]; // bit patterns
// An outfit piece's vertices covered by clothing (SkinHideBuffer): one bit per vertex of the model, the
// sub-mesh's first at uHideVertBase. Not drawn in any camera pass.
layout(std430, binding = 20) readonly buffer HideVertBlock { uint uHideVertBits[]; };
uniform int uHideVerts;
uniform int uHideVertBase;

void main() {
    vec4 localPos = vec4(aPos, 1.0);
    vec3 localNormal = aNormal;
    vec3 localTangent = aTangent;

    vHidden = 0.0;
    if (uHideVerts == 1) {
        int id = gl_VertexID + uHideVertBase;
        uint word = uHideVertBits[clamp(id >> 5, 0, uHideVertBits.length() - 1)];
        if (((word >> uint(id & 31)) & 1u) != 0u) vHidden = 1.0;
    }
    if (uUseSkinning == 1) {
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < uHideBoneCount; ++k)
                if (aBoneIDs[i] >= 0 && aBoneIDs[i] == uHideBones[k]) vHidden += aWeights[i];
        if (uSleeveMode != 0) {
            float sleeve = 0.0;
            for (int i = 0; i < 4; ++i) {
                int b = aBoneIDs[i];
                if (b >= 0 && b < 512 && ((uint(uSleeveBones[b >> 5]) >> uint(b & 31)) & 1u) != 0u) sleeve += aWeights[i];
            }
            vHidden += uSleeveMode == 1 ? sleeve : 1.0 - sleeve;
        }
        mat4 skinMat = mat4(0.0);
        float totalWeight = 0.0;
        for (int i = 0; i < 4; ++i) {
            if (aBoneIDs[i] >= 0) {
                skinMat += uBones[clamp(aBoneIDs[i], 0, uBones.length() - 1)] * aWeights[i]; // never OOB (#98)
                totalWeight += aWeights[i];
            }
        }
        if (totalWeight <= 0.0001) {
            skinMat = mat4(1.0);
        }
        localPos = skinMat * localPos;
        localNormal = mat3(skinMat) * aNormal;
        localTangent = mat3(skinMat) * aTangent;
    }

    vec4 world = uModel * localPos;
    vWorldPos = world.xyz;

    mat3 normalMat = mat3(uNormalMatrix); // inverse-transpose of uModel, computed once on the CPU (#104)
    vNormal = normalize(normalMat * localNormal);
    // Tangents transform with the model matrix's linear part directly (not the
    // inverse-transpose used for normals) — using normalMat here would skew tangents
    // under non-uniform scale.
    vec3 T = normalize(mat3(uModel) * localTangent);
    T = normalize(T - dot(T, vNormal) * vNormal);
    vec3 B = cross(vNormal, T) * aTangentSign;
    vTBN = mat3(T, B, vNormal);

    vUV = aUV;
    vColor = aColor;
    gl_Position = uProj * uView * world;
}
