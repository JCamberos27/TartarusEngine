#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "BloodFxPresets.h"

class BloodRenderer;

// The game side of the volumetric blood (docs/BLOOD_FX.md): turns a round going into a body into
// the blood it throws - a fluid spray out of the exit wound along the round's line (the imported
// sims, scaled to the hit and timed to real gravity) - and keeps each spray playing until it has
// fallen. Owns no GL; Submit() hands this frame's state to the BloodRenderer.
class BloodFx {
public:
    struct Settings {
        bool Enabled = true;
        float Size = 1.0f;       // x every spray's size (1 = the tuned default)
        int MaxSprays = 24;      // the oldest is dropped past this
    };
    Settings Config;

    // A round into a body (NpcDirector::FleshHit, plus where it came from).
    struct Hit {
        glm::vec3 Point{0.0f};
        glm::vec3 Direction{0.0f, 0.0f, -1.0f}; // the round's travel
        unsigned Entity = 0xFFFFFFFFu;
        int Part = -1;
        float Damage = 0.0f;
        bool Killed = false, Head = false, Corpse = false, Player = false;
        int Pellets = 1;
    };

    // The world ray the spray's obstacle test uses: true and the hit point / normal when something
    // solid lies within `maxDistance`. Defaults to PhysicsWorld::RaycastSolid; tests replace it.
    using RayFn = std::function<bool(const glm::vec3& origin, const glm::vec3& dir, float maxDistance,
                                     glm::vec3& point, glm::vec3& normal)>;
    void SetRaycast(RayFn fn) { m_Ray = std::move(fn); }
    // Sim name -> the renderer's metadata index (-1 = not loaded). Defaults to BloodRenderer::Get().
    using SimFn = std::function<int(const char* sim)>;
    void SetSimLookup(SimFn fn) { m_SimLookup = std::move(fn); }

    void OnFleshHit(const Hit& hit);
    void Update(float dt);
    void Submit(BloodRenderer& renderer) const;
    void Clear();

    // --- inspection (tests, the --npc-test blood scenario) ---
    struct Spray {
        int Sim = -1;
        glm::mat4 Model{1.0f};
        float Age = 0.0f, Duration = 1.0f;
        float FramesCount = 0.0f;
        glm::vec4 ClipPlane{0.0f, 0.0f, 0.0f, 1.0f};
        glm::vec3 Tint{1.0f};
        unsigned Entity = 0xFFFFFFFFu;
    };
    const std::vector<Spray>& Sprays() const { return m_Sprays; }
    int SpraysSpawned() const { return m_SpraysSpawned; }
    bool LastSprayClipped() const { return m_LastClipped; } // the last hit's spray met an obstacle

    // The frame a spray shows `t01` (0..1) of the way through its playback - the Unity asset's
    // BFX_ManualAnimationUpdate stepping, kept exact.
    static int FrameAt(float t01, float framesCount);
    // Which prefab a hit throws, and how big (x the prefab's authored size). `roll` in [0, 1).
    struct Choice { const char* Preset; float Size; };
    static Choice Choose(const Hit& hit, float roll);
    // Seconds a spray plays: the authored length, x sqrt(size) - a smaller splash falls a shorter
    // way, and free fall takes time with the square root of the distance.
    static float PlaybackSeconds(const BloodSprayDef& def, float animationSpeed, float size);
    // The prefab -> world transform for a spray thrown from `exitPoint` along `dir` (yaw only: the
    // sims' gravity is baked toward -Y).
    static glm::mat4 PrefabToWorld(const glm::vec3& exitPoint, const glm::vec3& dir, float size, float yawJitterRad);

private:
    void SpawnSprays(const BloodPresetDef& preset, const glm::mat4& prefabToWorld, float size, const glm::vec3& wound,
                     const glm::vec3& flatDir, unsigned entity, float tint);
    float Random01();

    RayFn m_Ray;
    SimFn m_SimLookup;
    std::vector<Spray> m_Sprays;
    std::uint32_t m_Rng = 0x9E3779B9u;
    float m_Now = 0.0f;
    int m_SpraysSpawned = 0;
    bool m_LastClipped = false;
    // Shotgun pellets land in one frame: one spray per body per moment, not one per pellet.
    struct Recent { unsigned Entity; float Time; };
    std::vector<Recent> m_Recent;
};
