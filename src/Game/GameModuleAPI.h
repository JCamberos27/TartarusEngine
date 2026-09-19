#pragma once

#include <cstddef>
#include <cstdint>

class World;

// Deliberately small and versioned: the host keeps ownership of the World, renderer, editor,
// and every long-lived resource. A hot-reloaded module only receives a non-owning view for its
// per-frame gameplay work, so unloading it cannot invalidate editor state.
// v8 (#187): OnLoad receives the previous module's saved state and returns false to reject a
// build (the host then restores the previous one); SaveState added.
// v9 (#144): FixedUpdate, and host GetTime / SetTimeScale.
constexpr std::uint32_t kGameModuleAPIVersion = 11; // v11: PlaySoundAt, GetGrabbedEntity, GetActorPosition

// Unity's Time, as seen by the game module (#144). Seconds throughout. POD.
struct GameTime {
    float DeltaTime = 0.0f;         // this frame, scaled by TimeScale (what Update receives)
    float UnscaledDeltaTime = 0.0f; // this frame, ignoring TimeScale (pause menus, UI tweens)
    float FixedDeltaTime = 0.0f;    // the FixedUpdate / physics step
    float TimeScale = 1.0f;         // effective scale (the game's own x the editor's slow-mo)
    double Time = 0.0;              // scaled seconds since startup
    double UnscaledTime = 0.0;
    double RealtimeSinceStartup = 0.0;
    std::uint64_t FrameCount = 0;
};

// One raycast hit against the PhysX world (#185 PR 2). POD, no glm — the API header stays
// dependency-free so a version mismatch is the only thing that can break the ABI. Position and
// normal are world space; Entity is the entt::entity id of the collider that was hit (compare
// against entt::entity values via static_cast), valid only when Hit is true.
struct RaycastHit {
    bool          Hit = false;
    float         Distance = 0.0f;
    float         Point[3] = {0.0f, 0.0f, 0.0f};
    float         Normal[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t Entity = 0xFFFFFFFFu; // entt::null
};

// #170 - Unity's layerMask + QueryTriggerInteraction for the filtered queries below. LayerMask
// bit N = hit colliders on layer N (Project Settings layers); HitTriggers 0 = ignore trigger
// colliders, 1 = hit them (Unity's default). The Play-mode Player capsule is never hit.
struct QueryFilter {
    std::uint32_t LayerMask = 0xFFFFFFFFu;
    std::uint32_t HitTriggers = 1;
};

// One trigger overlap transition this frame (#185 PR 5). `Trigger` is the entity whose collider
// has Is Trigger set; `Other` is what entered/stayed/left it — an entity id, or kPlayerEntity
// for the Play-mode Player capsule (which has no entity). POD, no glm.
struct TriggerEvent {
    enum Phase : std::uint32_t { Enter = 0, Stay = 1, Exit = 2 };
    std::uint32_t Kind  = Enter;
    std::uint32_t Trigger = 0xFFFFFFFFu;
    std::uint32_t Other   = 0xFFFFFFFFu;
};

// Sentinel `Other` value meaning "the Play-mode Player", which isn't an entity.
constexpr std::uint32_t kPlayerEntity = 0xFFFFFFFEu;

// One real (solid, non-trigger) contact transition this frame (#185 PR 7). A/B are the two
// collider entities; Point/Normal are world space at the first contact point; Impulse is the
// summed |impulse| PhysX applied resolving the pair (a good "how hard" measure); NormalSpeed
// is the closing speed along the normal at Enter (0 for resting/separating). POD, no glm.
struct ContactEvent {
    enum Phase : std::uint32_t { Enter = 0, Stay = 1, Exit = 2 };
    std::uint32_t Kind = Enter;
    std::uint32_t A = 0xFFFFFFFFu;
    std::uint32_t B = 0xFFFFFFFFu;
    float Point[3]   = {0.0f, 0.0f, 0.0f};
    float Normal[3]  = {0.0f, 0.0f, 0.0f};
    float Impulse     = 0.0f;
    float NormalSpeed = 0.0f;
};

// Snapshot of a dynamic body's motion state (#185 PR 7). Valid is false for an unknown entity,
// a non-Rigidbody collider, or outside Play.
struct BodyState {
    bool  Valid = false;
    bool  Sleeping = false;
    bool  Kinematic = false;
    float Velocity[3]        = {0.0f, 0.0f, 0.0f};
    float AngularVelocity[3] = {0.0f, 0.0f, 0.0f};
};

// Mirrors PxForceMode. Force: continuous, mass-dependent (N, apply every frame). Impulse:
// instantaneous, mass-dependent (N·s — the one for a hit/explosion). VelocityChange:
// instantaneous, mass-independent. Acceleration: continuous, mass-independent.
enum ForceMode : std::uint32_t { Force = 0, Impulse = 1, VelocityChange = 2, Acceleration = 3 };

// Host-owned operations exposed to gameplay modules. Keep this table narrow and composed of
// plain function pointers so the DLL never owns editor, renderer, or scene-lifetime state.
// (v5 adds GetTriggerEvents. v4 added Raycast — the first real host callback — backed by the
// PhysX world that exists only while playing; both are no-ops outside Play. v3 had dropped
// EnsureRoomDonutTestSet, the old cross-DLL hot-reload smoke test.)
struct GameModuleHostAPI {
    std::uint32_t Version = kGameModuleAPIVersion;

