// --editor-tests: end-to-end UI tests of the Editor Enhancers (vInspector, vTabs, vFavorites,
// vRuler) in the running editor. See EditorUiTests.h for the model; the checklist these automate
// is docs/VINSPECTOR_TEST_SHEET.md.
//
// How widgets are found: the host executable is built with IMGUI_ENABLE_TEST_ENGINE, so ImGui
// reports every submitted item (id, rectangle, label, item flags) to the hooks below while a test
// runs. Items drawn by the hot-reloadable editor module (toolbar, folder tree) are compiled
// without the hooks and can't be addressed; everything the Enhancers draw is host-side.

#include "EditorUiTests.h"
#include "MotionDemo.h"

#include "EditorLayer.h"
#include "EditorPanels.h"
#include "EditorSettings.h"
#include "EditorTestProbe.h"
#include "Shortcuts.h"
#include "World.h"
#include "VolumetricFog.h"
#include "AssetLibrary.h"
#include "Camera.h"
#include "Log.h"
#include "Components.h"
#include "ComponentRegistry.h"
#include "SceneSerializer.h"
#include "ProjectPaths.h"
#include "UserPaths.h"
#include "ScriptIDE.h"      // CancelPrompts - a test left "Unsaved script" up
#include "EditorTheme.h"
#include "Enhancers/EnhancerCore.h"
#include "Enhancers/EnhancerUserState.h"
#include "Enhancers/TabState.h"
#include "EditorTestHooks.h"
#include "EditorModuleAPI.h"       // EditorConsoleState
#include "HotReloadEditorModule.h" // EditorModuleHost::ConsoleState()
#include "GLDebug.h"
#include "Input.h" // Input::SimulateKeyTap - main-loop keys (F1..F4) for the PlayMode suite
#include "AtomicFile.h" // NoteSelfWrite - scratch files a suite writes are not external changes
#include "Log.h"
#include "gl.h"

#include <GLFW/glfw3.h>
#include <stb_image_write.h>

#include <imgui.h>
#include <IconsFontAwesome6.h>
#include <imgui_internal.h>
#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// =============================================================================================
// Item registry (ImGui test-engine hooks)
// =============================================================================================
namespace {
struct UiItem {
    ImGuiID Id = 0;
    ImRect Rect, Clip;
    std::string Label, Tag, Window, Root;
    ImGuiItemFlags ItemFlags = 0;
    ImGuiItemStatusFlags Status = 0;
};
struct Registry {
    std::vector<UiItem> Building, Last;
    std::unordered_map<ImGuiID, size_t> Index;
    UiItem& Get(ImGuiID id) {
        auto it = Index.find(id);
        if (it != Index.end()) return Building[it->second];
        Index.emplace(id, Building.size());
        Building.emplace_back();
        Building.back().Id = id;
        return Building.back();
    }
    void NextFrame() {
        Last = std::move(Building);
        Building.clear();
        Index.clear();
    }
};
Registry g_Reg;
} // namespace

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data) {
    if (id == 0 || !ctx->CurrentWindow) return;
    UiItem& it = g_Reg.Get(id);
    it.Rect = bb;
    it.Clip = ctx->CurrentWindow->ClipRect;
    it.Window = ctx->CurrentWindow->Name;
    it.Root = ctx->CurrentWindow->RootWindow ? ctx->CurrentWindow->RootWindow->Name : it.Window;
    if (data) { it.ItemFlags = data->ItemFlags; it.Status = data->StatusFlags; }
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    if (id == 0) return;
    UiItem& it = g_Reg.Get(id);
    if (label) it.Label = label;
    it.Status |= flags;
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
void EditorTestProbe_Tag(ImGuiID id, const char* tag) {
    if (id == 0 || !tag) return;
    g_Reg.Get(id).Tag = tag;
}
namespace {
// Handed to TartarusEditor.dll through ImGuiContext::TestEngine (EditorTestHooks.h), so items the
// module draws land in the same registry.
const EditorTestHookTable g_HookTable{&ImGuiTestEngineHook_ItemAdd, &ImGuiTestEngineHook_ItemInfo};
} // namespace

// =============================================================================================
// Access to the editor's internals (EditorLayer befriends this)
// =============================================================================================
struct EditorUiTestAccess {
    static entt::entity Selected(EditorLayer& e) { return e.m_Selected; }
    static std::vector<entt::entity> Selection(EditorLayer& e) { return e.GetSelectedItems(); }
    static std::string& AssetKey(EditorLayer& e) { return e.m_SelectedAssetKey; }
    static bool& AssetIsFolder(EditorLayer& e) { return e.m_SelectedAssetIsFolder; }
    static void Select(EditorLayer& e, entt::entity x) { e.ClearAssetSelection(); e.SelectItem(x, false); }
    static void SelectAsset(EditorLayer& e, const std::string& key) {
        e.ClearSelection(); e.ClearAssetSelection(); e.m_SelectedAssetKey = key; e.m_SelectedAssetIsFolder = false;
    }
    static void ResetScene(EditorLayer& e, World& w, const std::string& scenePath) {
        if (e.m_InPlayMode) return;
        w = World();
        e.ClearSelection();
        e.ClearAssetSelection();
        e.ClearUndoHistory();
        e.m_CurrentScenePath = scenePath;
        e.m_SelHistory.clear();
        e.m_SelHistoryPos = 0;
        e.m_SelSnapshotLast.clear();
        e.m_SelAssetLast.clear();
        e.m_SelScenePathLast.clear();
        e.m_SelEntryLast = {};
        e.m_Pins.clear();
        e.m_InspectorPickedComponents.clear();
        e.m_ComponentClipboard.clear();
        e.m_InspectorLocked = false;
        e.m_FavLocked = false;
        e.m_FavPage = 0;
        e.m_Dirty = false;
    }
    // Floating tool windows a failed test can leave up; in a narrow window they cover the panels.
    // Every dialog a test can leave up. The editor's confirmations (BeginCenteredModal) reopen
    // themselves each frame while their flag is set, so closing ImGui's popups alone brings them
    // straight back: the flags and the requests waiting to open one go too.
    static void CloseDialogs(EditorLayer& e) {
        e.m_RecoveryPromptPending = false;              // Recover Unsaved Changes?
        e.m_SceneVersionWarning.clear();                // Newer Scene Format
        e.m_ExitPromptPending = false;                  // Save changes? (quit)
        e.m_ExitDecision = EditorLayer::ExitDecision::None;
        e.m_RevertPromptPending = false;                // Revert Scene?
        e.m_ScenePromptPending = false;                 // Save changes? (New / Open Scene)
        e.m_PendingSceneSwitch = EditorLayer::PendingSceneSwitch::None;
        e.m_PendingScenePath.clear();
        e.m_ShowSaveLayout = false;                     // Save Layout
        e.m_ShowArrayDuplicate = false;                 // Duplicate Array
        e.m_OpenDeleteConfirmRequested = false;         // Delete Asset?
        e.m_PendingDelete.clear();
        e.m_OpenDiskDeleteRequested = false;            // Delete from Disk?
        e.m_PendingDiskDelete.clear();
        e.m_PendingDiskDeleteRefs.clear();
        e.m_OpenDeleteErrorRequested = false;           // Can't Delete
        e.m_DeleteError.clear();
        e.m_OpenWeaponWizard = false;                   // New First-Person Weapon
        if (e.m_ScriptIDE) e.m_ScriptIDE->CancelPrompts(); // Unsaved script
        // Then ImGui's popup stack, modals included (Remove parameter?, Custom Resolution and the
        // rest open once and stay up only there).
        if (ImGuiContext* g = ImGui::GetCurrentContext(); g && g->OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(0, false);
    }
    static void CloseToolWindows(EditorLayer& e) {
        e.m_ShowAnimator = false;
        e.m_ShowAudioDebug = false;
        e.m_ShowLighting = false; // a Lighting test that failed midway left it over the Hierarchy
        EditorSettings::Get().ShowAssetLibrary = false;
    }
    static std::string& ScenePath(EditorLayer& e) { return e.m_CurrentScenePath; }
    static void Deselect(EditorLayer& e) { e.ClearSelection(); e.ClearAssetSelection(); }
    static size_t UndoDepth(EditorLayer& e) { return e.m_UndoStack.size(); }
    static std::string UndoTop(EditorLayer& e) { return e.m_UndoStack.empty() ? std::string("-") : e.m_UndoStack.back().Label; }
    static void PushUndo(EditorLayer& e, World& w, const char* label) { e.PushUndo(w, label); }
    static bool InPlay(EditorLayer& e) { return e.m_InPlayMode; }
    static void RequestPlayStop(EditorLayer& e) { e.m_PlayStopRequested = true; }
    static const std::vector<EditorLayer::PinnedComponent>& Pins(EditorLayer& e) { return e.m_Pins; }
    static const std::set<std::string>& Picked(EditorLayer& e) { return e.m_InspectorPickedComponents; }
    static const std::vector<std::string>& Clipboard(EditorLayer& e) { return e.m_ComponentClipboard; }
    static const std::vector<Enhancers::PlayKeep>& Keeps(EditorLayer& e) { return e.m_PlayKeep; }
    static ImGuiID Removing(EditorLayer& e) { return e.m_RemovingSection; }
    static std::string HoverSection(EditorLayer& e) { return e.m_HoverSection.Label; }
    static bool RulerHeld(EditorLayer& e) { return e.m_RulerHeld; }
    static entt::entity RulerHit(EditorLayer& e) { return e.m_RulerHitEntity; }
    static entt::entity RulerBounds(EditorLayer& e) { return e.m_RulerBounds; }
    static int RulerDepth(EditorLayer& e) { return e.m_RulerDepth; }
    static bool FavVisible(EditorLayer& e) { return e.m_FavVisible; }
    static bool& FavLocked(EditorLayer& e) { return e.m_FavLocked; }
    static int& FavPage(EditorLayer& e) { return e.m_FavPage; }
    static bool& InspectorLocked(EditorLayer& e) { return e.m_InspectorLocked; }
    static void Lock(EditorLayer& e) { e.m_InspectorLocked = false; e.ToggleInspectorLock(); }
    static std::string& CurrentFolder(EditorLayer& e) { return e.m_CurrentAssetFolder; }
    static void NavigateFolder(EditorLayer& e, const std::string& f) { e.NavigateAssetFolder(f); }
    static std::string SceneKey(EditorLayer& e) { return e.CurrentSceneKey(); }
    // The motion demo.
    static void Frame(EditorLayer& e, World& w, Camera& cam) { e.FocusOnSelection(w, cam); }
    static void Duplicate(EditorLayer& e, World& w, AssetLibrary& a) { e.DuplicateSelection(w, a, false); }
    static void RequestPause(EditorLayer& e) { e.m_PauseToggleRequested = true; }
};
using X = EditorUiTestAccess;

// =============================================================================================
// Test model
// =============================================================================================
namespace {
struct Ctx {
    EditorLayer& E;
    World& W;
    AssetLibrary& A;
    Camera& Cam;
};

using StepFn = std::function<bool(Ctx&)>; // true when the step is finished
struct Step {
    std::string Name;
    StepFn Fn;
    int Timeout = 240; // frames
};
struct Test {
    std::string Name;
    std::vector<Step> Steps;
    // Runs when the test ends, passed or failed: a failed test stops at its failing step, so
    // cleanup written as its last step (deleting a scratch folder) never ran.
    std::function<void(Ctx&)> Finally;
};

std::string g_Filter;
std::vector<std::string> g_Exclude; // lowercase name prefixes from TARTARUS_EDITOR_TEST_EXCLUDE
bool g_Enabled = false;
std::vector<Test> g_Tests;
size_t g_TestIndex = 0, g_StepIndex = 0;
int g_StepFrames = 0;
bool g_TestFailed = false;
std::string g_FailMessage;
int g_Passed = 0, g_Failed = 0, g_Skipped = 0;
std::vector<std::string> g_FailLines;
bool g_Started = false, g_Done = false;
// End of this frame's harness input events in ImGui's input queue. The queue's front holds events
// ImGui's trickling deferred from last frame (harness events too, e.g. a Ctrl release queued with
// a mouse-up), then this frame's harness events; everything after (the OS cursor and keys the
// backend still reports before ImGui::NewFrame) is dropped.
int g_QueueEnd = 0;
// The ids of the harness's own events still in the queue (ImGui trickles some to the next frame).
std::vector<ImU32> g_HarnessEventIds;
void KeepOnlyHarnessInput() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !g_Enabled) return;
    MotionDemo::PinImGuiDelta(); // a demo run: exactly 1/60 s per frame
    auto& q = ctx->InputEventsQueue;
    q.resize(std::min(g_QueueEnd, q.Size));
}
std::string g_ScratchScene;

void Fail(const std::string& msg) {
    if (!g_TestFailed) { g_TestFailed = true; g_FailMessage = msg; }
}

// --- Health monitor ---------------------------------------------------------------------------
// Checked every frame of every test, so a test fails on the first broken thing it touches even
// when its own expectations pass: a new error in the editor log, a GL debug error, an ImGui
// recoverable error (Begin/End mismatch, bad PushID...), a conflicting widget id, or a NaN in a
// transform. Warnings are recorded in the report without failing.
struct TestReport {
    std::string Name;
    bool Passed = false;
    std::string Failure;
    std::vector<std::string> Warnings, Errors, Shots;
    double Seconds = 0.0;
};
std::vector<TestReport> g_Reports;
TestReport g_Cur;
std::chrono::steady_clock::time_point g_TestStart;

// A wall-clock watchdog for the run. Steps time out in frames, so a main thread blocked inside a
// frame, or frames slowed to a crawl, never reach that check; on CI the step then rode its
// timeout and the job lost its whole log (the editor outlived the killed shell). The watchdog
// thread names the test and step whenever no frame has finished for a minute or one test has run
// for 3 minutes, and aborts the run (CrashHandler writes a minidump with every thread's stack)
// when frames have stopped for 4 minutes or the run passes TARTARUS_EDITOR_TEST_DEADLINE minutes.
namespace watchdog {
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess();
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void* process, unsigned int exitCode);
std::atomic<uint64_t> g_Frames{0};
std::mutex g_Mutex;
std::string g_Where = "startup"; // "<test> / step '<step>'", under g_Mutex
size_t g_WhereTest = SIZE_MAX, g_WhereStep = SIZE_MAX;

void Note(const std::string& test, const std::string& step, size_t testIndex, size_t stepIndex) {
    g_Frames.fetch_add(1, std::memory_order_relaxed);
    if (testIndex == g_WhereTest && stepIndex == g_WhereStep) return;
    g_WhereTest = testIndex;
    g_WhereStep = stepIndex;
    std::lock_guard<std::mutex> lk(g_Mutex);
    g_Where = test + " / step '" + step + "'";
}

void Start() {
    double deadlineMin = 0.0;
    {
        char* v = nullptr;
        size_t n = 0;
        if (_dupenv_s(&v, &n, "TARTARUS_EDITOR_TEST_DEADLINE") == 0 && v) { deadlineMin = std::atof(v); std::free(v); }
    }
    std::thread([deadlineMin] {
        using clock = std::chrono::steady_clock;
        const auto runStart = clock::now();
        auto lastFrameAt = runStart, whereSince = runStart;
        uint64_t lastFrames = 0;
        std::string lastWhere;
        bool slowNoted = false;
        auto doneAt = clock::time_point{};
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const auto now = clock::now();
            if (g_Done) {
                // The result is in. On CI the editor's teardown after it sometimes took 13 minutes or
                // never finished (the step then timed out): give it a minute, then end the process
                // with the tests' own exit code. TerminateProcess, not exit(): ExitProcess runs DLL
                // detach, which can block on the same lock the teardown is stuck behind.
                if (doneAt == clock::time_point{}) doneAt = now;
                else if (now - doneAt >= std::chrono::seconds(60)) {
                    std::printf("[EditorTest] WATCHDOG: the editor hasn't exited a minute after the summary - ending it\n");
                    std::fflush(stdout);
                    TerminateProcess(GetCurrentProcess(), g_Failed == 0 && g_Passed > 0 ? 0u : 1u);
                }
                continue;
            }
            const uint64_t frames = g_Frames.load(std::memory_order_relaxed);
            if (frames != lastFrames) { lastFrames = frames; lastFrameAt = now; }
            std::string where;
            {
                std::lock_guard<std::mutex> lk(g_Mutex);
                where = g_Where;
            }
            const std::string test = where.substr(0, where.find(" / step '"));
            if (test != lastWhere.substr(0, lastWhere.find(" / step '"))) { whereSince = now; slowNoted = false; }
            lastWhere = where;
            const auto secs = [&](clock::time_point t) { return (long long)std::chrono::duration_cast<std::chrono::seconds>(now - t).count(); };
            const long long stalled = secs(lastFrameAt), inTest = secs(whereSince), total = secs(runStart);
            if (stalled >= 60 && stalled % 60 < 5)
                std::printf("[EditorTest] WATCHDOG: no frame finished for %llds, in %s (frame %llu)\n", stalled, where.c_str(), (unsigned long long)frames);
            if (inTest >= 180 && !slowNoted) {
                slowNoted = true;
                std::printf("[EditorTest] WATCHDOG: %s has run for %llds (frame %llu)\n", where.c_str(), inTest, (unsigned long long)frames);
            }
            const bool hung = stalled >= 240, late = deadlineMin > 0.0 && total >= (long long)(deadlineMin * 60.0);
            if (hung || late) {
                std::printf("[EditorTest] WATCHDOG: %s - aborting in %s (frame %llu) for a crash dump\n",
                            hung ? "frames stopped for 4 minutes" : "past TARTARUS_EDITOR_TEST_DEADLINE", where.c_str(), (unsigned long long)frames);
                std::fflush(stdout);
                std::abort();
            }
            std::fflush(stdout);
        }
    }).detach();
}
} // namespace watchdog
int g_GlErrBase = 0;
unsigned long long g_LogSeqSeen = 0;
std::vector<std::string> g_AllowLog;   // this test: error lines containing one of these are expected
std::vector<std::string> g_ImGuiErrors; // filled by the ImGui error callback
std::string g_LastImGuiError;

