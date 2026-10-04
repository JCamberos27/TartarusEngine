#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "BloodFxPresets.h"
#include "BloodFxImport.h"

class BloodRenderer;
class FxSprites;

// The game side of the volumetric blood (docs/BLOOD_FX.md): turns a round going into a body into
// the blood it throws - a fluid spray out of the exit wound along the round's line (the imported
// sims, scaled to the hit and timed to real gravity) - and where that blood lands: the splat on the
// ground under the spray (the prefab's own decal, placed and timed by how far it fell), the spatter
// on a wall or ceiling the spray met, and the pool that spreads under a corpse. Stains spread in,
// dry darker and matte, and fade out at the end of their life. Owns no GL; Submit() hands this
// frame's state to the BloodRenderer.
class BloodFx {
public:
    struct Settings {
        bool Enabled = true;
        float Size = 1.0f;       // x every spray's size (1 = the tuned default)
        int MaxSprays = 24;      // the oldest is dropped past this
        int MaxDecals = 512;     // stains; past this the farthest one out of view goes
        float DecalLifetime = 0.0f;   // seconds a stain stays (once out of view); 0 = for good
        float DrySeconds = 900.0f;    // fresh and wet to dried dark and matte: slow, never in front of you
        float Speed = 1.6f;           // x how fast the sprays play and the blood lands (1 = the packs' cinematic timing)
        bool Pools = true;            // a pool spreads under each corpse
        bool BodySplats = true;       // blood on bodies, ragdolls and props
        bool GearSpatter = true;      // the player's gun and hands, and their own wounds
        float EnergyScale = 1.0f;     // x every hit's energy (spray reach and speed, mist, exits)
        bool ImpactPuffs = true;      // the flipbook burst, mist and droplets on the frame a round goes in
        int Gore = 1;                 // 0 off, 1 on
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
        glm::vec3 Origin{0.0f};  // the shooter's muzzle
        bool ByPlayer = false;   // the player fired: at point blank it comes back onto the gun and hands
    };

    // How hard a hit is, ~1 for a rifle round at combat range: its damage (a shotgun's pellets arrive as one
    // load), less with distance, more for a head. It sets the spray's size and speed, the mist, and whether
    // the round comes out the far side.
    static float Energy(const Hit& hit);
    static constexpr float kExitEnergy = 0.6f; // below this the round stays in: blood only back out of the entry
    // A big headshot kill (the heavier gore sound over the flesh hit): a head kill at energy >= 1.3, not the player.
    static bool BigHeadshot(const Hit& hit, float energy, int gore) {
        return hit.Head && hit.Killed && !hit.Player && gore >= 1 && energy >= 1.3f;
    }

    // The world ray the spray's obstacle test uses: true and the hit point / normal when something
    // solid lies within `maxDistance`. Defaults to PhysicsWorld::RaycastSolid; tests replace it.
    using RayFn = std::function<bool(const glm::vec3& origin, const glm::vec3& dir, float maxDistance,
                                     glm::vec3& point, glm::vec3& normal)>;
    void SetRaycast(RayFn fn) { m_Ray = std::move(fn); }
    // Sim name -> the renderer's metadata index (-1 = not loaded). Defaults to BloodRenderer::Get().
    using SimFn = std::function<int(const char* sim)>;
    void SetSimLookup(SimFn fn) { m_SimLookup = std::move(fn); }
    // Decal set name -> the renderer's index (-1 = none). Defaults to BloodRenderer::Get().
    void SetDecalSetLookup(SimFn fn) { m_SetLookup = std::move(fn); }
    // Knife library entry name -> id and its cell count (-1 = not there). Defaults to KnifeFxLibrary::Get().
    using KnifeFn = std::function<int(const char* entry, int& cells)>;
    void SetKnifeLookup(KnifeFn fn) { m_KnifeLookup = std::move(fn); }
    // Where a body is now (its pelvis), for the pool under a corpse; false once it's gone. Set by the host.
    using BodyFn = std::function<bool(unsigned entity, glm::vec3& centre)>;
    void SetBodyLookup(BodyFn fn) { m_Body = std::move(fn); }

