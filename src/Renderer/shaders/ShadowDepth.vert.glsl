#version 460 core
// The sun's single-pass cascades (uCascadeLayered) pick their layer here. Enabling an extension
// the driver lacks is only a warning; the engine never sets uCascadeLayered then.
#extension GL_ARB_shader_viewport_layer_array : enable
#extension GL_AMD_vertex_shader_layer : enable
layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aUV;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4 aWeights;
uniform mat4 uModel;
uniform mat4 uLightViewProj;
// 1 = sun cascades in one pass: instance i renders into layer uCascadeFirst + i with that
// cascade's matrix, instead of one draw per cascade with uLightViewProj.
uniform int uCascadeLayered;
uniform int uCascadeFirst;
uniform mat4 uCascadeViewProj[4];
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
            if (aBoneIDs[i] >= 0) { skinMat += uBones[clamp(aBoneIDs[i], 0, uBones.length() - 1)] * aWeights[i]; tw += aWeights[i]; }
        }
        if (tw <= 0.0001) skinMat = mat4(1.0);
        localPos = skinMat * localPos;
    }
    vUV = aUV;
    vec4 worldPos = uModel * localPos;
    vWorldPos = worldPos.xyz;
#if defined(GL_ARB_shader_viewport_layer_array) || defined(GL_AMD_vertex_shader_layer)
    if (uCascadeLayered == 1) {
        int layer = uCascadeFirst + gl_InstanceID;
        gl_Layer = layer;
        gl_Position = uCascadeViewProj[layer] * worldPos;
        return;
    }
#endif
    gl_Position = uLightViewProj * worldPos;
}
