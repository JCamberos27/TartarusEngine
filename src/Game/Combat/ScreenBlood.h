#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

// Blood on the player's view when they're hurt (docs/BLOOD_FX.md, v2): Real Blood's "PlayerDamage" splash
// (the `blood_side` flipbook), thrown onto the edge of the screen the round came from. It splashes in over a
// tenth of a second, hangs there wet and slides a little, then thins away. Harder hits leave more of it; at
// low health some of it stays. Owns no GL: ScreenBloodOverlay draws Splats().
class ScreenBlood {
public:
    struct Splat {
        glm::vec2 Pos{0.5f};   // screen, 0..1 (y up)
        float Size = 0.3f;     // x the screen's height
        float Rot = 0.0f;      // radians
        float Age = 0.0f, Life = 2.0f;
        float Strength = 1.0f; // opacity
        bool Flip = false;     // mirrored (left / right)
    };
    int MaxSplats = 8;
    bool Enabled = true;

    // A hit: `damage` health points, `angleRad` where it came from (clockwise from straight ahead, as the HUD's
    // damage arcs), `health01` what's left after it.
    void OnHurt(float damage, float angleRad, float health01);
    void Update(float dt);
    void Clear() { m_Splats.clear(); }
    const std::vector<Splat>& Splats() const { return m_Splats; }

    // The splash's flipbook frame (of `frames`) and opacity at `age` into `life`: in fast, held, thinning out.
    static void FrameAt(float age, float life, int frames, int& frame, float& opacity);
    // The point on the screen's edge a hit from `angleRad` lands on (before jitter).
    static glm::vec2 EdgePoint(float angleRad);

private:
    std::vector<Splat> m_Splats;
    std::uint32_t m_Rng = 0x2545F491u;
    float Rand();
};
