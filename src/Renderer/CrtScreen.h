#pragma once

class Shader;

// The editor's CRT screen: draws a finished frame (an RGBA8 texture) to a framebuffer through the
// launch screen's tube shader (CrtScreen.frag.glsl). Only runs while Preferences > CRT Screen is on.
class CrtScreen {
public:
    CrtScreen() = default;
    ~CrtScreen();
    CrtScreen(const CrtScreen&) = delete;
    CrtScreen& operator=(const CrtScreen&) = delete;

    // srcTex: the frame (width x height). curve: 0 flat .. 1 the launcher's tube.
    // strength: 0..1 for the scanlines, glow and vignette.
    void Apply(unsigned int srcTex, int width, int height, unsigned int dstFbo,
               float timeSeconds, float curve, float strength);

    // The shader's barrel curve on a 0..1 screen position (either y direction): the position in
    // the source frame that is drawn at `x, y` on screen. Used to bend the mouse the same way.
    static void Curve(float& x, float& y, float curve);

private:
    Shader* m_Shader = nullptr; // owned; raw pointer keeps this header free of <memory>
    unsigned int m_Vao = 0;
};
