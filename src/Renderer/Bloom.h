#pragma once

#include "FullscreenPass.h"

class Shader;

// Bloom post-process: a Call-of-Duty / UE4-style mip pyramid. A soft-knee threshold extracts
// HDR energy above a luminance cutoff at half resolution (mip 0), a 4-tap box filter downsamples
// it through 4 progressively smaller mips, then a 3x3 tent filter walks back up, additively
// blending each mip into the next larger one. The result (mip 0, now holding the accumulated
// multi-scale glow) is added into linear HDR by the Tonemapper before the tone curve — this
// gives a soft, wide-radius glow from a handful of small fullscreen passes instead of one
// large-kernel blur, and matches how bright sources actually spread light in real optics.
class Bloom {
public:
    Bloom() = default;
    Bloom(const Bloom&) = delete;
    Bloom& operator=(const Bloom&) = delete;

    // width/height: the FULL source (screen) resolution — mip 0 is created at half that size.
    void Resize(int width, int height);

    void Compute(Shader& threshShader, Shader& downsampleShader, Shader& upsampleShader,
                 unsigned int hdrTex, float threshold, float knee);

    // Accumulated glow at half source resolution; add into HDR before tonemapping. Valid after Compute().
    unsigned int GlowTexture() const { return m_Mips[0].Texture(); }
    // True only when every mip FBO created AND validated complete (audit #358). ViewPipeline::ComputeBloom
    // gates running Compute() and adding the glow on this — an invalid Bloom just means no glow.
    bool IsValid() const { return m_Valid; }

private:
    static constexpr int kMipCount = 5; // mip0 = half-res; mip1..4 = quarter/8th/16th/32nd
    ColorTarget m_Mips[kMipCount]; // RGBA16F, each half the size of the one before
    bool m_Valid = false;          // every mip FBO validated complete (audit #358)
};
