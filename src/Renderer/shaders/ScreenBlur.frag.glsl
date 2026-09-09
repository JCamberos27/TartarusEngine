#version 460 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uDir;
void main() {
    vec2 o1 = uDir * 1.3846153846;
    vec2 o2 = uDir * 3.2307692308;
    vec3 c  = texture(uTex, vUV).rgb      * 0.2270270270;
    c += texture(uTex, vUV + o1).rgb * 0.3162162162;
    c += texture(uTex, vUV - o1).rgb * 0.3162162162;
    c += texture(uTex, vUV + o2).rgb * 0.0702702703;
    c += texture(uTex, vUV - o2).rgb * 0.0702702703;
    FragColor = vec4(c, 1.0);
}