    // Cast a ray from `origin` along `dir` (need not be normalised) up to `maxDistance` world
    // units. Returns true and fills `outHit` on the closest hit; returns false and leaves
    // `outHit` at its defaults on a miss or when no PhysX world is live. `dir` all-zero is a
    // miss.
    bool (*Raycast)(const float origin[3], const float dir[3], float maxDistance,
                    RaycastHit& outHit) = nullptr;

    // Copy up to `maxEvents` of this frame's trigger transitions into `out` and return the
    // total number that occurred (which may exceed `maxEvents`). The event list is rebuilt
    // each Step, so call this once per Update. Returns 0 when no PhysX world is live.
    int (*GetTriggerEvents)(TriggerEvent* out, int maxEvents) = nullptr;

    // --- Forces (#185 PR 7) --- all no-ops on an unknown entity, a kinematic/static body, or
    // outside Play. `entity` is an entt::entity id (static_cast it). `mode` is a ForceMode.
    void (*AddForce)(std::uint32_t entity, const float force[3], std::uint32_t mode) = nullptr;
    void (*AddTorque)(std::uint32_t entity, const float torque[3], std::uint32_t mode) = nullptr;
    // Force at a world-space point — off-centre hits impart spin.
    void (*AddForceAtPosition)(std::uint32_t entity, const float force[3],
                               const float worldPos[3], std::uint32_t mode) = nullptr;
    // Impulse to every dynamic body within `radius` of `center`, scaled by linear distance
    // falloff. `upwardBias` (world units) lifts the impulse direction so things pop up. The
    // grenade / shockwave primitive.
    void (*AddExplosionForce)(const float center[3], float radius, float strength,
                              float upwardBias) = nullptr;
    void (*SetLinearVelocity)(std::uint32_t entity, const float v[3]) = nullptr;

    // --- Read-back (#185 PR 7) ---
    // Fill `out` with a body's motion state. Returns false (out.Valid == false) for anything
    // that isn't a live dynamic body.
    bool (*GetBodyState)(std::uint32_t entity, BodyState& out) = nullptr;
    // This frame's solid-contact transitions (see GetTriggerEvents for the copy/return
    // convention). Resting contacts don't spew Stay events — only hits above a small impulse.
    int (*GetContactEvents)(ContactEvent* out, int maxEvents) = nullptr;

    // --- Shape queries (#185 PR 9) ---
    // Sweep a sphere of `radius` from `origin` along `dir` up to `maxDistance`; fills `outHit`
    // like Raycast on the first blocking hit. Good for fat projectiles / lookahead.
    bool (*SphereCast)(const float origin[3], const float dir[3], float radius,
                       float maxDistance, RaycastHit& outHit) = nullptr;
    // Entity ids of every solid collider overlapping the sphere; writes up to `maxEntities`
    // into `out` and returns the total found. The explosion-radius / area-of-effect query.
    int (*OverlapSphere)(const float center[3], float radius,
                         std::uint32_t* out, int maxEntities) = nullptr;

