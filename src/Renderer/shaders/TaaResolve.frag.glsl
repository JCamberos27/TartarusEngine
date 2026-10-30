#version 460 core
// TAA resolve (TemporalAA.cpp). Works in a tone-mapped space (c / (1 + max(c))) so a single bright texel can't
// dominate the blend (fireflies), with the neighbourhood statistics in YCoCg. Writes the new history: rgb in that
// space, a = this pixel's linear depth (the next frame's disocclusion test).
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uColor;     // this frame, linear HDR (jittered)
uniform sampler2D uDepth;     // this frame's depth (jittered)
uniform sampler2DMS uVelocity; // UV motion (current - previous); x > 2 = not written: reproject from depth
uniform sampler2D uHistory;   // last frame's history (bilinear)
uniform int  uHistoryValid;
uniform int  uOrtho;
uniform mat4 uInvViewProj;    // this frame's jittered clip -> world
uniform mat4 uCurViewProj;    // this frame, unjittered
uniform mat4 uPrevViewProj;   // last frame, unjittered
uniform vec2 uJitterUv;       // this frame's jitter in UV
uniform vec2 uTexel;

vec3 ToWork(vec3 c) { c = max(c, vec3(0.0)); return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 FromWork(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }
vec3 RgbToYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 YCoCgToRgb(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Clip `h` toward the box centre until it lies inside [lo, hi] (Playdead / Salvi): keeps the history's hue
// instead of the per-channel clamp's colour shifts.
vec3 ClipToBox(vec3 lo, vec3 hi, vec3 h) {
    vec3 c = 0.5 * (hi + lo), e = 0.5 * (hi - lo) + 1e-5;
    vec3 v = h - c;
    vec3 a = abs(v / e);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? c + v / m : h;
}

// 5-tap Catmull-Rom (bilinear taps at the corners dropped): keeps the history sharp under sub-pixel motion,
// where plain bilinear would blur it a little every frame.
vec3 SampleHistory(vec2 uv) {
    vec2 size = 1.0 / uTexel;
    vec2 pos = uv * size;
    vec2 c1 = floor(pos - 0.5) + 0.5;
    vec2 f = pos - c1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 tc0 = (c1 - 1.0) * uTexel, tc3 = (c1 + 2.0) * uTexel, tc12 = (c1 + w2 / w12) * uTexel;
    vec3 r = texture(uHistory, vec2(tc12.x, tc0.y)).rgb * (w12.x * w0.y)
           + texture(uHistory, vec2(tc0.x, tc12.y)).rgb * (w0.x * w12.y)
           + texture(uHistory, tc12).rgb * (w12.x * w12.y)
           + texture(uHistory, vec2(tc3.x, tc12.y)).rgb * (w3.x * w12.y)
           + texture(uHistory, vec2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / max(wsum, 1e-4), vec3(0.0));
}

vec3 WorldAt(vec2 uv, float d) {
    vec4 p = uInvViewProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 maxPx = textureSize(uColor, 0) - 1;
    vec2 uv = (vec2(px) + 0.5) * uTexel;

    // 3x3: colour moments for the variance box, and the nearest depth (its motion vector is used, so an edge
    // moving over a still background drags its own history along instead of the background's).
    vec3 m1 = vec3(0.0), m2 = vec3(0.0), cur = vec3(0.0);
    vec3 boxMin = vec3(1e9), boxMax = vec3(-1e9);
    float closest = 1.0, centreDepth = 1.0;
    ivec2 closestPx = px;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            ivec2 q = clamp(px + ivec2(x, y), ivec2(0), maxPx);
            vec3 c = RgbToYCoCg(ToWork(texelFetch(uColor, q, 0).rgb));
            m1 += c;
            m2 += c * c;
            boxMin = min(boxMin, c);
            boxMax = max(boxMax, c);
            float d = texelFetch(uDepth, q, 0).r;
            if (x == 0 && y == 0) { cur = c; centreDepth = d; }
            if (d < closest) { closest = d; closestPx = q; }
        }
    }

    vec3 world = WorldAt(uv - uJitterUv, centreDepth);
    float linDepth = min(uOrtho == 1 ? centreDepth * 1000.0 : (uCurViewProj * vec4(world, 1.0)).w, 60000.0);

    vec2 vel = texelFetch(uVelocity, closestPx, 0).rg;
    if (vel.x > 2.0) { // nothing wrote motion here: the camera's, from depth
        vec2 cuv = (vec2(closestPx) + 0.5) * uTexel;
        vec4 prev = uPrevViewProj * vec4(WorldAt(cuv - uJitterUv, closest), 1.0);
        vel = prev.w > 1e-5 ? (cuv - uJitterUv) - (prev.xy / prev.w * 0.5 + 0.5) : vec2(0.0);
    }
    vec2 prevUv = uv - vel;

    float blend = 1.0; // weight of the current frame
    vec3 result = cur;
    if (uHistoryValid == 1 && all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)))) {
        // Disocclusion: last frame something nearer covered this spot (its history depth is well in front of
        // where this surface was then) - the history is the occluder's colour, so start fresh.
        bool disoccluded = false;
        if (uOrtho == 0) {
            vec4 prevClip = uPrevViewProj * vec4(world, 1.0);
            float expected = prevClip.w;
            float histDepth = texelFetch(uHistory, clamp(ivec2(prevUv / uTexel), ivec2(0), maxPx), 0).a;
            disoccluded = histDepth < expected * 0.94 - 0.05;
        }
        if (!disoccluded) {
            vec3 hist = RgbToYCoCg(SampleHistory(prevUv));
            vec3 mean = m1 / 9.0;
            vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
            float speedPx = length(vel / uTexel);
            float gamma = mix(1.25, 0.9, clamp(speedPx / 4.0, 0.0, 1.0)); // tighter box in motion: less ghosting
            vec3 lo = max(mean - gamma * sigma, boxMin), hi = min(mean + gamma * sigma, boxMax);
            hist = ClipToBox(lo, hi, hist);
            // More of the new frame when moving (sub-pixel resampling blurs the history) and less when still.
            blend = clamp(0.08 + speedPx * 0.025, 0.08, 0.3);
            result = mix(hist, cur, blend);
        }
    }
    FragColor = vec4(YCoCgToRgb(result), linDepth);
}
