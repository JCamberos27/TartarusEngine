#pragma once

#include <memory>

class Shader;

// The frosted-glass backdrop behind the editor's floating viewport HUDs (the opt-in Glass theme).
//
// Takes the tonemapped Scene image (an 8-bit, display-encoded texture the size of the viewport) and
// produces a wide, smooth blur of it at half resolution, entirely in compute:
//
//   1. Downsample  src -> D0 -> D1 -> ... -> D(n-1)   13-tap filter per level, decoding sRGB to
//                                                    linear on the first read (RGBA16F pyramid)
//   2. Upsample    D(n-1) -> U(n-2) -> ... -> U0      3x3 tent per level, pure blur (no add-back)
//   3. Resolve     U0 -> Out                          linear -> sRGB encode, gentle saturation
//
// Out is an RGBA8 texture ImGui can draw directly; the HUD plates sample it with UVs mapped from
// their screen rect into the viewport (Glass::DrawFrostedPlate), so the glass shows exactly the
// blurred scene that's behind it. The work shrinks 4x per level, so the whole chain costs about
// 1.4x its first downsample: 13 taps per half-resolution pixel (not yet profiled on the 1080 Ti;
// the Statistics panel's GPU scopes show it as "Glass Backdrop").
//
// Runs only on frames where a frosted overlay is visible; holds no GPU memory until first used and
// frees it in Release() (called when the Glass theme is switched off).
class GlassBackdrop {
public:
    GlassBackdrop();
    ~GlassBackdrop();
    GlassBackdrop(const GlassBackdrop&) = delete;
    GlassBackdrop& operator=(const GlassBackdrop&) = delete;

    // Blurs `srcTex` (width x height, display-encoded RGB(A)8). Returns the blurred texture's GL name,
    // or 0 if the GPU path is unavailable (shader compile failure is logged once, then the HUDs
    // fall back to a plain translucent tint). `levels` is clamped so the smallest level is >= 4 px.
    // `ceiling` caps the result's encoded luminance (1 = no cap), for glass that carries text.
    unsigned int Update(unsigned int srcTex, int width, int height, int levels = 5, float saturation = 1.12f,
                        float ceiling = 1.0f);

    // Blurs only the `w` x `h` texel rect at (`x`, `y`) of `srcTex` (`texW` x `texH`), read as if the rect
    // were the whole texture: a rect with a margin of BlurReach texels around what is shown gives that
    // part the blur Update would give it. The result covers the rect at half resolution from the blurred
    // texture's (0, 0): OutU / OutV map a source texel into it. The targets only grow (in 256-texel
    // steps), so a rect that changes size every frame (a popup easing in) reallocates nothing.
    unsigned int UpdateRect(unsigned int srcTex, int texW, int texH, int x, int y, int w, int h, int levels,
                            float saturation, float ceiling);
    // Sizes the targets for UpdateRect over any rect of a `texW` x `texH` source, so the blurred texture
    // (returned; 0 if the GPU path is unavailable) keeps its name through this frame's UpdateRect: a caller
    // can draw with it first and blur once it knows where it drew.
    unsigned int Reserve(int texW, int texH, int levels);
    // How far (source texels) the blur with `levels` levels reaches: a pixel's result reads nothing further.
    static int BlurReach(int levels);
    // Where source texel (sx, sy) of the last UpdateRect lands in the blurred texture, as a texture UV.
    float OutU(float sx) const { return m_W > 0 ? (sx - (float)m_RectX) * 0.5f / (float)m_W : 0.0f; }
    float OutV(float sy) const { return m_H > 0 ? (sy - (float)m_RectY) * 0.5f / (float)m_H : 0.0f; }

    unsigned int Texture() const { return m_Out; }
    // Frees every GL object (textures and programs). The next Update recreates them.
    void Release();

private:
    bool EnsurePrograms();
    void EnsureTargets(int halfW, int halfH, int levels, bool grow);
    unsigned int Run(unsigned int srcTex, int texW, int texH, int x, int y, int w, int h, int levels, float saturation,
                     float ceiling, bool grow);
    void ReleaseTargets();

    std::unique_ptr<Shader> m_Down, m_Up, m_Resolve;
    bool m_ProgramsFailed = false;
    unsigned int m_DownTex = 0; // RGBA16F, `m_Levels` mips from half resolution
    unsigned int m_UpTex = 0;   // RGBA16F, same chain (levels 0 .. n-2 used)
    unsigned int m_Out = 0;     // RGBA8, half resolution
    int m_W = 0, m_H = 0, m_Levels = 0;    // the targets' size (UpdateRect: at least the rect's half size)
    int m_RectX = 0, m_RectY = 0;          // the last UpdateRect's rect origin, in source texels
};
