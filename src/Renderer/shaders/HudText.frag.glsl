#version 460 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uAtlas;
out vec4 FragColor;
void main() {
    FragColor = vec4(vColor.rgb, vColor.a * texture(uAtlas, vUV).r);
}
