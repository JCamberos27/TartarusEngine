#pragma once

#include <cstdint>

class World;

// Deliberately small and versioned: the host keeps ownership of the World, renderer, editor,
// and every long-lived resource. A hot-reloaded module only receives a non-owning view for its
// per-frame gameplay work, so unloading it cannot invalidate editor state.
constexpr std::uint32_t kGameModuleAPIVersion = 3;

// Host-owned operations exposed to gameplay modules. Keep this table narrow and composed of
// plain function pointers so the DLL never owns editor, renderer, or scene-lifetime state.
// (v3 dropped EnsureRoomDonutTestSet, the old cross-DLL hot-reload smoke test — see
// HotReloadGameModule.cpp. Nothing needs a host callback yet; add them back here as gameplay
// grows.)
struct GameModuleHostAPI {
    std::uint32_t Version = kGameModuleAPIVersion;
};

struct GameModuleAPI {
    std::uint32_t Version = kGameModuleAPIVersion;
    void (*OnLoad)() = nullptr;
    void (*OnUnload)() = nullptr;
    void (*Update)(const GameModuleHostAPI& host, World& world, float deltaTime) = nullptr;
};

using GetGameModuleAPIFn = const GameModuleAPI* (*)();