    // --- splats: blood on bodies, guns and props, pinned in their meshes' bind-pose space ---
    // Where blood on `entity` near `point` goes: every mesh that draws that body (a soldier's driver and each
    // clothing piece; a prop itself), each with its own world -> bind-pose transform through the hit part's bone
    // as posed now - pieces needn't share a bind pose. `Group` is the body (the cap and lifetime go by it).
    // False: nothing to bleed on.
    struct SplatSpace {
        unsigned Group = 0xFFFFFFFFu;
        std::vector<std::pair<unsigned, glm::mat4>> Members; // drawing entity, world -> its bind space
    };
    using SplatSpaceFn = std::function<bool(unsigned entity, int part, bool corpse, const glm::vec3& point, SplatSpace& out)>;
    void SetSplatSpace(SplatSpaceFn fn) { m_SplatSpace = std::move(fn); }
    // Whether `group` (a body) is still there; false once it's gone (its splats go with it). `entities` is unused.
    using MembersFn = std::function<bool(unsigned group, std::vector<unsigned>& entities)>;
    void SetSplatMembers(MembersFn fn) { m_Members = std::move(fn); }
    // A ray against the bodies of soldiers (hitboxes / ragdoll parts): who's caught in a spray. Defaults to
    // PhysicsWorld::RaycastBodyParts.
    using BodyRayFn = std::function<bool(const glm::vec3& origin, const glm::vec3& dir, float maxDistance, unsigned& entity, int& part,
                                         glm::vec3& point)>;
    void SetBodyRay(BodyRayFn fn) { m_BodyRay = std::move(fn); }
    // A ray that hits only loose props (simulated bodies with a mesh): the host's. None set: props aren't spattered.
    using PropRayFn = std::function<bool(const glm::vec3& origin, const glm::vec3& dir, float maxDistance, glm::vec3& point,
                                         glm::vec3& normal, unsigned& entity)>;
    void SetPropRay(PropRayFn fn) { m_PropRay = std::move(fn); }
    // Points on the player's gun and hands as drawn now (world), with the way each faces; returns how many.
    // Back-spatter and the player's own bleeding land there. None set: the player's gear stays clean.
    using GearFn = std::function<int(glm::vec3* points, glm::vec3* normals, int max)>;
    void SetPlayerGear(GearFn fn) { m_Gear = std::move(fn); }
    // The flipbook particles the impact puffs go to (none: no puffs).
    void SetSprites(FxSprites* sprites) { m_Sprites = sprites; }

