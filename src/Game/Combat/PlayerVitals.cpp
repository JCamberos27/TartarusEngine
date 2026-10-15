#include "PlayerVitals.h"

#include "Damage.h"

#include <algorithm>
#include <cmath>

void PlayerVitals::Reset(const PlayerVitalsSettings& settings) {
    const bool god = GodMode;
    *this = PlayerVitals{};
    GodMode = god;
    m_Settings = settings;
    m_Settings.MaxHealth = std::max(1.0f, m_Settings.MaxHealth);
    m_Health = m_Settings.MaxHealth;
}

float PlayerVitals::ApplyDamage(float amount, const glm::vec3& source) {
    if (m_Dead || !(amount > 0.0f)) return 0.0f;
    // Where it came from shows even when it costs nothing, so the player can find the shooter.
    // Hits from about the same direction share one arc, refreshed.
    bool merged = false;
    for (Indicator& ind : m_Indicators) {
        if (glm::length(ind.Source - source) < 2.0f) {
            ind.Source = source; ind.Age = 0.0f; ind.Strength = std::min(1.0f, ind.Strength + 0.35f);
            merged = true;
            break;
        }
    }
    if (!merged) {
        if ((int)m_Indicators.size() >= kMaxIndicators)
            m_Indicators.erase(std::max_element(m_Indicators.begin(), m_Indicators.end(),
                                                [](const Indicator& a, const Indicator& b) { return a.Age < b.Age; }));
        m_Indicators.push_back({source, 0.0f, 0.6f});
    }
    if (GodMode || m_Protection > 0.0f) return 0.0f;
    const float taken = std::min(amount, m_Health);
    m_Health -= taken;
    m_DamageTaken += taken;
    m_SinceHit = 0.0f;
    m_HurtFlash = std::min(1.0f, m_HurtFlash + 0.35f + 0.65f * taken / m_Settings.MaxHealth * 3.0f);
    if (m_Health <= 0.0f) {
        m_Health = 0.0f;
        m_Dead = true;
        m_DiedThisTick = true;
        m_DeadTime = 0.0f;
        ++m_Deaths;
    }
    return taken;
}

void PlayerVitals::Tick(float dt) {
    dt = std::max(0.0f, dt);
    m_JustDied = m_DiedThisTick;
    m_DiedThisTick = false;
    m_SinceHit += dt;
    m_Protection = std::max(0.0f, m_Protection - dt);
    if (m_Dead) m_DeadTime += dt;
    else if (m_SinceHit >= m_Settings.RegenDelay && m_Settings.RegenRate > 0.0f)
        m_Health = std::min(m_Settings.MaxHealth, m_Health + m_Settings.RegenRate * dt);
    for (Indicator& ind : m_Indicators) ind.Age += dt;
    m_Indicators.erase(std::remove_if(m_Indicators.begin(), m_Indicators.end(),
                                      [](const Indicator& i) { return i.Age >= kIndicatorLife; }),
                       m_Indicators.end());
    m_HurtFlash = std::max(0.0f, m_HurtFlash - dt * 1.6f);
    m_Hitmarker = std::max(0.0f, m_Hitmarker - dt * 4.0f);
}

void PlayerVitals::Respawned() {
    m_Dead = false;
    m_DeadTime = 0.0f;
    m_Health = m_Settings.MaxHealth;
    m_SinceHit = 1e9f;
    m_Protection = m_Settings.SpawnProtection;
    m_Indicators.clear();
    m_HurtFlash = 0.0f;
}

float PlayerVitals::RespawnProgress() const {
    if (!m_Dead) return 0.0f;
    return m_Settings.RespawnDelay > 0.0f ? std::clamp(m_DeadTime / m_Settings.RespawnDelay, 0.0f, 1.0f) : 1.0f;
}

void PlayerVitals::MarkHit(bool kill, bool head) {
    m_Hitmarker = 1.0f;
    m_HitmarkerKill = kill;
    m_HitmarkerHead = head;
}

PlayerHudState MakePlayerHudState(const PlayerVitals& v, const glm::vec3& cameraPos, float cameraYawDeg) {
    PlayerHudState s;
    s.Visible = true;
    s.Health01 = v.Health01();
    s.HurtFlash = v.HurtFlash();
    s.Dead = v.IsDead();
    s.DeathFade = v.IsDead() ? std::clamp(v.DeadTime() / 1.2f, 0.0f, 1.0f) : 0.0f;
    s.RespawnProgress = v.RespawnProgress();
    s.Protected = v.Protected();
    for (const PlayerVitals::Indicator& ind : v.Indicators()) {
        if (s.Arcs >= PlayerVitals::kMaxIndicators) break;
        const float life = 1.0f - ind.Age / PlayerVitals::kIndicatorLife;
        s.ArcAngle[s.Arcs] = DamageIndicatorAngle(cameraPos, cameraYawDeg, ind.Source);
        s.ArcAlpha[s.Arcs] = std::clamp(life * life * (0.5f + 0.5f * ind.Strength), 0.0f, 1.0f);
        ++s.Arcs;
    }
    s.Hitmarker = v.Hitmarker();
    s.HitmarkerKill = v.HitmarkerKill();
    s.HitmarkerHead = v.HitmarkerHead();
    return s;
}
