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

#include <cstdio>
#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <windows.h>

namespace fs = std::filesystem;

namespace {

// The window native dialogs opened on the module's behalf are parented to. File-scope because the
// host API is a table of plain function pointers with no user-data slot.
GLFWwindow* g_ParentWindow = nullptr;

// The live editor + world + assets + editor camera the Stats-panel callbacks (API v3) and the
// toolbar's menu bodies / commands (API v4) read from, refreshed every frame by
// HotReloadEditorModule::SetFrameContext(). Same file-scope rationale as g_ParentWindow: the host
// API is a flat function-pointer table with nowhere to hang a context pointer.
EditorLayer* g_Editor = nullptr;
World* g_World = nullptr;
AssetLibrary* g_Assets = nullptr;
Camera* g_Camera = nullptr;

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

void StatsSetHideEngineMarkFn(bool hide) {
    if (g_Editor) g_Editor->SetHideEngineMarkForStats(hide);
}

float StatsSampleViewportLuminanceFn(float screenCenterX, float screenCenterY, float boxPx) {
    return g_Editor ? g_Editor->SampleStatsHudLuminance(screenCenterX, screenCenterY, boxPx) : -1.0f;
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

int  TbGetEditorTheme() { return EditorSettings::Get().EditorTheme; }
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
    &StatsGetViewportRectFn,
    &StatsGetRenderStatsFn,
    &StatsGetProfilerSamplesFn,
    &StatsGetGLFrameStatsFn,
    &StatsGetSceneEntityCountsFn,
    &StatsGetSmoothedFrameMsFn,
    &StatsSetHideEngineMarkFn,
    &StatsSampleViewportLuminanceFn,
    // --- Toolbar / menus (API v4) — order must match EditorModuleHostAPI exactly ---
    &TbGetToolbarMetrics,
    &TbGetEditorTheme,
    &TbSetTitleBarDragHovered,
    &TbWindowMinimize,
    &TbWindowToggleMaximize,
    &TbWindowClose,
    &TbWindowIsMaximized,
    &TbGetGizmoOp,            &TbSetGizmoOp,
    &TbGetShadingMode,        &TbSetShadingMode,
    &TbGetGizmoLocalSpace,    &TbSetGizmoLocalSpace,
    &TbGetGizmoPivotCenter,   &TbSetGizmoPivotCenter,
    &TbGetShowGrid,           &TbSetShowGrid,
    &TbGetGridSnapEnabled,    &TbSetGridSnapEnabled,
    &TbGetShowHistory,        &TbSetShowHistory,
    &TbGetShowStats,          &TbSetShowStats,
    &TbGetShowLightGizmos,    &TbSetShowLightGizmos,
    &TbIsOrthographic,        &TbToggleOrthographic,
    &TbUndo,
    &TbRedo,
    &TbCanSnapSelectionToGround,
    &TbSnapSelectionToGround,
    &TbRequestResetLayout,
    &TbOpenPreferences,
    &TbDrawFileMenuBody,
    &TbDrawAddEntityMenuItems,
    &TbDrawViewMenuBody,
    &TbDrawWindowMenuBody,
    &TbDrawCaptureOptionsPopupBody,
    &TbRequestCapture,
    &TbGetCaptureButtonTooltip,
    // --- Asset Browser, thin slice (API v5) — order must match EditorModuleHostAPI exactly ---
    &AbGetShow,                 &AbSetShow,
    &AbGetSearch,               &AbSetSearch,
    &AbGetLabelMenuFilter,      &AbSetLabelMenuFilter,
    &AbGetCurrentFolder,
    &AbSetCurrentFolder,
    &AbGetTreeWidth,
    &AbSetTreeWidth,
    &AbConsumeSearchFocus,
    &AbSetFocused,
    &AbIsFolderExpanded,
    &AbSetFolderExpanded,
    &AbTreeFrameSetup,
    &AbGetFolderCount,
    &AbGetFolder,
    &AbGetKnownLabelCount,
    &AbGetKnownLabel,
    &AbCreateFolder,
    &AbRenameFolder,
    &AbMoveAsset,
    &AbBeginRenameFolder,
    &AbImportViaDialog,
    // --- Asset grid layout slice (API v6) — order must match EditorModuleHostAPI exactly ---
    &AbGridFrameBegin,
    &AbGridCellCount,
    &AbGetGridMetrics,
    &AbDrawCell,
    &AbHandleGridBackground,
    &AbGetSelectionSummary,
    &AbGetIconSize,
    &AbSetIconSize,
    &AbGridFrameEnd,
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

    const auto getAPI = reinterpret_cast<GetEditorModuleAPIFn>(::GetProcAddress(candidate, "TartarusGetEditorModuleAPI"));
    const EditorModuleAPI* candidateAPI = getAPI ? getAPI() : nullptr;
    if (!candidateAPI || candidateAPI->Version != kEditorModuleAPIVersion || !candidateAPI->Draw) {
        Log::Error("Editor hot reload: TartarusEditor.dll has an incompatible module API.");
        ::FreeLibrary(candidate);
        fs::remove(copyPath, ec);
        m_LastFailedSourceWrite = sourceWrite; // don't re-attempt this exact build every poll
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
    m_LastFailedSourceWrite = {};
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
