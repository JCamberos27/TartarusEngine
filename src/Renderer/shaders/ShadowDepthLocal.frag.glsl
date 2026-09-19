#version 460 core
in vec2 vUV;
in vec3 vWorldPos;
uniform int uAlphaTest;
uniform float uAlphaCutoff; // #101
uniform sampler2D uAlbedo;
uniform vec2 uUVTiling; // #102 — (0,0) = unset -> (1,1)
uniform vec2 uUVOffset;
uniform vec3 uShadowLightPos;
uniform float uShadowFar;
void main() {
    vec2 uv = vUV * (uUVTiling == vec2(0.0) ? vec2(1.0) : uUVTiling) + uUVOffset;
    if (uAlphaTest == 1 && texture(uAlbedo, uv).a < uAlphaCutoff) discard;
    gl_FragDepth = clamp(distance(vWorldPos, uShadowLightPos) / max(uShadowFar, 1e-3), 0.0, 1.0);
}
