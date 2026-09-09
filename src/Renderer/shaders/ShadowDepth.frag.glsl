#version 460 core
in vec2 vUV;
uniform int uAlphaTest;
uniform sampler2D uAlbedo;
void main() {
    if (uAlphaTest == 1 && texture(uAlbedo, vUV).a < 0.5) discard;
}
