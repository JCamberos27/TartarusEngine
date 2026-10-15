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
    unsigned int Update(unsigned int srcTex, int width, int height, int levels = 5, float saturation = 1.12f);

    unsigned int Texture() const { return m_Out; }
    // Frees every GL object (textures and programs). The next Update recreates them.
    void Release();

private:
    bool EnsurePrograms();
    void EnsureTargets(int halfW, int halfH, int levels);
    void ReleaseTargets();

    std::unique_ptr<Shader> m_Down, m_Up, m_Resolve;
    bool m_ProgramsFailed = false;
    unsigned int m_DownTex = 0; // RGBA16F, `m_Levels` mips from half resolution
    unsigned int m_UpTex = 0;   // RGBA16F, same chain (levels 0 .. n-2 used)
    unsigned int m_Out = 0;     // RGBA8, half resolution
    int m_W = 0, m_H = 0, m_Levels = 0;
};
