#version 460 core
// --bake-impostors (ImpostorBake.cpp): writes the surface the model shader would light, unlit, into three targets.
// The material uniforms are the model shader's (Model::Draw binds them by name), read the way ModelFragment reads them.
in vec3 vPos;
in vec2 vUV;
in mat3 vTBN;
in vec4 vColor;

layout(location = 0) out vec4 oAlbedo;      // rgb linear albedo, a 1 (coverage)
layout(location = 1) out vec4 oNormalDepth; // rgb object-space normal, a depth: 0 at the bounding sphere's near side
layout(location = 2) out vec4 oMask;        // r AO, g roughness, b translucency, a metallic

uniform sampler2D uAlbedoMap;
uniform int uHasAlbedoMap;
uniform vec3 uBaseColor;
uniform int uAlphaClip;
uniform float uAlphaCutoff;
uniform sampler2D uNormalMap;
uniform int uHasNormalMap;
uniform float uNormalStrength;
uniform int uNormalFlipY;
uniform sampler2D uMetallicRoughnessMap;
uniform int uHasMetallicRoughnessMap;
uniform sampler2D uMetallicMap;
uniform int uHasMetallicMap;
uniform sampler2D uRoughnessMap;
uniform int uHasRoughnessMap;
uniform float uMetallic;
uniform float uRoughness;
uniform sampler2D uAOMap;
uniform int uHasAOMap;
uniform vec2 uUVTiling;
uniform vec2 uUVOffset;
uniform int uUseVertexColor;
uniform int uFoliage;
uniform float uTranslucency;

uniform vec3 uViewDir;  // object -> camera, object space
uniform vec3 uCentre;   // the bounding sphere, object space
uniform float uRadius;

vec3 UnpackNormal(vec3 s) {
    vec3 n = s * 2.0 - 1.0;
    if (s.b == 0.0) n.z = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    return n;
}

void main() {
    vec2 uv = vUV * (uUVTiling == vec2(0.0) ? vec2(1.0) : uUVTiling) + uUVOffset;
    vec4 albedoSample = texture(uAlbedoMap, uv);
    if (uUseVertexColor == 1) albedoSample.a *= vColor.a;
    if (uAlphaClip == 1 && uHasAlbedoMap == 1 && albedoSample.a < uAlphaCutoff) discard;
    vec3 albedo = (uHasAlbedoMap == 1 ? albedoSample.rgb : vec3(1.0)) * uBaseColor;
    if (uUseVertexColor == 1) albedo *= vColor.rgb;

    float metallic = uMetallic, roughness = uRoughness;
    if (uHasMetallicRoughnessMap == 1) {
        vec3 mr = texture(uMetallicRoughnessMap, uv).rgb;
        roughness *= mr.g;
        metallic *= mr.b;
    } else {
        if (uHasRoughnessMap == 1) roughness *= texture(uRoughnessMap, uv).r;
        if (uHasMetallicMap == 1) metallic *= texture(uMetallicMap, uv).r;
    }
    float ao = uHasAOMap == 1 ? texture(uAOMap, uv).r : 1.0;

    vec3 tn = uHasNormalMap == 1 ? UnpackNormal(texture(uNormalMap, uv).rgb) : vec3(0.0, 0.0, 1.0);
    if (uNormalFlipY == 1) tn.y = -tn.y;
    tn.xy *= uNormalStrength > 0.0 ? uNormalStrength : 1.0;
    vec3 N = normalize(vTBN * tn);
    // Every surface the bake sees faces the camera: a back face (a leaf card seen from behind) is the sheet turned
    // round, as ModelFragment treats a double-sided one, and a normal still pointing away is bent to the horizon.
    if (!gl_FrontFacing) N = -N;
    float nv = dot(N, uViewDir);
    if (nv < 0.02) N = normalize(N + uViewDir * (0.02 - nv));

    oAlbedo = vec4(albedo, 1.0);
    float depth = (uRadius - dot(vPos - uCentre, uViewDir)) / (2.0 * uRadius);
    oNormalDepth = vec4(N * 0.5 + 0.5, clamp(depth, 0.0, 1.0));
    oMask = vec4(ao, roughness, uFoliage == 1 ? uTranslucency : 0.0, metallic);
}
