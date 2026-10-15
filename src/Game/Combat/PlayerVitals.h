#pragma once

#include <glm/glm.hpp>

#include <vector>

// The player's health in Play: damage, regeneration, death and the respawn timer, plus what the
// HUD shows about it (the hurt flash, where hits came from, the hitmarker). Pure state - the host
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

    // A hit worth `amount` from `source` (world). Returns what it took (0 while dead, protected
    // or god mode). Kills the player at 0.
    float ApplyDamage(float amount, const glm::vec3& source);
    // Time passes: regeneration, the respawn timer, the HUD's fades.
    void Tick(float dt);

    bool IsDead() const { return m_Dead; }
    bool JustDied() const { return m_JustDied; }       // true for the Tick after the killing hit
    bool WantsRespawn() const { return m_Dead && m_DeadTime >= m_Settings.RespawnDelay; }
    void Respawned();                                   // full health, spawn protection on
    float Health() const { return m_Health; }
    float Health01() const { return m_Settings.MaxHealth > 0.0f ? m_Health / m_Settings.MaxHealth : 0.0f; }
    float DeadTime() const { return m_DeadTime; }
    float RespawnProgress() const;                       // 0..1 while dead
    bool Protected() const { return m_Protection > 0.0f; }
    int Deaths() const { return m_Deaths; }
    float DamageTaken() const { return m_DamageTaken; }  // total this Play
    bool GodMode = false;

    // The player's own rounds landed on an enemy (the hitmarker); `kill` when that one killed.
    void MarkHit(bool kill, bool head);

    // --- what the HUD draws ---
    struct Indicator { glm::vec3 Source{0.0f}; float Age = 0.0f; float Strength = 0.0f; };
    const std::vector<Indicator>& Indicators() const { return m_Indicators; }
    float HurtFlash() const { return m_HurtFlash; }      // 0..1, a red pulse on each hit
    float Hitmarker() const { return m_Hitmarker; }      // 0..1, fading
    bool HitmarkerKill() const { return m_HitmarkerKill; }
    bool HitmarkerHead() const { return m_HitmarkerHead; }
    static constexpr float kIndicatorLife = 2.0f;
    static constexpr int kMaxIndicators = 8;

private:
    PlayerVitalsSettings m_Settings;
    float m_Health = 100.0f;
    float m_SinceHit = 1e9f;
    bool m_Dead = false, m_JustDied = false, m_DiedThisTick = false;
    float m_DeadTime = 0.0f;
    float m_Protection = 0.0f;
    int m_Deaths = 0;
    float m_DamageTaken = 0.0f;
    std::vector<Indicator> m_Indicators;
    float m_HurtFlash = 0.0f;
    float m_Hitmarker = 0.0f;
    bool m_HitmarkerKill = false, m_HitmarkerHead = false;
};

// The HUD's numbers for one frame: what PlayerHud's shader takes.
struct PlayerHudState {
    bool Visible = false;
    float Health01 = 1.0f;
    float HurtFlash = 0.0f;
    bool Dead = false;
    float DeathFade = 0.0f;       // 0..1 into the death fade
    float RespawnProgress = 0.0f; // 0..1
    bool Protected = false;
    int Arcs = 0;
    float ArcAngle[PlayerVitals::kMaxIndicators] = {};  // radians clockwise from ahead
    float ArcAlpha[PlayerVitals::kMaxIndicators] = {};
    float Hitmarker = 0.0f;
    bool HitmarkerKill = false, HitmarkerHead = false;
};
// Builds the HUD state from the vitals and the play camera (position, yaw in degrees).
PlayerHudState MakePlayerHudState(const PlayerVitals& vitals, const glm::vec3& cameraPos, float cameraYawDeg);
