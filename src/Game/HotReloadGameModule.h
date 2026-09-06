#pragma once

#include <filesystem>

class World;

// Watches the freshly-built TartarusGame.dll and swaps it into the running editor. Windows
// keeps loaded DLLs locked, so every load uses a numbered private copy in the temp directory.
class HotReloadGameModule {
public:
    HotReloadGameModule() = default;
    ~HotReloadGameModule();

    HotReloadGameModule(const HotReloadGameModule&) = delete;
    HotReloadGameModule& operator=(const HotReloadGameModule&) = delete;

    void Initialize(const std::filesystem::path& sourceModule);
    void Tick(World& world, float deltaTime, bool playing);
    void Shutdown();

private:
    bool Reload(bool initialLoad);

    std::filesystem::path m_SourceModule;
    std::filesystem::path m_LoadedCopy;
    std::filesystem::file_time_type m_LastSourceWrite{};
    // Write-time of a build that loaded but failed validation (bad export / mismatched API
    // version). The poll skips re-attempting that exact timestamp so a broken DLL isn't retried
    // and re-logged every 0.35 s; a fresh build (new mtime) still is.
    std::filesystem::file_time_type m_LastFailedSourceWrite{};
    void* m_Handle = nullptr;
    const struct GameModuleAPI* m_API = nullptr;
    float m_PollElapsed = 0.0f;
    unsigned int m_Generation = 0;
};

