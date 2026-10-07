// --editor-tests: end-to-end UI tests of the Editor Enhancers (vInspector, vTabs, vFavorites,
// vRuler) in the running editor. See EditorUiTests.h for the model; the checklist these automate
// is docs/VINSPECTOR_TEST_SHEET.md.
//
// How widgets are found: the host executable is built with IMGUI_ENABLE_TEST_ENGINE, so ImGui
// reports every submitted item (id, rectangle, label, item flags) to the hooks below while a test
// runs. Items drawn by the hot-reloadable editor module (toolbar, folder tree) are compiled
// without the hooks and can't be addressed; everything the Enhancers draw is host-side.

#include "EditorUiTests.h"

#include "EditorLayer.h"
#include "EditorPanels.h"
#include "EditorSettings.h"
#include "EditorTestProbe.h"
#include "Shortcuts.h"
#include "World.h"
#include "AssetLibrary.h"
#include "Camera.h"
#include "Components.h"
#include "ComponentRegistry.h"
#include "SceneSerializer.h"
#include "ProjectPaths.h"
#include "UserPaths.h"
#include "EditorTheme.h"
#include "Enhancers/EnhancerCore.h"
#include "Enhancers/EnhancerUserState.h"
#include "Enhancers/TabState.h"
#include "Scripting/ScriptRuntime.h"
#include "Scripting/ScriptComponent.h"

#include <imgui.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // GetModuleFileNameW (the C# test assembly next to the exe)
#include <IconsFontAwesome6.h>
#include <imgui_internal.h>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
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
    static std::string& ScenePath(EditorLayer& e) { return e.m_CurrentScenePath; }
    static void Deselect(EditorLayer& e) { e.ClearSelection(); e.ClearAssetSelection(); }
    static size_t UndoDepth(EditorLayer& e) { return e.m_UndoStack.size(); }
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
    static std::vector<std::pair<std::string, std::string>> ShowValues(EditorLayer& e, std::uint32_t slot, std::uint32_t entity) {
        auto it = e.m_ScriptShowCache.find((static_cast<std::uint64_t>(slot) << 32) | entity);
        return it == e.m_ScriptShowCache.end() ? std::vector<std::pair<std::string, std::string>>{} : it->second.Values;
    }
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
};

std::string g_Filter;
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
void KeepOnlyHarnessInput() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !g_Enabled) return;
    auto& q = ctx->InputEventsQueue;
    q.resize(std::min(g_QueueEnd, q.Size));
}
std::string g_ScratchScene;

