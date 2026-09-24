#pragma once
#include <glm/glm.hpp>
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

    // Where and how the dot draws. Default: the white dot at the centre.
    struct Dot {
        bool Visible = true;
        glm::vec2 Pixel{-1.0f};          // bottom-left origin; negative = the centre
        glm::vec3 Color{1.0f};
        // > 0: a laser spot of this radius in pixels (hot core, soft glow, no rim) instead of the
        // fixed-size crosshair dot.
        float Radius = 0.0f;
    };
    // `charge` 0..1 while a throw charges, negative otherwise.
    void Draw(unsigned int dstFbo, int width, int height, bool holding, float charge, const Dot& dot = {});

private:
    std::unique_ptr<Shader> m_Shader;
    unsigned int m_VAO = 0;
};
