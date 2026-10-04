#version 460 core
// Screen blood (docs/BLOOD_FX.md, v2): one quad per splat from gl_VertexID, in screen space.
uniform vec4 uSplat;  // xy centre (0..1, y up), z size (x screen height), w rotation
uniform float uAspect; // width / height
uniform int uFlip;

out vec2 vUV;

const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                 vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    vec2 c = kCorners[gl_VertexID];
    float cr = cos(uSplat.w), sr = sin(uSplat.w);
    vec2 p = vec2(c.x * cr - c.y * sr, c.x * sr + c.y * cr) * (0.5 * uSplat.z); // in screen heights
    vec2 ndc = (uSplat.xy + vec2(p.x / uAspect, p.y)) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = c * 0.5 + 0.5;
    if (uFlip == 1) vUV.x = 1.0 - vUV.x;
}
