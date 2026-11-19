#pragma once


// The player's health in Play: damage, regeneration, death and the respawn timer. Pure state - the host
// teleports the player and refills the weapons when WantsRespawn() says so, then calls Respawned().
struct PlayerVitalsSettings {
    float MaxHealth = 100.0f;
    float RegenDelay = 5.0f;
    float RegenRate = 30.0f;
    float RespawnDelay = 3.0f;
    float SpawnProtection = 2.0f;
};

class PlayerVitals {
public:
    void Reset(const PlayerVitalsSettings& settings);

    // A hit worth `amount`. Returns what it took (0 while dead, protected or god mode). Kills the player at 0.
    float ApplyDamage(float amount);
    // Time passes: regeneration, the respawn timer.
    void Tick(float dt);

    bool IsDead() const { return m_Dead; }
    bool JustDied() const { return m_JustDied; }       // true for the Tick after the killing hit
    bool WantsRespawn() const { return m_Dead && m_DeadTime >= m_Settings.RespawnDelay; }
    void Respawned();                                   // full health, spawn protection on
    float Health() const { return m_Health; }
    float DeadTime() const { return m_DeadTime; }
    bool GodMode = false;

private:
    PlayerVitalsSettings m_Settings;
    float m_Health = 100.0f;
    float m_SinceHit = 1e9f;
    bool m_Dead = false, m_JustDied = false, m_DiedThisTick = false;
    float m_DeadTime = 0.0f;
    float m_Protection = 0.0f;
};