void OnImGuiError(ImGuiContext*, void*, const char* msg) {
    const std::string m = msg ? msg : "(no message)";
    if (g_ImGuiErrors.size() < 50) g_ImGuiErrors.push_back(m);
}
bool Allowed(const std::string& line) {
    for (const std::string& a : g_AllowLog) if (line.find(a) != std::string::npos) return true;
    return false;
}
std::string Trim(std::string s, size_t n = 300) {
    if (s.size() > n) s = s.substr(0, n) + "...";
    for (char& ch : s) if (ch == '\n' || ch == '\r') ch = ' ';
    return s;
}
void ResetHealth() {
    g_GlErrBase = GLDebug::ErrorCount();
    for (const LogEntry& e : Log::Entries()) g_LogSeqSeen = std::max(g_LogSeqSeen, e.Seq);
    g_AllowLog.clear();
    g_ImGuiErrors.clear();
}
void CheckHealth(World& w) {
    for (const LogEntry& e : Log::Entries()) {
        if (e.Seq <= g_LogSeqSeen) continue;
        g_LogSeqSeen = std::max(g_LogSeqSeen, e.Seq);
        if (e.Level == LogLevel::Warning) {
            if (g_Cur.Warnings.size() < 40) g_Cur.Warnings.push_back(Trim(e.Message));
        } else if (e.Level == LogLevel::Error) {
            if (g_Cur.Errors.size() < 40) g_Cur.Errors.push_back(Trim(e.Message));
            if (!Allowed(e.Message)) Fail("editor logged an error: " + Trim(e.Message));
        }
    }
    if (GLDebug::IsEnabled() && GLDebug::ErrorCount() > g_GlErrBase) {
        g_GlErrBase = GLDebug::ErrorCount();
        Fail("GL debug error (see the logged GL[...] line)");
    }
    if (!g_ImGuiErrors.empty()) {
        Fail("ImGui error: " + Trim(g_ImGuiErrors.front()));
        g_ImGuiErrors.clear();
    }
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.DebugDrawIdConflictsId != 0) {
        std::string label = "?";
        for (const UiItem& it : g_Reg.Last) if (it.Id == g.DebugDrawIdConflictsId) { label = it.Label + " in " + it.Window; break; }
        Fail("conflicting ImGui ids (" + label + ")");
    }
    for (auto [e, t] : w.Registry.view<const TransformComponent>().each()) {
        const float v[10] = {t.Position.x, t.Position.y, t.Position.z, t.Rotation.w, t.Rotation.x,
                             t.Rotation.y, t.Rotation.z, t.Scale.x, t.Scale.y, t.Scale.z};
        for (const float x : v)
            if (!std::isfinite(x)) {
                Fail("non-finite transform on an object");
                return;
            }
    }
}

// --- Screenshots -------------------------------------------------------------------------------
// Shot("name") captures the whole editor window after the frame is drawn into
// <TARTARUS_EDITOR_TEST_SHOTS or the scratch folder>/editor-test-shots/<test>/<name>.png. The
// listed windows must not be blank (one flat colour) in it - a panel that drew nothing, a black
// viewport.
std::string g_ShotDir;
std::string g_ShotPending; // path without extension, set by a Shot step, taken by AfterFrame
std::vector<std::string> g_ShotNonBlank;
bool g_ShotDone = false;
bool g_ShotForLastReport = false; // the pending shot is a failed test's, already reported
std::string Slug(const std::string& s) {
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch)) out += (char)std::tolower(ch);
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.substr(0, 80);
}

// --- Item queries ------------------------------------------------------------------------------
std::string Visible(const std::string& label) {
    const size_t at = label.find("##");
    return at == std::string::npos ? label : label.substr(0, at);
}
bool Contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

struct Query {
    std::string Label;  // matched against the visible label (contains)
    std::string Exact;  // the whole raw label must equal this
    std::string Tag;    // EditorTestTag name (equal)
    std::string TagPrefix; // EditorTestTag name starts with this (text fields add "=<value>")
    std::string Root;   // the item's root window name contains this ("" = any)
    std::function<bool(const UiItem&)> Pred;
    int Nth = 0;        // which match (top-to-bottom, left-to-right)
    float AnchorX = -1.0f; // Click: where across the item (0 = left edge, 1 = right); < 0 = its centre
};
Query ByLabel(std::string label, std::string root = {}) { Query q; q.Label = std::move(label); q.Root = std::move(root); return q; }
Query ByExact(std::string label, std::string root = {}, int nth = 0) {
    Query q; q.Exact = std::move(label); q.Root = std::move(root); q.Nth = nth; return q;
}
// Hierarchy rows ("row:<name>") are clicked on their name side: at a narrow panel width the
// row's centre lands on its component icons, which take the click for themselves.
Query ByTag(std::string tag) {
    Query q;
    if (tag.rfind("row:", 0) == 0) q.AnchorX = 0.25f;
    q.Tag = std::move(tag);
    return q;
}
// The same item, clicked near its right end (a row's x button).
Query RightEnd(Query q) { q.AnchorX = 0.97f; return q; }
Query ByTagPrefix(std::string prefix) { Query q; q.TagPrefix = std::move(prefix); return q; }

// Some widgets never report their label to the hooks (the pinned ImGui's BeginCombo has no
// ITEM_INFO), so a combo can't be found by label. Such an unlabelled item still matches `label`
// when its ID is `label` hashed in its window's own ID scope (nothing extra pushed).
bool UnlabelledIdMatches(const UiItem& it, const std::string& label) {
    if (!it.Label.empty()) return false;
    const ImGuiWindow* w = ImGui::FindWindowByName(it.Window.c_str());
    return w && it.Id == ImHashStr(label.c_str(), 0, w->ID);
}
std::vector<const UiItem*> FindAll(const Query& q) {
    std::vector<const UiItem*> out;
    for (const UiItem& it : g_Reg.Last) {
        if (!q.Tag.empty() && it.Tag != q.Tag) continue;
        if (!q.TagPrefix.empty() && it.Tag.compare(0, q.TagPrefix.size(), q.TagPrefix) != 0) continue;
        if (!q.Exact.empty() && it.Label != q.Exact && !UnlabelledIdMatches(it, q.Exact)) continue;
        if (!q.Label.empty() && !Contains(Visible(it.Label), q.Label)) continue;
        if (!q.Root.empty() && !Contains(it.Root, q.Root)) continue;
        if (q.Pred && !q.Pred(it)) continue;
        if (it.Rect.GetWidth() <= 0.0f || it.Rect.GetHeight() <= 0.0f) continue;
        out.push_back(&it);
    }
    std::stable_sort(out.begin(), out.end(), [](const UiItem* a, const UiItem* b) {
        if (std::fabs(a->Rect.Min.y - b->Rect.Min.y) > 2.0f) return a->Rect.Min.y < b->Rect.Min.y;
        return a->Rect.Min.x < b->Rect.Min.x;
    });
    return out;
}
const UiItem* Find(const Query& q) {
    const auto all = FindAll(q);
    return q.Nth < (int)all.size() ? all[(size_t)q.Nth] : nullptr;
}
// The visible centre of an item (its rectangle clipped to its window); false when fully clipped.
bool ClickPoint(const UiItem& it, ImVec2& out) {
    ImRect r = it.Rect;
    r.ClipWithFull(it.Clip);
    if (r.GetWidth() < 1.0f || r.GetHeight() < 1.0f) return false;
    out = r.GetCenter();
    return true;
}

// --- Input -----------------------------------------------------------------------------------
void Mods(int mods, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    if (mods & ImGuiMod_Ctrl)  { io.AddKeyEvent(ImGuiKey_LeftCtrl, down);  io.AddKeyEvent(ImGuiMod_Ctrl, down); }
    if (mods & ImGuiMod_Shift) { io.AddKeyEvent(ImGuiKey_LeftShift, down); io.AddKeyEvent(ImGuiMod_Shift, down); }
    if (mods & ImGuiMod_Alt)   { io.AddKeyEvent(ImGuiKey_LeftAlt, down);   io.AddKeyEvent(ImGuiMod_Alt, down); }
}
void MouseTo(ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); }

