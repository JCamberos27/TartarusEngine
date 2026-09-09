#version 460 core
layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aUV;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4 aWeights;
uniform mat4 uModel;
uniform mat4 uLightViewProj;
uniform int uUseSkinning;
layout(std430, binding = 1) readonly buffer BoneBlock { mat4 uBones[]; }; // shared with the model VS (#104)
out vec2 vUV;
out vec3 vWorldPos; // used by the local-light (spot/point) depth FS; the sun FS ignores it
void main() {
    vec4 localPos = vec4(aPos, 1.0);
    if (uUseSkinning == 1) {
        mat4 skinMat = mat4(0.0);
        float tw = 0.0;
        for (int i = 0; i < 4; ++i) {
            if (aBoneIDs[i] >= 0) { skinMat += uBones[clamp(aBoneIDs[i], 0, 99)] * aWeights[i]; tw += aWeights[i]; }
        }
        if (tw <= 0.0001) skinMat = mat4(1.0);
        localPos = skinMat * localPos;
    }
    vUV = aUV;
    vec4 worldPos = uModel * localPos;
    vWorldPos = worldPos.xyz;
    gl_Position = uLightViewProj * worldPos;
}
