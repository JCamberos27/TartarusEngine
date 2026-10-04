#include "ScreenBlood.h"

#include <algorithm>
#include <cmath>

float ScreenBlood::Rand() {
    m_Rng ^= m_Rng << 13;
    m_Rng ^= m_Rng >> 17;
    m_Rng ^= m_Rng << 5;
    return (float)(m_Rng & 0xFFFFFFu) / (float)0x1000000;
}

glm::vec2 ScreenBlood::EdgePoint(float angleRad) {
    // Clockwise from ahead = up on the screen; out toward the frame's edge, kept off the very corners.
    const float s = std::sin(angleRad), c = std::cos(angleRad);
    const float k = 1.0f / std::max(std::abs(s) / 0.42f, std::abs(c) / 0.40f);
    return glm::vec2(0.5f + s * k, 0.5f + c * k);
}

void ScreenBlood::OnHurt(float damage, float angleRad, float health01) {
    if (!Enabled || damage <= 0.0f) return;
    const int count = 1 + (damage >= 25.0f ? 1 : 0) + (health01 < 0.35f ? 1 : 0);
    for (int i = 0; i < count; ++i) {
        Splat s;
        // The first lands where the round came from; the rest scatter round that edge.
        const float a = angleRad + (i == 0 ? 0.0f : (Rand() - 0.5f) * 1.6f);
        const glm::vec2 edge = EdgePoint(a);
        s.Pos = glm::clamp(edge + glm::vec2(Rand() - 0.5f, Rand() - 0.5f) * 0.12f, glm::vec2(0.04f), glm::vec2(0.96f));
        s.Size = std::clamp(0.28f + damage / 120.0f, 0.3f, 0.6f) * (0.8f + 0.4f * Rand());
        // The splash's spray points in from the edge: the sheet's streaks run up-right, so turn them toward the centre.
        const glm::vec2 in = glm::vec2(0.5f) - s.Pos;
        s.Rot = std::atan2(in.y, in.x) - 0.7854f + (Rand() - 0.5f) * 0.5f;
        s.Flip = Rand() < 0.5f;
        s.Life = (health01 < 0.35f ? 3.2f : 2.0f) + Rand();
        s.Strength = std::clamp(0.6f + damage / 60.0f, 0.6f, 1.0f);
        if ((int)m_Splats.size() >= MaxSplats) m_Splats.erase(m_Splats.begin());
        m_Splats.push_back(s);
    }
}

void ScreenBlood::Update(float dt) {
    for (Splat& s : m_Splats) {
        s.Age += dt;
        // Wet blood slides down the lens a little while it's fresh.
        if (s.Age > 0.3f) s.Pos.y -= dt * 0.012f * std::max(0.0f, 1.0f - s.Age / s.Life);
    }
    m_Splats.erase(std::remove_if(m_Splats.begin(), m_Splats.end(), [](const Splat& s) { return s.Age >= s.Life; }), m_Splats.end());
}

void ScreenBlood::FrameAt(float age, float life, int frames, int& frame, float& opacity) {
    frames = std::max(frames, 1);
    const int hold = frames / 2; // the sheet's fullest splash
    const float in = 0.12f;      // splashes in over a tenth of a second
    const float out = 0.45f * life;
    if (age < in) {
        frame = std::min(hold, (int)(age / in * (float)(hold + 1)));
        opacity = 1.0f;
    } else if (age < life - out) {
        frame = hold;
        opacity = 1.0f;
    } else { // thinning: on through the sheet's breaking-up frames as it fades
        const float t = std::clamp((age - (life - out)) / out, 0.0f, 1.0f);
        frame = std::min(frames - 1, hold + (int)(t * (float)(frames - hold)));
        opacity = 1.0f - t * t;
    }
}