    void OnFleshHit(const Hit& hit);
    // The Blood Lab's direct effects: a pool spreading on the ground at `at`; a wall spatter (and its drips) at `at`.
    void SpawnPoolAt(const glm::vec3& at, const glm::vec3& normal, float size);
    void SpatterWallAt(const glm::vec3& at, const glm::vec3& normal, float size);
    // A footfall (FoleyAudio's step listener): `walker` -1 the player, else a soldier; `foot` 0 left, 1 right, -1 unknown.
    // Stepping in fresh blood on the ground wets that walker's soles; the next steps leave prints, fading.
    void OnFootstep(int walker, const glm::vec3& feet, const glm::vec3& velocity, int foot);
    static constexpr int kPrintSteps = 6;     // prints after stepping in it
    static constexpr float kFreshSeconds = 60.0f; // blood on the ground this young still marks a sole
    int PrintsSpawned() const { return m_PrintsSpawned; }
    int BloodySteps(int walker) const;        // prints left for that walker (0: clean soles)
    // Where the player looks from (each frame): stains only ever leave out of view - behind, or far off.
    void SetViewer(const glm::vec3& eye, const glm::vec3& forward) {
        m_Eye = eye;
        m_Forward = glm::length(forward) > 1e-5f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
        m_HasViewer = true;
    }
    static constexpr float kVisibleReach = 60.0f; // further than this a stain may go whatever the camera faces
    static constexpr int kMaxPrints = 48;         // footprints among themselves: the oldest out of view goes first
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
    struct Decal {
        int Set = -1;
        glm::mat4 Model{1.0f};            // unit box -> world, projecting along +Y
        float Age = 0.0f;                 // negative: not landed yet
        float Life = 1e9f;                // seconds; leaves only out of view once past it
        float RevealSeconds = 15.0f;      // the reveal curve's time scale
        const BloodCurve* Reveal = nullptr; // null: a pool, spreading over PoolGrow seconds
        float PoolGrow = 0.0f;
        float DrySeconds = 90.0f;
        float Opacity = 1.0f;
        // A Knife library decal (v2) instead of a KriptoFX set: its id and cell; a flipbook (the wall drips)
        // runs through Frames cells, FrameSeconds each, from landing.
        int Knife = -1;
        int Cell = 0, Frames = 1;
        float FrameSeconds = 0.0f;
    };
    const std::vector<Decal>& Decals() const { return m_Decals; }
    // Whether a stain could be on screen now: within reach and inside a cone wider than any view. True with no viewer.
    bool InView(const Decal& d) const;
    int DripsSpawned() const { return m_DripsSpawned; }
    int StainsMerged() const { return m_StainsMerged; }
    // A new stain that would land on a fresh one just like it (same image, centre within 15% of its size, same facing):
    // the old one stands for both. Index into Decals(), or -1.
    int DuplicateOf(int set, int knife, const glm::mat4& model) const;
    struct Splat {
        unsigned Group = 0xFFFFFFFFu;
        unsigned Member = 0xFFFFFFFFu; // the entity that draws it (its bind space)
        glm::vec3 Center{0.0f}, Normal{0, 1, 0}, Tangent{1, 0, 0}; // bind space
        float Radius = 0.1f, Depth = 0.1f;                         // bind units
        int Set = -1;
        float Age = 0.0f, Grow = 1.0f, DrySeconds = 120.0f, Opacity = 1.0f;
    };
    const std::vector<Splat>& Splats() const { return m_Splats; }
    int SplatsSpawned() const { return m_SplatHits; }
    int DecalsSpawned() const { return m_DecalsSpawned; }
    int PoolsSpawned() const { return m_PoolsSpawned; }
    // The mask cutout a stain shows now: BFX_ShaderProperies' reveal, held through its life, then the
    // rest of the curve as it shrinks away (1 = gone). Pools spread instead.
    static float DecalCutout(const Decal& d);
    int SpraysSpawned() const { return m_SpraysSpawned; }
    bool LastSprayClipped() const { return m_LastClipped; } // the last hit's spray met an obstacle
    // The last hit's exit wound: found on the far side of the body (`LastExitFound`) and where; no exit when
    // the round stayed in (`LastExited` false).
    bool LastExited() const { return m_LastExited; }
    bool LastExitFound() const { return m_LastExitFound; }
    glm::vec3 LastExitPoint() const { return m_LastExitPoint; }
    float LastEnergy() const { return m_LastEnergy; }
    int PuffsSpawned() const { return m_PuffsSpawned; }
    // Where the round leaves a body: back along its line from 0.7 m past the entry onto the same body's parts.
    // False (and `exit` a guess just past the entry) when the ray finds nothing of it.
    static bool FindExit(const glm::vec3& entry, const glm::vec3& dir, unsigned entity, bool head, const BodyRayFn& bodyRay,
                         glm::vec3& exit);

