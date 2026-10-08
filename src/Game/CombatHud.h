#pragma once

#include "HudText.h"
#include "Components.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

class NpcDirector;

// Everything the combat HUD draws with text and shapes (the health bar, hurt vignette and hitmarker are
// PlayerHudOverlay's): the ammo counter (bottom right), the kill feed and streak banner (top right /
// top centre), awareness chevrons round the
// crosshair for soldiers that are suspicious but not yet fighting, a GOD tag, and the AI debug overlay
// (the director's lines and a label per soldier). One HudText batch, one draw call.
struct CombatHudInput {
    unsigned Fbo = 0;
    int Width = 0, Height = 0;
    glm::mat4 ViewProj{1.0f};
    glm::vec3 CamPos{0.0f};
    float CamYawDeg = 0.0f;
    float RealTime = 0.0f;       // wall clock, for pulses
    bool PlayerDead = false;
    bool Armed = false;          // a gun in hand (the ammo counter)
    int Ammo = 0, Magazine = 0;
    const char* FireMode = "";
    bool Reloading = false;
    bool God = false;
    bool InfiniteAmmo = false;
    bool AiOverlay = false;
};

class CombatHud {
public:
    // Scene tuning (feed life, streak window); the defaults are the old fixed values.
    FxHudSettingsComponent Settings;
    // The kill streak after a kill `sinceLastKill` seconds after the previous one.
    static int NextStreak(const FxHudSettingsComponent& s,int streak,float sinceLastKill);
    static bool FeedExpired(const FxHudSettingsComponent& s,float age);
    // Forgets the feed and the streak (a new Play).
    void Reset();
    // The player's round killed `entity` (a soldier of `npcs`).
    void OnKill(const NpcDirector& npcs, unsigned entity, bool head);
    // Draws the frame into `in.Fbo`.
    void Draw(const CombatHudInput& in, NpcDirector& npcs);

    int Kills() const { return m_Kills; }
    int FeedRows() const { return m_FeedRows; }

private:
    HudText m_Text;
    std::vector<float> m_Lines;
    int m_Kills=0,m_FeedRows=0;
};
