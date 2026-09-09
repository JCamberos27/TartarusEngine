#version 460 core
in vec2 vUV;
in vec3 vWorldPos;
uniform int uAlphaTest;
uniform sampler2D uAlbedo;
uniform vec3 uShadowLightPos;
uniform float uShadowFar;
void main() {
    if (uAlphaTest == 1 && texture(uAlbedo, vUV).a < 0.5) discard;
    gl_FragDepth = clamp(distance(vWorldPos, uShadowLightPos) / max(uShadowFar, 1e-3), 0.0, 1.0);
}
