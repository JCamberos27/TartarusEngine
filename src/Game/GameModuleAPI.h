#pragma once

#include <cstdint>

class World;

// Deliberately small and versioned: the host keeps ownership of the World, renderer, editor,
// and every long-lived resource. A hot-reloaded module only receives a non-owning view for its
// per-frame gameplay work, so unloading it cannot invalidate editor state.
constexpr std::uint32_t kGameModuleAPIVersion = 2;

// Host-owned operations exposed to gameplay modules. Keep this table narrow and composed of
// plain function pointers so the DLL never owns editor, renderer, or scene-lifetime state.
struct GameModuleHostAPI {
    std::uint32_t Version = kGameModuleAPIVersion;
    void (*EnsureRoomDonutTestSet)(World& world) = nullptr;
};

struct GameModuleAPI {
    std::uint32_t Version = kGameModuleAPIVersion;
    void (*OnLoad)() = nullptr;
    void (*OnUnload)() = nullptr;
    void (*Update)(const GameModuleHostAPI& host, World& world, float deltaTime) = nullptr;
};

using GetGameModuleAPIFn = const GameModuleAPI* (*)();
