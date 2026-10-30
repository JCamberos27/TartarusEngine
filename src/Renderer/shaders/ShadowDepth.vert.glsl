#version 460 core
// The sun's single-pass cascades (uCascadeLayered) pick their layer here. Enabling an extension
// the driver lacks is only a warning; the engine never sets uCascadeLayered then.
#extension GL_ARB_shader_viewport_layer_array : enable
#extension GL_AMD_vertex_shader_layer : enable
layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aUV;
layout (location = 7) in vec4 aColor; // vertex colour (white when none): the foliage wind mask
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4 aWeights;
layout (location = 8) in ivec4 aBoneIDs2; // influences 5-8 (MAX_BONE_INFLUENCE 8)
layout (location = 9) in vec4 aWeights2;
uniform mat4 uModel;
uniform mat4 uLightViewProj;
// The camera depth pre-pass (SceneRenderer): uProj * (uView * world), the model VS's own expression, so both passes
// round a vertex to the same depth. A face square to the view has no slope for the polygon offset to work with, and
// far from the origin the premultiplied matrix rounded past the offset's few units: the opaque pass failed its
// GL_LEQUAL on the whole face, which was left the clear colour (black slab sides in the City Alley).
uniform int uSplitViewProj;
uniform mat4 uView;
uniform mat4 uProj;
// 1 = sun cascades in one pass: instance i renders into layer uCascadeFirst + i with that
// cascade's matrix, instead of one draw per cascade with uLightViewProj.
uniform int uCascadeLayered;
uniform int uCascadeFirst;
uniform mat4 uCascadeViewProj[4];
#include "FoliageWind.glsl"
uniform int uUseSkinning;
layout(std430, binding = 1) readonly buffer BoneBlock { mat4 uBones[]; }; // shared with the model VS (#104)
// uUseSkinning 2: skinned once this frame already (Model::PreSkin, SkinnedVertex.glsl); only the position is read.
layout(std430, binding = 26) readonly buffer SkinnedBlock { uint uSkinned[]; };
uniform int uSkinBase;
// Instanced draws (InstanceBatch.h): uInstanceLayers instances per caster (its cascades, 1 elsewhere), each caster's
// transform at entry (uInstanceBase + gl_InstanceID / uInstanceLayers) * 2 of the per-frame buffer.
uniform int uInstanced;
uniform int uInstanceBase;
uniform int uInstanceLayers;
layout(std430, binding = 27) readonly buffer InstanceBlock { mat4 uInstances[]; };
out vec2 vUV;
out vec3 vWorldPos; // used by the local-light (spot/point) depth FS; the sun FS ignores it
void main() {
    vec4 localPos = vec4(aPos, 1.0);
    if (uUseSkinning == 2) {
        uint s = uint(gl_VertexID + uSkinBase) * 5u;
        localPos = vec4(uintBitsToFloat(uSkinned[s]), uintBitsToFloat(uSkinned[s + 1u]), uintBitsToFloat(uSkinned[s + 2u]), 1.0);
    } else if (uUseSkinning == 1) {
        int boneIds[8] = int[8](aBoneIDs.x, aBoneIDs.y, aBoneIDs.z, aBoneIDs.w, aBoneIDs2.x, aBoneIDs2.y, aBoneIDs2.z, aBoneIDs2.w);
        float boneWeights[8] = float[8](aWeights.x, aWeights.y, aWeights.z, aWeights.w, aWeights2.x, aWeights2.y, aWeights2.z, aWeights2.w);
        mat4 skinMat = mat4(0.0);
        float tw = 0.0;
        for (int i = 0; i < 8; ++i) {
            if (boneIds[i] >= 0) { skinMat += uBones[clamp(boneIds[i], 0, uBones.length() - 1)] * boneWeights[i]; tw += boneWeights[i]; }
        }
        if (tw <= 0.0001) skinMat = mat4(1.0);
        localPos = skinMat * localPos;
    }
    vUV = aUV;
    mat4 model = uModel;
    int layerOffset = gl_InstanceID;
    if (uInstanced == 1) {
        int layers = max(uInstanceLayers, 1);
        model = uInstances[(uInstanceBase + gl_InstanceID / layers) * 2];
        layerOffset = gl_InstanceID % layers;
    }
    precise vec4 worldPos = model * localPos;
    worldPos.xyz += FoliageWindOffset(localPos.xyz, aColor, model[3].xyz); // same as ModelVertex.glsl
    vWorldPos = worldPos.xyz;
#if defined(GL_ARB_shader_viewport_layer_array) || defined(GL_AMD_vertex_shader_layer)
    if (uCascadeLayered == 1) {
        int layer = uCascadeFirst + layerOffset;
        gl_Layer = layer;
        gl_Position = uCascadeViewProj[layer] * worldPos;
        return;
    }
#endif
    gl_Position = uSplitViewProj == 1 ? uProj * (uView * worldPos) : uLightViewProj * worldPos;
}