// --- Step builders ----------------------------------------------------------------------------
Step Do(std::string name, std::function<void(Ctx&)> fn) {
    return {std::move(name), [fn](Ctx& c) { fn(c); return true; }};
}
// Expects the editor to log errors containing `fragment` for the rest of this test (a test that
// does something invalid on purpose, or an asset CI doesn't have).
Step AllowLogError(std::string fragment) {
    return Do("allow logged error '" + fragment + "'", [fragment](Ctx&) { g_AllowLog.push_back(fragment); });
}
// Captures the editor window once this frame is drawn (see g_ShotDir); `nonBlank` lists window
// names (substring, e.g. "###Scene") that must not be one flat colour in the capture.
Step Shot(std::string name, std::vector<std::string> nonBlank = {}) {
    auto phase = std::make_shared<int>(0);
    return {"shot " + name, [name, nonBlank, phase](Ctx&) {
        if (*phase == 0) {
            g_ShotPending = Slug(g_Cur.Name) + "/" + Slug(name);
            g_ShotNonBlank = nonBlank;
            g_ShotDone = false;
            *phase = 1;
            return false;
        }
        return g_ShotDone;
    }, 30};
}
Step Wait(int frames) {
    auto n = std::make_shared<int>(0);
    return {"wait " + std::to_string(frames), [n, frames](Ctx&) { return ++*n >= frames; }, frames + 5};
}
// Real time, not frames: a hidden test window runs far faster than 60 Hz, so fades and hold
// delays need seconds.
Step WaitSeconds(double seconds) {
    auto until = std::make_shared<double>(-1.0);
    return {"wait " + std::to_string(seconds) + "s", [until, seconds](Ctx&) {
        const double now = ImGui::GetTime();
        if (*until < 0.0) *until = now + seconds;
        return now >= *until;
    }, 100000};
}
Step Until(std::string name, std::function<bool(Ctx&)> pred, int timeout = 120) {
    return {std::move(name), std::move(pred), timeout};
}
// Fails the test (with `what` as the reason) when `check` returns a non-empty message.
Step Expect(std::string name, std::function<std::string(Ctx&)> check) {
    return {name, [name, check](Ctx& c) {
        const std::string why = check(c);
        if (!why.empty()) Fail(name + ": " + why);
        return true;
    }};
}
Step Print(std::string what) {
    return Do("print", [what](Ctx&) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        std::printf("    [%s] mouse=(%.0f,%.0f) down0=%d hovered='%s' hoveredId=%08X activeId=%08X wantCapture=%d\n", what.c_str(),
                    g.IO.MousePos.x, g.IO.MousePos.y, (int)g.IO.MouseDown[0], g.HoveredWindow ? g.HoveredWindow->Name : "-",
                    g.HoveredId, g.ActiveId, (int)g.IO.WantCaptureMouse);
    });
}
Step ExpectTrue(std::string name, std::function<bool(Ctx&)> pred) {
    return Expect(name, [pred](Ctx& c) { return pred(c) ? std::string() : std::string("false"); });
}
Step Move(std::string name, std::function<ImVec2(Ctx&)> where) {
    return {std::move(name), [where](Ctx& c) { MouseTo(where(c)); return true; }};
}
Step MoveToItem(Query q) {
    return {"move to " + (q.Tag.empty() ? (q.Exact.empty() ? q.Label : q.Exact) : q.Tag), [q](Ctx&) {
        const UiItem* it = Find(q);
        ImVec2 p;
        if (!it || !ClickPoint(*it, p)) return false;
        MouseTo(p);
        return true;
    }, 90};
}
// An entry of a popup, menu or combo list. ImGui places those windows over their first frames
// (auto-fit, moved back inside the editor, flipped when they don't fit below, shrunk as a search
// narrows them), so an entry's rect can still move after it is first seen.
bool InPopupWindow(const UiItem& it) {
    return Contains(it.Root, "##Popup") || Contains(it.Root, "##Menu") || Contains(it.Root, "##Combo");
}
// Moves onto the item, presses and releases `button` with `mods` held. Waits up to the timeout for
// the item to appear (a popup opening, a section expanding). A popup / menu / combo entry is
// clicked only once its rect has held still for two frames: a click on where it was a frame
// earlier lands outside the popup, which then closes with nothing chosen.
Step Click(Query q, int mods = 0, int button = 0, int clicks = 1) {
    auto phase = std::make_shared<int>(0);
    auto last = std::make_shared<ImRect>();
    auto still = std::make_shared<int>(0);
    const std::string desc = "click " + (q.Tag.empty() ? (q.Exact.empty() ? q.Label : q.Exact) : q.Tag);
    return {desc, [q, mods, button, clicks, phase, last, still](Ctx&) {
        ImGuiIO& io = ImGui::GetIO();
        switch (*phase) {
            case 0: {
                const UiItem* it = Find(q);
                ImVec2 p;
                if (!it || !ClickPoint(*it, p)) { *still = 0; return false; }
                if (InPopupWindow(*it)) {
                    *still = (it->Rect.Min.x == last->Min.x && it->Rect.Min.y == last->Min.y) ? *still + 1 : 0;
                    *last = it->Rect;
                    if (*still < 2) return false;
                }
                if (q.AnchorX >= 0.0f) p.x = it->Rect.Min.x + it->Rect.GetWidth() * q.AnchorX;
                MouseTo(p);
                Mods(mods, true);
                *phase = 1;
                return false;
            }
            default: {
                const int k = *phase - 1;            // 0,1 = press,release of click 1; 2,3 of click 2...
                io.AddMouseButtonEvent(button, (k % 2) == 0);
                ++*phase;
                if (k + 1 >= clicks * 2) { Mods(mods, false); return true; }
                return false;
            }
        }
    }, 120};
}
Step RightClick(Query q) { return Click(std::move(q), 0, 1); }
Step DoubleClick(Query q) { return Click(std::move(q), 0, 0, 2); }
Step Key(ImGuiKey key, int mods = 0) {
    auto phase = std::make_shared<int>(0);
    return {std::string("key ") + ImGui::GetKeyName(key), [key, mods, phase](Ctx&) {
        ImGuiIO& io = ImGui::GetIO();
        if (*phase == 0) { Mods(mods, true); ++*phase; return false; }
        if (*phase == 1) { io.AddKeyEvent(key, true); ++*phase; return false; }
        io.AddKeyEvent(key, false);
        Mods(mods, false);
        return true;
    }};
}
Step KeyDown(ImGuiKey key, int mods = 0) {
    return {std::string("hold ") + ImGui::GetKeyName(key), [key, mods](Ctx&) {
        Mods(mods, true);
        if (key != ImGuiKey_None) ImGui::GetIO().AddKeyEvent(key, true);
        return true;
    }};
}
Step KeyUp(ImGuiKey key, int mods = 0) {
    return {std::string("release ") + ImGui::GetKeyName(key), [key, mods](Ctx&) {
        if (key != ImGuiKey_None) ImGui::GetIO().AddKeyEvent(key, false);
        Mods(mods, false);
        return true;
    }};
}
Step MouseButton(int button, bool down) {
    return {std::string(down ? "press" : "release") + " mouse " + std::to_string(button), [button, down](Ctx&) {
        ImGui::GetIO().AddMouseButtonEvent(button, down);
        return true;
    }};
}
Step Wheel(float dy, float dx = 0.0f) {
    return {"wheel", [dy, dx](Ctx&) { ImGui::GetIO().AddMouseWheelEvent(dx, dy); return true; }};
}
// Press on `from`, move in a few steps to `to` (computed when reached, so it may be an item that
// only appears during the drag), release there.
Step Drag(Query from, std::function<bool(Ctx&, ImVec2&)> to, int mods = 0) {
    auto phase = std::make_shared<int>(0);
    auto start = std::make_shared<ImVec2>();
    return {"drag " + (from.Tag.empty() ? from.Label : from.Tag), [from, to, mods, phase, start](Ctx& c) {
        ImGuiIO& io = ImGui::GetIO();
        switch (*phase) {
            case 0: {
                const UiItem* it = Find(from);
                if (!it || !ClickPoint(*it, *start)) return false;
                MouseTo(*start); Mods(mods, true); ++*phase; return false;
            }
            case 1: io.AddMouseButtonEvent(0, true); ++*phase; return false;
            case 2: MouseTo(ImVec2(start->x + 12.0f, start->y + 6.0f)); ++*phase; return false;
            case 3: MouseTo(ImVec2(start->x + 30.0f, start->y + 14.0f)); ++*phase; return false;
            case 4: {
                ImVec2 p;
                if (!to(c, p)) return false; // the target may only appear once the drag started
                MouseTo(p); ++*phase; return false;
            }
            case 5: case 6: ++*phase; return false; // hover a couple of frames
            default:
                io.AddMouseButtonEvent(0, false); Mods(mods, false);
                return true;
        }
    }, 180};
}
std::function<bool(Ctx&, ImVec2&)> AtItem(Query q, ImVec2 offset = ImVec2(0, 0)) {
    return [q, offset](Ctx&, ImVec2& out) {
        const UiItem* it = Find(q);
        if (!it || !ClickPoint(*it, out)) return false;
        out.x += offset.x; out.y += offset.y;
        return true;
    };
}

// --- Scene fixture ----------------------------------------------------------------------------
entt::entity ByName(World& w, const char* name) {
    for (auto [e, n] : w.Registry.view<const NameComponent>().each()) if (n.Name == name) return e;
    return entt::null;
}
std::string NameOf(World& w, entt::entity e) {
    if (e == entt::null || !w.Registry.valid(e)) return "(none)";
    const auto* n = w.Registry.try_get<NameComponent>(e);
    return n ? n->Name : "(unnamed)";
}
// A fresh scene: Alpha (empty), Beta (Light + Audio Source), Gamma (Light).
Step FreshScene() {
    return Do("fresh scene", [](Ctx& c) {
        if (X::InPlay(c.E)) X::RequestPlayStop(c.E);
        ImGui::ClosePopupsExceptModals(); // a menu or popover a previous test left open
        // A test that failed midway can leave a field mid-edit, keyboard focus in a panel, the
        // Settings window up or a Console filter typed: any of those swallows the next test's
        // keys and clicks, so one failure cascaded through the rest of a suite.
        ImGui::ClearActiveID();
        ImGui::GetCurrentContext()->InputTextDeactivatedState.ClearFreeMemory();
        ImGui::SetWindowFocus(nullptr);
        c.E.CloseSettingsWindow();
        X::CloseToolWindows(c.E);
        EditorModuleHost::ConsoleState().Filter[0] = 0;
        X::ResetScene(c.E, c.W, g_ScratchScene);
        Enhancers::EnhancerUserState::Get().Reset();
        Enhancers::TabState::Get().Reset();
        EditorSettings::Get().InspectorAnimations = true;
        EditorSettings::Get().InspectorMinimal = false;
        const glm::vec3 zero(0.0f), one(1.0f);
        c.W.CreateEmptyEntity(glm::vec3(-3, 0, 0), zero, one, "Alpha");
        const entt::entity beta = c.W.CreateEmptyEntity(glm::vec3(0, 0, 0), zero, one, "Beta");
        c.W.Registry.emplace<LightComponent>(beta).Intensity = 9.0f;
        c.W.Registry.emplace<AudioSourceComponent>(beta).Volume = 0.3f;
        const entt::entity gamma = c.W.CreateEmptyEntity(glm::vec3(3, 0, 0), zero, one, "Gamma");
        c.W.Registry.emplace<LightComponent>(gamma).Intensity = 1.0f;
    });
}
Step SelectByName(const char* name) {
    return Do(std::string("select ") + name, [name](Ctx& c) {
        const entt::entity e = ByName(c.W, name);
        if (e == entt::null) Fail(std::string("no object ") + name);
        else X::Select(c.E, e);
    });
}
std::string ExpectSelected(Ctx& c, const char* name) {
    const std::string now = NameOf(c.W, X::Selected(c.E));
    return now == name ? std::string() : "selected " + now + ", expected " + name;
}

const char* kInspector = "###Inspector";
const char* kHierarchy = "###Hierarchy";
const char* kAssets = "###Assets";
const char* kScene = "###Scene";

