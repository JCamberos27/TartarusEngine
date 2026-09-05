#pragma once

#include <filesystem>

struct EditorConsoleState;

// Host-side state the reloadable editor module reads through EditorModuleHostAPI. It lives in the
// executable (not the DLL) so it survives a module reload untouched, and so host code — the
// toolbar's "Toggle Console" button, for one — can reach it directly.
namespace EditorModuleHost {
    // The Console panel's visibility / level toggles / filter text.
    EditorConsoleState& ConsoleState();
}

// Watches TartarusEditor.dll and swaps it while the host-owned ImGui frame is active.
class HotReloadEditorModule {
public:
    HotReloadEditorModule() = default;
    ~HotReloadEditorModule();

    HotReloadEditorModule(const HotReloadEditorModule&) = delete;
    HotReloadEditorModule& operator=(const HotReloadEditorModule&) = delete;

    // `parentWindow` is the GLFWwindow* native file dialogs opened by the module are parented to
    // (passed as void* so this header stays GLFW-free); may be null.
    void Initialize(const std::filesystem::path& sourceModule, void* parentWindow = nullptr);
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
