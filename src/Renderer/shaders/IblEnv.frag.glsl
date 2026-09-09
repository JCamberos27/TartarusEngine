#version 460 core
#include "IblCubeDir.glsl"
in vec2 vUV;
out vec4 FragColor;

uniform vec3 uHorizonColor;
uniform vec3 uZenithColor;

void main() {
    vec3 dir = FaceDirection(vUV);
    vec3 color;
    if (dir.y >= 0.0) {
        color = mix(uHorizonColor, uZenithColor, pow(clamp(dir.y, 0.0, 1.0), 0.5));
    } else {
        color = mix(uHorizonColor, uHorizonColor * 0.3, pow(clamp(-dir.y, 0.0, 1.0), 0.5));
    }
    FragColor = vec4(color, 1.0);
}
