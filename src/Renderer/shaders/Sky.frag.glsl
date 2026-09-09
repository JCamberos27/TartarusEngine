#version 460 core
in vec3 vNearPoint;
in vec3 vFarPoint;
out vec4 FragColor;

uniform vec3 uHorizonColor;
uniform vec3 uZenithColor;

void main() {
    vec3 dir = normalize(vFarPoint - vNearPoint);
    float t = clamp(dir.y, 0.0, 1.0);
    vec3 color = mix(uHorizonColor, uZenithColor, pow(t, 0.5));
    FragColor = vec4(color, 1.0);
}
