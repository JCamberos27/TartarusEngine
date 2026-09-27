#version 460 core
// Temporal accumulation for the volumetric clouds (one view). Each history texel is
// reprojected with the cloud depth the raymarch wrote, clamped to the current frame's 3x3
// neighbourhood (so a revealed or moved cloud can't leave a ghost), and blended with the
// current frame. The raymarch's per-frame jitter is what this averages away.
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

vec3 RgbToYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 YCoCgToRgb(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, uSize))) return;
    vec2 texel = 1.0 / vec2(uSize);
    vec2 uv = (vec2(px) + 0.5) * texel;
    vec4 cur = texelFetch(uCurrent, px, 0);
    if (uReset == 1) { imageStore(uOut, px, cur); return; }

    // Neighbourhood bounds in YCoCg (a better fit around the luma axis than an RGB box).
    vec4 mn = vec4(1e9), mx = vec4(-1e9);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec4 s = texelFetch(uCurrent, clamp(px + ivec2(x, y), ivec2(0), uSize - 1), 0);
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
        hist.rgb = RgbToYCoCg(hist.rgb);
        hist = clamp(hist, mn, mx);
        hist.rgb = YCoCgToRgb(hist.rgb);
        result = mix(hist, cur, uBlend);
    }
    imageStore(uOut, px, max(result, vec4(0.0)));
}
