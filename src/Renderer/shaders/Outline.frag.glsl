#version 460 core
out vec4 FragColor;
uniform vec3 uOutlineColor;
void main() {
    FragColor = vec4(uOutlineColor, 1.0);
}
