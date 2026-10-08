#pragma once

#include <glm/glm.hpp>

#include <vector>
#include "Scripting/GameFrames.h"

// The player's health in Play: damage, regeneration, death and the respawn timer, plus what the
// HUD shows about it (the hurt flash, where hits came from, the hitmarker). Pure state - the host
// teleports the player and refills the weapons when WantsRespawn() says so, then calls Respawned().
struct PlayerVitalsSettings {
    float MaxHealth = 0.0f;
    float RegenDelay = 0.0f;
    float RegenRate = 0.0f;
    float RespawnDelay = 0.0f;
    float SpawnProtection = 0.0f;
};

class PlayerVitals {
public:
    PlayerVitals();
    void Reset(const PlayerVitalsSettings& settings);

    // A hit worth `amount` from `source` (world). Returns what it took (0 while dead, protected
    // or god mode). Kills the player at 0.
    float ApplyDamage(float amount, const glm::vec3& source);
    // Time passes: regeneration, the respawn timer, the HUD's fades.
    void Tick(float dt);

    bool IsDead() const { return m_Frame.Dead!=0; }
    bool JustDied() const { return m_Frame.JustDied!=0; }
    bool WantsRespawn() const { return m_Frame.WantsRespawn!=0; }
    void Respawned();                                   // full health, spawn protection on
    float Health() const { return m_Frame.Health; }
    float Health01() const { return m_Frame.Health01; }
    float DeadTime() const { return m_Frame.DeadTime; }
    float RespawnProgress() const;                       // 0..1 while dead
    bool Protected() const { return m_Frame.Protection>0; }
    int Deaths() const { return m_Frame.Deaths; }
    float DamageTaken() const { return m_Frame.DamageTaken; }
    glm::vec3 DeathCameraOffset() const {return {0,m_Frame.DeathDrop,0};}
    float DeathCameraRoll() const {return m_Frame.DeathRoll;}
    Scripting::VitalsFrame Snapshot() const {return m_Frame;}
    void SetEyeHeight(float height) {m_Frame.EyeHeight=height;}
    bool GodMode = false;

    // The player's own rounds landed on an enemy (the hitmarker); `kill` when that one killed.
    void MarkHit(bool kill, bool head);

    // --- what the HUD draws ---
    struct Indicator { glm::vec3 Source{0.0f}; float Age = 0.0f; float Strength = 0.0f; };
    const std::vector<Indicator>& Indicators() const { return m_Indicators; }
    float HurtFlash() const { return m_Frame.HurtFlash; }
    float Hitmarker() const { return m_Frame.Hitmarker; }
    bool HitmarkerKill() const { return m_Frame.HitmarkerKill!=0; }
    bool HitmarkerHead() const { return m_Frame.HitmarkerHead!=0; }
    static constexpr float kIndicatorLife = 2.0f;
    static constexpr int kMaxIndicators = 8;

private:
    void Run(int operation);
    Scripting::VitalsFrame m_Frame;
    std::vector<Indicator> m_Indicators;
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
