#pragma once

#include <filesystem>

// Watches TartarusEditor.dll and swaps it while the host-owned ImGui frame is active.
class HotReloadEditorModule {
public:
    HotReloadEditorModule() = default;
    ~HotReloadEditorModule();

    HotReloadEditorModule(const HotReloadEditorModule&) = delete;
    HotReloadEditorModule& operator=(const HotReloadEditorModule&) = delete;

    void Initialize(const std::filesystem::path& sourceModule);
    void Draw(bool editorUIVisible, float deltaTime);
    void Shutdown();

private:
    bool Reload(bool initialLoad);

    std::filesystem::path m_SourceModule;
    std::filesystem::path m_LoadedCopy;
    std::filesystem::file_time_type m_LastSourceWrite{};
    void* m_Handle = nullptr;
    const struct EditorModuleAPI* m_API = nullptr;
    float m_PollElapsed = 0.0f;
    unsigned int m_Generation = 0;
};
