#include "CombatHud.h"

#include "AI/NpcDirector.h"
#include "Combat/Damage.h" // DamageIndicatorAngle

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr int kMaxFeed = 5;

glm::vec4 WithAlpha(glm::vec4 c, float a) { c.a *= a; return c; }

unsigned Id(entt::entity e) { return (unsigned)entt::to_integral(e); }

} // namespace

void CombatHud::Reset() {
    m_Feed.clear();
    m_Kills = m_Streak = 0;
    m_LastKillAt = m_StreakAt = -1e9f;
}

void CombatHud::OnKill(const NpcDirector& npcs, unsigned entity, bool head) {
    FeedRow row;
    row.Name = "SOLDIER";
    for (const auto& up : npcs.Npcs())
        if (up && Id(up->Root) == entity) {
            row.Name = "UNIT-" + std::to_string(UnitNumber(*up));
            break;
        }
    row.Head = head;
    row.At = npcs.Now();
    m_Feed.push_back(row);
    if ((int)m_Feed.size() > kMaxFeed) m_Feed.erase(m_Feed.begin());
    ++m_Kills;
    m_Streak = NextStreak(Settings, m_Streak, row.At - m_LastKillAt);
    m_LastKillAt = row.At;
    if (m_Streak >= 2) m_StreakAt = row.At;
}

bool CombatHud::Project(const CombatHudInput& in, const glm::vec3& p, glm::vec2& out) const {
    const glm::vec4 c = in.ViewProj * glm::vec4(p, 1.0f);
    if (c.w < 0.05f) return false;
    out = glm::vec2((c.x / c.w * 0.5f + 0.5f) * (float)in.Width, (1.0f - (c.y / c.w * 0.5f + 0.5f)) * (float)in.Height);
    return true;
}

void CombatHud::DrawAmmo(const CombatHudInput& in) {
    const float s = m_Text.Scale();
    const float W = (float)in.Width, H = (float)in.Height;
    const float right = W - 40.0f * s;
    const float barTop = H - 50.0f * s, barW = 260.0f * s, barH = 10.0f * s;
    const float t = in.RealTime;
    const int mag = std::max(1, in.Magazine);
    const float frac = std::clamp((float)in.Ammo / (float)mag, 0.0f, 1.0f);
    const bool low = in.Ammo <= std::max(1, mag / 4);
    const bool empty = in.Ammo <= 0;

    // The bar: the same dark track and white fill as the health bar, red and pulsing when low.
    m_Text.Rect(right - barW - 2.0f * s, barTop - 2.0f * s, barW + 4.0f * s, barH + 4.0f * s, glm::vec4(0.0f, 0.0f, 0.0f, 0.45f));
    glm::vec4 fill(0.95f, 0.95f, 0.95f, 0.9f);
    if (low) fill = glm::mix(glm::vec4(0.95f, 0.95f, 0.95f, 0.9f), glm::vec4(0.95f, 0.15f, 0.1f, 0.95f), 0.5f + 0.5f * std::sin(t * 9.0f));
    m_Text.Rect(right - barW, barTop, barW * frac, barH, fill);

    // The count: the magazine big, its size small beside it.
    const std::string total = in.InfiniteAmmo ? "/INF" : "/" + std::to_string(in.Magazine);
    const float smallSize = 34.0f, bigSize = 84.0f;
    const float totalW = m_Text.Measure(total, smallSize);
    const float bigY = barTop - 8.0f * s - m_Text.LineHeight(bigSize);
    glm::vec4 col(0.96f, 0.96f, 0.96f, 0.95f);
    if (low) col = glm::mix(col, glm::vec4(1.0f, 0.2f, 0.12f, 1.0f), 0.5f + 0.5f * std::sin(t * 9.0f));
    m_Text.Text(right - totalW - 6.0f * s, bigY, std::to_string(in.Ammo), bigSize, col, HudText::Align::Right);
    m_Text.Text(right, bigY + (bigSize - smallSize) * s * 0.92f, total, smallSize, glm::vec4(0.8f, 0.8f, 0.8f, 0.85f), HudText::Align::Right);

    // Fire mode above, the reload / empty call to the left of it.
    m_Text.Text(right, bigY - 4.0f * s, in.FireMode ? in.FireMode : "", 18.0f, glm::vec4(0.85f, 0.85f, 0.85f, 0.85f), HudText::Align::Right);
    if (in.Reloading) {
        m_Text.Text(right - barW, bigY + 6.0f * s, "RELOADING", 20.0f, glm::vec4(1.0f, 0.85f, 0.3f, 0.6f + 0.4f * std::sin(t * 12.0f)), HudText::Align::Left);
    } else if (empty) {
        m_Text.Text(right - barW, bigY + 6.0f * s, "RELOAD", 20.0f, glm::vec4(1.0f, 0.25f, 0.15f, 0.5f + 0.5f * std::sin(t * 10.0f)), HudText::Align::Left);
    }
}

