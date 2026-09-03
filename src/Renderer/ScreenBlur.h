#pragma once

class Shader;

// Full-screen separable Gaussian blur for the frosted modal backdrop. Runs at half resolution
// with a 9-tap linear-sampled Gaussian applied horizontally then vertically, repeated a few
// times, then upsampled — smooth and cheap, and only invoked while a modal dialog is open.
class ScreenBlur {
public:
    ScreenBlur() = default;
    ~ScreenBlur();
    ScreenBlur(const ScreenBlur&) = delete;
    ScreenBlur& operator=(const ScreenBlur&) = delete;

    // srcTex: an LDR RGBA8 texture holding the whole composited frame (width x height).
    // Writes the blurred result into dstFbo at full resolution.
    void Apply(unsigned int srcTex, int width, int height, unsigned int dstFbo, int iterations = 3);

private:
    void Ensure(int halfW, int halfH);

    Shader* m_Shader = nullptr;   // owned; raw ptr keeps this header free of <memory>
    unsigned int m_Vao = 0;
    unsigned int m_Fbo[2] = {0, 0};
    unsigned int m_Tex[2] = {0, 0};
    int m_W = 0, m_H = 0;
};
