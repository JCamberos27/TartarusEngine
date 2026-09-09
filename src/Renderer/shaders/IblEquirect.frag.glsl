#version 460 core
#include "IblCubeDir.glsl"
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uEquirect;
uniform float uExposure;

// Converts a 3D direction to equirectangular UV coordinates.
// atan(z, x) in [-PI, PI] mapped to [0,1]; asin(y) in [-PI/2, PI/2] mapped to [0,1].
const vec2 kInvAtan = vec2(0.15915494, 0.31830989); // 1/(2*PI), 1/PI

void main() {
    vec3 dir = normalize(FaceDirection(vUV));
    vec2 uv  = vec2(atan(dir.z, dir.x), asin(clamp(dir.y, -1.0, 1.0)));
    uv = uv * kInvAtan + 0.5;
    FragColor = vec4(texture(uEquirect, uv).rgb * uExposure, 1.0);
}
