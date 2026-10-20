#version 460 core
// World text (WorldText.h): one quad per glyph from gl_VertexID, no vertex buffer. The label's axes come
// from the CPU already turned for its facing (fixed, billboard, upright) and scaled to metres.

struct WorldGlyph {
    vec4 Anchor; // xyz world
    vec4 Right;  // xyz world metres per layout unit across, w outline
    vec4 Up;     // xyz world metres per layout unit up
    vec4 Rect;   // layout lo.xy, hi.xy
    vec4 Uv;     // u0 v0 (top) u1 v1 (bottom)
    vec4 Color;  // rgb linear, a fade
};
layout(std430, binding = 15) readonly buffer WorldGlyphs { WorldGlyph uGlyphs[]; };

uniform mat4 uViewProj;

out vec2 vUV;
out vec4 vColor;
out float vOutline;

const vec2 kCorners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                 vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main() {
    WorldGlyph g = uGlyphs[gl_BaseInstance + gl_InstanceID];
    vec2 c = kCorners[gl_VertexID];
    vec2 p = mix(g.Rect.xy, g.Rect.zw, c);
    vec3 world = g.Anchor.xyz + g.Right.xyz * p.x + g.Up.xyz * p.y;
    gl_Position = uViewProj * vec4(world, 1.0);
    vUV = vec2(mix(g.Uv.x, g.Uv.z, c.x), mix(g.Uv.w, g.Uv.y, c.y));
    vColor = g.Color;
    vOutline = g.Right.w;
}
