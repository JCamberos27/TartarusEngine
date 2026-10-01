#pragma once

#include "HudText.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

class NpcDirector;

// Everything the combat HUD draws with text and shapes (the health bar, hurt vignette and hitmarker are
// PlayerHudOverlay's): the ammo counter (bottom right), the kill feed and streak banner (top right /
// top centre), the radio subtitles with a direction arrow (bottom centre), awareness chevrons round the
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
    // Forgets the feed, the subtitles and the streak (a new Play).
    void Reset();
    // The player's round killed `entity` (a soldier of `npcs`).
    void OnKill(const NpcDirector& npcs, unsigned entity, bool head);
    // Draws the frame into `in.Fbo`.
    void Draw(const CombatHudInput& in, NpcDirector& npcs); // takes the new radio lines off the director

    int Kills() const { return m_Kills; }
    int Subtitles() const { return (int)m_Subs.size(); }
    int FeedRows() const { return (int)m_Feed.size(); }

private:
    struct FeedRow { std::string Name; bool Head = false; float At = 0.0f; };
    struct Subtitle { std::string Unit, Text; glm::vec3 Pos{0.0f}; int Squad = 0; float Start = 0.0f, AudioEnd = 0.0f, End = 0.0f; };

    void DrawAmmo(const CombatHudInput& in);
    void DrawFeed(const CombatHudInput& in, float now);
    void DrawSubtitles(const CombatHudInput& in, float now);
    void DrawAwareness(const CombatHudInput& in, const NpcDirector& npcs);
    void DrawDebug(const CombatHudInput& in, const NpcDirector& npcs);
    bool Project(const CombatHudInput& in, const glm::vec3& p, glm::vec2& out) const;

    HudText m_Text;
    std::vector<FeedRow> m_Feed;
    std::vector<Subtitle> m_Subs;
    std::vector<float> m_Lines; // scratch for the director's debug lines
    int m_Kills = 0, m_Streak = 0;
    float m_LastKillAt = -1e9f, m_StreakAt = -1e9f;
};
