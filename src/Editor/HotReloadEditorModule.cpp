#include "HotReloadEditorModule.h"

#include "EditorModuleAPI.h"
#include "EditorUIHelpers.h"
#include "FileDialog.h"
#include "Log.h"

#include <cstring>
#include <imgui.h>
#include <windows.h>

namespace fs = std::filesystem;

namespace {

// The window native dialogs opened on the module's behalf are parented to. File-scope because the
// host API is a table of plain function pointers with no user-data slot.
GLFWwindow* g_ParentWindow = nullptr;

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

void* GetImGuiContextPtr() {
    return ImGui::GetCurrentContext();
}

void GetImGuiAllocators(EditorModuleImGuiAllocFn* outAlloc, EditorModuleImGuiFreeFn* outFree, void** outUserData) {
    ImGuiMemAllocFunc allocFn = nullptr;
    ImGuiMemFreeFunc freeFn = nullptr;
    void* userData = nullptr;
    ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &userData);
    if (outAlloc) *outAlloc = allocFn;
    if (outFree) *outFree = freeFn;
    if (outUserData) *outUserData = userData;
}

// --- Log bridge -----------------------------------------------------------------------------
// Log's entry vector is a function-local static inside Log.cpp, which is compiled into this
// executable only. A DLL that compiled Log.cpp too would get its own separate, always-empty
// vector, so the module reads the real log exclusively through these.
unsigned int LogRevisionFn() { return Log::Revision(); }
int LogEntryCountFn() { return (int)Log::Entries().size(); }

bool LogGetEntryFn(int index, int* outLevel, const char** outMessage, const char** outTime, int* outCount) {
    const std::vector<LogEntry>& entries = Log::Entries();
    if (index < 0 || (size_t)index >= entries.size()) return false;
    const LogEntry& e = entries[(size_t)index];
    if (outLevel)   *outLevel = (int)e.Level;
    if (outMessage) *outMessage = e.Message.c_str();
    if (outTime)    *outTime = e.Time.c_str();
    if (outCount)   *outCount = e.Count;
    return true;
}

int LogCountOfFn(int level) { return Log::CountOf((LogLevel)level); }
void LogClearFn() { Log::Clear(); }
void LogInfoFn(const char* message) { Log::Info(message ? message : ""); }
void LogErrorFn(const char* message) { Log::Error(message ? message : ""); }

// --- Editor services -------------------------------------------------------------------------
// Not variadic: a format string crossing the boundary buys nothing, and the module can format its
// own text. Still routed here so EditorSettings::Get().ShowTooltips (another host-side singleton)
// governs module tooltips exactly like every host panel's.
void SetTooltipFn(const char* text) { EditorUI::SetTooltip("%s", text ? text : ""); }

bool SaveFileDialogFn(const char* filter, const char* defaultExt, char* outPath, int outPathSize) {
    if (!outPath || outPathSize <= 0) return false;
    outPath[0] = '\0';
    std::string path = FileDialog::SaveFile(filter, defaultExt, g_ParentWindow);
    if (path.empty() || (int)path.size() + 1 > outPathSize) return false;
    std::memcpy(outPath, path.c_str(), path.size() + 1);
    return true;
}

EditorConsoleState* ConsoleStateFn() { return &EditorModuleHost::ConsoleState(); }

const EditorModuleHostAPI kHostAPI{
    kEditorModuleAPIVersion,
    &DrawStatusPanel,
    &GetImGuiContextPtr,
    &GetImGuiAllocators,
    &LogRevisionFn,
    &LogEntryCountFn,
    &LogGetEntryFn,
    &LogCountOfFn,
    &LogClearFn,
    &LogInfoFn,
    &LogErrorFn,
    &SetTooltipFn,
    &SaveFileDialogFn,
    &ConsoleStateFn,
};

} // namespace

EditorConsoleState& EditorModuleHost::ConsoleState() {
    static EditorConsoleState state;
    return state;
}

HotReloadEditorModule::~HotReloadEditorModule() {
    Shutdown();
}

void HotReloadEditorModule::Initialize(const fs::path& sourceModule, void* parentWindow) {
    Shutdown();
    g_ParentWindow = static_cast<GLFWwindow*>(parentWindow);
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
