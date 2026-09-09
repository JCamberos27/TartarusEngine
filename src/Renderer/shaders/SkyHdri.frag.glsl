#version 460 core
in vec3 vNearPoint;
in vec3 vFarPoint;
out vec4 FragColor;

uniform samplerCube uEnvMap;
uniform float uRotation; // Y-axis rotation in radians

void main() {
    vec3 dir = normalize(vFarPoint - vNearPoint);
    // Rotate the direction around the world Y axis so the user can position the HDRI.
    float c = cos(uRotation), s = sin(uRotation);
    vec3 rotDir = vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
    FragColor = vec4(texture(uEnvMap, rotDir).rgb, 1.0);
}
