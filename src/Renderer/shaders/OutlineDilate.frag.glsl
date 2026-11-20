#version 460 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uMask; // silhouettes at the scene's size, 4x MSAA resolved: R the selection, G the hovered mesh
uniform vec3 uTexel;     // xy = 1.0 / scene size
uniform vec3 uColor;
uniform int uRadius;     // outline width, in scene pixels
// Motion v2 (EditorLayer::SelectionOutlineReveal): after a selection change the selection's ring sweeps on
// clockwise from the top around uSweepCenter (UV). uSweepHide is the part still to draw, so the
// default 0 (an unset uniform) is the whole ring.
uniform float uSweepHide;
uniform vec2 uSweepCenter;
uniform float uHoverFade; // the hover pre-highlight's ring: 0.65 = 35% of the selection's
void main() {
    // Anti-aliased rings. The mask is sampled on circles at sub-pixel offsets rather than on the
    // pixel grid, and its coverage is continuous, so a ring's value varies smoothly with how
    // far this pixel is from the silhouette: no stair-steps on its inner or outer edge.
    vec2 here = texture(uMask, vUV).rg;
    if (here.r > 0.999) discard;             // fully inside the selection: leave the surface alone
    const int kDirections = 16;
    const float kStep = 6.28318530718 / float(kDirections);
    vec2 ring = here;
    for (int ri = 1; ri <= 2; ++ri) {
        float r = float(uRadius) * float(ri) * 0.5;
        for (int i = 0; i < kDirections; ++i) {
            float a = (float(i) + 0.5 * float(ri - 1)) * kStep; // the two circles interleave
            ring = max(ring, texture(uMask, vUV + vec2(cos(a), sin(a)) * r * uTexel.xy).rg);
        }
    }
    float sel = ring.r * (1.0 - here.r);     // an edge pixel the mesh partly covers blends in
    vec3 color = uColor;
    if (uSweepHide > 0.0) {
        vec2 d = (vUV - uSweepCenter) / uTexel.xy;              // scene pixels, y up
        float a = fract(atan(d.x, d.y) / 6.28318530718 + 1.0);  // 0 at the top, clockwise
        float reveal = 1.0 - uSweepHide;
        sel *= 1.0 - smoothstep(reveal - 0.02, reveal, a);
        // The leading edge glints toward white as it travels.
        float head = 1.0 - smoothstep(0.0, 0.10, reveal - a);
        color = mix(uColor, vec3(1.0), head * step(a, reveal) * 0.75);
    }
    // The hovered mesh's ring, fainter, kept off the selection's surface.
    float hover = ring.g * (1.0 - here.g) * (1.0 - here.r) * (1.0 - uHoverFade);
    if (hover > sel) color = uColor;
    float alpha = max(sel, hover);
    if (alpha < 0.004) discard;
    FragColor = vec4(color, alpha);
}