void CombatHud::DrawFeed(const CombatHudInput& in, float now) {
    const float s = m_Text.Scale();
    const float W = (float)in.Width;
    m_Feed.erase(std::remove_if(m_Feed.begin(), m_Feed.end(), [&](const FeedRow& r) { return FeedExpired(Settings, now - r.At); }), m_Feed.end());
    float y = 40.0f * s;
    const float rowH = 38.0f * s, right = W - 40.0f * s;
    for (int i = (int)m_Feed.size() - 1; i >= 0; --i) {
        const FeedRow& r = m_Feed[(size_t)i];
        const float age = now - r.At;
        const float a = std::clamp((Settings.FeedLife - age) / 1.0f, 0.0f, 1.0f) * std::clamp(age / 0.08f + 0.2f, 0.0f, 1.0f);
        const float slide = (1.0f - std::clamp(age / 0.15f, 0.0f, 1.0f)) * 40.0f * s;
        const std::string head = r.Head ? "HEADSHOT  " : "";
        const float w = m_Text.Measure(r.Name, 26.0f) + m_Text.Measure(head, 26.0f) + m_Text.Measure("ELIMINATED  ", 26.0f);
        const float x = right + slide;
        m_Text.Rect(x - w - 12.0f * s, y - 3.0f * s, w + 26.0f * s, rowH - 4.0f * s, glm::vec4(0.0f, 0.0f, 0.0f, 0.4f * a));
        m_Text.Rect(x + 10.0f * s - 3.0f * s, y - 3.0f * s, 3.0f * s, rowH - 4.0f * s, glm::vec4(r.Head ? glm::vec3(1.0f, 0.8f, 0.25f) : glm::vec3(1.0f, 0.2f, 0.15f), 0.9f * a));
        float cx = x - w;
        cx += m_Text.Text(cx, y, "ELIMINATED  ", 26.0f, glm::vec4(0.8f, 0.8f, 0.8f, 0.9f * a));
        if (r.Head) cx += m_Text.Text(cx, y, head, 26.0f, glm::vec4(1.0f, 0.8f, 0.25f, a));
        m_Text.Text(cx, y, r.Name, 26.0f, glm::vec4(1.0f, 1.0f, 1.0f, a));
        y += rowH;
    }
    // A quick streak: a banner at the top centre.
    const float sage = now - m_StreakAt;
    if (m_Streak >= 2 && sage < 2.2f) {
        const char* label = m_Streak == 2 ? "DOUBLE KILL" : m_Streak == 3 ? "TRIPLE KILL" : "RAMPAGE";
        const float pop = 1.0f + 0.35f * std::exp(-sage * 9.0f);
        const float a = std::clamp((2.2f - sage) / 0.6f, 0.0f, 1.0f);
        const glm::vec4 c = m_Streak >= 4 ? glm::vec4(1.0f, 0.2f, 0.12f, a) : m_Streak == 3 ? glm::vec4(1.0f, 0.55f, 0.15f, a) : glm::vec4(1.0f, 0.85f, 0.3f, a);
        m_Text.Text(W * 0.5f, 150.0f * s, label, 44.0f * pop, c, HudText::Align::Center);
    }
}

void CombatHud::DrawAwareness(const CombatHudInput& in, const NpcDirector& npcs) {
    const float s = m_Text.Scale();
    const glm::vec2 centre((float)in.Width * 0.5f, (float)in.Height * 0.5f);
    const float radius = 150.0f * s;
    for (const auto& up : npcs.Npcs()) {
        if (!up || up->Dead || up->Mem.Known) continue;
        const float aw = std::clamp(up->Mem.Awareness, 0.0f, 1.0f);
        if (aw < 0.04f) continue;
        const float ang = DamageIndicatorAngle(in.CamPos, in.CamYawDeg, up->Feet);
        const glm::vec2 pos = centre + glm::vec2(std::sin(ang), -std::cos(ang)) * radius;
        const glm::vec3 white(0.95f), yellow(1.0f, 0.85f, 0.2f), red(1.0f, 0.2f, 0.1f);
        const glm::vec3 c = aw < 0.5f ? glm::mix(white, yellow, aw / 0.5f) : glm::mix(yellow, red, (aw - 0.5f) / 0.5f);
        // The chevron's two strokes: a dim full outline, filled from the wing tips toward the point.
        const float size = 26.0f * s, thick = 4.0f * s;
        const glm::vec2 fwd(std::sin(ang), -std::cos(ang)), side(-fwd.y, fwd.x);
        const glm::vec2 tip = pos + fwd * size * 0.5f;
        const glm::vec2 wing[2] = {pos - fwd * size * 0.5f + side * size * 0.6f, pos - fwd * size * 0.5f - side * size * 0.6f};
        for (const glm::vec2& w : wing) {
            m_Text.Line(w, tip, thick + 2.0f * s, glm::vec4(0.0f, 0.0f, 0.0f, 0.45f));
            m_Text.Line(w, tip, thick, glm::vec4(0.9f, 0.9f, 0.9f, 0.22f));
            m_Text.Line(w, w + (tip - w) * aw, thick, glm::vec4(c, 0.95f));
        }
    }
}

