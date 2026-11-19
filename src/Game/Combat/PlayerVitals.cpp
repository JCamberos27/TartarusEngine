#include "PlayerVitals.h"

#include <algorithm>

void PlayerVitals::Reset(const PlayerVitalsSettings& settings) {
    const bool god = GodMode;
    *this = PlayerVitals{};
    GodMode = god;
    m_Settings = settings;
    m_Settings.MaxHealth = std::max(1.0f, m_Settings.MaxHealth);
    m_Health = m_Settings.MaxHealth;
}

float PlayerVitals::ApplyDamage(float amount) {
    if (m_Dead || !(amount > 0.0f) || GodMode || m_Protection > 0.0f) return 0.0f;
    const float taken = std::min(amount, m_Health);
    m_Health -= taken;
    m_SinceHit = 0.0f;
    if (m_Health <= 0.0f) {
        m_Health = 0.0f;
        m_Dead = true;
        m_DiedThisTick = true;
        m_DeadTime = 0.0f;
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
}

void PlayerVitals::Respawned() {
    m_Dead = false;
    m_DeadTime = 0.0f;
    m_Health = m_Settings.MaxHealth;
    m_SinceHit = 1e9f;
    m_Protection = m_Settings.SpawnProtection;
}
