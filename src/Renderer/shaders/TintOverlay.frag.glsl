#version 460 core
out vec4 FragColor;
uniform vec3 uTintColor;
uniform float uAlpha;
void main() {
    FragColor = vec4(uTintColor, uAlpha);
}