void Fail(const std::string& msg) {
    if (!g_TestFailed) { g_TestFailed = true; g_FailMessage = msg; }
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
Query ByTag(std::string tag) { Query q; q.Tag = std::move(tag); return q; }
// The same item, clicked near its right end (a row's x button).
Query RightEnd(Query q) { q.AnchorX = 0.97f; return q; }
Query ByTagPrefix(std::string prefix) { Query q; q.TagPrefix = std::move(prefix); return q; }

std::vector<const UiItem*> FindAll(const Query& q) {
    std::vector<const UiItem*> out;
    for (const UiItem& it : g_Reg.Last) {
        if (!q.Tag.empty() && it.Tag != q.Tag) continue;
        if (!q.TagPrefix.empty() && it.Tag.compare(0, q.TagPrefix.size(), q.TagPrefix) != 0) continue;
        if (!q.Exact.empty() && it.Label != q.Exact) continue;
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
// Moves onto the item, presses and releases `button` with `mods` held. Waits up to the timeout for
// the item to appear (a popup opening, a section expanding).
Step Click(Query q, int mods = 0, int button = 0, int clicks = 1) {
    auto phase = std::make_shared<int>(0);
    const std::string desc = "click " + (q.Tag.empty() ? (q.Exact.empty() ? q.Label : q.Exact) : q.Tag);
    return {desc, [q, mods, button, clicks, phase](Ctx&) {
        ImGuiIO& io = ImGui::GetIO();
        switch (*phase) {
            case 0: {
                const UiItem* it = Find(q);
                ImVec2 p;
                if (!it || !ClickPoint(*it, p)) return false;
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
// A field of the first C# script on Alpha, from its saved JSON (null when not saved).
nlohmann::json ScriptField(Ctx& c, const char* field) {
    const auto* comp = c.W.Registry.try_get<CSharpScriptComponent>(ByName(c.W, "Alpha"));
    if (!comp) return nlohmann::json();
    const auto slots = Scripting::GetSlots(*comp);
    if (slots.empty()) return nlohmann::json();
    const nlohmann::json j = nlohmann::json::parse(slots[0].Fields, nullptr, false);
    return j.is_object() && j.contains(field) ? j.at(field) : nlohmann::json();
}
// Scratch values a test carries between its steps.
double g_Number = 0.0;
std::string g_Text;
glm::vec3 g_Vec{0.0f};

void Add(std::string name, std::vector<Step> steps) {
    g_Tests.push_back({std::move(name), std::move(steps)});
}

// =============================================================================================
// The tests
// =============================================================================================
void RegisterTests() {
    // ------------------------------------------------------------------ 1. Selection history
    Add("history: back and forward through objects and an asset", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(2),
        SelectByName("Beta"), Wait(2),
        Do("select an asset", [](Ctx& c) { X::SelectAsset(c.E, "assets/Scripts/Bob.cs"); }), Wait(2),
        Key(ImGuiKey_LeftBracket, ImGuiMod_Ctrl), Wait(2),
        Expect("back to Beta", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
        Key(ImGuiKey_LeftBracket, ImGuiMod_Ctrl), Wait(2),
        Expect("back to Alpha", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
        Key(ImGuiKey_RightBracket, ImGuiMod_Ctrl), Wait(2),
        Key(ImGuiKey_RightBracket, ImGuiMod_Ctrl), Wait(2),
        Expect("forward to the asset", [](Ctx& c) {
            if (X::Selected(c.E) != entt::null) return "an object is selected: " + NameOf(c.W, X::Selected(c.E));
            return X::AssetKey(c.E) == "assets/Scripts/Bob.cs" ? std::string() : "asset key is '" + X::AssetKey(c.E) + "'";
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
        Do("select an asset", [](Ctx& c) { X::SelectAsset(c.E, "assets/Scripts/Bob.cs"); }), Wait(3),
        Click(ByLabel(ICON_FA_BOOKMARK, kInspector)), Wait(3),
        Click(ByTag("bm:toggle")), Wait(3),
        ExpectTrue("asset bookmarked", [](Ctx&) {
            const auto& bm = Enhancers::EnhancerUserState::Get().InspectorBookmarks;
            return bm.size() == 1 && bm[0].Kind == Enhancers::RefKind::Asset;
        }),
        SelectByName("Beta"), Wait(3),
        Click(ByLabel(ICON_FA_BOOKMARK, kInspector)), Wait(3),
        Click(ByTag("bm:Bob.cs")), Wait(3),
        Expect("asset shown", [](Ctx& c) {
            if (X::Selected(c.E) != entt::null) return std::string("an object is still selected");
            return X::AssetKey(c.E) == "assets/Scripts/Bob.cs" ? std::string() : "asset is " + X::AssetKey(c.E);
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
        Do("add an Audio Listener", [](Ctx& c) { c.W.Registry.emplace<AudioListenerComponent>(ByName(c.W, "Beta")); }), Wait(2),
        SelectByName("Beta"), Wait(3),
        MoveToItem(Header("Audio Listener")), Wait(2),
        Key(ImGuiKey_A), Wait(3),
        ExpectTrue("Enabled off", [](Ctx& c) { return !c.W.Registry.get<AudioListenerComponent>(ByName(c.W, "Beta")).Enabled; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo turns it back on", [](Ctx& c) { return c.W.Registry.get<AudioListenerComponent>(ByName(c.W, "Beta")).Enabled; }),
    });
    Add("sections: animations off remove at once; minimal mode hides the actions button", {
        FreshScene(),
        Do("animations off", [](Ctx&) { EditorSettings::Get().InspectorAnimations = false; }), Wait(2),
        SelectByName("Beta"), Wait(3),
        MoveToItem(Header("Audio Source")), Wait(2),
        Key(ImGuiKey_X), Wait(2),
        ExpectTrue("removed the same frame", [](Ctx& c) { return !c.W.Registry.all_of<AudioSourceComponent>(ByName(c.W, "Beta")); }),
        Do("minimal mode on", [](Ctx&) { EditorSettings::Get().InspectorMinimal = true; }),
        HoverPanel(kHierarchy), Wait(3),
        ExpectTrue("no Light actions button while not hovered", [](Ctx&) { return Find(MoreButton("Light")) == nullptr; }),
        MoveToItem(Header("Light")), Wait(3),
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
        Click(Header("Audio Source")), Wait(3),
        ExpectTrue("enabled while 3D", [](Ctx&) {
            const UiItem* f = Find(ByTag("field:Audio Source/Min Distance"));
            return f && !(f->ItemFlags & ImGuiItemFlags_Disabled);
        }),
        Do("3D Sound off", [](Ctx& c) { c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).Spatial = false; }), Wait(3),
        ExpectTrue("greyed out", [](Ctx&) {
            const UiItem* f = Find(ByTag("field:Audio Source/Min Distance"));
            return f && (f->ItemFlags & ImGuiItemFlags_Disabled);
        }),
        Do("change the volume", [](Ctx& c) { c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).Volume = 0.77f; }), Wait(2),
        RightClick(ByTag("field:Audio Source/Volume")), Wait(2),
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
        ClickUnlessShown(Header("Audio Source"), ByTag("field:Audio Source/Min Distance")), Wait(3),
        Click(ByTag("field:Audio Source/Min Distance"), ImGuiMod_Ctrl), Wait(3),
        TypeText("2"), Wait(2),
        Key(ImGuiKey_Enter), Wait(3),
        ExpectTrue("Min Distance = 2", [](Ctx& c) { return c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).MinDistance == 2.0f; }),
    });
    // ------------------------------------------------------------------ 10. C# script attributes
    // The project's assets/Editor has a [CustomEditor("*")] for every script, so this exercises the
    // managed inspector path (SerializedProperty / PropertyField / EditorHost) - what the user sees.
    Add("attributes: C# script attributes, buttons, OnValueChanged, dictionary, read-outs", {
        FreshScene(),
        Do("load the test assembly and attach AttributeProbe", [](Ctx& c) {
            wchar_t exe[32768]{};
            GetModuleFileNameW(nullptr, exe, 32768);
            const std::string fixture = (std::filesystem::path(exe).parent_path() / "ScriptTests/Tartarus.Gameplay.Tests.dll").u8string();
            if (!std::filesystem::exists(std::filesystem::u8path(fixture))) { Fail("test assembly not built: " + fixture); return; }
            if (!Scripting::Invoke(0, const_cast<char*>(fixture.c_str()), Scripting::kVersion)) { Fail("couldn't load the test assembly"); return; }
            auto& comp = c.W.Registry.emplace<CSharpScriptComponent>(ByName(c.W, "Alpha"));
            Scripting::Attach(comp, "", "Tartarus.Tests.AttributeProbe");
        }), Wait(2),
        SelectByName("Alpha"), Wait(3),
        Click(Header("C# Script")), Wait(4),
        ExpectTrue("Hits is read-only", [](Ctx&) { const UiItem* f = Find(ByTag("cs:Hits")); return f && (f->ItemFlags & ImGuiItemFlags_Disabled); }),
        ExpectTrue("Note hidden while Advanced is off", [](Ctx&) { return Find(ByTagPrefix("cs:Note=")) == nullptr; }),
        ExpectTrue("Boost enabled (Mode = Slow)", [](Ctx&) { const UiItem* f = Find(ByTag("cs:Boost")); return f && !(f->ItemFlags & ImGuiItemFlags_Disabled); }),
        ExpectTrue("Script / Source rows moved under Advanced", [](Ctx&) { return Find(ByExact("##class", kInspector)) == nullptr; }),
        Click(ByTag("cs:Advanced")), Wait(3),
        ExpectTrue("Note shown once Advanced is on", [](Ctx&) { return Find(ByTagPrefix("cs:Note=")) != nullptr; }),
        Do("Mode = Off", [](Ctx& c) {
            auto& comp = c.W.Registry.get<CSharpScriptComponent>(ByName(c.W, "Alpha"));
            auto slots = Scripting::GetSlots(comp);
            nlohmann::json f = nlohmann::json::parse(slots[0].Fields, nullptr, false);
            if (!f.is_object()) f = nlohmann::json::object();
            f["Mode"] = 0;
            slots[0].Fields = f.dump();
            Scripting::SetSlots(comp, slots);
        }), Wait(3),
        ExpectTrue("Boost greyed out while Mode = Off", [](Ctx&) { const UiItem* f = Find(ByTag("cs:Boost")); return f && (f->ItemFlags & ImGuiItemFlags_Disabled); }),
        Click(ByExact("5##Speed", kInspector)), Wait(3),
        ExpectTrue("variant chip set Speed = 5", [](Ctx& c) { return ScriptField(c, "Speed") == nlohmann::json(5.0); }),
        Click(ByLabel("DoubleSpeed", kInspector)), Wait(3),
        ExpectTrue("DoubleSpeed button ran (10)", [](Ctx& c) { return ScriptField(c, "Speed") == nlohmann::json(10.0); }),
        Click(ByTag("cs:Max Speed"), ImGuiMod_Ctrl), Wait(3), // Ctrl+click: type a value
        TypeText("4"), Key(ImGuiKey_Enter), Wait(4),
        ExpectTrue("OnValueChanged clamped Speed to MaxSpeed", [](Ctx& c) { return ScriptField(c, "Speed") == nlohmann::json(4.0); }),
        Click(ByLabel("Reset Hits", kInspector)), Wait(3),
        ExpectTrue("Reset Hits ran", [](Ctx& c) { return ScriptField(c, "Hits") == nlohmann::json(0); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo brings Hits back", [](Ctx& c) { const auto v = ScriptField(c, "Hits"); return v.is_null() || v == nlohmann::json(3); }),
        Click(ByTagPrefix("cs:New key##Weights=")), Wait(2),
        TypeText("legs"), Wait(2),
        Click(ByExact("Add##Weights", kInspector)), Wait(3),
        ExpectTrue("dictionary key added", [](Ctx& c) { const auto w = ScriptField(c, "Weights"); return w.is_object() && w.contains("legs") && w.contains("head"); }),
        Until("read-out shows Doubled = 8", [](Ctx&) { return Find(ByTag("cs:Doubled##show=8")) != nullptr; }, 5000),
    });

    // ------------------------------------------------------------------ 11. Inspector tabs
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
        Click(ByExact("##tab", kInspector, 1)), Wait(3),
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
        Click(ByExact("##tab", kInspector, 2), 0, 2), Wait(3), // the third "##tab" is the second pinned tab
        ExpectTrue("middle-click closed it", [](Ctx&) { return Enhancers::TabState::Get().Inspector.Tabs.size() == 1; }),
        ExpectTrue("no undo entries from tab use", [](Ctx& c) { return (double)X::UndoDepth(c.E) == g_Number; }),
    });
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
        Do("star an asset", [](Ctx& c) { c.E.SetAssetFavorites({"assets/Scripts/Bob.cs"}, true); }), Wait(4),
        ExpectTrue("starred", [](Ctx& c) { return c.E.IsAssetFavorite("assets/Scripts/Bob.cs"); }),
        ExpectTrue("on a page", [](Ctx&) {
            return Enhancers::FindFavorite(Enhancers::EnhancerUserState::Get().FavoritePages,
                                           Enhancers::EditorRef::MakeAsset("assets/Scripts/Bob.cs")) >= 0;
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(4),
        ExpectTrue("undo un-stars it", [](Ctx& c) { return !c.E.IsAssetFavorite("assets/Scripts/Bob.cs"); }),
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
}
} // namespace

// =============================================================================================
// Driver
// =============================================================================================
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
    g_Reg.NextFrame();
    EditorTestInputHook() = &KeepOnlyHarnessInput;
    g_QueueEnd = ctx->InputEventsQueue.Size;
    struct QueueMark { ImGuiContext* c; ~QueueMark() { g_QueueEnd = c->InputEventsQueue.Size; } } mark{ctx};

    if (!g_Started) {
        g_Started = true;
        g_ScratchScene = (std::filesystem::path(UserPaths::Root()) / "editor-tests-scene.json").generic_string();
        RegisterTests();
        // The CRT screen effect bends mouse input to match its curved picture; tests click flat.
        EditorSettings::Get().CrtScreen = false;
        // Let the editor lay out its dock space first.
        g_StepFrames = -30;
        std::cout << "[EditorTest] " << g_Tests.size() << " test(s)" << std::endl;
    }
    if (g_StepFrames < 0) { ++g_StepFrames; return false; }

    Ctx c{editor, world, assets, camera};
    while (g_TestIndex < g_Tests.size()) {
        Test& t = g_Tests[g_TestIndex];
        std::string lower = t.Name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
        if (!g_Filter.empty() && lower.find(g_Filter) == std::string::npos) { ++g_Skipped; ++g_TestIndex; continue; }

        auto finishTest = [&]() {
            // Release anything a failed test may have left held.
            ImGuiIO& io = ImGui::GetIO();
            for (int b = 0; b < 3; ++b) io.AddMouseButtonEvent(b, false);
            Mods(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt, false);
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
        if (g_TestFailed || g_StepIndex >= t.Steps.size()) { finishTest(); return false; }

        Step& s = t.Steps[g_StepIndex];
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
    return true;
}

} // namespace EditorUiTests