void CombatHud::DrawDebug(const CombatHudInput& in, const NpcDirector& npcs) {
    const float s = m_Text.Scale();
    // The director's lines (cover points, sight lines, goals, tokens), clipped to the near plane.
    m_Lines.clear();
    npcs.DebugLines(m_Lines);
    const float kNear = 0.05f, maxDist2 = 80.0f * 80.0f;
    for (size_t i = 0; i + 14 <= m_Lines.size(); i += 14) {
        const glm::vec3 a(m_Lines[i], m_Lines[i + 1], m_Lines[i + 2]), b(m_Lines[i + 7], m_Lines[i + 8], m_Lines[i + 9]);
        const glm::vec4 col(m_Lines[i + 3], m_Lines[i + 4], m_Lines[i + 5], m_Lines[i + 6]);
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
    // A label over each soldier's head.
    for (const auto& up : npcs.Npcs()) {
        if (!up || up->Dead) continue;
        const Npc& n = *up;
        const glm::vec3 head = n.Feet + glm::vec3(0.0f, n.Crouched ? 1.5f : 2.05f, 0.0f);
        if (glm::length(head - in.CamPos) > 70.0f) continue;
        glm::vec2 p;
        if (!Project(in, head, p)) continue;
        char l1[96], l2[128], l3[128];
        std::snprintf(l1, sizeof l1, "%s  %s  %s  HP %.0f", n.Name.c_str(), BehaviourName(n.Doing), RoleName(n.Role), n.Health);
        std::snprintf(l2, sizeof l2, "%s", n.Why.c_str());
        std::snprintf(l3, sizeof l3, "aw %.2f %s%s  sup %.2f%s%s%s", n.Mem.Awareness, n.Mem.Known ? "KNOWN " : "", n.Mem.Visible ? "SEES " : "", n.Suppression,
                      n.HasAttackToken ? "  ATK" : "", n.HasFlankToken ? "  FLK" : "", n.HasPushToken ? "  PSH" : "");
        const float size = 15.0f;
        const float w = std::max({m_Text.Measure(l1, size), m_Text.Measure(l2, size), m_Text.Measure(l3, size)});
        const float lh = m_Text.LineHeight(size);
        const float h = lh * 3.0f + 6.0f * s;
        const float x = p.x, y = p.y - h - 6.0f * s;
        const glm::vec3 tint = n.Mem.Visible ? glm::vec3(1.0f, 0.35f, 0.3f) : n.Mem.Known ? glm::vec3(1.0f, 0.75f, 0.2f)
                                                                                     : n.Mem.Awareness > 0.04f ? glm::vec3(1.0f, 0.95f, 0.4f) : glm::vec3(0.7f, 1.0f, 0.7f);
        m_Text.Rect(x - w * 0.5f - 5.0f * s, y - 2.0f * s, w + 10.0f * s, h, glm::vec4(0.0f, 0.0f, 0.0f, 0.55f));
        m_Text.Rect(x - w * 0.5f - 5.0f * s, y - 2.0f * s, 3.0f * s, h, glm::vec4(tint, 0.95f));
        m_Text.Text(x, y, l1, size, glm::vec4(tint, 1.0f), HudText::Align::Center, false);
        m_Text.Text(x, y + lh, l2, size, glm::vec4(0.85f, 0.85f, 0.85f, 1.0f), HudText::Align::Center, false);
        m_Text.Text(x, y + lh * 2.0f, l3, size, glm::vec4(0.7f, 0.85f, 1.0f, 1.0f), HudText::Align::Center, false);
    }
    m_Text.Text(40.0f * s, 40.0f * s, "AI DEBUG (F9)", 18.0f, glm::vec4(0.55f, 0.9f, 1.0f, 0.9f));
}

void CombatHud::Draw(const CombatHudInput& in, NpcDirector& npcs) {
    if (in.Width <= 0 || in.Height <= 0) return;
    m_Text.Begin(in.Width, in.Height);
    const float now = npcs.Now();
    const float s = m_Text.Scale();
    if (in.AiOverlay) DrawDebug(in, npcs);
    if (!in.PlayerDead) {
        if (in.Armed && in.Magazine > 0) DrawAmmo(in);
        DrawAwareness(in, npcs);
        if (in.God) m_Text.Text(40.0f * s, (float)in.Height - 50.0f * s - 34.0f * s, "GOD MODE", 24.0f, glm::vec4(1.0f, 0.82f, 0.25f, 0.95f));
    }
    DrawFeed(in, now);
    m_Text.Flush(in.Fbo);
}
