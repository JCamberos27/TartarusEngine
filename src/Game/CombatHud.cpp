#include "CombatHud.h"

#include "AI/NpcDirector.h"

#include <cmath>
#include <cstdio>

bool CombatHud::Project(const CombatHudInput& in, const glm::vec3& p, glm::vec2& out) const {
    const glm::vec4 c = in.ViewProj * glm::vec4(p, 1.0f);
    if (c.w < 0.05f) return false;
    out = glm::vec2((c.x / c.w * 0.5f + 0.5f) * (float)in.Width, (1.0f - (c.y / c.w * 0.5f + 0.5f)) * (float)in.Height);
    return true;
}

void CombatHud::DrawLines(const CombatHudInput& in, const std::vector<float>& lines) {
    const float s = m_Text.Scale();
    const float kNear = 0.05f, maxDist2 = 80.0f * 80.0f;
    for (size_t i = 0; i + 14 <= lines.size(); i += 14) {
        const glm::vec3 a(lines[i], lines[i + 1], lines[i + 2]), b(lines[i + 7], lines[i + 8], lines[i + 9]);
        const glm::vec4 col(lines[i + 3], lines[i + 4], lines[i + 5], lines[i + 6]);
        const glm::vec3 da = a - in.CamPos, db = b - in.CamPos;
        if (glm::dot(da, da) > maxDist2 && glm::dot(db, db) > maxDist2) continue;
        glm::vec4 ca = in.ViewProj * glm::vec4(a, 1.0f), cb = in.ViewProj * glm::vec4(b, 1.0f);
        if (ca.w < kNear && cb.w < kNear) continue;
        if (ca.w < kNear) ca = ca + (cb - ca) * ((kNear - ca.w) / (cb.w - ca.w));
        if (cb.w < kNear) cb = cb + (ca - cb) * ((kNear - cb.w) / (ca.w - cb.w));
        const glm::vec2 pa((ca.x / ca.w * 0.5f + 0.5f) * (float)in.Width, (1.0f - (ca.y / ca.w * 0.5f + 0.5f)) * (float)in.Height);
        const glm::vec2 pb((cb.x / cb.w * 0.5f + 0.5f) * (float)in.Width, (1.0f - (cb.y / cb.w * 0.5f + 0.5f)) * (float)in.Height);
        m_Text.Line(pa, pb, 2.0f * s, col);
    }
}

void CombatHud::DrawDebug(const CombatHudInput& in, const NpcDirector& npcs) {
    const float s = m_Text.Scale();
    // The director's lines (each dummy's look and goal), clipped to the near plane.
    m_Lines.clear();
    npcs.DebugLines(m_Lines);
    DrawLines(in, m_Lines);
    // A label over each dummy's head.
    for (const auto& up : npcs.Npcs()) {
        if (!up || up->Dead) continue;
        const Npc& n = *up;
        const glm::vec3 head = n.Feet + glm::vec3(0.0f, 2.05f, 0.0f);
        if (glm::length(head - in.CamPos) > 70.0f) continue;
        glm::vec2 p;
        if (!Project(in, head, p)) continue;
        char l1[96];
        std::snprintf(l1, sizeof l1, "%s  HP %.0f  %s", n.Name.c_str(), n.Health, n.WantMove ? "walking" : n.Roam ? "standing about" : "standing");
        const float size = 15.0f;
        const float w = m_Text.Measure(l1, size);
        const float h = m_Text.LineHeight(size) + 6.0f * s;
        const float x = p.x, y = p.y - h - 6.0f * s;
        const glm::vec3 tint(0.7f, 1.0f, 0.7f);
        m_Text.Rect(x - w * 0.5f - 5.0f * s, y - 2.0f * s, w + 10.0f * s, h, glm::vec4(0.0f, 0.0f, 0.0f, 0.55f));
        m_Text.Rect(x - w * 0.5f - 5.0f * s, y - 2.0f * s, 3.0f * s, h, glm::vec4(tint, 0.95f));
        m_Text.Text(x, y, l1, size, glm::vec4(tint, 1.0f), HudText::Align::Center, false);
    }
    m_Text.Text(40.0f * s, 40.0f * s, "AI DEBUG", 18.0f, glm::vec4(0.55f, 0.9f, 1.0f, 0.9f));
}

void CombatHud::Draw(const CombatHudInput& in, const NpcDirector& npcs) {
    if (in.Width <= 0 || in.Height <= 0) return;
    m_Text.Begin(in.Width, in.Height);
    if (in.AiOverlay) DrawDebug(in, npcs);
    if (in.Lines) DrawLines(in, *in.Lines);
    m_Text.Flush(in.Fbo);
}