// The centre of a docked panel's window, or false if it isn't shown.
bool PanelCentre(const char* name, ImVec2& out) {
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        if (w->WasActive && !w->Hidden && Contains(w->Name, name) && !Contains(w->Name, "/")) {
            out = w->InnerRect.GetCenter();
            return true;
        }
    return false;
}
Step HoverPanel(const char* name) {
    return {std::string("hover ") + name, [name](Ctx&) {
        ImVec2 p;
        if (!PanelCentre(name, p)) return false;
        MouseTo(p);
        return true;
    }};
}
// The header item (InvisibleButton "<icon>  <Component>") of a component section.
Query Header(const char* component) {
    Query q;
    q.Root = kInspector;
    const std::string suffix = std::string("  ") + component;
    q.Pred = [suffix](const UiItem& it) {
        return it.Label.size() >= suffix.size() && it.Label.compare(it.Label.size() - suffix.size(), suffix.size(), suffix) == 0 &&
               it.Rect.GetWidth() > 120.0f;
    };
    return q;
}
// The Particle System's curves header: "Lifetime & Emission Curves", or "Curves" in a narrow Inspector.
Query ParticleCurvesHeader() {
    Query q;
    q.Root = kInspector;
    q.Pred = [](const UiItem& it) {
        const std::string id = "###ParticleCurves";
        return it.Label.size() > id.size() && it.Label.compare(it.Label.size() - id.size(), id.size(), id) == 0;
    };
    return q;
}
// Clicks `q` unless `shown` is already on screen - e.g. a header an earlier test left open.
Step ClickUnlessShown(Query q, Query shown) {
    Step click = Click(std::move(q));
    auto skip = std::make_shared<int>(-1);
    click.Name += " (unless shown)";
    click.Fn = [inner = click.Fn, shown, skip](Ctx& c) {
        if (*skip < 0) *skip = Find(shown) ? 1 : 0;
        return *skip == 1 || inner(c);
    };
    return click;
}
// The "..." button on a component's header row.
Query MoreButton(const char* component) {
    Query q;
    q.Root = kInspector;
    const std::string suffix = std::string("  ") + component;
    q.Pred = [suffix](const UiItem& it) {
        if (Visible(it.Label) != ICON_FA_ELLIPSIS_VERTICAL) return false;
        for (const UiItem& h : g_Reg.Last)
            if (h.Label.size() >= suffix.size() && h.Label.compare(h.Label.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                h.Rect.GetWidth() > 120.0f && it.Rect.GetCenter().y > h.Rect.Min.y && it.Rect.GetCenter().y < h.Rect.Max.y)
                return true;
        return false;
    };
    return q;
}
Query PopupItem(const char* label) { return ByLabel(label, "##Popup"); }
Query MenuItem(const char* label) {
    Query q = ByLabel(label);
    q.Pred = [](const UiItem& it) { return Contains(it.Root, "##Popup") || Contains(it.Root, "##Menu"); };
    return q;
}
// A section's open state (ImGui storage of the window that drew its header).
int SectionOpen(const char* component) {
    const UiItem* h = Find(Header(component));
    if (!h) return -1;
    ImGuiWindow* w = ImGui::FindWindowByName(h->Window.c_str());
    return w ? w->StateStorage.GetInt(h->Id, 0) : -1;
}

// Scrolls the Inspector (or whatever scrolls the item) until `q` is fully on screen. A tall section
// (an open Light) pushes the ones below it out of a 1080p Inspector at 150% DPI.
Step InspReveal(Query q) {
    auto tries = std::make_shared<int>(0);
    return {"reveal", [q, tries](Ctx&) {
        const UiItem* it = Find(q);
        if (!it) {
            // A clipped widget is registered without its label (ImGui returns before reporting
            // it), so a label query can't see it: page the panel down until it shows up, from the
            // top again once at the bottom (an earlier test can leave the panel scrolled past it).
            if (++*tries > 30) return false;
            for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
                if (w->WasActive && !w->Hidden && w->ScrollMax.y > 0.0f && Contains(w->Name, q.Root.empty() ? kInspector : q.Root)) {
                    ImGui::SetScrollY(w, w->Scroll.y >= w->ScrollMax.y ? 0.0f
                                                                       : std::min(w->Scroll.y + w->InnerRect.GetHeight() * 0.5f, w->ScrollMax.y));
                    break;
                }
            return false;
        }
        if (it->Rect.Min.y >= it->Clip.Min.y && it->Rect.Max.y <= it->Clip.Max.y) return true;
        if (++*tries > 30) return true; // can't scroll further: let the click report it
        ImGuiWindow* w = ImGui::FindWindowByName(it->Window.c_str());
        while (w && w->ScrollMax.y <= 0.0f) w = w->ParentWindow;
        if (w) ImGui::SetScrollFromPosY(w, it->Rect.GetCenter().y - w->Pos.y, 0.5f);
        return false;
    }, 120};
}

// The object actions "..." in the Inspector's header card.
Query ObjectMenu() {
    Query q;
    q.Root = kInspector;
    q.Pred = [](const UiItem& it) { return Visible(it.Label) == ICON_FA_ELLIPSIS_VERTICAL && Contains(it.Window, "entityHeaderCard"); };
    return q;
}
// The "+" on the dictionary editor's "New key" row.
Query PlusBesideNewKey() {
    Query q;
    q.Root = kInspector;
    q.Pred = [](const UiItem& it) {
        if (it.Label != ICON_FA_PLUS) return false;
        for (const UiItem& k : g_Reg.Last)
            if (k.Label == "##newkey" && it.Rect.GetCenter().y > k.Rect.Min.y && it.Rect.GetCenter().y < k.Rect.Max.y) return true;
        return false;
    };
    return q;
}
// Types into the focused text field.
Step TypeText(std::string text) {
    return {"type " + text, [text](Ctx&) { ImGui::GetIO().AddInputCharactersUTF8(text.c_str()); return true; }};
}
// Scratch values a test carries between its steps.
double g_Number = 0.0;
std::string g_Text;
glm::vec3 g_Vec{0.0f};

void Add(std::string name, std::vector<Step> steps, std::function<void(Ctx&)> finally = {}) {
    g_Tests.push_back({std::move(name), std::move(steps), std::move(finally)});
}

// =============================================================================================
// The Glass motion demo (MotionDemo.h): a scripted tour of the editor's motion, recorded at 60 fps.
// Only registered when TARTARUS_MOTION_DEMO / TARTARUS_FFMPEG ask for it.
// =============================================================================================
// Moves the pointer to `to` over `frames` frames on a smooth curve, as a hand would.
Step Glide(std::function<bool(Ctx&, ImVec2&)> to, int frames = 26) {
    struct G { int f = -1; ImVec2 a, b; };
    auto g = std::make_shared<G>();
    return {"glide", [to, frames, g](Ctx& c) {
        if (g->f < 0) {
            if (!to(c, g->b)) return false;
            const ImVec2 cur = ImGui::GetIO().MousePos;
            g->a = ImGui::IsMousePosValid(&cur) ? cur : g->b;
            g->f = 0;
        }
        ++g->f;
        const float t = std::min(1.0f, (float)g->f / (float)frames);
        const float e = t * t * (3.0f - 2.0f * t);
        MouseTo(ImVec2(g->a.x + (g->b.x - g->a.x) * e, g->a.y + (g->b.y - g->a.y) * e));
        return g->f >= frames;
    }, frames + 240};
}
std::function<bool(Ctx&, ImVec2&)> AtPoint(std::function<ImVec2(Ctx&)> p) {
    return [p](Ctx& c, ImVec2& out) { out = p(c); return true; };
}
// A press and release where the pointer is, a few frames apart so the press reads on video.
std::vector<Step> ClickHere(int holdFrames = 6) {
    return {MouseButton(0, true), Wait(holdFrames), MouseButton(0, false)};
}
// A docked panel's tab in the dock space (its label is the window's name).
Query DockTab(const char* windowId) {
    Query q;
    const std::string id = windowId;
    q.Pred = [id](const UiItem& it) { return Contains(it.Label, id) && it.Rect.GetHeight() < 60.0f && it.Rect.GetWidth() < 400.0f; };
    return q;
}
std::vector<Step> Cat(std::initializer_list<std::vector<Step>> parts) {
    std::vector<Step> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

void RegisterMotionDemo() {
    Add("motion demo", Cat({
        {
            FreshScene(),
            Do("demo scene", [](Ctx& c) {
                c.W.CreateModelEntity(c.A.CreatePrimitive("cube"), glm::vec3(-2.5f, 0, 0), glm::vec3(0, 25, 0), glm::vec3(1), "Near Cube");
                c.W.CreateModelEntity(c.A.CreatePrimitive("cube"), glm::vec3(3, 0.5f, -6), glm::vec3(0, 40, 0), glm::vec3(2), "Far Cube");
                c.W.CreateModelEntity(c.A.CreatePrimitive("sphere"), glm::vec3(0.5f, 0, -2), glm::vec3(0), glm::vec3(1.2f), "Sphere");
                c.W.CreateModelEntity(c.A.CreatePrimitive("plane"), glm::vec3(0, -1.0f, -3), glm::vec3(0), glm::vec3(24, 1, 24), "Ground");
                c.Cam.Position = glm::vec3(0.0f, 2.0f, 8.0f);
                c.Cam.Yaw = -90.0f;
                c.Cam.Pitch = -10.0f;
                c.Cam.Orthographic = false;
                c.E.RequestEditorTheme(1); // Black Glass
            }),
            WaitSeconds(2.0), // the theme and its glass settle
            Move("start at the viewport", [](Ctx& c) {
                const glm::vec2 p = c.E.ViewportPos(), s = c.E.ViewportSize();
                return ImVec2(p.x + s.x * 0.55f, p.y + s.y * 0.55f);
            }),
            WaitSeconds(0.2),
            Do("record", [](Ctx&) {
                const ImVec2 d = ImGui::GetIO().DisplaySize;
                MotionDemo::StartRecording((int)d.x, (int)d.y);
            }),
            WaitSeconds(0.8),
            // 1. Hover: rows light as the pointer passes (velocity-gated), then select one.
            Glide(AtItem(ByTag("row:Alpha")), 34), WaitSeconds(0.35),
            Glide(AtItem(ByTag("row:Beta")), 16), WaitSeconds(0.35),
            Glide(AtItem(ByTag("row:Gamma")), 16), WaitSeconds(0.35),
            Glide(AtItem(ByTag("row:Beta")), 16),
        },
        ClickHere(),
        {
            WaitSeconds(0.9),
            // 2. A component foldout: the chevron turns and the body reveals.
            Glide(AtItem(Header("Light")), 30), WaitSeconds(0.3),
        },
        ClickHere(), {WaitSeconds(0.9)}, ClickHere(),
        {
            WaitSeconds(1.0),
            // 3. Tools: the rail's marker glides from tool to tool.
            Do("rotate", [](Ctx& c) { c.E.SetGizmoOpIndex(1); }), WaitSeconds(0.6),
            Do("scale", [](Ctx& c) { c.E.SetGizmoOpIndex(2); }), WaitSeconds(0.6),
            Do("transform", [](Ctx& c) { c.E.SetGizmoOpIndex(4); }), WaitSeconds(0.6),
            Do("move", [](Ctx& c) { c.E.SetGizmoOpIndex(0); }), WaitSeconds(0.8),
            // 4. Frame selection (F): the camera glides on the UI's curve.
            SelectByName("Far Cube"), Do("frame", [](Ctx& c) { X::Frame(c.E, c.W, c.Cam); }), WaitSeconds(1.2),
            SelectByName("Near Cube"), Do("frame", [](Ctx& c) { X::Frame(c.E, c.W, c.Cam); }), WaitSeconds(1.2),
            // 5. Dock tabs: the overline glides, the focused panel's edge glints.
            Glide(AtItem(DockTab("###Game")), 30), WaitSeconds(0.2),
        },
        ClickHere(), {WaitSeconds(0.9), Glide(AtItem(DockTab("###Scene")), 22), WaitSeconds(0.2)}, ClickHere(),
        {
            WaitSeconds(0.9),
            // 6. Drag a row: the ghost trails, the reorder marker glides slot to slot, and on the
            // drop it hops in and the moved row flashes.
            Glide(AtItem(ByTag("row:Gamma")), 30), WaitSeconds(0.2),
            MouseButton(0, true), WaitSeconds(0.1),
            Glide(AtItem(ByTag("row:Gamma"), ImVec2(14.0f, -6.0f)), 8),
            Glide(AtItem(ByTag("row:Beta"), ImVec2(0.0f, 9.0f)), 26), WaitSeconds(0.4),
            Glide(AtItem(ByTag("row:Alpha"), ImVec2(0.0f, 9.0f)), 20), WaitSeconds(0.4),
            Glide(AtItem(ByTag("row:Alpha")), 14), WaitSeconds(0.5),
            MouseButton(0, false), WaitSeconds(1.2),
            // ...and a drag let go over nothing drifts home.
            Glide(AtItem(ByTag("row:Sphere")), 26), WaitSeconds(0.2),
            MouseButton(0, true), WaitSeconds(0.1),
            Glide(AtItem(ByTag("row:Sphere"), ImVec2(14.0f, -6.0f)), 8),
            Glide(AtPoint([](Ctx& c) {
                const glm::vec2 p = c.E.ViewportPos(), s = c.E.ViewportSize();
                return ImVec2(p.x + s.x * 0.12f, p.y + s.y * 0.08f); // the viewport's top-left, under the rail: no target
            }), 40), WaitSeconds(0.3),
            MouseButton(0, false), WaitSeconds(1.0),
            // 7. Duplicate: the copy's row flashes once.
            SelectByName("Beta"), WaitSeconds(0.3),
            Do("duplicate", [](Ctx& c) { X::Duplicate(c.E, c.W, c.A); }), WaitSeconds(1.2),
            // 8. Save: the unsaved dot becomes a check that draws on and fades.
            Key(ImGuiKey_S, ImGuiMod_Ctrl), WaitSeconds(1.6),
            // 9. Play: triangle morphs to square, the play frame breathes, dims on pause.
            Glide(AtItem(ByExact("##transport")), 34), WaitSeconds(0.2),
        },
        ClickHere(), {WaitSeconds(2.6), Glide(AtItem(ByLabel("Pause")), 22), WaitSeconds(0.2)}, ClickHere(),
        {WaitSeconds(1.4), Glide(AtItem(ByLabel("Resume")), 10), WaitSeconds(0.2)}, ClickHere(),
        {WaitSeconds(1.2), Glide(AtItem(ByExact("##transport")), 22), WaitSeconds(0.2)}, ClickHere(),
        {
            WaitSeconds(1.2),
            // 10. A module reload that landed: a toast whose check draws on.
            Do("module reloaded", [](Ctx&) { Log::Info("Hot reload: TartarusGame module reloaded."); }), WaitSeconds(2.2),
            // 11. Preferences: the checkbox tick draws on and erases; the About title staggers in.
            Do("preferences", [](Ctx& c) { c.E.OpenPreferences(); }), WaitSeconds(0.9),
            Glide(AtItem([] { Query q; q.Pred = [](const UiItem& it) { return (it.Status & ImGuiItemStatusFlags_Checkable) != 0 && Contains(it.Root, "Settings"); }; return q; }()), 30),
            WaitSeconds(0.3),
        },
        ClickHere(), {WaitSeconds(0.9)}, ClickHere(),
        {
            WaitSeconds(0.9),
            Do("about", [](Ctx& c) { c.E.OpenAbout(); }), WaitSeconds(2.0),
            Do("close", [](Ctx& c) { c.E.CloseSettingsWindow(); }), WaitSeconds(1.0),
            Do("stop", [](Ctx&) { MotionDemo::StopRecording(); }),
        },
    }));
}

// =============================================================================================
// The tests
// =============================================================================================
// The editor smoke suites: every panel, menu, tool and action of the editor (see
// src/Tests/EditorSmoke/README.md). Included here, inside this file's anonymous namespace, so they
// share the harness above; each defines Register<Suite>Tests().
#include "EditorSmoke/Menus.inl"
#include "EditorSmoke/Panels.inl"
#include "EditorSmoke/Hierarchy.inl"
#include "EditorSmoke/Inspector.inl"
#include "EditorSmoke/Scenes.inl" // before SaveLoad, which uses smoke_scenes::MissingExternalFiles
#include "EditorSmoke/SceneTools.inl"
#include "EditorSmoke/PlayMode.inl"
#include "EditorSmoke/Prefabs.inl"
#include "EditorSmoke/SaveLoad.inl"
#include "EditorSmoke/Assets.inl"
#include "EditorSmoke/Animator.inl"
#include "EditorSmoke/Curves.inl"
#include "EditorSmoke/Particles.inl"
#include "EditorSmoke/ScriptIDE.inl"
#include "EditorSmoke/HotReload.inl"
#include "EditorSmoke/Settings.inl"
#include "EditorSmoke/Decals.inl"

void RegisterTests() {
    if (MotionDemo::Active()) { RegisterMotionDemo(); return; } // a demo run is only the demo
    // ------------------------------------------------------------------ 1. Selection history
    Add("history: back and forward through objects and an asset", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(2),
        SelectByName("Beta"), Wait(2),
        Do("select an asset", [](Ctx& c) { X::SelectAsset(c.E, "assets/AI/npc_soldier.controller"); }), Wait(2),
        Key(ImGuiKey_LeftBracket, ImGuiMod_Ctrl), Wait(2),
        Expect("back to Beta", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
        Key(ImGuiKey_LeftBracket, ImGuiMod_Ctrl), Wait(2),
        Expect("back to Alpha", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
        Key(ImGuiKey_RightBracket, ImGuiMod_Ctrl), Wait(2),
        Key(ImGuiKey_RightBracket, ImGuiMod_Ctrl), Wait(2),
        Expect("forward to the asset", [](Ctx& c) {
            if (X::Selected(c.E) != entt::null) return "an object is selected: " + NameOf(c.W, X::Selected(c.E));
            return X::AssetKey(c.E) == "assets/AI/npc_soldier.controller" ? std::string() : "asset key is '" + X::AssetKey(c.E) + "'";
        }),
    });
    Add("history: survives undo (entities rebuilt by the reload)", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(2),
        SelectByName("Beta"), Wait(2),
        Do("rename Gamma with undo", [](Ctx& c) {
            X::PushUndo(c.E, c.W, "Rename");
            c.W.Registry.get<NameComponent>(ByName(c.W, "Gamma")).Name = "Gamma2";
        }), Wait(2),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo applied", [](Ctx& c) { return ByName(c.W, "Gamma") != entt::null; }),
        Do("select Beta again", [](Ctx& c) { X::Select(c.E, ByName(c.W, "Beta")); }), Wait(2),
        Key(ImGuiKey_LeftBracket, ImGuiMod_Ctrl), Wait(2),
        Expect("back lands on Alpha", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
    });
    Add("history: survives Play / Stop", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(2),
        SelectByName("Beta"), Wait(2),
        Do("play", [](Ctx& c) { X::RequestPlayStop(c.E); }),
        Until("in play", [](Ctx& c) { return X::InPlay(c.E); }), Wait(5),
        Do("stop", [](Ctx& c) { X::RequestPlayStop(c.E); }),
        Until("stopped", [](Ctx& c) { return !X::InPlay(c.E); }), Wait(3),
        Expect("Beta still selected", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
        Key(ImGuiKey_LeftBracket, ImGuiMod_Ctrl), Wait(2),
        Expect("back lands on Alpha", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
    });
    Add("history: skips deleted objects and other scenes", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(2),
        SelectByName("Beta"), Wait(2),
        SelectByName("Gamma"), Wait(2),
        Do("delete Beta", [](Ctx& c) { c.W.Registry.destroy(ByName(c.W, "Beta")); }), Wait(2),
        Key(ImGuiKey_LeftBracket, ImGuiMod_Ctrl), Wait(2),
        Expect("Beta skipped", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
        Do("switch to another scene key", [](Ctx& c) { X::ScenePath(c.E) = g_ScratchScene + ".other.json"; }), Wait(2),
        Key(ImGuiKey_RightBracket, ImGuiMod_Ctrl), Wait(2),
        Expect("nothing from the old scene", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
        Do("restore the scene key", [](Ctx& c) { X::ScenePath(c.E) = g_ScratchScene; }),
    });

    Add("history: Back reaches the empty selection after a scene switch", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(2),
        Do("deselect", [](Ctx& c) { X::Deselect(c.E); }), Wait(2),
        Do("switch to another scene key", [](Ctx& c) { X::ScenePath(c.E) = g_ScratchScene + ".other.json"; }), Wait(2),
        SelectByName("Beta"), Wait(2),
        Key(ImGuiKey_LeftBracket, ImGuiMod_Ctrl), Wait(2),
        ExpectTrue("back to nothing selected", [](Ctx& c) { return X::Selection(c.E).empty(); }),
        Key(ImGuiKey_RightBracket, ImGuiMod_Ctrl), Wait(2),
        Expect("forward to Beta", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
        Do("restore the scene key", [](Ctx& c) { X::ScenePath(c.E) = g_ScratchScene; }),
    });

    // ------------------------------------------------------------------ fonts
    // Inter and JetBrains Mono carry their own private-use glyphs from U+E000; every icon a style can
    // name must still be drawn by the merged Font Awesome source (index 1 in both faces).
    Add("fonts: every Font Awesome icon draws from the icon font", {
        ExpectTrue("no icon shadowed by the text faces", [](Ctx&) {
            std::size_t n = 0;
            const Enhancers::FAIcon* icons = Enhancers::FAIconTable(&n);
            int bad = 0;
            for (ImFont* font : {EditorTheme::UiFont(), EditorTheme::MonoFont()}) {
                ImFontBaked* baked = font->GetFontBaked(EditorTheme::BodySize());
                for (std::size_t i = 0; i < n; ++i) {
                    unsigned int c = 0;
                    ImTextCharFromUtf8(&c, icons[i].Glyph, nullptr);
                    if (c < 0xE000 || c > 0xF8FF) continue; // digits / letters are text glyphs on purpose
                    const ImFontGlyph* g = baked->FindGlyphNoFallback((ImWchar)c);
                    if (!g || g->SourceIdx != 1) {
                        if (bad++ < 5) std::printf("    icon '%s' (U+%04X) source %d\n", icons[i].Name, c, g ? (int)g->SourceIdx : -1);
                    }
                }
            }
            return bad == 0;
        }),
    });

    // ------------------------------------------------------------------ 2. Inspector nav bar
    // Bookmarks live behind one button per header (Bookmark chips off, the default): it opens a
    // list whose first row bookmarks / un-bookmarks what the Inspector shows.
    Add("nav bar: bookmark the object, the list selects", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(3),
        Click(ByLabel(ICON_FA_BOOKMARK, kInspector)), Wait(3),
        Click(ByTag("bm:toggle")), Wait(3),
        ExpectTrue("Alpha bookmarked", [](Ctx&) {
            const auto& bm = Enhancers::EnhancerUserState::Get().InspectorBookmarks;
            return bm.size() == 1 && bm[0].Label == "Alpha";
        }),
        SelectByName("Gamma"), Wait(3),
        Click(ByLabel(ICON_FA_BOOKMARK, kInspector)), Wait(3),
        Click(ByTag("bm:Alpha")), Wait(3),
        Expect("list selected Alpha", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
        SelectByName("Gamma"), Wait(3),
        Click(ByLabel(ICON_FA_BOOKMARK, kInspector)), Wait(3),
        Click(ByTag("bm:Alpha"), ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("Ctrl+click adds", [](Ctx& c) { return X::Selection(c.E).size() == 2; }),
        Key(ImGuiKey_Escape), Wait(2),
        SelectByName("Alpha"), Wait(3),
        Click(ByLabel(ICON_FA_BOOKMARK, kInspector)), Wait(3),
        Click(ByTag("bm:toggle")), Wait(3),
        ExpectTrue("bookmark toggled off", [](Ctx&) { return Enhancers::EnhancerUserState::Get().InspectorBookmarks.empty(); }),
    });
    Add("nav bar: asset bookmark", {
        FreshScene(), Wait(2),
        Do("select an asset", [](Ctx& c) { X::SelectAsset(c.E, "assets/AI/npc_soldier.controller"); }), Wait(3),
        Click(ByLabel(ICON_FA_BOOKMARK, kInspector)), Wait(3),
        Click(ByTag("bm:toggle")), Wait(3),
        ExpectTrue("asset bookmarked", [](Ctx&) {
            const auto& bm = Enhancers::EnhancerUserState::Get().InspectorBookmarks;
            return bm.size() == 1 && bm[0].Kind == Enhancers::RefKind::Asset;
        }),
        SelectByName("Beta"), Wait(3),
        Click(ByLabel(ICON_FA_BOOKMARK, kInspector)), Wait(3),
        Click(ByTag("bm:npc_soldier.controller")), Wait(3),
        Expect("asset shown", [](Ctx& c) {
            if (X::Selected(c.E) != entt::null) return std::string("an object is still selected");
            return X::AssetKey(c.E) == "assets/AI/npc_soldier.controller" ? std::string() : "asset is " + X::AssetKey(c.E);
        }),
    });
    Add("nav bar: drop a Hierarchy row to bookmark it", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(3),
        Drag(ByTag("row:Gamma"), AtItem(ByLabel(ICON_FA_BOOKMARK, kInspector))), Wait(2),
        ExpectTrue("Gamma bookmarked by drop", [](Ctx&) {
            for (const auto& r : Enhancers::EnhancerUserState::Get().InspectorBookmarks) if (r.Label == "Gamma") return true;
            return false;
        }),
    });
    Add("nav bar: Hierarchy bookmarks, remove from the list", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        Click(ByLabel(ICON_FA_BOOKMARK, kHierarchy)), Wait(3),
        Click(ByTag("bm:toggle")), Wait(3),
        ExpectTrue("Beta bookmarked", [](Ctx&) {
            const auto& bm = Enhancers::EnhancerUserState::Get().EntityBookmarks;
            return bm.size() == 1 && bm[0].Label == "Beta";
        }),
        SelectByName("Alpha"), Wait(3),
        Click(ByLabel(ICON_FA_BOOKMARK, kHierarchy)), Wait(3),
        Click(ByTag("bm:Beta")), Wait(3),
        Expect("list selected Beta", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
        Click(ByLabel(ICON_FA_BOOKMARK, kHierarchy)), Wait(3),
        Click(RightEnd(ByTag("bm:Beta"))), Wait(3),
        ExpectTrue("removed with the x", [](Ctx&) { return Enhancers::EnhancerUserState::Get().EntityBookmarks.empty(); }),
    });

    // ------------------------------------------------------------------ 3/4. Component sections
    Add("sections: Open in Window from the actions menu", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        Click(MoreButton("Light")), Wait(2),
        Click(MenuItem("Open in Window")), Wait(2),
        ExpectTrue("Light pinned", [](Ctx& c) {
            const int order = c.W.Registry.get<OrderComponent>(ByName(c.W, "Beta")).Value;
            for (const auto& p : X::Pins(c.E)) if (p.Order == order && p.Component == "Light") return true;
            return false;
        }),
    });
    Add("sections: Alt+drag a header out opens a window, without toggling it", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        Drag(Header("Audio Source"), [](Ctx&, ImVec2& out) {
            ImVec2 p;
            if (!PanelCentre(kScene, p)) return false;
            out = p;
            return true;
        }, ImGuiMod_Alt), Wait(3),
        ExpectTrue("Audio Source pinned", [](Ctx& c) {
            for (const auto& p : X::Pins(c.E)) if (p.Component == "Audio Source") return true;
            return false;
        }),
        ExpectTrue("section still closed", [](Ctx&) { return SectionOpen("Audio Source") == 0; }),
    });
    Add("sections: Ctrl+click picks, Copy Selected, Paste as New, undo, Paste Values", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        Click(Header("Light"), ImGuiMod_Ctrl), Wait(2),
        Click(Header("Audio Source"), ImGuiMod_Ctrl), Wait(2),
        ExpectTrue("two picked", [](Ctx& c) { return X::Picked(c.E).size() == 2; }),
        ExpectTrue("Ctrl+click didn't open Light", [](Ctx&) { return SectionOpen("Light") == 0; }),
        Click(MoreButton("Light")), Wait(2),
        Click(MenuItem("Copy 2 Selected Components")), Wait(2),
        ExpectTrue("clipboard holds two", [](Ctx& c) { return X::Clipboard(c.E).size() == 2; }),
        SelectByName("Alpha"), Wait(3),
        Click(ObjectMenu()), Wait(2),
        Click(MenuItem("Paste Components as New (2)")), Wait(3),
        Expect("Alpha got both", [](Ctx& c) {
            const entt::entity a = ByName(c.W, "Alpha");
            const auto* l = c.W.Registry.try_get<LightComponent>(a);
            const auto* s = c.W.Registry.try_get<AudioSourceComponent>(a);
            if (!l || !s) return std::string("missing a component");
            return l->Intensity == 9.0f && s->Volume == 0.3f ? std::string() : std::string("values not copied");
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("one undo removes both", [](Ctx& c) {
            const entt::entity a = ByName(c.W, "Alpha");
            return !c.W.Registry.all_of<LightComponent>(a) && !c.W.Registry.all_of<AudioSourceComponent>(a);
        }),
        SelectByName("Gamma"), Wait(3),
        Click(ObjectMenu()), Wait(2),
        Click(MenuItem("Paste Component Values (1)")), Wait(3),
        ExpectTrue("Gamma's Light overwritten, nothing added", [](Ctx& c) {
            const entt::entity g = ByName(c.W, "Gamma");
            return c.W.Registry.get<LightComponent>(g).Intensity == 9.0f && !c.W.Registry.all_of<AudioSourceComponent>(g);
        }),
    });
    Add("sections: hover keys isolate, collapse / expand all, remove (X) with undo", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        MoveToItem(Header("Light")), Wait(2),
        Key(ImGuiKey_E, ImGuiMod_Shift), WaitSeconds(0.3),
        Expect("isolated Light", [](Ctx&) {
            const int l = SectionOpen("Light"), a = SectionOpen("Audio Source"), t = SectionOpen("Transform");
            return l == 1 && a == 0 && t == 0 ? std::string()
                : "Light=" + std::to_string(l) + " Audio=" + std::to_string(a) + " Transform=" + std::to_string(t);
        }),
        MoveToItem(Header("Light")), Wait(2),
        Key(ImGuiKey_E, ImGuiMod_Ctrl | ImGuiMod_Shift), WaitSeconds(0.3),
        ExpectTrue("all collapsed", [](Ctx&) { return SectionOpen("Light") == 0 && SectionOpen("Transform") == 0; }),
        Key(ImGuiKey_E, ImGuiMod_Ctrl | ImGuiMod_Shift), WaitSeconds(0.3),
        InspReveal(Header("Audio Source")), // below the expanded Light
        ExpectTrue("all expanded", [](Ctx&) { return SectionOpen("Light") == 1 && SectionOpen("Audio Source") == 1; }),
        Key(ImGuiKey_E, ImGuiMod_Ctrl | ImGuiMod_Shift), WaitSeconds(0.3), // collapse again so the header stays put
        MoveToItem(Header("Audio Source")), Wait(2),
        Key(ImGuiKey_X), WaitSeconds(0.4),
        ExpectTrue("Audio Source removed after the fade", [](Ctx& c) { return !c.W.Registry.all_of<AudioSourceComponent>(ByName(c.W, "Beta")); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo restores it", [](Ctx& c) { return c.W.Registry.all_of<AudioSourceComponent>(ByName(c.W, "Beta")); }),
    });
    Add("sections: A toggles the Enabled field with undo; disabled header", {
        FreshScene(),
        Do("add an Audio Listener", [](Ctx& c) {
            X::PushUndo(c.E, c.W, "Add Audio Listener");
            c.W.Registry.emplace<AudioListenerComponent>(ByName(c.W, "Beta"));
        }), Wait(2),
        SelectByName("Beta"), Wait(3),
        InspReveal(Header("Audio Listener")), MoveToItem(Header("Audio Listener")), Wait(2),
        Key(ImGuiKey_A), Wait(3),
        ExpectTrue("Enabled off", [](Ctx& c) { return !c.W.Registry.get<AudioListenerComponent>(ByName(c.W, "Beta")).Enabled; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo turns it back on", [](Ctx& c) { return c.W.Registry.get<AudioListenerComponent>(ByName(c.W, "Beta")).Enabled; }),
    });
    Add("sections: animations off remove at once; minimal mode hides the actions button", {
        FreshScene(),
        Do("animations off", [](Ctx&) { EditorSettings::Get().InspectorAnimations = false; }), Wait(2),
        SelectByName("Beta"), Wait(3),
        InspReveal(Header("Audio Source")), MoveToItem(Header("Audio Source")), Wait(2),
        Key(ImGuiKey_X), Wait(2),
        ExpectTrue("removed the same frame", [](Ctx& c) { return !c.W.Registry.all_of<AudioSourceComponent>(ByName(c.W, "Beta")); }),
        Do("minimal mode on", [](Ctx&) { EditorSettings::Get().InspectorMinimal = true; }),
        HoverPanel(kHierarchy), Wait(3),
        ExpectTrue("no Light actions button while not hovered", [](Ctx&) { return Find(MoreButton("Light")) == nullptr; }),
        InspReveal(Header("Light")), MoveToItem(Header("Light")), Wait(3),
        ExpectTrue("actions button on hover", [](Ctx&) { return Find(MoreButton("Light")) != nullptr; }),
        Do("settings back", [](Ctx&) { EditorSettings::Get().InspectorMinimal = false; EditorSettings::Get().InspectorAnimations = true; }),
    });

    // ------------------------------------------------------------------ 5. Keep changes after Play
    Add("play: Keep Changes After Play (Light and Transform), one undo", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        Do("play", [](Ctx& c) { X::RequestPlayStop(c.E); }),
        Until("in play", [](Ctx& c) { return X::InPlay(c.E); }), Wait(5),
        Do("change values in Play", [](Ctx& c) {
            const entt::entity b = ByName(c.W, "Beta");
            c.W.Registry.get<LightComponent>(b).Intensity = 42.0f;
            c.W.Registry.get<TransformComponent>(b).Position = glm::vec3(5.0f, 0.0f, 0.0f);
        }), Wait(2),
        Click(MoreButton("Light")), Wait(2),
        Click(MenuItem("Keep Changes After Play")), Wait(2),
        Click(MoreButton("Transform")), Wait(2),
        Click(MenuItem("Keep Changes After Play")), Wait(2),
        ExpectTrue("two keeps", [](Ctx& c) { return X::Keeps(c.E).size() == 2; }),
        Do("stop", [](Ctx& c) { X::RequestPlayStop(c.E); }),
        Until("stopped", [](Ctx& c) { return !X::InPlay(c.E); }), Wait(3),
        Expect("kept", [](Ctx& c) {
            const entt::entity b = ByName(c.W, "Beta");
            const float i = c.W.Registry.get<LightComponent>(b).Intensity;
            const float x = c.W.Registry.get<TransformComponent>(b).Position.x;
            return i == 42.0f && x == 5.0f ? std::string() : "intensity " + std::to_string(i) + ", x " + std::to_string(x);
        }),
        ExpectTrue("choices reset", [](Ctx& c) { return X::Keeps(c.E).empty(); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("one undo reverts both", [](Ctx& c) {
            const entt::entity b = ByName(c.W, "Beta");
            return c.W.Registry.get<LightComponent>(b).Intensity == 9.0f && c.W.Registry.get<TransformComponent>(b).Position.x == 0.0f;
        }),
    });

    // ------------------------------------------------------------------ 9. C++ field attributes
    Add("attributes: Audio Source distances grey out without 3D Sound; Reset to Default", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        InspReveal(Header("Audio Source")), Click(Header("Audio Source")), Wait(3),
        ExpectTrue("enabled while 3D", [](Ctx&) {
            const UiItem* f = Find(ByTag("field:Audio Source/Min Distance"));
            return f && !(f->ItemFlags & ImGuiItemFlags_Disabled);
        }),
        Do("3D Sound off", [](Ctx& c) { c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).Spatial = false; }), Wait(3),
        ExpectTrue("greyed out", [](Ctx&) {
            const UiItem* f = Find(ByTag("field:Audio Source/Min Distance"));
            return f && (f->ItemFlags & ImGuiItemFlags_Disabled);
        }),
        Do("change the volume", [](Ctx& c) {
            X::PushUndo(c.E, c.W, "Change Volume");
            c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).Volume = 0.77f;
        }), Wait(2),
        InspReveal(ByTag("field:Audio Source/Volume")), RightClick(ByTag("field:Audio Source/Volume")), Wait(2),
        Click(MenuItem("Reset to Default")), Wait(3),
        ExpectTrue("volume back to default", [](Ctx& c) {
            return c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).Volume == AudioSourceComponent{}.Volume;
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo restores 0.77", [](Ctx& c) {
            return std::fabs(c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).Volume - 0.77f) < 1e-5f;
        }),
    });

    // Ctrl+click turns a drag into a text field; typing replaces the value. Regression: the
    // harness used to drop the trickled Ctrl release, leaving Ctrl held so ImGui ignored the text.
    Add("typing: Ctrl+click a native float and type a value", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        InspReveal(Header("Audio Source")),
        ClickUnlessShown(Header("Audio Source"), ByTag("field:Audio Source/Min Distance")), Wait(3),
        InspReveal(ByTag("field:Audio Source/Min Distance")),
        Click(ByTag("field:Audio Source/Min Distance"), ImGuiMod_Ctrl), Wait(3),
        TypeText("2"), Wait(2),
        Key(ImGuiKey_Enter), Wait(3),
        ExpectTrue("Min Distance = 2", [](Ctx& c) { return c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).MinDistance == 2.0f; }),
    });
    // ------------------------------------------------------------------ 11. Inspector tabs
    // A narrow Inspector (CI's ~1044px window) fits about one tab, and a tab scrolled out of the
    // strip has no label (ImGui only reports a label for unclipped items). Wheel the strip right
    // (plain wheel scrolls it) until the Nth "##tab" shows at least 8px; a no-op when it already
    // does. Tabs further right than the visible one only, which is all the tests need.
    // An Inspector tab by its label (the strip tags each tab "tab:<label>").
    auto InspTab = [](const std::string& label) { Query q = ByTag("tab:" + label); q.Root = kInspector; return q; };
    // Wheels the strip right until the tab is on screen (a tab scrolled out of view isn't drawn).
    auto TabIntoView = [InspTab](std::string label) -> Step {
        auto frame = std::make_shared<int>(0);
        return {"scroll the " + label + " tab into view", [label, frame, InspTab](Ctx&) {
            auto shown = [](const UiItem* t) { ImRect r = t->Rect; r.ClipWithFull(t->Clip); return r.GetWidth(); };
            if (const UiItem* it = Find(InspTab(label)); it && shown(it) >= 8.0f) return true;
            const UiItem* any = Find(ByExact("##tab", kInspector));
            if (any && (*frame)++ % 8 == 0) { // let the strip's eased scroll settle between notches
                MouseTo(any->Clip.GetCenter());
                ImGui::GetIO().AddMouseWheelEvent(0.0f, -1.0f);
            }
            return false;
        }, 120};
    };
    Add("tabs: drop a Hierarchy row on the Inspector; selecting returns to Selection", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(3),
        Drag(ByTag("row:Gamma"), AtItem(ByExact("##tab", kInspector))), Wait(3),
        Expect("a Gamma tab is active", [](Ctx&) {
            const auto& s = Enhancers::TabState::Get().Inspector;
            if (s.Tabs.size() != 1 || s.Active != 0) return "tabs=" + std::to_string(s.Tabs.size()) + " active=" + std::to_string(s.Active);
            return s.Tabs[0].Label == "Gamma" ? std::string() : "tab is " + s.Tabs[0].Label;
        }),
        SelectByName("Alpha"), Wait(3),
        ExpectTrue("selecting returned to Selection", [](Ctx&) { return Enhancers::TabState::Get().Inspector.Active == -1; }),
        ExpectTrue("Selection shows Alpha (no Light)", [](Ctx&) { return Find(Header("Light")) == nullptr; }),
        TabIntoView("Gamma"),
        Click(InspTab("Gamma")), Wait(3),
        ExpectTrue("the Gamma tab is active again", [](Ctx&) { return Enhancers::TabState::Get().Inspector.Active == 0; }),
        Expect("selection still Alpha", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
        ExpectTrue("the tab shows Gamma (its Light section)", [](Ctx&) { return Find(Header("Light")) != nullptr; }),
    });
    Add("tabs: Ctrl+T pins, Ctrl+W closes, Ctrl+Shift+T reopens (over the Inspector)", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(3),
        HoverPanel(kInspector), Wait(2),
        Key(ImGuiKey_T, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("pinned", [](Ctx&) { const auto& s = Enhancers::TabState::Get().Inspector; return s.Tabs.size() == 1 && s.Active == 0; }),
        HoverPanel(kInspector), Wait(2),
        Key(ImGuiKey_W, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("closed", [](Ctx&) { return Enhancers::TabState::Get().Inspector.Tabs.empty(); }),
        HoverPanel(kInspector), Wait(2),
        Key(ImGuiKey_T, ImGuiMod_Ctrl | ImGuiMod_Shift), Wait(3),
        ExpectTrue("reopened", [](Ctx&) { return Enhancers::TabState::Get().Inspector.Tabs.size() == 1; }),
    });
    Add("tabs: Shift+wheel switches, Ctrl+Shift+wheel moves, middle-click closes, no undo entries", {
        FreshScene(), Wait(2),
        Do("open two tabs", [](Ctx& c) {
            auto& s = Enhancers::TabState::Get().Inspector;
            const std::string key = X::SceneKey(c.E);
            for (const char* n : {"Alpha", "Gamma"})
                s.Open(Enhancers::EditorRef::MakeEntity(key, c.W.Registry.get<OrderComponent>(ByName(c.W, n)).Value, n));
            s.Active = 0;
        }), Wait(3),
        Do("note the undo depth", [](Ctx& c) { g_Number = (double)X::UndoDepth(c.E); }),
        MoveToItem(ByExact("##tab", kInspector)), Wait(2),
        KeyDown(ImGuiKey_None, ImGuiMod_Shift), Wheel(-1.0f), Wait(2), KeyUp(ImGuiKey_None, ImGuiMod_Shift), Wait(2),
        ExpectTrue("Shift+wheel switched", [](Ctx&) { return Enhancers::TabState::Get().Inspector.Active == 1; }),
        KeyDown(ImGuiKey_None, ImGuiMod_Ctrl | ImGuiMod_Shift), Wheel(1.0f), Wait(2), KeyUp(ImGuiKey_None, ImGuiMod_Ctrl | ImGuiMod_Shift), Wait(2),
        ExpectTrue("Ctrl+Shift+wheel moved Gamma first", [](Ctx&) {
            const auto& s = Enhancers::TabState::Get().Inspector;
            return s.Tabs.size() == 2 && s.Tabs[0].Label == "Gamma" && s.Active == 0;
        }),
        // Middle-click a tab that is actually on screen: at the default layout width the strip only
        // fits the active tab, and the others are scrolled out of view (and, clipped, carry no label).
        // In a narrow window even the active tab is partly clipped, so take the tab showing the
        // most pixels: the active Gamma tab, which the strip keeps in view (rightmost on a tie, as
        // the Selection tab, which middle-click never closes, comes first).
        Click([] {
            Query q = ByExact("##tab", kInspector);
            q.Pred = [](const UiItem& it) {
                auto shown = [](const UiItem& t) { ImRect r = t.Rect; r.ClipWithFull(t.Clip); return std::max(0.0f, r.GetWidth()); };
                const UiItem* best = nullptr;
                for (const UiItem* t : FindAll(ByExact("##tab", kInspector)))
                    if (!best || shown(*t) >= shown(*best)) best = t;
                return best && shown(*best) >= 8.0f && it.Id == best->Id;
            };
            return q;
        }(), 0, 2), Wait(3),
        ExpectTrue("middle-click closed it", [](Ctx&) { return Enhancers::TabState::Get().Inspector.Tabs.size() == 1; }),
        ExpectTrue("no undo entries from tab use", [](Ctx& c) { return (double)X::UndoDepth(c.E) == g_Number; }),
    });
    // More tabs than the strip fits (at the default layout even two don't): its overflow controls
    // reach a tab scrolled out of view - the open-tabs list, then the < > arrows.
    {
        auto shownW = [](const UiItem& t) { ImRect r = t.Rect; r.ClipWithFull(t.Clip); return std::max(0.0f, r.GetWidth()); };
        auto popupItem = [](const char* label) {
            Query q = ByLabel(label);
            q.Pred = [](const UiItem& it) { return Contains(it.Root, "##Popup"); };
            return q;
        };
        // A step that runs only when the arrows were found (g_Number, set just before).
        auto IfArrows = [](Step st) {
            return Step{st.Name + " (if the arrows show)", [fn = st.Fn](Ctx& c) { return g_Number == 0.0 || fn(c); }, st.Timeout};
        };
        auto activeIs = [](int i) { return [i](Ctx&) { return Enhancers::TabState::Get().Inspector.Active == i; }; };
        Add("tabs: an overflowing strip's list and < > arrows reach every tab", {
            FreshScene(), Wait(2),
            Do("open Alpha, Beta and Gamma tabs; Alpha active", [](Ctx& c) {
                auto& s = Enhancers::TabState::Get().Inspector;
                const std::string key = X::SceneKey(c.E);
                for (const char* n : {"Alpha", "Beta", "Gamma"})
                    s.Open(Enhancers::EditorRef::MakeEntity(key, c.W.Registry.get<OrderComponent>(ByName(c.W, n)).Value, n));
                s.Active = 0;
            }), Wait(30), // the strip's eased scroll settles
            Until("the overflow controls", [](Ctx&) { return Find(ByExact(ICON_FA_ANGLE_DOWN, kInspector)) != nullptr; }, 60),
            Expect("a tab is scrolled out of view", [shownW](Ctx&) {
                // A tab scrolled wholly out of view is clipped away (not drawn, so not listed).
                const auto tabs = FindAll(ByExact("##tab", kInspector));
                if (tabs.size() < 4) return std::string();
                for (const UiItem* t : tabs)
                    if (shownW(*t) < t->Rect.GetWidth() - 1.0f) return std::string();
                return std::string("every tab is in view - the strip doesn't overflow");
            }),
            Click(ByExact(ICON_FA_ANGLE_DOWN, kInspector)), Wait(3),
            Until("the list names every tab", [popupItem](Ctx&) {
                return Find(popupItem("Selection")) && Find(popupItem("Alpha")) && Find(popupItem("Beta")) && Find(popupItem("Gamma"));
            }, 60),
            Shot("tab-list", {kInspector}),
            Click(popupItem("Gamma")), Wait(3),
            Until("Gamma is active", activeIs(2), 30),
            ExpectTrue("the Inspector shows Gamma (its Light section)", [](Ctx&) { return Find(Header("Light")) != nullptr; }),
            Until("Gamma's tab scrolled into view", [shownW, InspTab](Ctx&) {
                const UiItem* g = Find(InspTab("Gamma"));
                return g && shownW(*g) >= std::min(g->Rect.GetWidth(), g->Clip.GetWidth()) - 1.0f;
            }, 120),
            Shot("gamma", {kInspector}),
            // < and > show when the strip has room for them as well as a tab (not at CI's size).
            Do("are the arrows shown?", [](Ctx&) { g_Number = Find(ByExact(ICON_FA_ANGLE_LEFT, kInspector)) ? 1.0 : 0.0; }),
            IfArrows(Click(ByExact(ICON_FA_ANGLE_LEFT, kInspector))), Wait(3),
            IfArrows(Until("< steps to Beta", activeIs(1), 30)),
            IfArrows(Click(ByExact(ICON_FA_ANGLE_LEFT, kInspector))), Wait(3),
            IfArrows(Click(ByExact(ICON_FA_ANGLE_LEFT, kInspector))), Wait(3),
            IfArrows(Until("then Alpha, then Selection", activeIs(-1), 30)),
            IfArrows(Click(ByExact(ICON_FA_ANGLE_LEFT, kInspector))), Wait(3),
            IfArrows(ExpectTrue("< stops at the first tab", activeIs(-1))),
            IfArrows(Click(ByExact(ICON_FA_ANGLE_RIGHT, kInspector))), Wait(3),
            IfArrows(Until("> steps to Alpha", activeIs(0), 30)),
        });
    }
    Add("tabs: drag a component header onto the strip opens a component tab", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        Drag(Header("Light"), AtItem(ByExact("##tab", kInspector))), Wait(3),
        ExpectTrue("a Light tab", [](Ctx&) {
            const auto& s = Enhancers::TabState::Get().Inspector;
            return s.Tabs.size() == 1 && s.Tabs[0].Sub == "Light" && s.Tabs[0].Label == "Beta";
        }),
    });

    // ------------------------------------------------------------------ 12. Asset Browser tabs
    Add("tabs: Asset Browser tabs follow navigation; clicking a tab navigates", {
        FreshScene(), Wait(2),
        Do("start at the root folder", [](Ctx& c) { X::NavigateFolder(c.E, ""); }), Wait(2),
        HoverPanel(kAssets), Wait(2),
        Key(ImGuiKey_T, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("two tabs, the second active", [](Ctx&) { const auto& s = Enhancers::TabState::Get().Assets; return s.Tabs.size() == 2 && s.Active == 1; }),
        Do("navigate into a folder", [](Ctx& c) {
            if (c.A.Folders().empty()) { Fail("the project has no folders"); return; }
            g_Text = c.A.Folders().front();
            X::NavigateFolder(c.E, g_Text);
        }), Wait(3),
        ExpectTrue("active tab followed", [](Ctx&) { const auto& s = Enhancers::TabState::Get().Assets; return s.Tabs[1].Path == g_Text && s.Tabs[0].Path.empty(); }),
        Click(ByExact("##tab", kAssets)), Wait(3),
        ExpectTrue("clicking tab 1 went back to the root", [](Ctx& c) { return X::CurrentFolder(c.E).empty(); }),
        HoverPanel(kAssets), Wait(2),
        Key(ImGuiKey_W, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("Ctrl+W closed it", [](Ctx&) { return Enhancers::TabState::Get().Assets.Tabs.size() == 1; }),
    });
    // The Asset Browser's strip is wide enough for the < > arrows next to its list button.
    Add("tabs: an overflowing Asset Browser strip steps with < > and opens from its list", {
        FreshScene(), Wait(2),
        Do("twelve folder tabs, the last active", [](Ctx& c) {
            auto& s = Enhancers::TabState::Get().Assets;
            s.Tabs.clear();
            s.Open(Enhancers::EditorRef::MakeFolder("", "Assets"));
            for (const std::string& f : c.A.Folders()) {
                if (s.Tabs.size() >= 12) break;
                const size_t slash = f.find_last_of('/');
                s.Open(Enhancers::EditorRef::MakeFolder(f, slash == std::string::npos ? f : f.substr(slash + 1)));
            }
            if (s.Tabs.size() < 12) Fail("the project has fewer than 11 folders");
            s.Active = (int)s.Tabs.size() - 1;
            X::NavigateFolder(c.E, s.Tabs.back().Path);
        }), Wait(30),
        Until("the overflow controls, arrows too", [](Ctx&) {
            return Find(ByExact(ICON_FA_ANGLE_DOWN, kAssets)) && Find(ByExact(ICON_FA_ANGLE_LEFT, kAssets)) && Find(ByExact(ICON_FA_ANGLE_RIGHT, kAssets));
        }, 60),
        Shot("asset-overflow", {kAssets}),
        Click(ByExact(ICON_FA_ANGLE_LEFT, kAssets)), Wait(4),
        Expect("< stepped to the previous tab and the browser followed", [](Ctx& c) {
            const auto& s = Enhancers::TabState::Get().Assets;
            if (s.Active != (int)s.Tabs.size() - 2) return "active " + std::to_string(s.Active);
            return X::CurrentFolder(c.E) == s.Tabs[(size_t)s.Active].Path ? std::string() : "browsing '" + X::CurrentFolder(c.E) + "'";
        }),
        Click(ByExact(ICON_FA_ANGLE_RIGHT, kAssets)), Wait(4),
        Click(ByExact(ICON_FA_ANGLE_RIGHT, kAssets)), Wait(4),
        ExpectTrue("> back to the last tab, and no further", [](Ctx&) {
            const auto& s = Enhancers::TabState::Get().Assets;
            return s.Active == (int)s.Tabs.size() - 1;
        }),
        Click(ByExact(ICON_FA_ANGLE_DOWN, kAssets)), Wait(3),
        Click([] { Query q = ByLabel("Assets"); q.Pred = [](const UiItem& it) { return Contains(it.Root, "##Popup"); }; return q; }()), Wait(4),
        Until("the list opened the first tab (the root)", [](Ctx& c) {
            return Enhancers::TabState::Get().Assets.Active == 0 && X::CurrentFolder(c.E).empty();
        }, 60),
    });

    // ------------------------------------------------------------------ 13. Favorites
    Add("favorites: hold Alt over the Asset Browser shows the overlay", {
        FreshScene(), Wait(2),
        HoverPanel(kAssets), Wait(2),
        KeyDown(ImGuiKey_None, ImGuiMod_Alt), WaitSeconds(0.5),
        ExpectTrue("visible while held", [](Ctx& c) { return X::FavVisible(c.E); }),
        KeyUp(ImGuiKey_None, ImGuiMod_Alt), WaitSeconds(0.5),
        ExpectTrue("hidden after release", [](Ctx& c) { return !X::FavVisible(c.E); }),
        HoverPanel(kHierarchy), Wait(2),
        KeyDown(ImGuiKey_None, ImGuiMod_Alt), WaitSeconds(0.5),
        ExpectTrue("Alt elsewhere does nothing", [](Ctx& c) { return !X::FavVisible(c.E); }),
        KeyUp(ImGuiKey_None, ImGuiMod_Alt), Wait(2),
    });
    Add("favorites: Ctrl+Alt+B adds, Ctrl+Alt+F locks, a click opens and closes", {
        FreshScene(), Wait(2),
        SelectByName("Gamma"), Wait(2),
        Key(ImGuiKey_B, ImGuiMod_Ctrl | ImGuiMod_Alt), Wait(3),
        ExpectTrue("Gamma on page 1", [](Ctx&) {
            const auto& p = Enhancers::EnhancerUserState::Get().FavoritePages;
            return !p.empty() && !p[0].Items.empty() && p[0].Items[0].Label == "Gamma";
        }),
        SelectByName("Alpha"), Wait(2),
        HoverPanel(kAssets), Wait(2),
        Key(ImGuiKey_F, ImGuiMod_Ctrl | ImGuiMod_Alt), WaitSeconds(0.4),
        ExpectTrue("locked open", [](Ctx& c) { return X::FavVisible(c.E) && X::FavLocked(c.E); }),
        Click(ByExact("##fav", "##vFavorites")), Wait(3),
        Expect("opened Gamma", [](Ctx& c) { return ExpectSelected(c, "Gamma"); }),
        WaitSeconds(0.4),
        ExpectTrue("overlay closed", [](Ctx& c) { return !X::FavVisible(c.E); }),
    });
    Add("favorites: pages, number keys, and no Alt shortcuts while open", {
        FreshScene(), Wait(2),
        HoverPanel(kAssets), Wait(2),
        Key(ImGuiKey_F, ImGuiMod_Ctrl | ImGuiMod_Alt), WaitSeconds(0.4),
        Click(ByExact("+", "##vFavorites")), Wait(3),
        ExpectTrue("a second page", [](Ctx& c) { return Enhancers::EnhancerUserState::Get().FavoritePages.size() == 2 && X::FavPage(c.E) == 1; }),
        TypeText("Level"), Key(ImGuiKey_Enter), Wait(3), // the new page starts in rename
        ExpectTrue("renamed", [](Ctx&) { return Enhancers::EnhancerUserState::Get().FavoritePages[1].Name == "Level"; }),
        ExpectTrue("still open", [](Ctx& c) { return X::FavVisible(c.E); }),
        Key(ImGuiKey_1, ImGuiMod_Alt), Wait(2),
        ExpectTrue("Alt+1 picked page 1", [](Ctx& c) { return X::FavPage(c.E) == 0; }),
        ExpectTrue("and did not focus the Hierarchy", [](Ctx&) { return !Shortcuts::Triggered("panel.focus.hierarchy"); }),
        Key(ImGuiKey_RightArrow), Wait(2),
        ExpectTrue("Right arrow -> page 2", [](Ctx& c) { return X::FavPage(c.E) == 1; }),
        Key(ImGuiKey_Escape), WaitSeconds(0.4),
        ExpectTrue("Esc closed it", [](Ctx& c) { return !X::FavVisible(c.E); }),
    });
    Add("favorites: the asset star is a favorite and undoes", {
        FreshScene(), Wait(2),
        Do("note the undo depth", [](Ctx& c) { g_Number = (double)X::UndoDepth(c.E); }),
        Do("star an asset", [](Ctx& c) { c.E.SetAssetFavorites({"assets/AI/npc_soldier.controller"}, true); }), Wait(4),
        ExpectTrue("starred", [](Ctx& c) { return c.E.IsAssetFavorite("assets/AI/npc_soldier.controller"); }),
        ExpectTrue("on a page", [](Ctx&) {
            return Enhancers::FindFavorite(Enhancers::EnhancerUserState::Get().FavoritePages,
                                           Enhancers::EditorRef::MakeAsset("assets/AI/npc_soldier.controller")) >= 0;
        }),
        Until("the star is an undo step", [](Ctx& c) { return (double)X::UndoDepth(c.E) > g_Number; }, 60),
        Do("note the depth with it", [](Ctx& c) { g_Number = (double)X::UndoDepth(c.E); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(4),
        Expect("undo un-stars it", [](Ctx& c) {
            if (!c.E.IsAssetFavorite("assets/AI/npc_soldier.controller")) return std::string();
            const ImGuiIO& io = ImGui::GetIO();
            return "still starred; undo depth " + std::to_string((int)g_Number) + " -> " + std::to_string(X::UndoDepth(c.E)) +
                   ", top '" + X::UndoTop(c.E) + "', wantText=" + std::to_string((int)io.WantTextInput) +
                   " activeId=" + std::to_string(ImGui::GetActiveID());
        }),
    });

    // ------------------------------------------------------------------ 14. Ruler
    Add("ruler: Shift+R measures, the wheel steps objects, a click shows bounds without selecting", {
        FreshScene(),
        Do("two cubes in front of the camera", [](Ctx& c) {
            const entt::entity a = c.W.CreateModelEntity(c.A.CreatePrimitive("cube"), glm::vec3(0, 0, 0), glm::vec3(0), glm::vec3(1), "Near Cube");
            const entt::entity b = c.W.CreateModelEntity(c.A.CreatePrimitive("cube"), glm::vec3(0, 0, -4), glm::vec3(0), glm::vec3(2), "Far Cube");
            (void)a; (void)b;
            c.Cam.Position = glm::vec3(0.0f, 0.0f, 6.0f);
            c.Cam.Yaw = -90.0f;
            c.Cam.Pitch = 0.0f;
            c.Cam.Orthographic = false;
        }), Wait(3),
        SelectByName("Alpha"), Wait(2),
        Move("to the viewport centre", [](Ctx& c) {
            const glm::vec2 p = c.E.ViewportPos(), s = c.E.ViewportSize();
            return ImVec2(p.x + s.x * 0.5f, p.y + s.y * 0.5f);
        }), Wait(2),
        Do("note the camera", [](Ctx& c) { g_Vec = c.Cam.Position; }),
        KeyDown(ImGuiKey_R, ImGuiMod_Shift), Wait(3),
        Expect("measuring the near cube", [](Ctx& c) {
            if (!X::RulerHeld(c.E)) return std::string("ruler not held");
            return NameOf(c.W, X::RulerHit(c.E)) == "Near Cube" ? std::string() : "hit " + NameOf(c.W, X::RulerHit(c.E));
        }),
        Wheel(-1.0f), Wait(3),
        Expect("the wheel stepped to the far cube", [](Ctx& c) {
            return NameOf(c.W, X::RulerHit(c.E)) == "Far Cube" ? std::string() : "hit " + NameOf(c.W, X::RulerHit(c.E));
        }),
        ExpectTrue("the camera didn't zoom", [](Ctx& c) { return glm::length(c.Cam.Position - g_Vec) < 1e-4f; }),
        MouseButton(0, true), Wait(1), MouseButton(0, false), Wait(3),
        Expect("bounds shown for the far cube", [](Ctx& c) {
            return NameOf(c.W, X::RulerBounds(c.E)) == "Far Cube" ? std::string() : "bounds " + NameOf(c.W, X::RulerBounds(c.E));
        }),
        Expect("the click didn't select", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
        KeyUp(ImGuiKey_R, ImGuiMod_Shift), Wait(3),
        ExpectTrue("released: nothing held, bounds cleared", [](Ctx& c) { return !X::RulerHeld(c.E) && X::RulerBounds(c.E) == entt::null; }),
        Key(ImGuiKey_R), Wait(2),
        ExpectTrue("plain R did not start the ruler", [](Ctx& c) { return !X::RulerHeld(c.E); }),
    });

    // ------------------------------------------------------------------ Editor smoke suites
    RegisterMenusTests();
    RegisterPanelsTests();
    RegisterHierarchyTests();
    RegisterInspectorTests();
    RegisterSceneToolsTests();
    RegisterPlayModeTests();
    RegisterPrefabsTests();
    RegisterSaveLoadTests();
    RegisterAssetsTests();
    RegisterAnimatorTests();
    RegisterCurvesTests();
    RegisterParticlesTests();
    RegisterScriptIDETests();
    RegisterHotReloadTests();
    RegisterSettingsTests();
    RegisterScenesTests();
    RegisterDecalsTests();
    RegisterDecalsLitTests();
}
} // namespace

// =============================================================================================
// Driver
// =============================================================================================
namespace {
// <shots>/report.md: every test with its result, failure, logged errors/warnings and shots.
void WriteReport() {
    std::ofstream f(std::filesystem::path(g_ShotDir) / "report.md", std::ios::binary);
    if (!f) return;
    f << "# Editor smoke test report\n\n" << g_Passed << " passed, " << g_Failed << " failed";
    if (g_Skipped) f << ", " << g_Skipped << " filtered out";
    f << "\n\n| Result | Test | Seconds | Errors | Warnings | Shots |\n|---|---|---|---|---|---|\n";
    for (const TestReport& r : g_Reports) {
        char secs[16];
        std::snprintf(secs, sizeof secs, "%.1f", r.Seconds);
        f << "| " << (r.Passed ? "PASS" : "**FAIL**") << " | " << r.Name << " | " << secs << " | " << r.Errors.size() << " | "
          << r.Warnings.size() << " | " << r.Shots.size() << " |\n";
    }
    for (const TestReport& r : g_Reports) {
        if (r.Passed && r.Errors.empty() && r.Warnings.empty() && r.Shots.empty()) continue;
        f << "\n## " << (r.Passed ? "PASS " : "FAIL ") << r.Name << "\n\n";
        if (!r.Passed) f << "Failure: `" << r.Failure << "`\n\n";
        for (const std::string& e : r.Errors) f << "- error: `" << e << "`\n";
        for (const std::string& w : r.Warnings) f << "- warning: `" << w << "`\n";
        for (const std::string& s : r.Shots) f << "- shot: [" << s << "](" << s << ")\n";
    }
}
} // namespace

namespace EditorUiTests {

void Configure(const std::string& filter) {
    g_Enabled = true;
    g_Filter = filter;
    std::transform(g_Filter.begin(), g_Filter.end(), g_Filter.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
}

bool Enabled() { return g_Enabled; }

int ExitCode() { return g_Failed == 0 && g_Passed > 0 ? 0 : 1; }

bool BeforeFrame(EditorLayer& editor, World& world, AssetLibrary& assets, Camera& camera) {
    if (!g_Enabled) return true;
    if (g_Done) return true;
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) return false;
    ctx->TestEngineHookItems = true;
    ctx->TestEngine = const_cast<EditorTestHookTable*>(&g_HookTable);
    ctx->ErrorCallback = &OnImGuiError;
    ctx->IO.ConfigErrorRecoveryEnableAssert = false; // reported by OnImGuiError and failed instead
    g_Reg.NextFrame();
    EditorTestInputHook() = &KeepOnlyHarnessInput;
    // Only the harness's own events stay in the queue. What PollEvents queued since last frame is the
    // OS reaching a window nobody is using: another window opening or closing (a second editor
    // running its own tests) can take the OS focus, whose focus-lost event releases every key and
    // button a step is holding, or send a cursor-leave, which the backend reports as the mouse
    // position -FLT_MAX - a step's press then lands on no window and clears ImGui's focus.
    for (int i = ctx->InputEventsQueue.Size - 1; i >= 0; --i)
        if (std::find(g_HarnessEventIds.begin(), g_HarnessEventIds.end(), ctx->InputEventsQueue[i].EventId) == g_HarnessEventIds.end())
            ctx->InputEventsQueue.erase(ctx->InputEventsQueue.Data + i);
    g_QueueEnd = ctx->InputEventsQueue.Size;
    struct QueueMark {
        ImGuiContext* c;
        ~QueueMark() {
            g_QueueEnd = c->InputEventsQueue.Size;
            g_HarnessEventIds.clear();
            for (const ImGuiInputEvent& e : c->InputEventsQueue) g_HarnessEventIds.push_back(e.EventId);
        }
    } mark{ctx};

    if (!g_Started) {
        g_Started = true;
        g_ScratchScene = (std::filesystem::path(UserPaths::Root()) / "editor-tests-scene.json").generic_string();
        {
            char* v = nullptr;
            size_t n = 0;
            if (_dupenv_s(&v, &n, "TARTARUS_EDITOR_TEST_SHOTS") == 0 && v) { g_ShotDir = v; std::free(v); }
            if (g_ShotDir.empty()) g_ShotDir = (std::filesystem::path(UserPaths::Root()) / "editor-test-shots").string();
            std::error_code ec;
            std::filesystem::remove_all(g_ShotDir, ec);
            std::filesystem::create_directories(g_ShotDir, ec);
            std::cout << "[EditorTest] screenshots -> " << g_ShotDir << std::endl;
        }
        {
            // TARTARUS_EDITOR_TEST_EXCLUDE=scenes:,assets: skips every test whose name starts with
            // one of the comma-separated prefixes (CI skips the suites that need missing assets).
            char* v = nullptr;
            size_t n = 0;
            std::string list;
            if (_dupenv_s(&v, &n, "TARTARUS_EDITOR_TEST_EXCLUDE") == 0 && v) { list = v; std::free(v); }
            std::transform(list.begin(), list.end(), list.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
            size_t start = 0;
            while (start <= list.size()) {
                size_t end = list.find(',', start);
                if (end == std::string::npos) end = list.size();
                std::string p = list.substr(start, end - start);
                p.erase(0, p.find_first_not_of(' '));
                p.erase(p.find_last_not_of(' ') + 1);
                if (!p.empty()) { g_Exclude.push_back(p); std::cout << "[EditorTest] excluding " << p << std::endl; }
                start = end + 1;
            }
        }
        RegisterTests();
        // Let the editor lay out its dock space first.
        g_StepFrames = -30;
        std::cout << "[EditorTest] " << g_Tests.size() << " test(s)" << std::endl;
        watchdog::Start();
    }
    watchdog::Note("startup", "layout", SIZE_MAX, SIZE_MAX);
    if (g_StepFrames < 0) { ++g_StepFrames; return false; }

    Ctx c{editor, world, assets, camera};
    while (g_TestIndex < g_Tests.size()) {
        Test& t = g_Tests[g_TestIndex];
        std::string lower = t.Name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
        if (!g_Filter.empty() && lower.find(g_Filter) == std::string::npos) { ++g_Skipped; ++g_TestIndex; continue; }
        if (std::any_of(g_Exclude.begin(), g_Exclude.end(), [&](const std::string& p) { return lower.rfind(p, 0) == 0; })) {
            ++g_Skipped; ++g_TestIndex; continue;
        }

        auto finishTest = [&]() {
            // Release anything a failed test may have left held.
            ImGuiIO& io = ImGui::GetIO();
            for (int b = 0; b < 3; ++b) io.AddMouseButtonEvent(b, false);
            Mods(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt, false);
            if (t.Finally) t.Finally(c);
            std::string dump;
            {
                char* v = nullptr;
                size_t n = 0;
                if (_dupenv_s(&v, &n, "TARTARUS_EDITOR_TEST_DUMP") == 0 && v) { dump = v; std::free(v); }
            }
            if (g_TestFailed && !dump.empty()) {
                // Diagnostics: every widget the named window drew last frame.
                for (const UiItem& it : g_Reg.Last)
                    if (Contains(it.Root, dump))
                        std::printf("    [%08X] '%s' tag='%s' win='%s' (%.0f,%.0f)-(%.0f,%.0f) clip(%.0f,%.0f)-(%.0f,%.0f) flags=%X\n",
                                    it.Id, it.Label.c_str(), it.Tag.c_str(), it.Window.c_str(), it.Rect.Min.x, it.Rect.Min.y,
                                    it.Rect.Max.x, it.Rect.Max.y, it.Clip.Min.x, it.Clip.Min.y, it.Clip.Max.x, it.Clip.Max.y, (unsigned)it.ItemFlags);
            }
            g_Cur.Passed = !g_TestFailed;
            g_Cur.Failure = g_TestFailed ? g_FailMessage : std::string();
            g_Cur.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_TestStart).count();
            g_Reports.push_back(g_Cur);
            g_Cur = TestReport{};
            g_ShotPending.clear();
            // A failed test leaves its state on screen for the rest of this frame: capture it.
            if (g_TestFailed) {
                g_ShotPending = Slug(t.Name) + "/failure";
                g_ShotNonBlank.clear();
                g_ShotForLastReport = true;
            }
            if (g_TestFailed) {
                ++g_Failed;
                g_FailLines.push_back(t.Name + " -- " + g_FailMessage);
                std::cout << "[EditorTest] FAIL " << t.Name << "\n             " << g_FailMessage << std::endl;
            } else {
                ++g_Passed;
                std::cout << "[EditorTest] PASS " << t.Name << std::endl;
            }
            g_TestFailed = false;
            g_FailMessage.clear();
            g_StepIndex = 0;
            g_StepFrames = 0;
            ++g_TestIndex;
        };
        if (g_Cur.Name != t.Name) {
            // Whatever the last test left up (a failed one stops mid-dialog) goes before this one
            // starts - a frame after the failure screenshot, which shows it. Otherwise a modal
            // left open blocks every click of every later test.
            X::CloseDialogs(editor);
            g_Cur = TestReport{};
            g_Cur.Name = t.Name;
            g_TestStart = std::chrono::steady_clock::now();
            ResetHealth();
        }
        CheckHealth(world);
        if (g_TestFailed || g_StepIndex >= t.Steps.size()) { finishTest(); return false; }

        Step& s = t.Steps[g_StepIndex];
        watchdog::Note(t.Name, s.Name, g_TestIndex, g_StepIndex);
        const bool done = s.Fn(c);
        ++g_StepFrames;
        if (done) { ++g_StepIndex; g_StepFrames = 0; }
        else if (g_StepFrames > s.Timeout) Fail("step '" + s.Name + "' timed out");
        return false;
    }

    // Summary.
    g_Done = true;
    ctx->TestEngineHookItems = false;
    std::cout << "\n[EditorTest] ==== Summary ====" << std::endl;
    for (const auto& l : g_FailLines) std::cout << "[EditorTest] FAIL " << l << std::endl;
    std::cout << "[EditorTest] " << g_Passed << " passed, " << g_Failed << " failed";
    if (g_Skipped) std::cout << ", " << g_Skipped << " filtered out";
    std::cout << std::endl;
    WriteReport();
    return true;
}

void AfterFrame(int fbWidth, int fbHeight) {
    if (!g_Enabled || g_ShotPending.empty() || fbWidth <= 0 || fbHeight <= 0) return;
    const std::string rel = g_ShotPending;
    g_ShotPending.clear();
    g_ShotDone = true;
    std::vector<unsigned char> px((size_t)fbWidth * fbHeight * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadPixels(0, 0, fbWidth, fbHeight, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<unsigned char> top(px.size());
    for (int y = 0; y < fbHeight; ++y)
        std::memcpy(&top[(size_t)y * fbWidth * 4], &px[(size_t)(fbHeight - 1 - y) * fbWidth * 4], (size_t)fbWidth * 4);
    for (size_t i = 3; i < top.size(); i += 4) top[i] = 255;
    const std::filesystem::path out = std::filesystem::path(g_ShotDir) / (rel + ".png");
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    stbi_write_png(out.string().c_str(), fbWidth, fbHeight, 4, top.data(), fbWidth * 4);
    if (g_ShotForLastReport && !g_Reports.empty()) {
        g_Reports.back().Shots.push_back(rel + ".png");
        g_ShotForLastReport = false;
        return;
    }
    g_Cur.Shots.push_back(rel + ".png");

    // The windows that must have drawn something: not one flat colour across their inner rect.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImVec2 scale = g.IO.DisplayFramebufferScale;
    for (const std::string& name : g_ShotNonBlank) {
        ImGuiWindow* win = nullptr;
        for (ImGuiWindow* w : g.Windows)
            if (w->WasActive && !w->Hidden && Contains(w->Name, name) && !Contains(w->Name, "/")) { win = w; break; }
        if (!win) { Fail("shot " + rel + ": window " + name + " is not shown"); continue; }
        // A window that is only a menu bar (the ##Toolbar strip) has an empty inner rect.
        const ImRect area = win->InnerRect.GetHeight() >= 4.0f ? win->InnerRect : win->Rect();
        const int x0 = std::clamp((int)(area.Min.x * scale.x), 0, fbWidth - 1);
        const int y0 = std::clamp((int)(area.Min.y * scale.y), 0, fbHeight - 1);
        const int x1 = std::clamp((int)(area.Max.x * scale.x), 0, fbWidth);
        const int y1 = std::clamp((int)(area.Max.y * scale.y), 0, fbHeight);
        if (x1 - x0 < 4 || y1 - y0 < 4) { Fail("shot " + rel + ": window " + name + " has no area"); continue; }
        const unsigned char* first = &top[((size_t)y0 * fbWidth + x0) * 4];
        bool flat = true;
        for (int y = y0; y < y1 && flat; y += 2)
            for (int x = x0; x < x1; x += 2) {
                const unsigned char* p = &top[((size_t)y * fbWidth + x) * 4];
                if (std::abs(p[0] - first[0]) > 3 || std::abs(p[1] - first[1]) > 3 || std::abs(p[2] - first[2]) > 3) { flat = false; break; }
            }
        if (flat) {
            char rgb[32];
            std::snprintf(rgb, sizeof rgb, "%d,%d,%d", first[0], first[1], first[2]);
            Fail("shot " + rel + ": window " + name + " is blank (flat " + rgb + ")");
        }
    }
}

} // namespace EditorUiTests
