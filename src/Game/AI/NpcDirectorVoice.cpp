// The squad's radio side of NpcDirector: Callout() routes a bark to SquadVoice, and UpdateVoice() adds the
// barks that need no AI decision of their own (reloading, covering fire, kill confirmed, stealth
// suspicion, idle chatter). Nothing here changes what a soldier decides.
#include "NpcDirector.h"

#include <cmath>

namespace {

// A repeatable pseudo-random 0..1 (the AI's own generator is left alone, so its stream is unchanged).
float Hash01(int a, float b) {
    const float v = std::sin((float)a * 12.9898f + b * 78.233f) * 43758.5453f;
    return v - std::floor(v);
}

} // namespace

void NpcDirector::Callout(Npc& n, Bark ev) {
    if (m_Now - n.LastCallout < 2.5f && m_Voice.Priority(ev) < 7) return; // urgent calls skip the per-soldier gap
    // A squadmate to answer "copy": the nearest other one who's alive.
    int resp = -1, respUnit = 0;
    glm::vec3 respPos{0.0f};
    if (BarkDefaults(ev).Responds) {
        float best = 40.0f;
        for (const auto& up : m_Npcs) {
            if (!up || up->Dead || up->Index == n.Index || up->Squad != n.Squad) continue;
            const float d = glm::length(up->Feet - n.Feet);
            if (d < best) {
                best = d;
                resp = up->Index;
                respUnit = UnitNumber(*up);
                respPos = up->Eye;
            }
        }
    }
    if (!m_Voice.Say(m_Now, n.Squad, n.Index, UnitNumber(n), ev, n.Eye, resp, respUnit, respPos)) return;
    n.LastCallout = m_Now;
    n.Callout = m_Voice.History().back().Text;
    n.CalloutAt = m_Now;
    // Squadmates in earshot who aren't busy shooting glance toward whoever called.
    for (auto& o : m_Npcs) {
        if (!o || o->Dead || o.get() == &n || o->Squad != n.Squad || o->Intent.Fire) continue;
        if (glm::length(o->Feet - n.Feet) > 18.0f) continue;
        o->GlanceAt = n.Eye;
        o->GlanceUntil = m_Now + 0.7f + 0.6f * Hash01(o->Index, m_Now);
    }
}

void NpcDirector::UpdateVoice(const PlayerSnapshot& p) {
    m_Voice.Update(m_Now);
    // The player went down: the nearest soldier who was in the fight confirms it.
    if (p.Dead && !m_PlayerWasDead) {
        Npc* who = nullptr;
        float best = 1e9f;
        for (auto& up : m_Npcs) {
            if (!up || up->Dead || !up->Mem.Known) continue;
            const float d = glm::length(up->Feet - p.Feet);
            if (d < best) { best = d; who = up.get(); }
        }
        if (who) Callout(*who, Bark::TargetDown);
    }
    m_PlayerWasDead = p.Dead;

    for (auto& up : m_Npcs) {
        if (!up || up->Dead) continue;
        Npc& n = *up;
        const bool reloading = n.Reloading && n.Mem.Known;
        if (reloading && !n.VcReloading) Callout(n, Bark::Reloading);
        n.VcReloading = reloading;
        const bool covering = n.Intent.Suppress && n.TriggerHeld;
        if (covering && !n.VcCovering) Callout(n, Bark::Covering);
        n.VcCovering = covering;
        const bool suspicious = !n.Mem.Known && n.Mem.Awareness > 0.3f;
        if (suspicious && !n.VcSuspicious) Callout(n, Bark::Suspicious);
        n.VcSuspicious = suspicious;
        // Idle chatter, now and then, while nothing is going on.
        if (n.Doing == Behaviour::Idle && !n.Mem.Known) {
            if (n.NextChatter <= 0.0f) n.NextChatter = m_Now + 10.0f + 25.0f * Hash01(n.Index, m_Now);
            else if (m_Now >= n.NextChatter) {
                Callout(n, Bark::Idle);
                n.NextChatter = m_Now + 25.0f + 35.0f * Hash01(n.Index + 7, m_Now);
            }
        }
    }
}
