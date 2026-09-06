#pragma once

#include <filesystem>

struct EditorConsoleState;
class EditorLayer;
class World;

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
    // The live editor + world the module's host callbacks (Stats panel data, API v3) read from.
    // Call once per frame before Draw(); both are host-owned and outlive the module. Passing
    // nullptr is safe — the affected callbacks then report empty/zero.
    void SetFrameContext(EditorLayer* editor, World* world);
    void Draw(bool editorUIVisible, float deltaTime);
    void Shutdown();

private:
    bool Reload(bool initialLoad);

    std::filesystem::path m_SourceModule;
    std::filesystem::path m_LoadedCopy;
    std::filesystem::file_time_type m_LastSourceWrite{};
    // The source write-time of a build that loaded but then failed validation (bad export,
    // mismatched API version). The poll skips re-attempting that exact timestamp so a genuinely
    // broken DLL isn't retried — and re-logged — every 0.35 s; a fresh build (new mtime) is.
    std::filesystem::file_time_type m_LastFailedSourceWrite{};
    void* m_Handle = nullptr;
    const struct EditorModuleAPI* m_API = nullptr;
    float m_PollElapsed = 0.0f;
    unsigned int m_Generation = 0;
};