    // --- Time (#144) ---
    void (*GetTime)(GameTime& out) = nullptr;
    // The game's time scale (Unity's Time.timeScale): 0 freezes physics, animators and the
    // DeltaTime Update receives; clamped to [0, 100]. Reset to Project Settings > Time on each Play.
    void (*SetTimeScale)(float scale) = nullptr;

    // --- Filtered queries (#170) --- same as Raycast / SphereCast / OverlapSphere above, plus
    // a QueryFilter. Rotations are quaternions (x, y, z, w); a null rotation = identity.
    bool (*RaycastFiltered)(const float origin[3], const float dir[3], float maxDistance,
                            const QueryFilter& filter, RaycastHit& outHit) = nullptr;
    // Every hit along the ray, nearest first. Writes up to `maxHits`, returns the total found.
    int (*RaycastAll)(const float origin[3], const float dir[3], float maxDistance,
                      const QueryFilter& filter, RaycastHit* out, int maxHits) = nullptr;
    bool (*SphereCastFiltered)(const float origin[3], const float dir[3], float radius, float maxDistance,
                               const QueryFilter& filter, RaycastHit& outHit) = nullptr;
    // Sweep a box (centre, half extents, rotation) along `dir`.
    bool (*BoxCast)(const float center[3], const float halfExtents[3], const float rotation[4],
                    const float dir[3], float maxDistance, const QueryFilter& filter, RaycastHit& outHit) = nullptr;
    // Sweep a capsule whose sphere centres are `point1` and `point2` (Unity's CapsuleCast).
    bool (*CapsuleCast)(const float point1[3], const float point2[3], float radius, const float dir[3],
                        float maxDistance, const QueryFilter& filter, RaycastHit& outHit) = nullptr;
    int (*OverlapSphereFiltered)(const float center[3], float radius, const QueryFilter& filter,
                                 std::uint32_t* out, int maxEntities) = nullptr;
    int (*OverlapBox)(const float center[3], const float halfExtents[3], const float rotation[4],
                      const QueryFilter& filter, std::uint32_t* out, int maxEntities) = nullptr;
    // v11. A one-shot 3D sound at `position` (world space) on the SFX bus. `path` is a sound file,
    // absolute or project-relative (as a component's sound field stores it); it's loaded on first
    // use. `pitch` 1 = as recorded.
    void (*PlaySoundAt)(const char* path, const float position[3], float volume, float pitch) = nullptr;
    // v11. The entity the player's gravity gun is holding, or 0xFFFFFFFF.
    std::uint32_t (*GetGrabbedEntity)() = nullptr;
    // v11. World position of an entity's physics actor (any collider, triggers included).
    bool (*GetActorPosition)(std::uint32_t entity, float out[3]) = nullptr;
};

struct GameModuleAPI {
    std::uint32_t Version = kGameModuleAPIVersion;
    // Reload contract (#172/#187, shared with the editor module; see Core/HotReloadSwap.h): the
    // host validates a rebuilt DLL, asks the OLD module to SaveState, calls its OnUnload and frees
    // it, and only then calls the NEW module's OnLoad with that state. The two never overlap.
    //   OnLoad: `state` is whatever the previous module's SaveState wrote (nullptr/0 on the first
    //     load or when it saved nothing). Return false to reject the build: undo anything this
    //     call set up first (the host won't call OnUnload), and the host reloads the previous
    //     build and hands it the same state. Every member may be null except Update.
    //   SaveState: called with (nullptr, 0) to ask how many bytes are needed, then with a buffer
    //     of that size; return the bytes written. The format is the module's own business, so
    //     version it if a later build must read an older build's state.
    bool (*OnLoad)(const void* state, std::size_t stateSize) = nullptr;
    void (*OnUnload)() = nullptr;
    void (*Update)(const GameModuleHostAPI& host, World& world, float deltaTime) = nullptr;
    std::size_t (*SaveState)(void* buffer, std::size_t capacity) = nullptr;
    // Optional (#144). Runs while playing, before each fixed physics sub-step, with the fixed
    // step length - zero, one or several times a frame depending on frame rate and time scale.
    // Forces and anything that must be frame-rate independent belong here, as in Unity.
    void (*FixedUpdate)(const GameModuleHostAPI& host, World& world, float fixedDeltaTime) = nullptr;
};

using GetGameModuleAPIFn = const GameModuleAPI* (*)();
