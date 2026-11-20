#pragma once

#include "FullscreenPass.h"

#include <memory>

class Shader;

// Full-screen separable Gaussian blur for the frosted modal backdrop. Runs at half resolution
// with a 9-tap linear-sampled Gaussian applied horizontally then vertically, repeated a few
// times, then upsampled — smooth and cheap, and only invoked while a modal dialog is open.
class ScreenBlur {
public:
    ScreenBlur();
    ~ScreenBlur();
    ScreenBlur(const ScreenBlur&) = delete;
    ScreenBlur& operator=(const ScreenBlur&) = delete;

    // srcTex: an LDR RGBA8 texture holding the whole composited frame (width x height).
    // Writes the blurred result into dstFbo at full resolution.
    void Apply(unsigned int srcTex, int width, int height, unsigned int dstFbo, int iterations = 3);

private:
    std::unique_ptr<Shader> m_Shader;
    ColorTarget m_Half[2]; // half-resolution ping-pong
};
