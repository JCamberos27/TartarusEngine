#version 460 core
// #107 - backdrop behind the material preview (Inspector sphere + Asset Browser thumbnails).
// Drawn twice per preview: once in linear HDR into the refraction source (uTonemap = 0, so a
// _TRANSMISSION material bends the same backdrop the viewer sees), and once tonemapped into the
// visible target (uTonemap = 1, the same Reinhard + gamma the model shader applies there).
in vec2 vUV;
out vec4 FragColor;

uniform int  uChecker; // 1: checkerboard, so transparent / cutout / glass materials read as such
uniform int  uTonemap;
uniform vec2 uSize;    // target size in pixels

void main() {
    vec3 top    = vec3(0.17, 0.18, 0.21);
    vec3 bottom = vec3(0.055, 0.06, 0.07);
    vec3 c = mix(bottom, top, smoothstep(0.0, 1.0, vUV.y));
    if (uChecker == 1) {
        float cellPx = max(uSize.y / 9.0, 4.0);
        vec2 cell = floor(gl_FragCoord.xy / cellPx);
        if (mod(cell.x + cell.y, 2.0) > 0.5) c = c * 2.2 + vec3(0.02);
    }
    vec2 d = vUV - 0.5;
    c *= 1.0 - dot(d, d) * 0.7; // soft vignette
    if (uTonemap == 1) {
        c = c / (c + vec3(1.0));
        c = pow(c, vec3(1.0 / 2.2));
    }
    FragColor = vec4(c, 1.0);
}
