#include "HotReloadEditorModule.h"

#include "EditorModuleAPI.h"
#include "Log.h"

#include <imgui.h>
#include <windows.h>

namespace fs = std::filesystem;

namespace {

void DrawStatusPanel(const char* title, const char* message, const char* accent) {
    bool open = true;
    if (!ImGui::Begin(title, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", message);
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.32f, 0.86f, 0.62f, 1.0f), "%s", accent);
    ImGui::End();
}

const EditorModuleHostAPI kHostAPI{
    kEditorModuleAPIVersion,
    &DrawStatusPanel,
};

} // namespace

HotReloadEditorModule::~HotReloadEditorModule() {
    Shutdown();
}

void HotReloadEditorModule::Initialize(const fs::path& sourceModule) {
    Shutdown();
    m_SourceModule = sourceModule;
    m_PollElapsed = 0.0f;
    Reload(true);
}

void HotReloadEditorModule::Draw(bool editorUIVisible, float deltaTime) {
    m_PollElapsed += deltaTime;
    if (m_PollElapsed >= 0.35f) {
        m_PollElapsed = 0.0f;
        std::error_code ec;
        const fs::file_time_type sourceWrite = fs::last_write_time(m_SourceModule, ec);
        if (!ec && sourceWrite != m_LastSourceWrite) Reload(false);
    }

    if (editorUIVisible && m_API && m_API->Draw) m_API->Draw(kHostAPI);
}

bool HotReloadEditorModule::Reload(bool initialLoad) {
    std::error_code ec;
    const fs::file_time_type sourceWrite = fs::last_write_time(m_SourceModule, ec);
    if (ec) {
        if (initialLoad) Log::Warn("Editor hot reload: TartarusEditor.dll was not found; editor modules are disabled.");
        return false;
    }

    const fs::path cacheDir = fs::temp_directory_path(ec) / "TartarusEngine" / "EditorHotReload";
    if (ec || (fs::create_directories(cacheDir, ec), ec)) {
        Log::Error("Editor hot reload: couldn't create a temporary DLL directory.");
        return false;
    }

    const fs::path copyPath = cacheDir / ("TartarusEditor_" + std::to_string(++m_Generation) + ".dll");
    fs::copy_file(m_SourceModule, copyPath, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        Log::Warn("Editor hot reload: TartarusEditor.dll is still being written; will retry.");
        return false;
    }

    HMODULE candidate = ::LoadLibraryW(copyPath.c_str());
    if (!candidate) {
        Log::Error("Editor hot reload: couldn't load the rebuilt TartarusEditor.dll.");
        fs::remove(copyPath, ec);
        return false;
    }

    const auto getAPI = reinterpret_cast<GetEditorModuleAPIFn>(::GetProcAddress(candidate, "TartarusGetEditorModuleAPI"));
    const EditorModuleAPI* candidateAPI = getAPI ? getAPI() : nullptr;
    if (!candidateAPI || candidateAPI->Version != kEditorModuleAPIVersion || !candidateAPI->Draw) {
        Log::Error("Editor hot reload: TartarusEditor.dll has an incompatible module API.");
        ::FreeLibrary(candidate);
        fs::remove(copyPath, ec);
        return false;
    }

    if (candidateAPI->OnLoad) candidateAPI->OnLoad();
    HMODULE previous = static_cast<HMODULE>(m_Handle);
    const fs::path previousCopy = m_LoadedCopy;
    if (m_API && m_API->OnUnload) m_API->OnUnload();
    if (previous) ::FreeLibrary(previous);
    if (!previousCopy.empty()) fs::remove(previousCopy, ec);

    m_Handle = candidate;
    m_API = candidateAPI;
    m_LoadedCopy = copyPath;
    m_LastSourceWrite = sourceWrite;
    Log::Info(initialLoad ? "Editor hot reload: TartarusEditor module loaded."
                          : "Editor hot reload: TartarusEditor module reloaded.");
    return true;
}

void HotReloadEditorModule::Shutdown() {
    if (m_API && m_API->OnUnload) m_API->OnUnload();
    if (m_Handle) ::FreeLibrary(static_cast<HMODULE>(m_Handle));

    std::error_code ec;
    if (!m_LoadedCopy.empty()) fs::remove(m_LoadedCopy, ec);
    m_Handle = nullptr;
    m_API = nullptr;
    m_LoadedCopy.clear();
}
