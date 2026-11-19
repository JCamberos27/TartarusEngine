#pragma once

#include "HudText.h"

#include <glm/glm.hpp>

#include <vector>

class NpcDirector;

// The debug overlay drawn over Play with text and shapes: the AI overlay (the director's lines and a label per dummy) and the
// F-row's world lines (F6 Nav / Hitboxes). One HudText batch, one draw call.
struct CombatHudInput {
    unsigned Fbo = 0;
    int Width = 0, Height = 0;
    glm::mat4 ViewProj{1.0f};
    glm::vec3 CamPos{0.0f};
    bool AiOverlay = false;
    const std::vector<float>* Lines = nullptr; // F6 Nav / Hitboxes: world lines, 7 floats per vertex (xyz rgba), two per line
};

class CombatHud {
public:
    // Draws the frame into `in.Fbo`.
    void Draw(const CombatHudInput& in, const NpcDirector& npcs);

private:
    void DrawDebug(const CombatHudInput& in, const NpcDirector& npcs);
    void DrawLines(const CombatHudInput& in, const std::vector<float>& lines); // world lines, clipped to the near plane
    bool Project(const CombatHudInput& in, const glm::vec3& p, glm::vec2& out) const;

    HudText m_Text;
    std::vector<float> m_Lines; // scratch for the director's debug lines
};
