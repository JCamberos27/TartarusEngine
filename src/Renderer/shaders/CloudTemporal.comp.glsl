#version 460 core
// Temporal accumulation for the volumetric clouds (one view). Each history texel is
// reprojected with the cloud depth the raymarch wrote, clamped to the current frame's 3x3
// neighbourhood (so a revealed or moved cloud can't leave a ghost), and blended with the
// current frame. The raymarch's per-frame jitter is what this averages away.
// Checkerboarded (uChecker >= 0): only one texel of each 2x2 block was marched this frame. It
// blends as above; the other three keep their reprojected history, clamped to the box of the
// fresh texels around them, and fall back to the nearest fresh texel where history is missing.
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba16f) writeonly uniform image2D uOut;

layout(binding = 7) uniform sampler2D uCurrent;      // rgb radiance, a transmittance
layout(binding = 8) uniform sampler2D uCurrentDepth; // km
layout(binding = 9) uniform sampler2D uHistory;

uniform mat4 uInvViewProj;
uniform mat4 uPrevViewProj;
uniform vec3 uCameraWorld;
uniform float uKmToWorld;
uniform ivec2 uSize;
uniform float uBlend;   // weight of the current frame
uniform int uReset;     // 1 = no usable history (first frame, resize, camera cut)
uniform ivec2 uChecker; // the marched texel of each 2x2 block this frame; (-1, -1) = all

// This view's depth (the depth pre-pass, SSAO's copy of it) at full resolution, uDivisor scene pixels per cloud texel.
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

vec3 RgbToYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 YCoCgToRgb(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, uSize))) return;
    // Hidden by the scene all around (CloudTexelHidden): no clouds here this frame. Alpha 2 (a transmittance never is)
    // marks it, so a texel that reprojects onto it next frame starts afresh instead of from stale clouds.
    if (CloudTexelHidden(px, 1)) { imageStore(uOut, px, vec4(0.0, 0.0, 0.0, 2.0)); return; }
    vec2 texel = 1.0 / vec2(uSize);
    vec2 uv = (vec2(px) + 0.5) * texel;
    const bool checker = uChecker.x >= 0;
    const ivec2 base = checker ? (px & ~1) + uChecker : px; // nearest texel marched this frame
    const int stride = checker ? 2 : 1;
    const bool fresh = !checker || base == px;
    vec4 cur = texelFetch(uCurrent, min(base, uSize - 1), 0);
    if (uReset == 1) { imageStore(uOut, px, cur); return; }

    // Neighbourhood bounds in YCoCg (a better fit around the luma axis than an RGB box), over
    // this frame's marched texels in a window centred on this one: its 3x3 without the
    // checkerboard; with it, the marched texels within two texels on each axis (-2/0/+2 on an
    // axis it shares with them, -1/+1 on one it doesn't), so every texel's box is centred on it.
    const ivec2 lo = checker ? px - 2 + ((px + uChecker) & 1) : px - 1; // first marched texel >= px - 2
    const ivec2 n = checker ? 3 - ((px + uChecker) & 1) : ivec2(3);
    vec4 mn = vec4(1e9), mx = vec4(-1e9);
    for (int y = 0; y < n.y; ++y)
        for (int x = 0; x < n.x; ++x) {
            vec4 s = texelFetch(uCurrent, clamp(lo + ivec2(x, y) * stride, ivec2(0), uSize - 1), 0);
            if (s.a > 1.5) continue; // a hidden texel, not marched
            s.rgb = RgbToYCoCg(s.rgb);
            mn = min(mn, s);
            mx = max(mx, s);
        }
    // A loose box: the raymarch's jitter noise spans most of it, so a tight (variance-clipped)
    // box would pin the history to this frame's noise and never average it away. Clouds move
    // slowly enough that ghosting isn't the risk it is for geometry.
    vec4 extent = (mx - mn) * 0.25;
    mn -= extent;
    mx += extent;

    // Reproject: the pixel's cloud point, seen by last frame's camera.
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 nearP = uInvViewProj * vec4(ndc, -1.0, 1.0);
    vec4 farP = uInvViewProj * vec4(ndc, 1.0, 1.0);
    vec3 rd = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);
    float depthKm = texelFetch(uCurrentDepth, px, 0).r;
    vec3 worldPt = uCameraWorld + rd * depthKm * uKmToWorld;
    vec4 prevClip = uPrevViewProj * vec4(worldPt, 1.0);
    vec2 prevUv = prevClip.w > 0.0 ? prevClip.xy / prevClip.w * 0.5 + 0.5 : vec2(-1.0);

    vec4 result = cur;
    if (all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)))) {
        vec4 hist = texture(uHistory, prevUv);
        if (any(isnan(hist)) || any(isinf(hist)) || hist.a > 1.001) hist = cur; // > 1: touches a texel that was hidden
        if (mn.x > mx.x) { mn = vec4(-1e9); mx = vec4(1e9); } // (every neighbour hidden: no box)
        hist.rgb = RgbToYCoCg(hist.rgb);
        hist = clamp(hist, mn, mx);
        hist.rgb = YCoCgToRgb(hist.rgb);
        result = fresh ? mix(hist, cur, uBlend) : hist;
    }
    imageStore(uOut, px, max(result, vec4(0.0)));
}
