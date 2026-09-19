#pragma once
#include <memory>

class Shader;

// The Play-mode crosshair (dot, gravity gun hold ring and throw-charge arc), drawn straight into a
// finished frame: the Game view's framebuffer, the maximized backbuffer or a built game's
// window. One fullscreen triangle with alpha blending; owns its program and an empty VAO.
class CrosshairOverlay {
public:
    CrosshairOverlay();
    ~CrosshairOverlay();
    CrosshairOverlay(const CrosshairOverlay&) = delete;
    CrosshairOverlay& operator=(const CrosshairOverlay&) = delete;

    // `charge` 0..1 while a throw charges, negative otherwise.
    void Draw(unsigned int dstFbo, int width, int height, bool holding, float charge);

private:
    std::unique_ptr<Shader> m_Shader;
    unsigned int m_VAO = 0;
};
