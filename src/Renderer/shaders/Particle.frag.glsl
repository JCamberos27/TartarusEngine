#version 460 core
// #177 - soft round particle: alpha falls off smoothly towards the quad's edge.
in vec2 vCorner;
in vec4 vColor;
out vec4 FragColor;

void main() {
    float r2 = dot(vCorner, vCorner);
    if (r2 >= 1.0) discard;
    float soft = 1.0 - smoothstep(0.0, 1.0, r2);
    FragColor = vec4(vColor.rgb, vColor.a * soft);
}
