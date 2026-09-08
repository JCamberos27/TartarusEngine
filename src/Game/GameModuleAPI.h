#pragma once

#include <cstdint>

class World;

// Deliberately small and versioned: the host keeps ownership of the World, renderer, editor,
// and every long-lived resource. A hot-reloaded module only receives a non-owning view for its
// per-frame gameplay work, so unloading it cannot invalidate editor state.
constexpr std::uint32_t kGameModuleAPIVersion = 5;

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
};

struct GameModuleAPI {
    std::uint32_t Version = kGameModuleAPIVersion;
    void (*OnLoad)() = nullptr;
    void (*OnUnload)() = nullptr;
    void (*Update)(const GameModuleHostAPI& host, World& world, float deltaTime) = nullptr;
};

using GetGameModuleAPIFn = const GameModuleAPI* (*)();
