#pragma once
#include <memory>

class Shader;
struct PlayerHudState;

// The combat HUD (Combat/PlayerVitals.h's PlayerHudState) drawn straight into a finished frame,
// like CrosshairOverlay: one fullscreen triangle with alpha blending.
class PlayerHudOverlay {
public:
    PlayerHudOverlay();
    ~PlayerHudOverlay();
    PlayerHudOverlay(const PlayerHudOverlay&) = delete;
    PlayerHudOverlay& operator=(const PlayerHudOverlay&) = delete;

    void Draw(unsigned int dstFbo, int width, int height, const PlayerHudState& state, float time);

private:
    std::unique_ptr<Shader> m_Shader;
    unsigned int m_VAO = 0;
    bool m_Failed = false;
};
