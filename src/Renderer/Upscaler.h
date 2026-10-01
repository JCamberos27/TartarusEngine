#pragma once
#include <memory>

class Shader;

// Render-resolution upscale: the 3D frame renders and tonemaps at an internal resolution
// (EditorSettings::RenderHeight, 1080p by default) and this pass scales the finished LDR image
// to the display. A 4K monitor then costs what a 1080p one does, while the HUD and the editor
// UI, drawn after this pass, stay at native resolution.
//
// Two fullscreen passes: RCAS sharpening at the internal resolution (FSR1's sharpener), then a
// 9-tap Catmull-Rom bicubic to the display size. About 0.1 ms at 1080p -> 4K.
class Upscaler {
public:
    Upscaler();
    ~Upscaler();
    Upscaler(const Upscaler&) = delete;
    Upscaler& operator=(const Upscaler&) = delete;

    // The internal size for a `w` x `h` output: `targetHeight` tall with the same aspect, or the
    // output size itself when targetHeight <= 0 or the output is already no taller than it.
    static void InternalSize(int w, int h, int targetHeight, int& outW, int& outH);

    // srcTex: LDR colour, srcW x srcH, with linear filtering and clamp-to-edge. Writes dstW x dstH at (dstX, dstY) of dstFbo (0 = window).
    // sharpness 0..1 (0 skips the sharpen pass).
    void Apply(unsigned int srcTex, int srcW, int srcH, unsigned int dstFbo,
               int dstX, int dstY, int dstW, int dstH, float sharpness);

private:
    bool EnsureCreated();
    std::unique_ptr<Shader> m_Rcas, m_Upscale;
    unsigned int m_Vao = 0;
    unsigned int m_SharpTex = 0, m_SharpFbo = 0;
    int m_SharpW = 0, m_SharpH = 0;
    bool m_Failed = false;
};