    // The frame a spray shows `t01` (0..1) of the way through its playback - the Unity asset's
    // BFX_ManualAnimationUpdate stepping, kept exact.
    static int FrameAt(float t01, float framesCount);
    // Which prefab a hit throws, and how big (x the prefab's authored size). `roll` in [0, 1).
    struct Choice { const char* Preset; float Size; };
    static Choice Choose(const Hit& hit, float roll);
    // Seconds a spray plays: the authored length, x sqrt(size) - a smaller splash falls a shorter
    // way, and free fall takes time with the square root of the distance. (Config.Speed divides it.)
    static float PlaybackSeconds(const BloodSprayDef& def, float animationSpeed, float size);
    // The prefab -> world transform for a spray thrown from `exitPoint` along `dir`: the prefab's own
    // spray direction `prefabAxis` (horizontal) turned onto the round's flattened line - yaw only, the
    // sims' gravity is baked toward -Y.
    // `stretch` > 1 draws it out along the flattened line - the same fall in the same time, thrown faster.
    static glm::mat4 PrefabToWorld(const glm::vec3& exitPoint, const glm::vec3& dir, float size, float yawJitterRad,
                                   const glm::vec3& prefabAxis = glm::vec3(1.0f, 0.0f, 0.0f), float stretch = 1.0f);
    // Which way a prefab throws its blood, in its own frame: where its sims' fluid is heading early on, flattened
    // (+X when the sims aren't loaded or it's a burst with no direction).
    static glm::vec3 PrefabAxis(const BloodPresetDef& preset, const std::function<const BloodFxImport::VatFrame*(const char* sim, glm::vec3& origin)>& lastFrame);

private:
    void SpawnSprays(const BloodPresetDef& preset, const glm::mat4& prefabToWorld, float size, const glm::vec3& wound,
                     const glm::vec3& flatDir, unsigned entity, float tint);
    void SpawnFloorDecals(const BloodPresetDef& preset, const glm::mat4& prefabToWorld, float size, const glm::vec3& wound,
                          const glm::vec3& flatDir);
    // A stain on the surface at `centre` facing `up`, its streaks along `along`; `extent` is the box (x along,
    // y depth, z across) in metres.
    Decal* AddDecal(const char* set, const glm::vec3& centre, const glm::vec3& up, const glm::vec3& along, const glm::vec3& extent,
                    float delay);
    glm::vec3 RandomTangent(const glm::vec3& n);
    // A Knife decal: `entry`'s cell `cell` (-1 a random one) on the surface at `centre` facing `up`; the image's
    // u along `along`, its v (top to bottom) along up x along. Null when the library lacks it.
    Decal* AddKnifeDecal(const char* entry, const glm::vec3& centre, const glm::vec3& up, const glm::vec3& along, const glm::vec3& extent,
                         float delay, int cell = -1);
    // Drips running down a wall from the spatter at `at` (facing `n`), from `land` seconds on.
    void SpawnWallDrips(const glm::vec3& at, const glm::vec3& n, float size, float land);
    KnifeFn m_KnifeLookup;
    int m_DripsSpawned = 0;
    int m_StainsMerged = 0;
    struct Walker {
        int Id = 0;
        int Steps = 0;          // prints still to leave
        int Shoe = 0;           // the footprint cell (one sole pattern per walker)
        bool Left = false;      // which foot printed last (when the step doesn't say)
        bool HasLast = false;
        glm::vec3 Last{0.0f}, Forward{0.0f, 0.0f, -1.0f};
    };
    std::vector<Walker> m_Walkers;
    // Room for one more stain: the farthest out of view goes (a print for a print), else the oldest.
    void MakeRoom(bool print);
    int m_PrintId = -2;
    glm::vec3 m_Eye{0.0f}, m_Forward{0.0f, 0.0f, -1.0f};
    bool m_HasViewer = false;
    int m_PrintsSpawned = 0;
    // Fresh blood on the ground under `feet`: a stain (not a print) younger than kFreshSeconds.
    bool InFreshBlood(const glm::vec3& feet) const;
    // A splat on `entity` at world `point`, facing world `normal`, `radius` / `depth` in metres.
    void AddSplat(unsigned entity, int part, bool corpse, const glm::vec3& point, const glm::vec3& normal, const glm::vec3& along,
                  const char* set, float radius, float depth, float delay, float grow);
    void EnsureHooks();
    float Random01();
    // The frame the round goes in: a flipbook burst and mist at the entry (and the exit), droplets thrown on.
    void SpawnPuffs(const Hit& hit, const glm::vec3& dir, float energy, bool exited, const glm::vec3& exitPoint, bool extraPellet);
    FxSprites* m_Sprites = nullptr;
    int m_PuffsSpawned = 0;
    bool m_LastExited = false, m_LastExitFound = false;
    glm::vec3 m_LastExitPoint{0.0f};
    float m_LastEnergy = 0.0f;

    RayFn m_Ray;
    SimFn m_SimLookup, m_SetLookup;
    BodyFn m_Body;
    SplatSpaceFn m_SplatSpace;
    MembersFn m_Members;
    BodyRayFn m_BodyRay;
    PropRayFn m_PropRay;
    GearFn m_Gear;
    void SpatterGear(int drops, float sizeScale, float delay);
    std::vector<Splat> m_Splats;
    int m_SplatsSpawned = 0;
    int m_SplatHits = 0; // splat placements (one per hit, however many meshes it lands on)
    std::vector<Spray> m_Sprays;
    std::vector<Decal> m_Decals;
    int m_DecalsSpawned = 0, m_PoolsSpawned = 0;
    struct PendingPool { unsigned Entity; float At; float Size; };
    std::vector<PendingPool> m_Pools;
    std::uint32_t m_Rng = 0x9E3779B9u;
    float m_Now = 0.0f;
    int m_SpraysSpawned = 0;
    bool m_LastClipped = false;
    // Shotgun pellets land in one frame: one spray per body per moment, not one per pellet.
    struct Recent { unsigned Entity; float Time; };
    std::vector<Recent> m_Recent;
};
