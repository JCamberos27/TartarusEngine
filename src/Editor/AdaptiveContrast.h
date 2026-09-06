#pragma once
#include <imgui.h>

// Async (PBO-backed) state for one adaptive-contrast sampling call site (#178). SampleTextureLuminance
// ping-pongs two pixel-pack buffer objects: each call kicks off a non-blocking glReadPixels into
// one PBO and, in the same call, maps+consumes whatever the OTHER PBO was loaded with by the
// previous kickoff (one throttled ~100ms tick earlier) — so the GPU is never stalled waiting for
// the readback. Each HUD element that samples independently (Stats HUD, History HUD, status bar,
// nav-gizmo cluster, Play/Stop button, the Game-view overlays) needs its own instance so their
// reads don't clobber one another.
struct AsyncLuminanceReadback {
    unsigned int Pbo[2] = { 0, 0 }; // fixed-size (kMaxLumPatch^2 * 4 bytes) buffers, DSA-mapped
    int Pending[2] = { 0, 0 };      // pixel count kicked into that slot and not yet consumed (0 = none)
    int Next = 0;                   // slot to write into on the next call; the other slot is read
    unsigned int SampleFbo = 0;     // scratch read-FBO this readback attaches the sampled texture to
};

// Non-blocking readback of the mean luminance (0..1; <0 while the first result is still in flight)
// of a small screen-space box over `colorTex`. `imgPos`/`imgSize` are where that texture is drawn
// on screen — may be letterboxed and a different size than the texture itself (the Game view).
// Throttle calls yourself (~10 Hz); it still does a GPU->CPU sync, just a cheap non-blocking one.
float SampleTextureLuminance(AsyncLuminanceReadback& rb, unsigned int colorTex, int texW, int texH,
                             ImVec2 imgPos, ImVec2 imgSize, ImVec2 centerScreen, float boxPx);

// Maps a sampled luminance to a 0..1 "how light should the overlay be": 1 = white-on-dark,
// 0 = black-on-light, smoothstep across the mid range. The curve both HUD call sites used.
inline float ContrastForLuminance(float lum) {
    float t = (lum - 0.30f) / (0.62f - 0.30f);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}
