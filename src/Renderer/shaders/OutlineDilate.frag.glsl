#version 460 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uMask;
uniform vec3 uTexel;     // xy = 1.0 / mask size, in texels
uniform vec3 uColor;
uniform int uRadius;     // outline half-width, in pixels
void main() {
    float here = texture(uMask, vUV).r;
    if (here > 0.5) discard;                 // inside the selection: leave the surface alone
    float adj = 0.0;
    for (int y = -uRadius; y <= uRadius; ++y) {
        for (int x = -uRadius; x <= uRadius; ++x) {
            if (x * x + y * y > uRadius * uRadius) continue; // round brush
            adj = max(adj, texture(uMask, vUV + vec2(float(x), float(y)) * uTexel.xy).r);
        }
    }
    if (adj < 0.5) discard;                  // not adjacent to the selection
    FragColor = vec4(uColor, 1.0);
}
