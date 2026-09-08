#pragma once

#include <cstdint>

class World;

// Deliberately small and versioned: the host keeps ownership of the World, renderer, editor,
// and every long-lived resource. A hot-reloaded module only receives a non-owning view for its
// per-frame gameplay work, so unloading it cannot invalidate editor state.
constexpr std::uint32_t kGameModuleAPIVersion = 4;

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

// Host-owned operations exposed to gameplay modules. Keep this table narrow and composed of
// plain function pointers so the DLL never owns editor, renderer, or scene-lifetime state.
// (v4 adds Raycast — the first real host callback — backed by the PhysX world that exists only
// while playing; it returns false when called outside Play. v3 had dropped EnsureRoomDonutTestSet,
// the old cross-DLL hot-reload smoke test — see HotReloadGameModule.cpp.)
struct GameModuleHostAPI {
    std::uint32_t Version = kGameModuleAPIVersion;

    // Cast a ray from `origin` along `dir` (need not be normalised) up to `maxDistance` world
    // units. Returns true and fills `outHit` on the closest hit; returns false and leaves
    // `outHit` at its defaults on a miss or when no PhysX world is live. `dir` all-zero is a
    // miss.
    bool (*Raycast)(const float origin[3], const float dir[3], float maxDistance,
                    RaycastHit& outHit) = nullptr;
};

struct GameModuleAPI {
    std::uint32_t Version = kGameModuleAPIVersion;
    void (*OnLoad)() = nullptr;
    void (*OnUnload)() = nullptr;
    void (*Update)(const GameModuleHostAPI& host, World& world, float deltaTime) = nullptr;
};

using GetGameModuleAPIFn = const GameModuleAPI* (*)();
