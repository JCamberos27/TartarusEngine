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
    // Capped well below half-float max: a sun texel stored near 65k would otherwise overflow the
    // HDR target's bloom downsample (a sum of several texels) to Inf and draw a black square on
    // the sun. Anything this bright is pure white after tonemapping anyway.
    FragColor = vec4(min(texture(uEnvMap, rotDir).rgb, vec3(1000.0)), 1.0);
}
