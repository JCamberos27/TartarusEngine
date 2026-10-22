#version 460 core
// The spray splat: a cone footprint (soft Gaussian to a hard core by the cap's hardness), grain in
// the core and speckled overspray out to the ring. Premultiplied colour into the layer; paint
// thickness (the relief) into the second target. Erase runs blend it the other way (C++ side).
#include "GraffitiCommon.glsl"

in vec2 vLocal;
flat in vec4 vParams;
flat in vec4 vColor;
flat in uint vSeed;
flat in float vRing;

uniform float uDotPx; // overspray dot size in texels (grows with the canvas)

layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oThick;

void main() {
    float r = length(vLocal);
    if (r > vRing) discard;
    float flow = vParams.y, over = vParams.z, hard = vParams.w;

    float soft = exp(-2.7 * r * r) * smoothstep(1.0, 0.82, r);
    float firm = 1.0 - smoothstep(0.78, 1.0, r);
    float core = mix(soft, firm, hard);

    // Per-texel grain, hashed off the texel and the splat's seed (integer maths: the same everywhere).
    uvec2 cell = uvec2(gl_FragCoord.xy / uDotPx);
    uint h = GHash(cell.x * 0x9E3779B1u ^ GHash(cell.y ^ vSeed));
    float grain = mix(1.0, 0.55 + 0.9 * GRand(h), 0.4 * (1.0 - hard));
    float a = flow * core * grain;

    // Overspray: sparse dots thinning out from the core edge to the ring.
    if (over > 0.0 && r > 0.55) {
        float density = over * min(flow * 6.0, 1.0) * (1.0 - smoothstep(0.55, vRing, r)) * 0.22;
        uint h2 = GHash(h ^ 0x51ED27u);
        if (GRand(h2) < density) a = max(a, 0.35 + 0.5 * GRand(GHash(h2)));
    }
    a = clamp(a, 0.0, 1.0);
    if (a <= 0.0) discard;
    oColor = vec4(vColor.rgb * a, a);
    oThick = vec4(a * vColor.w, 0.0, 0.0, a);
}
