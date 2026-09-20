#include "HotReloadEditorModule.h"

#include "EditorModuleAPI.h"
#include "EditorUIHelpers.h"
#include "FileDialog.h"
#include "Log.h"
#include "EditorLayer.h"
#include "EditorSettings.h"
#include "World.h"
#include "Components.h"
#include "Profiler.h"
#include "GLStateCache.h"
#include "AssetLibrary.h"
#include "Camera.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <set>
#include <unordered_map> // #178 - stack-trace resolve cache
#include <string>
#include <vector>
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <windows.h>

#include <shellapi.h>

#include "HotReloadSwap.h"
#include "UserPaths.h"

namespace fs = std::filesystem;

namespace {

// The window native dialogs opened on the module's behalf are parented to. File-scope because the
// host API is a table of plain function pointers with no user-data slot.
GLFWwindow* g_ParentWindow = nullptr;

// The live editor + world + assets + editor camera the Stats-panel callbacks (API v3) and the
// toolbar's menu bodies / commands (API v4) read from, refreshed every frame by
// HotReloadEditorModule::SetFrameContext(). Same file-scope rationale as g_ParentWindow: the host
// API is a flat function-pointer table with nowhere to hang a context pointer.
//
// This is a correctness-bearing single-instance assumption (audit ARCH-204 / #375): every one of
// the exported Fn callbacks above reads these directly, with no per-instance identity, so two live
// HotReloadEditorModule objects would silently clobber each other's frame context. Only one has
// ever existed (main.cpp's `editorModule`) and the class isn't copyable, but nothing enforced that
// invariant — g_InstanceLive below does, and Shutdown() clears these four so a callback invoked
// after teardown (there shouldn't be one — Draw() is gated on m_API, which Shutdown() also clears)
// reads null instead of a dangling pointer.
EditorLayer* g_Editor = nullptr;
World* g_World = nullptr;
AssetLibrary* g_Assets = nullptr;
Camera* g_Camera = nullptr;

// Set for the lifetime of the one HotReloadEditorModule allowed to hold the g_* globals above
// (from Initialize() to Shutdown()/destruction). A second live instance would share — and fight
// over — the same globals, so Initialize() asserts this is false and logs instead of silently
// corrupting the first instance's state.
bool g_InstanceLive = false;

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

bool LogGetEntryContextFn(int index, int* outEntityOrder, const char** outAssetPath) {
    const std::vector<LogEntry>& entries = Log::Entries();
    if (index < 0 || (size_t)index >= entries.size()) return false;
    const LogContext& c = entries[(size_t)index].Context;
    if (outEntityOrder) *outEntityOrder = c.EntityOrder;
    if (outAssetPath)   *outAssetPath = c.AssetPath.c_str();
    return true;
}

// #178 - resolved lazily and cached by entry Seq: symbol resolution is slow, and the same row
// is re-asked for every frame its stack is open. Keyed on Seq rather than index because the
// ring buffer shifts indices when old entries are trimmed.
bool LogGetEntryStackFn(int index, const char** outStack) {
    const std::vector<LogEntry>& entries = Log::Entries();
    if (index < 0 || (size_t)index >= entries.size()) return false;
    const LogEntry& e = entries[(size_t)index];
    if (e.Stack.empty()) return false;

    static std::unordered_map<unsigned long long, std::string> s_Cache;
    auto it = s_Cache.find(e.Seq);
    if (it == s_Cache.end()) {
        if (s_Cache.size() > 256) s_Cache.clear(); // bounded: these are only ever a debugging aid
        it = s_Cache.emplace(e.Seq, Log::ResolveStack(e.Stack)).first;
    }
    if (it->second.empty()) return false;
    if (outStack) *outStack = it->second.c_str();
    return true;
}

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

// --- Console click-to-navigate (API v27) ------------------------------------------------------
bool SelectEntityByOrderFn(int orderValue) {
    return g_Editor && g_World && g_Editor->SelectEntityByOrder(*g_World, orderValue);
}
bool PingAssetPathFn(const char* path) {
    return g_Editor && g_Assets && path && g_Editor->PingAssetPath(*g_Assets, path);
}

// --- Notification bell (API v28) ----------------------------------------------------------------
int GetNotificationUnreadCountFn() { return g_Editor ? g_Editor->NotificationUnreadCount() : 0; }
void MarkNotificationsReadFn() { if (g_Editor) g_Editor->MarkNotificationsRead(); }
void DrawNotificationsPopupBodyFn() { if (g_Editor) g_Editor->DrawNotificationsPopupBody(); }

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

// --- Stats panel (API v3) ------------------------------------------------------------------
// Bodies for the reloadable Statistics HUD's read-only view of host state. Everything the panel
// shows is owned by EditorLayer / the renderer / the Profiler singletons, which are linked into
// the executable only — the module reads them exclusively through these, exactly like the Log
// bridge above. g_Editor / g_World are refreshed each frame by SetFrameContext().

void StatsGetViewportRectFn(float* outX, float* outY, float* outW, float* outH,
                            float* outUIScale, bool* outEnabled) {
    const bool haveEditor = g_Editor != nullptr;
    const glm::vec2 pos  = haveEditor ? g_Editor->ViewportPos()  : glm::vec2(0.0f);
    const glm::vec2 size = haveEditor ? g_Editor->ViewportSize() : glm::vec2(0.0f);
    if (outX) *outX = pos.x;
    if (outY) *outY = pos.y;
    if (outW) *outW = size.x;
    if (outH) *outH = size.y;
    if (outUIScale) *outUIScale = haveEditor ? g_Editor->UIScale() : 1.0f;
    // Mirrors the old DrawStatsPanel guards: the toolbar/menu toggle, plus a live non-degenerate
    // Scene viewport to pin to.
    if (outEnabled) {
        *outEnabled = haveEditor && EditorSettings::Get().SceneShowStats &&
                      g_Editor->IsSceneViewportVisible() && size.x >= 1.0f && size.y >= 1.0f;
    }
}

void StatsGetRenderStatsFn(EditorModuleRenderStats* out) {
    if (!out) return;
    *out = EditorModuleRenderStats{};
    if (!g_Editor) return;
    const EditorLayer::RenderStats& rs = g_Editor->GetRenderStats();
    out->DrawCalls = rs.DrawCalls;
    out->Triangles = rs.Triangles;
    out->Vertices = rs.Vertices;
    out->Culled = rs.Culled;
    out->LightBufferOverflowed = rs.LightBufferOverflowed;
    out->ClusterSaturated = rs.ClusterSaturated;
}

int StatsGetProfilerSamplesFn(EditorModuleProfilerSample* outArr, int maxCount, bool gpu) {
    if (!outArr || maxCount <= 0) return 0;
    const std::vector<Profiler::Entry>& src = gpu ? Profiler::GetLastFrameGpu() : Profiler::GetLastFrame();
    // Not std::min: <windows.h> (included below for LoadLibrary etc.) defines a min() macro.
    const int srcCount = (int)src.size();
    const int n = maxCount < srcCount ? maxCount : srcCount;
    for (int i = 0; i < n; ++i) {
        std::snprintf(outArr[i].Name, sizeof(outArr[i].Name), "%s", src[(size_t)i].Name.c_str());
        outArr[i].Milliseconds = src[(size_t)i].Milliseconds;
    }
    return n;
}

void StatsGetGLFrameStatsFn(EditorModuleGLFrameStats* out) {
    if (!out) return;
    const GLStateCache::FrameStats& gl = GLStateCache::GetFrameStats();
    out->ProgramBinds = gl.ProgramBinds;
    out->ProgramBindsSkipped = gl.ProgramBindsSkipped;
    out->TextureBinds = gl.TextureBinds;
    out->TextureBindsSkipped = gl.TextureBindsSkipped;
    out->VaoBinds = gl.VaoBinds;
    out->VaoBindsSkipped = gl.VaoBindsSkipped;
}

void StatsGetSceneEntityCountsFn(int* outEntities, int* outRenderers, int* outColliders,
                                 int* outLights, int* outInactive) {
    int entities = 0, renderers = 0, colliders = 0, lights = 0, inactive = 0;
    if (g_World) {
        // Same tally as the old EditorLayer::DrawStatsPanel loop.
        for (auto entity : g_World->Registry.view<TransformComponent>()) {
            ++entities;
            if (g_World->Registry.all_of<RenderableComponent>(entity)) ++renderers;
            if (g_World->Registry.all_of<LightComponent>(entity)) ++lights;
            if (g_World->Registry.all_of<ColliderComponent>(entity)) ++colliders;
            if (g_World->Registry.all_of<InactiveTag>(entity)) ++inactive;
        }
    }
    if (outEntities) *outEntities = entities;
    if (outRenderers) *outRenderers = renderers;
    if (outColliders) *outColliders = colliders;
    if (outLights) *outLights = lights;
    if (outInactive) *outInactive = inactive;
}

float StatsGetSmoothedFrameMsFn() { return g_Editor ? g_Editor->SmoothedFrameMs() : 0.0f; }
int StatsGetFrameTimeHistoryFn(float* out, int maxCount) {
    return g_Editor ? g_Editor->FrameTimeHistory(out, maxCount) : 0;
}

ImFont* GetMonoFontFn() { return g_Editor ? g_Editor->GetMonoFont() : nullptr; }

void StatsSetHideEngineMarkFn(bool hide) {
    if (g_Editor) g_Editor->SetHideEngineMarkForStats(hide);
}

// --- Toolbar / menus (API v4) --------------------------------------------------------------
// Bodies for the reloadable toolbar strip (EditorModuleToolbar.cpp). The strip's window,
// XP chrome and icon-row layout are module-side; every toggle it shows and every menu it opens
// is host state / host code reached through these, exactly like the Stats bridge above. The
// g_* pointers are refreshed each frame by SetFrameContext().

void TbGetToolbarMetrics(float* outWinW, float* outToolbarH, float* outUIScale) {
    // Main-window client width == the main ImGui viewport size (the editor runs a single
    // viewport) — what the old caller read via glfwGetWindowSize.
    float w = 0.0f;
    if (const ImGuiViewport* vp = ImGui::GetMainViewport()) w = vp->Size.x;
    if (outWinW)     *outWinW = w;
    if (outToolbarH) *outToolbarH = g_Editor ? g_Editor->ToolbarHeightPx() : 0.0f;
    if (outUIScale)  *outUIScale = g_Editor ? g_Editor->UIScale() : 1.0f;
}

void TbSetTitleBarDragHovered(bool hovered) { if (g_Editor) g_Editor->SetTitleBarDragHovered(hovered); }

void TbWindowMinimize() { if (g_ParentWindow) glfwIconifyWindow(g_ParentWindow); }
void TbWindowToggleMaximize() {
    if (!g_ParentWindow) return;
    if (glfwGetWindowAttrib(g_ParentWindow, GLFW_MAXIMIZED)) glfwRestoreWindow(g_ParentWindow);
    else                                                     glfwMaximizeWindow(g_ParentWindow);
}
void TbWindowClose() { if (g_ParentWindow) glfwSetWindowShouldClose(g_ParentWindow, GLFW_TRUE); }
bool TbWindowIsMaximized() { return g_ParentWindow && glfwGetWindowAttrib(g_ParentWindow, GLFW_MAXIMIZED) != 0; }

int  TbGetGizmoOp() { return g_Editor ? g_Editor->GizmoOpIndex() : 0; }
void TbSetGizmoOp(int op) { if (g_Editor) g_Editor->SetGizmoOpIndex(op); }
int  TbGetShadingMode() { return g_Editor ? g_Editor->ShadingModeIndex() : 0; }
void TbSetShadingMode(int mode) { if (g_Editor) g_Editor->SetShadingModeIndex(mode); }
bool TbGetGizmoLocalSpace() { return g_Editor && g_Editor->GizmoLocalSpace(); }
void TbSetGizmoLocalSpace(bool on) { if (g_Editor) g_Editor->SetGizmoLocalSpace(on); }
bool TbGetGizmoPivotCenter() { return g_Editor && g_Editor->GizmoPivotCenter(); }
void TbSetGizmoPivotCenter(bool on) { if (g_Editor) g_Editor->SetGizmoPivotCenter(on); }
bool TbGetShowGrid() { return g_Editor && g_Editor->ShowGrid(); }
void TbSetShowGrid(bool on) { if (g_Editor) g_Editor->SetShowGrid(on); }
bool TbGetGridSnapEnabled() { return g_Editor && g_Editor->GridSnapEnabled(); }
void TbSetGridSnapEnabled(bool on) { if (g_Editor) g_Editor->SetGridSnapEnabled(on); }
bool TbGetShowHistory() { return g_Editor && g_Editor->ShowHistory(); }
void TbSetShowHistory(bool on) { if (g_Editor) g_Editor->SetShowHistory(on); }
bool TbGetShowStats() { return EditorSettings::Get().SceneShowStats; }
void TbSetShowStats(bool on) { EditorSettings::Get().SceneShowStats = on; EditorSettings::Save(); }
bool TbGetShowLightGizmos() { return EditorSettings::Get().ShowLightGizmos; }
void TbSetShowLightGizmos(bool on) { EditorSettings::Get().ShowLightGizmos = on; EditorSettings::Save(); }
bool TbIsOrthographic() { return g_Camera && g_Camera->Orthographic; }
void TbToggleOrthographic() {
    if (g_Editor && g_World && g_Camera) g_Editor->ToolbarToggleOrthographic(*g_World, *g_Camera);
}
void TbUndo() { if (g_Editor && g_World && g_Assets) g_Editor->ToolbarUndo(*g_World, *g_Assets); }
void TbRedo() { if (g_Editor && g_World && g_Assets) g_Editor->ToolbarRedo(*g_World, *g_Assets); }
bool TbCanSnapSelectionToGround() {
    return g_Editor && g_World && g_Editor->ToolbarCanSnapToGround(*g_World);
}
void TbSnapSelectionToGround() { if (g_Editor && g_World) g_Editor->ToolbarSnapToGround(*g_World); }
void TbRequestResetLayout() { if (g_Editor) g_Editor->RequestResetLayout(); }
void TbOpenPreferences() { if (g_Editor) g_Editor->OpenPreferences(); }
void TbOpenProjectSettings() { if (g_Editor) g_Editor->OpenProjectSettings(); }
void TbOpenShortcutsReference() { if (g_Editor) g_Editor->OpenShortcutsReference(); }

// --- Help menu (API v33, #184) -------------------------------------------------------------
constexpr const wchar_t* kRepoUrl = L"https://github.com/JCamberos27/TartarusEngine";

void OpenUrl(const std::wstring& url) {
    ::ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
// Explorer on the Logs folder with the current session's log selected (the folder alone if the
// log doesn't exist yet).
void RevealEditorLog() {
    const fs::path logs = fs::u8path(UserPaths::Resolve("Logs"));
    const fs::path log = logs / "Editor.log";
    std::error_code ec;
    if (fs::exists(log, ec)) {
        const std::wstring args = L"/select,\"" + log.wstring() + L"\"";
        ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ::ShellExecuteW(nullptr, L"open", logs.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
void HelpOpenAbout() { if (g_Editor) g_Editor->OpenAbout(); }
void HelpOpenDocumentation() { OpenUrl(std::wstring(kRepoUrl) + L"#readme"); }
void HelpOpenLogFolder() { RevealEditorLog(); }
void HelpReportBug() {
    if (g_Editor) {
        const std::string report = g_Editor->SystemReportText();
        if (!report.empty()) ImGui::SetClipboardText(report.c_str());
    }
    RevealEditorLog();
    OpenUrl(std::wstring(kRepoUrl) + L"/issues/new");
    Log::Info("Report a Bug: system report copied to the clipboard; attach Logs/Editor.log to the issue.");
}

void TbDrawFileMenuBody() {
    if (g_Editor && g_World && g_Assets) g_Editor->DrawFileMenuBody(*g_World, *g_Assets);
}
void TbDrawAddEntityMenuItems() {
    if (g_Editor && g_World && g_Assets && g_Camera)
        g_Editor->DrawAddEntityItems(*g_World, *g_Assets, *g_Camera);
}
void TbDrawViewMenuBody() {
    if (g_Editor && g_World && g_Camera) g_Editor->DrawViewMenuBody(*g_World, *g_Camera);
}
void TbDrawWindowMenuBody() { if (g_Editor) g_Editor->DrawWindowMenuBody(); }
void TbDrawCaptureOptionsPopupBody() { if (g_Editor) g_Editor->DrawCaptureOptionsPopupBody(); }
void TbRequestCapture() { if (g_Editor) g_Editor->RequestCapture(); }
void TbGetCaptureButtonTooltip(char* out, int outSize) {
    if (!out || outSize <= 0) return;
    const EditorSettings& s = EditorSettings::Get();
    static const char* kModes[] = { "Full editor window", "Scene viewport",
                                    "Scene viewport (clean)", "Game view" };
    int cm = s.CaptureMode; cm = cm < 0 ? 0 : (cm > 3 ? 3 : cm);
    std::snprintf(out, (size_t)outSize, "Capture screenshot \xe2\x80\x94 %s%s (Print Screen)",
                  kModes[cm], (cm == 1 || cm == 2) && s.CaptureScale > 1 ? " x2+" : "");
}

// --- Asset Browser, thin slice (API v5) --------------------------------------------------
// The Asset Browser's chrome lives in EditorModuleAssetBrowser.cpp; the grid stays host-side.
// AssetLibrary never crosses the boundary — folders come back as a per-frame string snapshot
// and every mutation is an undoable command routed through EditorLayer's public bridge methods.
// g_Assets / g_World / g_Editor are refreshed each frame by SetFrameContext().

void AbPutStr(char* out, int n, const std::string& s) {
    if (!out || n <= 0) return;
    const int m = (int)s.size() < n - 1 ? (int)s.size() : n - 1;
    std::memcpy(out, s.data(), (size_t)m);
    out[m] = '\0';
}

bool  AbGetShow() { return g_Editor && g_Editor->GetShowAssetBrowser(); }
void  AbSetShow(bool on) { if (g_Editor) g_Editor->SetShowAssetBrowser(on); }
void  AbGetSearch(char* o, int n) { AbPutStr(o, n, g_Editor ? g_Editor->AssetSearchFilter() : std::string()); }
void  AbSetSearch(const char* s) { if (g_Editor) g_Editor->SetAssetSearchFilter(s ? s : ""); }
void  AbGetLabelMenuFilter(char* o, int n) { AbPutStr(o, n, g_Editor ? g_Editor->AssetLabelMenuFilter() : std::string()); }
void  AbSetLabelMenuFilter(const char* s) { if (g_Editor) g_Editor->SetAssetLabelMenuFilter(s ? s : ""); }
void  AbGetCurrentFolder(char* o, int n) { AbPutStr(o, n, g_Editor ? g_Editor->CurrentAssetFolder() : std::string()); }
void  AbSetCurrentFolder(const char* f) { if (g_Editor) g_Editor->SetCurrentAssetFolderFromTree(f ? f : ""); }
float AbGetTreeWidth() { return g_Editor ? g_Editor->AssetTreeWidthPx() : 180.0f; }
void  AbSetTreeWidth(float px, bool commit) { if (g_Editor) g_Editor->SetAssetTreeWidthPx(px, commit); }
bool  AbConsumeSearchFocus() { return g_Editor && g_Editor->ConsumeAssetSearchFocusRequest(); }
void  AbSetFocused(bool f) { if (g_Editor) g_Editor->SetAssetBrowserFocused(f); }
bool  AbIsFolderExpanded(const char* p) { return g_Editor && p && g_Editor->IsAssetFolderExpanded(p); }
void  AbSetFolderExpanded(const char* p, bool e, bool r) {
    if (g_Editor && g_Assets && p) g_Editor->AssetBrowserSetFolderExpanded(*g_Assets, p, e, r);
}
void  AbTreeFrameSetup(char* o, int n) {
    AbPutStr(o, n, (g_Editor && g_Assets) ? g_Editor->AssetBrowserTreeFrameSetup(*g_Assets) : std::string());
}
int   AbGetFolderCount() { return g_Assets ? (int)g_Assets->Folders().size() : 0; }
bool  AbGetFolder(int i, char* o, int n) {
    if (!g_Assets) return false;
    const std::vector<std::string>& f = g_Assets->Folders();
    if (i < 0 || (size_t)i >= f.size()) return false;
    AbPutStr(o, n, f[(size_t)i]);
    return true;
}
int   AbGetKnownLabelCount() { return g_Assets ? (int)g_Assets->AllKnownLabels().size() : 0; }
bool  AbGetKnownLabel(int i, char* o, int n) {
    if (!g_Assets) return false;
    const std::set<std::string>& s = g_Assets->AllKnownLabels();
    if (i < 0 || (size_t)i >= s.size()) return false;
    auto it = s.begin();
    std::advance(it, i);
    AbPutStr(o, n, *it);
    return true;
}
void  AbCreateFolder(const char* p) {
    if (g_Editor && g_World && g_Assets && p) g_Editor->AssetBrowserCreateFolder(*g_World, *g_Assets, p);
}
void  AbRenameFolder(const char* oldP, const char* newP) {
    if (g_Editor && g_World && g_Assets && oldP && newP)
        g_Editor->AssetBrowserRenameFolder(*g_World, *g_Assets, oldP, newP);
}
void  AbMoveAsset(const char* key, const char* folder) {
    if (g_Editor && g_World && g_Assets && key && folder)
        g_Editor->AssetBrowserMoveAssetToFolder(*g_World, *g_Assets, key, folder);
}
void  AbBeginRenameFolder(const char* p) { if (g_Editor && p) g_Editor->AssetBrowserBeginRenameFolder(p); }
void  AbImportViaDialog(int kind, const char* into) {
    if (g_Editor && g_World && g_Assets)
        g_Editor->AssetBrowserImportViaDialog(*g_World, *g_Assets, kind, into ? into : "");
}
void  AbGridFrameBegin() { if (g_Editor && g_World && g_Assets) g_Editor->AssetGridFrameBegin(*g_World, *g_Assets); }
int   AbGridCellCount() { return g_Editor ? g_Editor->AssetGridCellCount() : 0; }
void  AbGetGridMetrics(float* i, float* u, float* l) { if (g_Editor) g_Editor->GetAssetGridMetrics(i, u, l); }
void  AbDrawCell(int idx, float w, float h, bool grid) {
    if (g_Editor && g_World && g_Assets) g_Editor->DrawAssetCell(*g_World, *g_Assets, idx, w, h, grid);
}
void  AbHandleGridBackground() { if (g_Editor && g_World && g_Assets) g_Editor->HandleAssetGridBackground(*g_World, *g_Assets); }
void  AbGetSelectionSummary(char* o, int n) {
    if (g_Editor && g_Assets) g_Editor->GetAssetSelectionSummary(*g_Assets, o, n);
    else if (o && n > 0) o[0] = '\0';
}
float AbGetIconSize() { return g_Editor ? g_Editor->GetAssetIconSize() : 64.0f; }
void  AbSetIconSize(float px, bool commit) { if (g_Editor) g_Editor->SetAssetIconSize(px, commit); }
void  AbGridFrameEnd() { if (g_Editor && g_World && g_Assets) g_Editor->AssetGridFrameEnd(*g_World, *g_Assets); }

// --- Scene Hierarchy, thin slice (API v6) ----------------------------------------------------
// The module owns Begin("Scene Hierarchy") + the search box + the Expand/Collapse-all buttons;
// the entity tree stays host-side in DrawHierarchyTreeBody.
bool  HierGetShow() { return g_Editor && g_Editor->GetShowHierarchy(); }
void  HierSetShow(bool on) { if (g_Editor) g_Editor->SetShowHierarchy(on); }
void  HierGetFilter(char* o, int n) {
    if (!o || n <= 0) return;
    const std::string& s = g_Editor ? g_Editor->HierarchyFilterText() : std::string();
    const int m = (int)s.size() < n - 1 ? (int)s.size() : n - 1;
    std::memcpy(o, s.data(), (size_t)m);
    o[m] = '\0';
}
void  HierSetFilter(const char* s) { if (g_Editor) g_Editor->SetHierarchyFilterText(s ? s : ""); }
void  HierExpandAll(bool open) { if (g_Editor && g_World) g_Editor->HierarchyExpandAll(*g_World, open); }
void  HierDrawTreeBody() {
    if (g_Editor && g_World && g_Assets) g_Editor->DrawHierarchyTreeBody(*g_World, *g_Assets);
}

// --- Inspector, frame only (API v8) --------------------------------------------------------
bool  InspGetShow() { return g_Editor && g_Editor->GetShowInspector(); }
void  InspSetShow(bool on) { if (g_Editor) g_Editor->SetShowInspector(on); }
void  InspDrawBody() {
    if (g_Editor && g_World && g_Assets) g_Editor->DrawInspectorBody(*g_World, *g_Assets);
}
void  TbDrawGridSnapPopupBody() { if (g_Editor) g_Editor->DrawGridSnapPopupBody(); }
void  TbDrawGizmosPopupBody()   { if (g_Editor) g_Editor->DrawGizmosPopupBody(); }
bool  TbGetGizmosMasterVisible() { return g_Editor && g_Editor->GizmosMasterVisible(); }
void  TbSetGizmosMasterVisible(bool on) { if (g_Editor) g_Editor->SetGizmosMasterVisible(on); }
bool  TbGetHandTool() { return g_Editor && g_Editor->HandToolActive(); }
void  TbSetHandTool(bool on) { if (g_Editor) g_Editor->SetHandToolActive(on); }
bool  TbGetLockViewToSelection() { return g_Editor && g_Editor->LockViewToSelection(); }
void  TbSetLockViewToSelection(bool on) { if (g_Editor) g_Editor->SetLockViewToSelection(on); }
int   TbGetAssetSort() {
    return EditorSettings::Get().AssetSortMode * 2 + (EditorSettings::Get().AssetSortDesc ? 1 : 0);
}
void  TbSetAssetSort(int packed) {
    auto& s = EditorSettings::Get();
    s.AssetSortMode = (packed >> 1) & 3;
    s.AssetSortDesc = (packed & 1) != 0;
    EditorSettings::Save();
}
void  TbRefreshAssetBrowser() { if (g_Editor) g_Editor->RefreshAssetBrowser(); }
float TbGetAssetRefreshFlash() { return g_Editor ? g_Editor->AssetRefreshFlash() : 0.0f; }
bool  TbGetMeasureTool() { return g_Editor && g_Editor->MeasureToolActive(); }
void  TbSetMeasureTool(bool on) { if (g_Editor) g_Editor->SetMeasureToolActive(on); }
void  TbRequestDuplicateArray() { if (g_Editor) g_Editor->RequestArrayDuplicateModal(); }
bool  TbGetInspectorLocked() { return g_Editor && g_Editor->IsInspectorLocked(); }
void  TbToggleInspectorLock() { if (g_Editor) g_Editor->ToggleInspectorLock(); }
bool  TbGetAssetSearchGlobal() { return EditorSettings::Get().AssetSearchGlobal; }
void  TbSetAssetSearchGlobal(bool on) { EditorSettings::Get().AssetSearchGlobal = on; EditorSettings::Save(); }
bool  TbGetAssetFavoritesOnly() { return g_Editor && g_Editor->AssetFavoritesOnly(); }
void  TbSetAssetFavoritesOnly(bool on) { if (g_Editor) g_Editor->SetAssetFavoritesOnly(on); }

// --- History HUD, frame only (API v15) ---------------------------------------------------
// The Undo History HUD's window + pin/height math + eased tint live in EditorModuleHistory.cpp;
// the rows (undo/redo stacks + World&) stay host-side, exactly like the Hierarchy tree body.
bool  HistGetHudFrame(float* vx, float* vy, float* vw, float* vh, float* uiScale, int* rowCount) {
    if (!g_Editor) {
        if (vx) *vx = 0.0f; if (vy) *vy = 0.0f; if (vw) *vw = 0.0f; if (vh) *vh = 0.0f;
        if (uiScale) *uiScale = 1.0f; if (rowCount) *rowCount = 0;
        return false;
    }
    return g_Editor->HistoryHudFrame(vx, vy, vw, vh, uiScale, rowCount);
}
void  HistDrawListBody() {
    if (g_Editor && g_World && g_Assets) g_Editor->DrawHistoryListBody(*g_World, *g_Assets);
}

// --- Document strip (API v20, Phase 3 item 1) --------------------------------------------
void TbGetSceneDisplayName(char* out, int n) {
    std::string name = g_Editor ? fs::path(g_Editor->CurrentScenePath()).filename().string() : std::string();
    if (name.empty()) name = "Untitled";
    AbPutStr(out, n, name);
}
bool TbGetSceneDirty() { return g_Editor && g_Editor->IsDirty(); }
void TbDoSaveScene() { if (g_Editor && g_World && g_Assets) g_Editor->SaveScene(*g_World, *g_Assets); }

// --- Play controls, Zone B (API v21, Phase 3 item 2) -------------------------------------
void TbDrawPlayControlsBody() { if (g_Editor) g_Editor->DrawPlayControlsBody(); }

// --- Play-mode panel tint (API v22, Q12 / Phase 4 #6) -------------------------------------
bool TbGetInPlayMode() { return g_Editor && g_Editor->InPlayMode(); }

// --- Asset Browser folder history (API v23, Phase 5 item 3) ------------------------------
void AbFolderHistoryBack()          { if (g_Editor) g_Editor->AssetFolderHistoryBack(); }
void AbFolderHistoryForward()       { if (g_Editor) g_Editor->AssetFolderHistoryForward(); }
bool AbCanFolderHistoryBack()       { return g_Editor && g_Editor->CanAssetFolderHistoryBack(); }
bool AbCanFolderHistoryForward()    { return g_Editor && g_Editor->CanAssetFolderHistoryForward(); }

// --- Asset Browser view-mode toggle (API v24, Phase 5 item 3 remainder) ------------------
void AbToggleAssetViewMode()        { if (g_Editor) g_Editor->ToggleAssetViewMode(); }

// --- Hierarchy type filter + sort (API v25, Phase 5 item 6 remainder) --------------------
int  HierGetTypeFilter()            { return EditorSettings::Get().HierarchyTypeFilterMask; }
void HierSetTypeFilter(int mask)    { EditorSettings::Get().HierarchyTypeFilterMask = mask; EditorSettings::Save(); }
int  HierGetSort() {
    return EditorSettings::Get().HierarchySortMode * 2 + (EditorSettings::Get().HierarchySortDesc ? 1 : 0);
}
void HierSetSort(int packed) {
    auto& s = EditorSettings::Get();
    s.HierarchySortMode = (packed >> 1) & 3;
    s.HierarchySortDesc = (packed & 1) != 0;
    EditorSettings::Save();
}

// --- Asset Browser Details view (API v26, Phase 5 item 4) --------------------------------
int  AbGetViewMode()                { return g_Editor ? g_Editor->GetAssetViewMode() : 0; }

// Built field-by-field, by name (#172 follow-up): the old positional aggregate initializer
// compiled cleanly with any two same-signature entries swapped (e.g. GetShowGrid <->
// GetGridSnapEnabled) and silently cross-wired the toolbar. Assigning by name makes an
// out-of-order or misnamed entry impossible; a field nobody assigns stays nullptr, which
// every module call site already treats as "not provided", and CountUnassignedHostSlots
// reports it at startup.
EditorModuleHostAPI MakeHostAPI() {
    EditorModuleHostAPI api;
    api.Version = kEditorModuleAPIVersion;
    api.DrawStatusPanel = &DrawStatusPanel;
    api.GetImGuiContext = &GetImGuiContextPtr;
    api.GetImGuiAllocators = &GetImGuiAllocators;
    api.LogRevision = &LogRevisionFn;
    api.LogEntryCount = &LogEntryCountFn;
    api.LogGetEntry = &LogGetEntryFn;
    api.LogCountOf = &LogCountOfFn;
    api.LogClear = &LogClearFn;
    api.LogInfo = &LogInfoFn;
    api.LogError = &LogErrorFn;
    api.SelectEntityByOrder = &SelectEntityByOrderFn;
    api.PingAssetPath = &PingAssetPathFn;
    api.GetNotificationUnreadCount = &GetNotificationUnreadCountFn;
    api.MarkNotificationsRead = &MarkNotificationsReadFn;
    api.DrawNotificationsPopupBody = &DrawNotificationsPopupBodyFn;
    api.SetTooltip = &SetTooltipFn;
    api.SaveFileDialog = &SaveFileDialogFn;
    api.ConsoleState = &ConsoleStateFn;
    api.GetViewportRect = &StatsGetViewportRectFn;
    api.GetRenderStats = &StatsGetRenderStatsFn;
    api.GetProfilerSamples = &StatsGetProfilerSamplesFn;
    api.GetGLFrameStats = &StatsGetGLFrameStatsFn;
    api.GetSceneEntityCounts = &StatsGetSceneEntityCountsFn;
    api.GetSmoothedFrameMs = &StatsGetSmoothedFrameMsFn;
    api.GetFrameTimeHistory = &StatsGetFrameTimeHistoryFn;
    api.GetMonoFont = &GetMonoFontFn;
    api.SetHideEngineMark = &StatsSetHideEngineMarkFn;
    api.GetToolbarMetrics = &TbGetToolbarMetrics;
    api.SetTitleBarDragHovered = &TbSetTitleBarDragHovered;
    api.WindowMinimize = &TbWindowMinimize;
    api.WindowToggleMaximize = &TbWindowToggleMaximize;
    api.WindowClose = &TbWindowClose;
    api.WindowIsMaximized = &TbWindowIsMaximized;
    api.GetGizmoOp = &TbGetGizmoOp;
    api.SetGizmoOp = &TbSetGizmoOp;
    api.GetShadingMode = &TbGetShadingMode;
    api.SetShadingMode = &TbSetShadingMode;
    api.GetGizmoLocalSpace = &TbGetGizmoLocalSpace;
    api.SetGizmoLocalSpace = &TbSetGizmoLocalSpace;
    api.GetGizmoPivotCenter = &TbGetGizmoPivotCenter;
    api.SetGizmoPivotCenter = &TbSetGizmoPivotCenter;
    api.GetShowGrid = &TbGetShowGrid;
    api.SetShowGrid = &TbSetShowGrid;
    api.GetGridSnapEnabled = &TbGetGridSnapEnabled;
    api.SetGridSnapEnabled = &TbSetGridSnapEnabled;
    api.GetShowHistory = &TbGetShowHistory;
    api.SetShowHistory = &TbSetShowHistory;
    api.GetShowStats = &TbGetShowStats;
    api.SetShowStats = &TbSetShowStats;
    api.GetShowLightGizmos = &TbGetShowLightGizmos;
    api.SetShowLightGizmos = &TbSetShowLightGizmos;
    api.IsOrthographic = &TbIsOrthographic;
    api.ToggleOrthographic = &TbToggleOrthographic;
    api.ToolbarUndo = &TbUndo;
    api.ToolbarRedo = &TbRedo;
    api.CanSnapSelectionToGround = &TbCanSnapSelectionToGround;
    api.SnapSelectionToGround = &TbSnapSelectionToGround;
    api.RequestResetLayout = &TbRequestResetLayout;
    api.OpenPreferences = &TbOpenPreferences;
    api.OpenProjectSettings = &TbOpenProjectSettings;
    api.OpenShortcutsReference = &TbOpenShortcutsReference;
    api.LogGetEntryContext = &LogGetEntryContextFn;
    api.LogGetEntryStack = &LogGetEntryStackFn; // #178
    api.OpenAbout = &HelpOpenAbout;
    api.OpenDocumentation = &HelpOpenDocumentation;
    api.OpenLogFolder = &HelpOpenLogFolder;
    api.ReportBug = &HelpReportBug;
    api.DrawFileMenuBody = &TbDrawFileMenuBody;
    api.DrawAddEntityMenuItems = &TbDrawAddEntityMenuItems;
    api.DrawViewMenuBody = &TbDrawViewMenuBody;
    api.DrawWindowMenuBody = &TbDrawWindowMenuBody;
    api.DrawCaptureOptionsPopupBody = &TbDrawCaptureOptionsPopupBody;
    api.RequestCapture = &TbRequestCapture;
    api.GetCaptureButtonTooltip = &TbGetCaptureButtonTooltip;
    api.GetShowAssetBrowser = &AbGetShow;
    api.SetShowAssetBrowser = &AbSetShow;
    api.GetAssetSearch = &AbGetSearch;
    api.SetAssetSearch = &AbSetSearch;
    api.GetAssetLabelMenuFilter = &AbGetLabelMenuFilter;
    api.SetAssetLabelMenuFilter = &AbSetLabelMenuFilter;
    api.GetCurrentAssetFolder = &AbGetCurrentFolder;
    api.SetCurrentAssetFolder = &AbSetCurrentFolder;
    api.GetAssetTreeWidth = &AbGetTreeWidth;
    api.SetAssetTreeWidth = &AbSetTreeWidth;
    api.ConsumeAssetSearchFocus = &AbConsumeSearchFocus;
    api.SetAssetBrowserFocused = &AbSetFocused;
    api.IsAssetFolderExpanded = &AbIsFolderExpanded;
    api.SetAssetFolderExpanded = &AbSetFolderExpanded;
    api.AssetTreeFrameSetup = &AbTreeFrameSetup;
    api.GetFolderCount = &AbGetFolderCount;
    api.GetFolder = &AbGetFolder;
    api.GetKnownLabelCount = &AbGetKnownLabelCount;
    api.GetKnownLabel = &AbGetKnownLabel;
    api.CreateFolderUndoable = &AbCreateFolder;
    api.RenameFolderUndoable = &AbRenameFolder;
    api.MoveAssetToFolderUndoable = &AbMoveAsset;
    api.BeginRenameFolder = &AbBeginRenameFolder;
    api.ImportAssetViaDialog = &AbImportViaDialog;
    api.AssetGridFrameBegin = &AbGridFrameBegin;
    api.AssetGridCellCount = &AbGridCellCount;
    api.GetAssetGridMetrics = &AbGetGridMetrics;
    api.DrawAssetCell = &AbDrawCell;
    api.HandleAssetGridBackground = &AbHandleGridBackground;
    api.GetAssetSelectionSummary = &AbGetSelectionSummary;
    api.GetAssetIconSize = &AbGetIconSize;
    api.SetAssetIconSize = &AbSetIconSize;
    api.AssetGridFrameEnd = &AbGridFrameEnd;
    api.GetShowHierarchy = &HierGetShow;
    api.SetShowHierarchy = &HierSetShow;
    api.GetHierarchyFilter = &HierGetFilter;
    api.SetHierarchyFilter = &HierSetFilter;
    api.HierarchyExpandAll = &HierExpandAll;
    api.DrawHierarchyTreeBody = &HierDrawTreeBody;
    api.GetShowInspector = &InspGetShow;
    api.SetShowInspector = &InspSetShow;
    api.DrawInspectorBody = &InspDrawBody;
    api.DrawGridSnapPopupBody = &TbDrawGridSnapPopupBody;
    api.DrawGizmosPopupBody = &TbDrawGizmosPopupBody;
    api.GetGizmosMasterVisible = &TbGetGizmosMasterVisible;
    api.SetGizmosMasterVisible = &TbSetGizmosMasterVisible;
    api.GetHandTool = &TbGetHandTool;
    api.SetHandTool = &TbSetHandTool;
    api.GetLockViewToSelection = &TbGetLockViewToSelection;
    api.SetLockViewToSelection = &TbSetLockViewToSelection;
    api.GetAssetSort = &TbGetAssetSort;
    api.SetAssetSort = &TbSetAssetSort;
    api.RefreshAssetBrowser = &TbRefreshAssetBrowser;
    api.GetAssetRefreshFlash = &TbGetAssetRefreshFlash;
    api.GetMeasureTool = &TbGetMeasureTool;
    api.SetMeasureTool = &TbSetMeasureTool;
    api.RequestDuplicateArray = &TbRequestDuplicateArray;
    api.GetInspectorLocked = &TbGetInspectorLocked;
    api.ToggleInspectorLock = &TbToggleInspectorLock;
    api.GetAssetSearchGlobal = &TbGetAssetSearchGlobal;
    api.SetAssetSearchGlobal = &TbSetAssetSearchGlobal;
    api.GetAssetFavoritesOnly = &TbGetAssetFavoritesOnly;
    api.SetAssetFavoritesOnly = &TbSetAssetFavoritesOnly;
    api.GetHistoryHudFrame = &HistGetHudFrame;
    api.DrawHistoryListBody = &HistDrawListBody;
    api.RequestBakeReflectionProbes = +[]() { /* probe bake: main.cpp's probeArray.Update() already runs every frame */ };
    api.GetSceneDisplayName = &TbGetSceneDisplayName;
    api.GetSceneDirty = &TbGetSceneDirty;
    api.DoSaveScene = &TbDoSaveScene;
    api.DrawPlayControlsBody = &TbDrawPlayControlsBody;
    api.GetInPlayMode = &TbGetInPlayMode;
    api.AssetFolderHistoryBack = &AbFolderHistoryBack;
    api.AssetFolderHistoryForward = &AbFolderHistoryForward;
    api.CanAssetFolderHistoryBack = &AbCanFolderHistoryBack;
    api.CanAssetFolderHistoryForward = &AbCanFolderHistoryForward;
    api.ToggleAssetViewMode = &AbToggleAssetViewMode;
    api.GetHierarchyTypeFilter = &HierGetTypeFilter;
    api.SetHierarchyTypeFilter = &HierSetTypeFilter;
    api.GetHierarchySort = &HierGetSort;
    api.SetHierarchySort = &HierSetSort;
    api.GetAssetViewMode = &AbGetViewMode;
    return api;
}

const EditorModuleHostAPI kHostAPI = MakeHostAPI();

// Every EditorModuleHostAPI member after Version is a function pointer; count the ones
// MakeHostAPI left null so a field added to the struct but never wired up is reported at startup
// instead of silently disabling a panel feature.
int CountUnassignedHostSlots(const EditorModuleHostAPI& api) {
    static_assert(sizeof(void (*)()) == sizeof(void*), "function pointers are assumed pointer-sized");
    static_assert(offsetof(EditorModuleHostAPI, DrawStatusPanel) == sizeof(void*),
                  "Version is expected to be followed directly by the function-pointer slots");
    static_assert(sizeof(EditorModuleHostAPI) % sizeof(void*) == 0, "unexpected EditorModuleHostAPI layout");
    constexpr std::size_t slots = sizeof(EditorModuleHostAPI) / sizeof(void*) - 1;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&api) + sizeof(void*);
    int unassigned = 0;
    for (std::size_t i = 0; i < slots; ++i) {
        void* slot = nullptr;
        std::memcpy(&slot, bytes + i * sizeof(void*), sizeof(void*));
        if (!slot) ++unassigned;
    }
    return unassigned;
}

// The candidate / rollback validation: exported entry point, matching API version, a Draw.
const EditorModuleAPI* ResolveAPI(HMODULE module) {
    const auto getAPI = reinterpret_cast<GetEditorModuleAPIFn>(::GetProcAddress(module, "TartarusGetEditorModuleAPI"));
    const EditorModuleAPI* api = getAPI ? getAPI() : nullptr;
    return (api && api->Version == kEditorModuleAPIVersion && api->Draw) ? api : nullptr;
}

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
    // ARCH-204: two live instances would share (and fight over) g_Editor/g_World/g_Assets/g_Camera
    // — see the comment at their definition. Shutdown() above always clears g_InstanceLive first
    // for *this* instance's own re-Initialize, so this only trips for a genuinely second instance.
    if (g_InstanceLive) {
        Log::Error("Editor hot reload: a HotReloadEditorModule instance is already live; "
                   "refusing to Initialize a second one (would corrupt shared frame-context state).");
        return;
    }
    g_InstanceLive = true;
    m_OwnsInstanceGuard = true;
    g_ParentWindow = static_cast<GLFWwindow*>(parentWindow);
    m_SourceModule = sourceModule;
    m_PollElapsed = 0.0f;
    if (const int missing = CountUnassignedHostSlots(kHostAPI))
        Log::Warn("Editor hot reload: " + std::to_string(missing) +
                  " EditorModuleHostAPI callback(s) are not wired up in MakeHostAPI.");
    Reload(true);
}

void HotReloadEditorModule::SetFrameContext(EditorLayer* editor, World* world,
                                           AssetLibrary* assets, Camera* editorCamera) {
    g_Editor = editor;
    g_World = world;
    g_Assets = assets;
    g_Camera = editorCamera;
}

void HotReloadEditorModule::Draw(bool editorUIVisible, float deltaTime) {
    m_PollElapsed += deltaTime;
    if (m_PollElapsed >= 0.35f) {
        m_PollElapsed = 0.0f;
        std::error_code ec;
        const fs::file_time_type sourceWrite = fs::last_write_time(m_SourceModule, ec);
        if (!ec && sourceWrite != m_LastSourceWrite && sourceWrite != m_LastFailedSourceWrite)
            Reload(false);
    }

    if (editorUIVisible && m_API && m_API->Draw) {
        m_API->Draw(kHostAPI);
    } else if (!editorUIVisible && g_Editor) {
        // The module's Stats panel is what clears this each frame; with the module not drawing
        // (editor UI hidden), clear it here so the corner engine-mark isn't left suppressed by a
        // stale overflow decision when the panels come back.
        g_Editor->SetHideEngineMarkForStats(false);
    }
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

    // On startup, clear numbered copies left behind by an earlier crash or force-kill (a clean
    // Shutdown deletes its own). Anything still locked by another running instance just fails the
    // remove and is left alone.
    if (initialLoad) {
        std::error_code sweepEc;
        for (const auto& entry : fs::directory_iterator(cacheDir, sweepEc)) {
            const std::wstring name = entry.path().filename().wstring();
            if (name.rfind(L"TartarusEditor_", 0) == 0 && entry.path().extension() == L".dll") {
                std::error_code rmEc;
                fs::remove(entry.path(), rmEc);
            }
        }
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
        m_LastFailedSourceWrite = sourceWrite; // don't re-attempt this exact build every poll
        return false;
    }

    const EditorModuleAPI* candidateAPI = ResolveAPI(candidate);
    if (!candidateAPI) {
        Log::Error("Editor hot reload: TartarusEditor.dll has an incompatible module API.");
        ::FreeLibrary(candidate);
        fs::remove(copyPath, ec);
        m_LastFailedSourceWrite = sourceWrite; // don't re-attempt this exact build every poll
        return false;
    }

    // The candidate is validated; only now touch the live module. HotReloadSwap owns the order
    // (#172): old SaveState -> old OnUnload + free -> new OnLoad(state), so the old module can
    // never undo shared host state (ImGui context, callbacks) the new one has just set up, with a
    // rollback to the previous build if the new OnLoad rejects itself.
    HotReloadSwap::Slot<EditorModuleAPI> live{static_cast<HMODULE>(m_Handle), m_API, m_LoadedCopy};
    const HotReloadSwap::Result result = HotReloadSwap::Swap<EditorModuleAPI>(
        live, {candidate, candidateAPI, copyPath}, &ResolveAPI, "Editor hot reload");
    m_Handle = live.Handle;
    m_API = live.Api;
    m_LoadedCopy = live.Copy;

    if (result != HotReloadSwap::Result::Committed) {
        m_LastFailedSourceWrite = sourceWrite; // this build rejected itself; wait for the next one
        return false;
    }
    m_LastSourceWrite = sourceWrite;
    m_LastFailedSourceWrite = {};
    Log::Info(initialLoad ? "Editor hot reload: TartarusEditor module loaded."
                          : "Editor hot reload: TartarusEditor module reloaded.");
    return true;
}

void HotReloadEditorModule::Shutdown() {
    HotReloadSwap::Slot<EditorModuleAPI> live{static_cast<HMODULE>(m_Handle), m_API, m_LoadedCopy};
    HotReloadSwap::Unload(live);
    m_Handle = nullptr;
    m_API = nullptr;
    m_LoadedCopy.clear();

    // ARCH-204: release the single-instance guard and null the shared frame-context pointers —
    // but only if this instance actually holds the guard (a second instance's Initialize() bailed
    // out before ever setting m_OwnsInstanceGuard, so its Shutdown() must not rip the guard/context
    // out from under the first, still-live instance).
    if (m_OwnsInstanceGuard) {
        g_Editor = nullptr;
        g_World = nullptr;
        g_Assets = nullptr;
        g_Camera = nullptr;
        g_ParentWindow = nullptr;
        g_InstanceLive = false;
        m_OwnsInstanceGuard = false;
    }
}
