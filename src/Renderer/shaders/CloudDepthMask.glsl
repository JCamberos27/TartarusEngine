// The scene depth the cloud passes skip hidden texels by (CloudRaymarch / CloudTemporal).
layout(binding = 12) uniform sampler2D uSceneDepth;
uniform int uSceneDepthOn;
uniform ivec2 uDepthSize;
uniform int uDivisor;
// True when every scene pixel within `margin` cloud texels of `t` has geometry in front of the sky. The temporal pass
// leaves out what is hidden within 1 (nothing that reads such a texel - the sky's bilinear fetch - can show it); the
// raymarch leaves out what is hidden within 2, so the marched texel every resolved one falls back on is always there.
bool CloudTexelHidden(ivec2 t, int margin) {
    if (uSceneDepthOn == 0) return false;
    ivec2 p0 = max((t - margin) * uDivisor, ivec2(0));
    ivec2 p1 = min((t + margin + 1) * uDivisor, uDepthSize);
    vec2 inv = 1.0 / vec2(uDepthSize);
    for (int y = p0.y; y < p1.y; y += 2)
        for (int x = p0.x; x < p1.x; x += 2) {
            vec4 d = textureGather(uSceneDepth, (vec2(x, y) + 1.0) * inv); // pixels x..x+1, y..y+1
            if (max(max(d.x, d.y), max(d.z, d.w)) >= 1.0) return false;    // the far plane: sky shows here
        }
    return true;
}
