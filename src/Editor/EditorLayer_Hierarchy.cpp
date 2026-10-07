// Hierarchy panel: the entity tree, selection, rename, grouping/parenting, and the
// Add-entity menu body. Split out of EditorLayer.cpp for build time (#179).

#include "EditorLayer.h"
#include "EditorTestProbe.h" // --editor-tests widget names
#include "EditorLayerInternal.h"
#include "EditorTheme.h"
#include "EditorModuleAPI.h" // kHierarchyFilter* bitmask constants, shared with EditorModuleHierarchy.cpp
#include "ComponentRegistry.h" // vHierarchy component minimap
#include "Enhancers/EnhancerCore.h" // FAIconGlyph (row style icons)
#include "Enhancers/StyleWidgets.h" // the Row Style menu's colour + icon pickers
#include "Enhancers/EnhancerUserState.h" // vFavorites (row menu)
#include "FileDialog.h"
#include "AssetLibrary.h"
#include "World.h"
#include "Camera.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"
#include "MaterialAsset.h"
#include "AudioEngine.h"
#include "Screenshot.h"
#include "SceneSerializer.h"
#include "AABB.h"
#include "Log.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "AssetImporterInspector.h"
#include "Profiler.h"
#include "ProjectPaths.h"
#include "GLStateCache.h"
#include "Framebuffer.h"
#include "gl.h"

#include <imgui.h>
#include <imgui_internal.h> // ImMax/ImFloor, ImGuiWindow, and the item-flag helpers the panels use
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <IconsFontAwesome6.h>

#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp> // extractEulerAngleYXZ - must match ComposeTransform's order (#108)

#include <filesystem>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <sstream>
#include <fstream>
#include <cmath>
#include <cctype>
#include <cstring>
#include <functional>
#include <cfloat>

using namespace EditorInternal;


namespace {

// Where a newly added object goes: a few units in front of the editor camera. If the camera
// has somehow gone non-finite, fall back to the origin so the object is still findable rather
// than spawned at inf/NaN and lost.
inline glm::vec3 SafeSpawnInFrontOf(const Camera& cam, float distance = 5.0f) {
    glm::vec3 p = cam.Position + cam.Front() * distance;
    if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) return p;
    return glm::vec3(0.0f);
}

// Turn a freshly created light entity into a sun: same raking angle / intensity / disc size the
// SceneSerializer synthesises for a scene with no directional light, so an added one casts a
// readable shadow immediately rather than sitting near-overhead and washed out (#125).
inline void MakeDirectionalLight(World& world, entt::entity e) {
    if (e == entt::null || !world.Registry.valid(e)) return;
    auto& lc = world.Registry.get<LightComponent>(e);
    lc.Kind = LightComponent::Type::Directional;
    lc.Intensity = 6.0f;
    lc.AngularSizeDegrees = 2.0f;
    lc.Shadow.Enabled = true;      // a freshly added sun casts shadows by default
    lc.Shadow.Softness = 0.25f;    // crisper penumbra out of the box (~0.25 on the Softness slider)
    world.Registry.get<TransformComponent>(e).SetRotationEuler(glm::vec3(-36.25f, 53.13f, 0.0f));
}

// The Hierarchy lists entities by OrderComponent (a stable per-entity sequence assigned at
// creation and preserved through save/load), so a snapshot round-trip — undo/redo, Play->Stop —
// no longer reshuffles the list. Entities predating OrderComponent (0) keep a stable relative
// order via the entity-handle tiebreak.
template <typename View>
std::vector<entt::entity> ViewInCreationOrder(const entt::registry& reg, View view) {
    std::vector<entt::entity> entities(view.begin(), view.end());
    std::sort(entities.begin(), entities.end(), [&](entt::entity a, entt::entity b) {
        const auto* oa = reg.try_get<OrderComponent>(a);
        const auto* ob = reg.try_get<OrderComponent>(b);
        int va = oa ? oa->Value : 0, vb = ob ? ob->Value : 0;
        return va != vb ? va < vb : a < b;
    });
    return entities;
}

// Phase 5 item 6 — the type-filter chips' bitmask, and the tiebreak key the Type sort mode uses.
// Independent of the per-row kind BADGE (DrawHierarchyRowBody's own hasMesh/hasLight/hasCamera),
// which picks one primary glyph by priority — this instead sets every bit an entity qualifies
// for, so a mesh-with-a-light still matches either chip.
int HierarchyKindMask(const entt::registry& reg, entt::entity e) {
    int mask = 0;
    if (reg.all_of<RenderableComponent>(e)) mask |= kHierarchyFilterMesh;
    if (reg.all_of<LightComponent>(e))      mask |= kHierarchyFilterLight;
    if (reg.all_of<CameraComponent>(e))     mask |= kHierarchyFilterCamera;
    if (mask == 0) mask |= kHierarchyFilterOther;
    return mask;
}

// The open/closed flag of `e`'s row lives in the Hierarchy window's storage under "##node",
// hashed inside the row's ancestor-to-self PushID chain (DrawHierarchyTreeBody pushes it before
// each row). Rebuilds that chain into `scratch` (reused, no allocation once warm). Must be called
// with the Hierarchy window current and no extra IDs pushed - the same contract as
// SetHierarchyExpandedRecursive.
ImGuiID HierarchyNodeStateId(const World& world, entt::entity e, std::vector<entt::entity>& scratch) {
    scratch.clear();
    for (entt::entity w = e; w != entt::null && world.Registry.valid(w);) {
        scratch.push_back(w);
        const auto* h = world.Registry.try_get<HierarchyComponent>(w);
        w = h ? h->Parent : entt::null;
    }
    for (auto it = scratch.rbegin(); it != scratch.rend(); ++it) ImGui::PushID((int)entt::to_integral(*it));
    const ImGuiID id = ImGui::GetID("##node");
    for (size_t i = 0; i < scratch.size(); ++i) ImGui::PopID();
    return id;
}

} // namespace

// "Object 7" for an entity the user never named (#21 P10): its 1-based position in creation
// order. This used to re-sort every entity per call - once per unnamed visible row per frame -
// so the ordinals are now built once per frame instead.
int EditorLayer::HierarchyCreationOrdinal(const World& world, entt::entity e) {
    const int frame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -2;
    if (frame != m_HierOrdinalsFrame) {
        m_HierOrdinalsFrame = frame;
        m_HierOrdinals.clear();
        const auto list = ViewInCreationOrder(world.Registry, world.Registry.view<const NameComponent>());
        for (size_t i = 0; i < list.size(); ++i) m_HierOrdinals[list[i]] = (int)i + 1;
    }
    const auto it = m_HierOrdinals.find(e);
    return it != m_HierOrdinals.end() ? it->second : 0;
}


void EditorLayer::CopySelection(World& world) {
    auto selection = GetSelectedItems();
    if (selection.empty()) return;
    m_Clipboard = SceneSerializer::SaveEntitiesToString(world, selection);
    Log::Info("Copied " + std::to_string(selection.size()) +
        (selection.size() == 1 ? " object." : " objects."));
}

void EditorLayer::PasteClipboard(World& world, AssetLibrary& assets) {
    if (m_Clipboard.empty()) return;
    PushUndo(world, "Paste");
    std::vector<entt::entity> pasted;
    if (!SceneSerializer::AppendEntitiesFromString(world, assets, m_Clipboard, pasted) || pasted.empty()) {
        Log::Warn("Paste failed: clipboard content could not be rebuilt.");
        return;
    }
    // Offset so a paste is visibly distinct from the original instead of landing exactly on top
    // of it — matching what Duplicate already does.
    for (entt::entity e : pasted) {
        if (auto* transform = world.Registry.try_get<TransformComponent>(e)) {
            const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
            bool isRoot = !hier || hier->Parent == entt::null;
            if (isRoot) transform->Position += glm::vec3(1.0f, 0.0f, 1.0f);
        }
    }
    ClearSelection();
    for (entt::entity e : pasted) AddToSelectionIfAbsent(e);
    if (m_Selected == entt::null && !pasted.empty()) SelectItem(pasted.front(), false);
}

void EditorLayer::ClearSelection() {
    CancelEyedropper(); // #93 — the armed field belonged to the old selection
    m_Selected = entt::null;
    m_ExtraSelection.clear();
    m_SelectionAnchor = entt::null;
    m_RenamingEntity = entt::null;
    m_LightHandleArmedFor = entt::null;
}

bool EditorLayer::SelectEntityByOrder(World& world, int orderValue) {
    for (auto [entity, order] : world.Registry.view<const OrderComponent>().each()) {
        if (order.Value != orderValue) continue;
        SelectItem(entity, false);
        return true;
    }
    return false;
}

void EditorLayer::SelectItem(entt::entity entity, bool addToSelection) {
    CancelEyedropper(); // #93 — the armed field belonged to the old selection
    // Any selection that isn't a viewport icon-click disarms the light grab handles;
    // HandleViewportPicking re-arms them right after it calls this for a light it picked.
    m_LightHandleArmedFor = entt::null;
    bool isPrimary = m_Selected == entity;

    if (!addToSelection) {
        m_ExtraSelection.clear();
        m_Selected = entity;
        m_SelectionAnchor = entity; // plain click (re)anchors range selection here
        if (m_FrameOnSelect && entity != entt::null) m_PendingFrameSelect = true;
        return;
    }

    // Any Ctrl+Click also moves the range anchor to the clicked row, matching Unity.
    m_SelectionAnchor = entity;

    if (isPrimary) {
        // Demote: promote the most recently added extra to primary, or clear if none left.
        if (!m_ExtraSelection.empty()) {
            m_Selected = m_ExtraSelection.back();
            m_ExtraSelection.pop_back();
        } else {
            m_Selected = entt::null;
        }
        return;
    }

    for (auto it = m_ExtraSelection.begin(); it != m_ExtraSelection.end(); ++it) {
        if (*it == entity) {
            m_ExtraSelection.erase(it); // already co-selected — toggle it back off
            return;
        }
    }

    if (!HasAnySelection()) {
        m_Selected = entity;
    } else {
        m_ExtraSelection.push_back(entity);
    }
}

void EditorLayer::AddToSelectionIfAbsent(entt::entity entity) {
    if (IsSelected(entity)) return; // leave already-selected items alone — don't toggle them off
    if (!HasAnySelection()) {
        m_Selected = entity;
    } else {
        m_ExtraSelection.push_back(entity);
    }
}

void EditorLayer::SelectAllVisibleInHierarchy() {
    if (m_HierarchyVisibleOrder.empty()) return;
    m_Selected = entt::null;
    m_ExtraSelection.clear();
    m_RenamingEntity = entt::null;
    for (entt::entity e : m_HierarchyVisibleOrder) AddToSelectionIfAbsent(e);
    m_SelectionAnchor = m_HierarchyVisibleOrder.front();
    // Deliberately no m_PendingFrameSelect here — snapping the camera to fit the entire scene
    // every time the user hits Ctrl+A would be more disruptive than helpful.
}

void EditorLayer::SelectAllEntities(World& world) {
    m_Selected = entt::null;
    m_ExtraSelection.clear();
    m_RenamingEntity = entt::null;
    for (entt::entity e : world.Registry.view<const NameComponent>()) AddToSelectionIfAbsent(e);
    if (m_Selected != entt::null) m_SelectionAnchor = m_Selected;
}

// --- Selection history (#236 R2; by identity since vInspector) ------------------------------
namespace {
std::vector<entt::entity> SnapshotSelection(entt::entity primary, const std::vector<entt::entity>& extra) {
    std::vector<entt::entity> s;
    if (primary != entt::null) s.push_back(primary);
    s.insert(s.end(), extra.begin(), extra.end());
    return s;
}

// Enhancers::OrdersResolveFn over a World: true when any of the orders still names an entity.
bool AnyOrderResolves(const std::vector<int>& orders, void* ctx) {
    const World& world = *static_cast<const World*>(ctx);
    for (auto [e, o] : world.Registry.view<const OrderComponent>().each())
        if (std::find(orders.begin(), orders.end(), o.Value) != orders.end()) return true;
    return false;
}
} // namespace

void EditorLayer::RecordSelectionHistory(const World& world) {
    auto cur = SnapshotSelection(m_Selected, m_ExtraSelection);
    // Only a lone file selection (nothing in the scene, not a folder) is what the Inspector shows,
    // so that's the only asset state the history follows.
    static const std::string kNoAsset;
    const std::string& assetNow = (cur.empty() && !m_SelectedAssetIsFolder) ? m_SelectedAssetKey : kNoAsset;
    const bool entitiesChanged = cur != m_SelSnapshotLast;
    if (!entitiesChanged && assetNow == m_SelAssetLast) { m_EditPushedThisFrame = false; return; }
    m_SelSnapshotLast = cur;
    m_SelAssetLast = assetNow;

    Enhancers::SelectionHistoryEntry entry;
    entry.Orders = CaptureSelectedOrders(world, cur);
    if (entry.Orders.empty() && !assetNow.empty()) entry.Asset = assetNow;
    else entry.Scene = CurrentSceneKey();
    Enhancers::SelectionHistoryEntry prev = std::move(m_SelEntryLast);
    if (prev.Scene.empty() && prev.Orders.empty() && prev.Asset.empty()) prev.Scene = entry.Scene.empty() ? CurrentSceneKey() : entry.Scene; // never polled yet: the empty start state
    m_SelEntryLast = entry;

    // Our own back/forward (or Undo/Redo/JumpTo*) change — advance the "last seen" marker, don't
    // append to either history.
    if (m_SelHistoryNavigating) { m_SelHistoryNavigating = false; m_EditPushedThisFrame = false; return; }

    // vTabs: selecting something brings the Inspector back to its Selection tab, so it shows what
    // was just picked. (The Inspector lock is what keeps a view while selecting elsewhere.)
    if (auto& tabs = Enhancers::TabState::Get().Inspector; tabs.Active >= 0) {
        tabs.Active = -1;
        Enhancers::TabState::Get().MarkDirty();
    }

    // Phase 6 item 6 / Q6 — a genuine user selection change becomes its own m_UndoStack entry,
    // UNLESS an edit already pushed one this frame (Duplicate/Paste/Add Cube/... all change
    // selection as a side effect of their own PushUndo/CommitStagedUndo; that's not a second,
    // separate user action worth its own row). PushUndo's dedup only rejects a push whose scene
    // hash AND selection both match the current top — a selection-only change always differs in
    // SelectedOrders, so it's never silently dropped there.
    //
    // The entry must hold the PRE-change selection (`prev`), not the current one — same "snapshot
    // taken before the thing this entry undoes" contract every other UndoEntry follows. Passed as
    // an override since CaptureSelectedOrders(world) alone would only ever see the live (already
    // new) selection by the time this runs. Taken from the identity entry recorded last frame
    // (the raw handles may already be stale), and only for the scene that is open now. An
    // asset-only change records history but is not a scene edit, so it pushes no undo entry.
    if (entitiesChanged && !m_EditPushedThisFrame) {
        const std::vector<int> prevOrders = prev.Scene == entry.Scene ? prev.Orders : std::vector<int>();
        PushUndo(world, SelectionUndoLabel(world), /*selectionOnly=*/true, &prevOrders);
    }
    m_EditPushedThisFrame = false;

    if (m_SelHistory.empty()) {           // seed with the pre-change state so Back can reach it
        m_SelHistory.push_back(std::move(prev));
        m_SelHistoryPos = 0;
    } else if (m_SelHistoryPos + 1 < m_SelHistory.size()) {
        m_SelHistory.resize(m_SelHistoryPos + 1); // drop the forward branch
    }
    if (m_SelHistory.back() != entry) m_SelHistory.push_back(std::move(entry));

    constexpr size_t kCap = 64;
    if (m_SelHistory.size() > kCap)
        m_SelHistory.erase(m_SelHistory.begin(), m_SelHistory.begin() + (m_SelHistory.size() - kCap));
    m_SelHistoryPos = m_SelHistory.size() - 1;
}

void EditorLayer::ApplySelectionEntry(World& world, const Enhancers::SelectionHistoryEntry& entry) {
    if (!entry.Orders.empty()) {
        RestoreSelectionByOrder(world, entry.Orders); // sets m_SelHistoryNavigating
        return;
    }
    ClearSelection();
    if (!entry.Asset.empty()) {
        ClearAssetSelection();
        m_SelectedAssetKey = entry.Asset;
        m_SelectedAssetIsFolder = false;
    } else if (!m_SelectedAssetIsFolder) {
        ClearAssetSelection(); // an empty selection: nothing in the Inspector
    }
    m_SelHistoryNavigating = true; // next RecordSelectionHistory() poll swallows this change
}

bool EditorLayer::CanSelectionHistoryBack() const {
    return Enhancers::StepSelectionHistory(m_SelHistory, (int)m_SelHistoryPos, -1, CurrentSceneKey()) >= 0;
}

bool EditorLayer::CanSelectionHistoryForward() const {
    return Enhancers::StepSelectionHistory(m_SelHistory, (int)m_SelHistoryPos, +1, CurrentSceneKey()) >= 0;
}

void EditorLayer::SelectionHistoryBack(World& world) {
    const int i = Enhancers::StepSelectionHistory(m_SelHistory, (int)m_SelHistoryPos, -1, CurrentSceneKey(),
                                                  &AnyOrderResolves, &world);
    if (i < 0) return;
    m_SelHistoryPos = (size_t)i;
    ApplySelectionEntry(world, m_SelHistory[m_SelHistoryPos]);
}

void EditorLayer::SelectionHistoryForward(World& world) {
    const int i = Enhancers::StepSelectionHistory(m_SelHistory, (int)m_SelHistoryPos, +1, CurrentSceneKey(),
                                                  &AnyOrderResolves, &world);
    if (i < 0) return;
    m_SelHistoryPos = (size_t)i;
    ApplySelectionEntry(world, m_SelHistory[m_SelHistoryPos]);
}

void EditorLayer::InvertSelection(World& world) {
    std::vector<entt::entity> nowSelected;
    for (entt::entity e : world.Registry.view<const NameComponent>())
        if (!IsSelected(e)) nowSelected.push_back(e);
    m_Selected = entt::null;
    m_ExtraSelection.clear();
    m_RenamingEntity = entt::null;
    for (entt::entity e : nowSelected) AddToSelectionIfAbsent(e);
    if (m_Selected != entt::null) m_SelectionAnchor = m_Selected;
}

void EditorLayer::HandleHierarchyKeyboardNav(World& world) {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return; // renaming, or the search box has the keyboard

    const auto& vis = m_HierarchyVisibleOrder;
    if (vis.empty()) return;

    // A node's open/closed flag lives in this window's storage under the "##node" id computed
    // from the row's ancestor-to-self PushID chain (DrawHierarchyTreeBody pushes PushID(root) …
    // PushID(entity), one per ancestor, before drawing each row). Rebuild the whole chain here or
    // the id won't match for any row below the top level (which is why Left/Right did nothing on
    // nested rows).
    auto nodeId = [&](entt::entity e) { return HierarchyNodeStateId(world, e, m_HierChainScratch); };
    auto nodeOpen    = [&](entt::entity e) { return ImGui::GetStateStorage()->GetInt(nodeId(e), 0) != 0; };
    auto setNodeOpen = [&](entt::entity e, bool open) { ImGui::GetStateStorage()->SetInt(nodeId(e), open ? 1 : 0); };

    int idx = -1;
    for (int i = 0; i < (int)vis.size(); ++i) if (vis[i] == m_Selected) { idx = i; break; }

    const bool shift = io.KeyShift;
    auto pick = [&](int i) {
        i = i < 0 ? 0 : i >= (int)vis.size() ? (int)vis.size() - 1 : i;
        entt::entity e = vis[i];
        if (shift && m_SelectionAnchor != entt::null) SelectHierarchyRange(world, e, io.KeyCtrl);
        else SelectItem(e, false);
        m_HierarchyScrollToEntity = e;
    };

    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
        pick(idx < 0 ? 0 : idx + 1);
    } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
        pick(idx < 0 ? (int)vis.size() - 1 : idx - 1);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Home, true)) {
        pick(0);
    } else if (ImGui::IsKeyPressed(ImGuiKey_End, true)) {
        pick((int)vis.size() - 1);
    } else if (idx >= 0 && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(vis[idx]);
        const bool hasKids = h && !h->Children.empty();
        if (hasKids && !nodeOpen(vis[idx])) setNodeOpen(vis[idx], true);
        else if (hasKids) pick(idx + 1); // already open: step into the first child
    } else if (idx >= 0 && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(vis[idx]);
        const bool hasKids = h && !h->Children.empty();
        if (hasKids && nodeOpen(vis[idx])) {
            setNodeOpen(vis[idx], false);
        } else if (h && h->Parent != entt::null) {
            SelectItem(h->Parent, false);
            m_HierarchyScrollToEntity = h->Parent;
        }
    }

    // Type-to-select: printable keystrokes (no Ctrl/Alt) build a prefix that resets after a short
    // idle, then jump to the next visible row whose name starts with it, wrapping past the end.
    // Skipped on a frame where an Editor Enhancers hover key (E, A, X, ...) acted on the row
    // under the mouse - that letter was a command, not the start of a name.
    if (!io.KeyCtrl && !io.KeyAlt && !m_HierarchyHoverKeyUsed && io.InputQueueCharacters.Size > 0) {
        const double now = ImGui::GetTime();
        const size_t before = m_HierarchyTypeAhead.size();
        for (ImWchar c : io.InputQueueCharacters) {
            if (c < 32 || c > 126) continue;
            if (now - m_HierarchyTypeAheadAt > 0.9) m_HierarchyTypeAhead.clear();
            m_HierarchyTypeAheadAt = now;
            m_HierarchyTypeAhead += (char)std::tolower((unsigned char)c);
        }
        if (m_HierarchyTypeAhead.size() != before && !m_HierarchyTypeAhead.empty()) {
            const int n = (int)vis.size();
            for (int step = 1; step <= n; ++step) {
                entt::entity e = vis[(std::max(idx, 0) + step) % n];
                const auto* nm = world.Registry.try_get<NameComponent>(e);
                std::string lower = nm ? nm->Name : std::string();
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char ch) { return (char)std::tolower(ch); });
                if (lower.rfind(m_HierarchyTypeAhead, 0) == 0) {
                    SelectItem(e, false);
                    m_HierarchyScrollToEntity = e;
                    break;
                }
            }
        }
    }
}

void EditorLayer::SelectHierarchyRange(World& world, entt::entity target, bool additive) {
    // No usable anchor (first click was Shift, or the anchor row was deleted / scrolled out of
    // the visible set): fall back to a plain pick so Shift+Click is never a dead input.
    const auto& order = m_HierarchyVisibleOrder;
    auto indexOf = [&](entt::entity e) -> int {
        for (int i = 0; i < (int)order.size(); ++i) if (order[i] == e) return i;
        return -1;
    };
    int ai = (m_SelectionAnchor != entt::null && world.Registry.valid(m_SelectionAnchor))
                 ? indexOf(m_SelectionAnchor) : -1;
    int ti = indexOf(target);
    if (ai < 0 || ti < 0) {
        SelectItem(target, additive);
        return;
    }
    int lo = ai < ti ? ai : ti;
    int hi = ai < ti ? ti : ai;

    std::vector<entt::entity> keep;
    if (additive) { // Ctrl+Shift: preserve whatever was already selected, then union the range in
        keep = m_ExtraSelection;
        if (m_Selected != entt::null) keep.push_back(m_Selected);
    }

    m_ExtraSelection.clear();
    m_Selected = target;              // the row just clicked is the active object
    m_RenamingEntity = entt::null;
    auto addUnique = [&](entt::entity e) {
        if (e == m_Selected) return;
        if (std::find(m_ExtraSelection.begin(), m_ExtraSelection.end(), e) == m_ExtraSelection.end())
            m_ExtraSelection.push_back(e);
    };
    for (int i = lo; i <= hi; ++i) addUnique(order[i]);
    for (entt::entity e : keep) if (world.Registry.valid(e)) addUnique(e);
    // m_SelectionAnchor intentionally left untouched — Unity keeps it fixed so the next
    // Shift+Click can grow or shrink the same range.
}

void EditorLayer::DeleteSelection(World& world) {
    PushUndo(world, "Delete");

    // entt::entity handles don't shift when another entity is destroyed (unlike the vector
    // indices this replaced), so unlike before there's no careful ordering needed here at all —
    // and no more separate "boxes soft-delete, models hard-erase" split, either.
    // Destroys children recursively too, so parenting one entity under another means deleting
    // the parent doesn't leave the child pointing at a dead entt::entity.
    int count = (m_Selected != entt::null ? 1 : 0) + (int)m_ExtraSelection.size();
    if (m_Selected != entt::null) world.DestroyEntityAndChildren(m_Selected);
    for (entt::entity e : m_ExtraSelection) {
        if (world.Registry.valid(e)) world.DestroyEntityAndChildren(e);
    }

    ClearSelection();
    Log::Info("Deleted " + std::to_string(count) + (count == 1 ? " object." : " objects."));
}
void EditorLayer::DuplicateSelection(World& world, AssetLibrary& assets, bool inPlace) {
    if (!HasAnySelection()) return;
    PushUndo(world, "Duplicate");

    std::vector<entt::entity> source;
    if (m_Selected != entt::null) source.push_back(m_Selected);
    for (entt::entity e : m_ExtraSelection) source.push_back(e);

    // Routed through the same save-fragment/append-fragment path Copy/Paste already use (see
    // PasteClipboard above) instead of hand-copying components branch by branch. That old code
    // silently dropped whatever component wasn't in its particular branch's copy list (Collider,
    // Camera, Animator, Tag, Static/Inactive...), never copied HierarchyComponent at all (so a
    // duplicated parent lost its children and a duplicated child came out unparented with its
    // local-space Position reinterpreted as world-space), and offset every copy including
    // children by (1,0,1) instead of only roots. The serializer path already solves all of that
    // correctly for Paste, so Duplicate just reuses it.
    std::string fragment = SceneSerializer::SaveEntitiesToString(world, source);
    std::vector<entt::entity> created;
    std::unordered_map<int, entt::entity> sourceOrder;
    if (fragment.empty() || !SceneSerializer::AppendEntitiesFromString(world, assets, fragment, created, &sourceOrder) || created.empty()) {
        Log::Warn("Duplicate failed: selection could not be copied.");
        return;
    }

    // Build the scene's name set once (not once per duplicated entity) and only offset/rename
    // roots — a duplicated child keeps its original local position and name relative to its
    // (also-duplicated) parent, matching how Paste already treats fragment roots vs. children.
    std::set<std::string> existingNames;
    for (auto e : world.Registry.view<NameComponent>()) existingNames.insert(world.Registry.get<NameComponent>(e).Name);

    for (entt::entity e : created) {
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        bool isRoot = !hier || hier->Parent == entt::null;
        if (!isRoot) continue;
        if (auto* transform = world.Registry.try_get<TransformComponent>(e); transform && !inPlace) {
            transform->Position += glm::vec3(1.0f, 0.0f, 1.0f);
        }
        if (auto* name = world.Registry.try_get<NameComponent>(e)) {
            existingNames.erase(name->Name);
            name->Name = NextDuplicateName(existingNames, name->Name.empty() ? "Object" : name->Name);
        }
    }
    // #119 — the fragment made every copied root a scene root; put copies of a child back under
    // the same parent, as the original's next sibling.
    PlaceCopiesBesideSources(world, source, sourceOrder, /*besideSource=*/true);

    // Select the new duplicates instead of the originals, so you can immediately drag them
    // into place without having to re-pick them from the Hierarchy.
    ClearSelection();
    for (size_t i = 0; i < created.size(); ++i) {
        if (i == 0) m_Selected = created[i];
        else m_ExtraSelection.push_back(created[i]);
    }
    Log::Info("Duplicated " + std::to_string(created.size()) + (created.size() == 1 ? " object." : " objects."));
}

void EditorLayer::DuplicateSelectionArray(World& world, AssetLibrary& assets,
                                          int cx, int cy, int cz, const glm::vec3& step) {
    if (!HasAnySelection()) return;
    cx = std::clamp(cx, 1, 32); cy = std::clamp(cy, 1, 32); cz = std::clamp(cz, 1, 32);
    const int cells = cx * cy * cz;
    if (cells <= 1 || cells > 512) return; // nothing to do, or absurd — bail without an undo entry

    std::vector<entt::entity> source;
    if (m_Selected != entt::null) source.push_back(m_Selected);
    for (entt::entity e : m_ExtraSelection) source.push_back(e);

    const std::string fragment = SceneSerializer::SaveEntitiesToString(world, source);
    if (fragment.empty()) { Log::Warn("Duplicate Array failed: selection could not be copied."); return; }

    PushUndo(world, "Duplicate Array");

    std::set<std::string> existingNames;
    for (auto e : world.Registry.view<NameComponent>()) existingNames.insert(world.Registry.get<NameComponent>(e).Name);

    std::vector<entt::entity> allCreated;
    for (int n = 1; n < cells; ++n) {                 // cell 0 is the original selection
        const int i = n % cx, j = (n / cx) % cy, k = n / (cx * cy);
        const glm::vec3 offset = step * glm::vec3((float)i, (float)j, (float)k);

        std::vector<entt::entity> created;
        std::unordered_map<int, entt::entity> sourceOrder;
        if (!SceneSerializer::AppendEntitiesFromString(world, assets, fragment, created, &sourceOrder) || created.empty())
            continue;
        for (entt::entity e : created) {
            const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
            if (hier && hier->Parent != entt::null) continue; // offset / rename roots only
            if (auto* t = world.Registry.try_get<TransformComponent>(e)) t->Position += offset;
            if (auto* name = world.Registry.try_get<NameComponent>(e)) {
                existingNames.erase(name->Name);
                name->Name = NextDuplicateName(existingNames, name->Name.empty() ? "Object" : name->Name);
                existingNames.insert(name->Name);
            }
        }
        PlaceCopiesBesideSources(world, source, sourceOrder, /*besideSource=*/false); // #119
        allCreated.insert(allCreated.end(), created.begin(), created.end());
    }

    ClearSelection();
    for (size_t i = 0; i < allCreated.size(); ++i) {
        if (i == 0) m_Selected = allCreated[i];
        else m_ExtraSelection.push_back(allCreated[i]);
    }
    Log::Info("Duplicate Array: created " + std::to_string(allCreated.size()) + " object(s).");
}

void EditorLayer::PlaceCopiesBesideSources(World& world, const std::vector<entt::entity>& sources,
                                           const std::unordered_map<int, entt::entity>& sourceOrder,
                                           bool besideSource) {
    const std::set<entt::entity> sourceSet(sources.begin(), sources.end());
    auto parentOf = [&](entt::entity e) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        return h ? h->Parent : entt::null;
    };
    for (entt::entity src : sources) {
        if (!world.Registry.valid(src)) continue;
        // Only fragment roots: a selected entity with a selected ancestor was copied as part of
        // that ancestor's subtree and is already parented correctly.
        bool nested = false;
        for (entt::entity a = parentOf(src); a != entt::null; a = parentOf(a))
            if (sourceSet.count(a)) { nested = true; break; }
        if (nested) continue;

        const auto* order = world.Registry.try_get<OrderComponent>(src);
        if (!order) continue;
        auto it = sourceOrder.find(order->Value);
        if (it == sourceOrder.end() || !world.Registry.valid(it->second)) continue;
        const entt::entity copy = it->second;

        // The fragment stored the root in world space; SetParent keeps that world pose.
        const entt::entity parent = parentOf(src);
        if (parent != entt::null && parentOf(copy) != parent) world.SetParent(copy, parent);
        if (besideSource) ReorderHierarchySiblings(world, {copy}, src, /*after=*/true, /*recordUndo=*/false);
    }
}

void EditorLayer::DrawArrayDuplicateModal(World& world, AssetLibrary& assets) {
    if (!m_ShowArrayDuplicate) return;
    if (!HasAnySelection()) { m_ShowArrayDuplicate = false; return; }

    if (BeginCenteredModal("Duplicate Array##ArrayDup", &m_ShowArrayDuplicate)) {
        ImGui::TextDisabled("Count per axis (the selection is cell 0,0,0)");
        ImGui::PushItemWidth(200.0f * m_UIScale);
        ImGui::InputInt3("Count", m_ArrayDupCount);
        ImGui::InputFloat3("Step (world units)", m_ArrayDupStep, "%.2f");
        ImGui::PopItemWidth();
        for (int& c : m_ArrayDupCount) c = std::clamp(c, 1, 32);

        const long long total = (long long)m_ArrayDupCount[0] * m_ArrayDupCount[1] * m_ArrayDupCount[2];
        ImGui::TextDisabled("%lld new copies", std::max(0LL, total - 1));
        const bool ok = total > 1 && total <= 512;
        if (!ok) ImGui::TextColored(EditorTheme::Warning,
                                    total <= 1 ? "Increase a count above 1." : "Too many (max 512).");
        ImGui::Separator();

        ImGui::BeginDisabled(!ok);
        if (PrimaryButton("Create", ImVec2(110.0f * m_UIScale, 0.0f))) { // #37
            DuplicateSelectionArray(world, assets, m_ArrayDupCount[0], m_ArrayDupCount[1], m_ArrayDupCount[2],
                                    glm::vec3(m_ArrayDupStep[0], m_ArrayDupStep[1], m_ArrayDupStep[2]));
            m_ShowArrayDuplicate = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (PrimaryButton("Cancel", ImVec2(110.0f * m_UIScale, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { // #37
            m_ShowArrayDuplicate = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// The Add-menu body, shared verbatim by the File-menu-bar "Add" menu and the Shift+A quick-add
// popup (ImGui::MenuItem works inside BeginMenu and BeginPopup alike).
void EditorLayer::DrawAddEntityItems(World& world, AssetLibrary& assets, Camera& editorCamera) {
    auto spawnPrimitive = [&](const char* kind, const char* displayName) {
        PushUndo(world, std::string("Create ") + displayName);
        auto model = assets.CreatePrimitive(kind);
        glm::vec3 position = SafeSpawnInFrontOf(editorCamera);
        entt::entity e = world.CreateModelEntity(model, position, glm::vec3(0.0f), glm::vec3(1.0f), UniqueNameFor(world, displayName));
        // Unity parity: GameObject > 3D Object primitives come with a matching collider. Without
        // one a freshly created floor Plane let the Play-mode player fall straight through it.
        ColliderComponent col;
        const glm::vec3 half = model ? (model->BoundsMax() - model->BoundsMin()) * 0.5f : glm::vec3(0.5f);
        const std::string k = kind;
        if (k == "cube") {
            col.Kind = ColliderComponent::Shape::Box; // zero extents = fit the render bounds
        } else if (k == "sphere") {
            col.Kind = ColliderComponent::Shape::Sphere;
            col.HalfExtents = glm::vec3(std::max({half.x, half.y, half.z}), 0.0f, 0.0f);
        } else if (k == "capsule") {
            col.Kind = ColliderComponent::Shape::Capsule;
            const float r = std::max(half.x, half.z);
            col.HalfExtents = glm::vec3(r, std::max(half.y - r, 0.0f), 0.0f);
        } else if (k == "plane" || k == "donut") {
            col.Kind = ColliderComponent::Shape::Mesh;       // Unity: MeshCollider
        } else {
            col.Kind = ColliderComponent::Shape::ConvexHull; // cylinder / cone / pyramid
        }
        if (model) col.Center = (model->BoundsMax() + model->BoundsMin()) * 0.5f;
        if (col.Kind == ColliderComponent::Shape::Box || col.Kind == ColliderComponent::Shape::ConvexHull ||
            col.Kind == ColliderComponent::Shape::Mesh)
            col.Center = glm::vec3(0.0f); // auto-fit / mesh shapes place themselves
        world.Registry.emplace<ColliderComponent>(e, col);
        ApplyDefaultParent(world, e); // vHierarchy D: lands under the scene's default parent, if any
        SelectItem(e, false);
        Log::Info(std::string("Added ") + displayName + ".");
    };
    if (ImGui::MenuItem(ICON_FA_CUBE "  Cube")) spawnPrimitive("cube", "Cube");
    if (ImGui::MenuItem(ICON_FA_CIRCLE "  Sphere")) spawnPrimitive("sphere", "Sphere");
    if (ImGui::MenuItem(ICON_FA_DATABASE "  Cylinder")) spawnPrimitive("cylinder", "Cylinder");
    if (ImGui::MenuItem(ICON_FA_CAPSULES "  Capsule")) spawnPrimitive("capsule", "Capsule");
    if (ImGui::MenuItem(ICON_FA_FILTER "  Cone")) spawnPrimitive("cone", "Cone");
    if (ImGui::MenuItem(ICON_FA_MOUNTAIN "  Pyramid")) spawnPrimitive("pyramid", "Pyramid");
    if (ImGui::MenuItem(ICON_FA_LIFE_RING "  Donut")) spawnPrimitive("donut", "Donut");
    if (ImGui::MenuItem(ICON_FA_SQUARE "  Plane")) spawnPrimitive("plane", "Plane");

    EditorUIPrimitives::SectionHeader("Objects");
    if (ImGui::MenuItem(ICON_FA_DIAGRAM_PROJECT "  Empty")) {
        CreateEmptyAt(world, &editorCamera, "Empty", false);
    }
    // Editor Enhancers / vHierarchy: an empty drawn as a section header in the Hierarchy. It can
    // parent rows like any empty, so it doubles as a folder.
    if (ImGui::MenuItem(ICON_FA_GRIP_LINES "  Separator")) {
        entt::entity e = CreateEmptyAt(world, &editorCamera, "Separator", false);
        HierarchyStyleComponent st;
        st.Separator = true;
        world.Registry.emplace_or_replace<HierarchyStyleComponent>(e, st);
    }
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("A section header row for organising the Hierarchy. Drag objects onto it to group them.");
    if (ImGui::MenuItem(ICON_FA_LIGHTBULB "  Point Light")) {
        CreateEmptyAt(world, &editorCamera, "Point Light", true);
    }
    if (ImGui::MenuItem(ICON_FA_BULLSEYE "  Spot Light")) {
        entt::entity e = CreateEmptyAt(world, &editorCamera, "Spot Light", true);
        world.Registry.get<LightComponent>(e).Kind = LightComponent::Type::Spot;
    }
    if (ImGui::MenuItem(ICON_FA_SUN "  Directional Light")) {
        MakeDirectionalLight(world, CreateEmptyAt(world, &editorCamera, "Directional Light", true));
    }
    if (ImGui::MenuItem(ICON_FA_PERSON "  First Person Player")) {
        // The player root: a First Person Controller (capsule, look, weapon) and a First Person Body. Add the
        // rigged body pieces as its children and an Animator Controller on one of them (Asset Browser > Create >
        // Create Body Locomotion Controller); the body's Setup box lists what is still missing.
        entt::entity e = CreateEmptyAt(world, &editorCamera, "Player Spawn", false);
        world.Registry.emplace_or_replace<FirstPersonControllerComponent>(e);
        world.Registry.emplace_or_replace<FirstPersonBodyComponent>(e);
        SelectItem(e, false);
        Log::Info("Added a Player Spawn with a First Person Controller and Body. Next: add the body pieces as children and a locomotion controller; the Body's Setup box lists what is missing.");
    }
    if (ImGui::MenuItem(ICON_FA_VIDEO "  Camera")) {
        entt::entity e = CreateEmptyAt(world, &editorCamera, "Camera", false);
        world.Registry.emplace<CameraComponent>(e);
        // Aim it back at the world origin so its Game-view preview isn't just black —
        // ComposeTransform rotates Y(yaw) then X(pitch), local -Z is forward.
        auto& t = world.Registry.get<TransformComponent>(e);
        glm::vec3 d = t.Position;
        if (glm::dot(d, d) > 1.0e-4f) {
            d = glm::normalize(-d); // direction from the camera toward the origin
            t.SetRotationEuler(glm::vec3(
                glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))),
                glm::degrees(std::atan2(-d.x, -d.z)),
                0.0f));
        }
    }
}
// Expand / collapse every root's whole subtree at once (audit #71) — the host half of the
// module's Expand-all / Collapse-all buttons.
void EditorLayer::HierarchyExpandAll(World& world, bool open) {
    for (auto e : world.Registry.view<const NameComponent>()) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        if (!h || h->Parent == entt::null) SetHierarchyExpandedRecursive(world, e, open);
    }
}

// The Scene Hierarchy's entity tree — everything below the search row. The module
// (EditorModuleHierarchy.cpp, #229) owns Begin("Scene Hierarchy") + the search box + the two
// Expand/Collapse-all buttons and calls this inside that window scope; the tree, drag-reparent,
// context menus, selection, undo and Ctrl+A stay here (EnTT + Components.h never cross the DLL
// boundary).
void EditorLayer::DrawHierarchyTreeBody(World& world, AssetLibrary& assets) {
    // Filled by FlattenHierarchyRows below, then published to m_HierarchyVisibleOrder just before
    // this function returns. See the header for why the two buffers are separate.
    m_HierarchyVisibleBuild.clear();

    // #236 B — clear the spring-load dwell target whenever there's no row drag in flight, so a
    // stale entity can't auto-expand a folder on the next unrelated drag.
    if (const ImGuiPayload* d = ImGui::GetDragDropPayload(); !d || !d->IsDataType("HIERARCHY_ENTITY"))
        m_HierarchySpringRow = entt::null;

    // Rows start collapsed, so a selection made elsewhere (a Scene-view pick, the Inspector) can sit
    // under a closed parent: open its ancestors, once per new selection, and scroll to it.
    if (m_Selected != m_HierarchyRevealed) {
        m_HierarchyRevealed = m_Selected;
        std::vector<entt::entity> chain; // the selection's ancestors, nearest first
        for (entt::entity w = m_Selected; world.Registry.valid(w);) {
            const auto* h = world.Registry.try_get<HierarchyComponent>(w);
            w = h ? h->Parent : entt::null;
            if (w != entt::null) chain.push_back(w);
        }
        bool opened = false;
        for (size_t depth = 0; depth < chain.size(); ++depth) { // root first
            for (size_t i = chain.size(); i-- > chain.size() - 1 - depth;) ImGui::PushID((int)entt::to_integral(chain[i]));
            ImGuiStorage* st = ImGui::GetStateStorage();
            const ImGuiID id = ImGui::GetID("##node");
            if (st->GetInt(id, 0) == 0) { st->SetInt(id, 1); opened = true; }
            for (size_t i = 0; i <= depth; ++i) ImGui::PopID();
        }
        if (opened) m_HierarchyScrollToEntity = m_Selected;
    }

    // Phase 5 item 6 — a type-filter chip narrows the list exactly like a text filter does: rows
    // that match show flat, ignoring parent/child structure, same as a name/tag search.
    const bool filtering = !m_HierarchyFilter.empty() || EditorSettings::Get().HierarchyTypeFilterMask != 0;

    // Defect #51 — whether the scene has any entities at all, independent of the current filter,
    // so the empty-state message below can tell "nothing in the scene" apart from "nothing
    // matches the search".
    const auto nameView = world.Registry.view<const NameComponent>();
    const bool sceneHasEntities = nameView.begin() != nameView.end();

    // One flat list — every entity in creation order, no "Level Geometry" / "Objects" split.
    // A box, a model, a light and an empty are all just entities; the old grouping only ever
    // added a header to scroll past. LevelGeometryTag survives as an invisible serialization /
    // built-in-collider detail, nothing the Hierarchy shows.
    //
    // Tighter per-level indent than the editor-wide default — this panel is narrow, so a few
    // levels of nesting otherwise push names off the right edge fast (#153).
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 13.0f * m_UIScale);
    // Phase 5 item 6 — calibrated to a 24px row pitch at 1x UI scale: FrameHeight (font size 16 +
    // FramePadding.y*2 = 3*2 = 6, giving 22) plus a 2px gap between rows = 24. Only the vertical
    // FramePadding is touched — horizontal stays the theme default so button/text padding elsewhere
    // in the row is unaffected.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x, 3.0f * m_UIScale));
    // #234 layer 3: a fixed, tight row gap so every row is the same height regardless of content
    // (the SaaS-list look).
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f * m_UIScale));

    // Defect #45 — flatten the expanded/filtered tree into one ordered list first, then let
    // ImGuiListClipper decide which rows are actually worth drawing this frame. Before this, every
    // entity's row was walked and measured every frame regardless of scroll position: invisible at
    // 61 objects, ~4,000 text measurements/frame at 1,000+.
    // Reused member buffer (Editor Enhancers perf pass): clear() keeps its capacity.
    std::vector<HierarchyFlatRow>& flatRows = m_HierFlatRows;
    flatRows.clear();
    std::vector<entt::entity> rootEntities =
        ViewInCreationOrder(world.Registry, world.Registry.view<const NameComponent>());
    ApplyHierarchyDisplaySort(world, rootEntities); // Phase 5 item 6 — display-only, doesn't touch OrderComponent
    for (entt::entity entity : rootEntities) {
        if (!MatchesHierarchyFilter(world, entity)) continue;
        // A parented entity draws nested under its parent, not as a sibling — except while
        // filtering, where the parent may be filtered out, so matches are shown flat instead.
        if (!filtering) {
            const auto* hier = world.Registry.try_get<HierarchyComponent>(entity);
            if (hier && hier->Parent != entt::null) continue;
        }
        FlattenHierarchyRows(world, entity, /*depth=*/0, flatRows);
    }

    // Editor Enhancers / vHierarchy per-frame state, read by every row below.
    const EditorSettings& es = EditorSettings::Get();
    m_HierarchyHoverEntity = entt::null;
    m_HierDefaultParentNow = ResolveDefaultParent(world);
    const bool drawTreeLines = es.HierarchyTreeLines && !filtering;

    // The full list, independent of clipping — Ctrl+A, Shift+Click and keyboard nav all key off
    // this, not off which rows happen to be on-screen this frame.
    m_HierarchyVisibleBuild.reserve(flatRows.size());
    for (const HierarchyFlatRow& row : flatRows) m_HierarchyVisibleBuild.push_back(row.Entity);

    const float rowPitch = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    ImGuiListClipper clipper;
    clipper.Begin((int)flatRows.size(), rowPitch);
    // A keyboard-nav / rename-reveal target may be scrolled off-screen; force its row into this
    // frame's processed range so the SetScrollHereY() call inside DrawHierarchyRowBody still
    // fires — Step() otherwise skips indices outside both the visible window and any included
    // range. Must be called after Begin() (which allocates the clipper's TempData that this
    // writes into — calling it first dereferenced null and crashed the editor on Hierarchy
    // keyboard nav) and before the first Step().
    if (m_HierarchyScrollToEntity != entt::null) {
        for (int i = 0; i < (int)flatRows.size(); ++i) {
            if (flatRows[i].Entity == m_HierarchyScrollToEntity) { clipper.IncludeItemByIndex(i); break; }
        }
    }
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const HierarchyFlatRow& row = flatRows[i];
            if (!world.Registry.valid(row.Entity)) continue;

            // Push this row's full ancestor-to-self ID chain so every ID (rename buffer, drag
            // payload, popups, the "##node" open/closed flag) lands exactly where the old
            // recursive walk would have left it at the equivalent nesting depth. The chain lives
            // in a reused member buffer (it was a fresh vector per visible row per frame).
            std::vector<entt::entity>& chain = m_HierChainScratch;
            chain.clear();
            for (entt::entity w = row.Entity; w != entt::null; ) {
                chain.push_back(w);
                const auto* h = world.Registry.try_get<HierarchyComponent>(w);
                w = h ? h->Parent : entt::null;
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                ImGui::PushID((int)entt::to_integral(*it));
            const size_t pushed = chain.size(); // the row body may reuse m_HierChainScratch

            const ImVec2 rp = ImGui::GetCursorScreenPos();
            const ImVec2 wp = ImGui::GetWindowPos();
            const float rowH = ImGui::GetFrameHeight();
            const float indentW = ImGui::GetStyle().IndentSpacing;
            ImDrawList* rowDl = ImGui::GetWindowDrawList();

            // Alternate rows carry a faint stripe, full width, so a long list is easy to follow
            // (Editor Enhancers: "zebra", now a preference).
            if (es.HierarchyZebra && i % 2 == 1)
                rowDl->AddRectFilled(ImVec2(wp.x, rp.y), ImVec2(wp.x + ImGui::GetWindowWidth(), rp.y + rowH),
                                     EditorTheme::U32(EditorTheme::Stripe));

            // vHierarchy row colour: a wash from the row's own indent to the panel edge, flat or
            // fading out to the right. Painted before the row so the selection highlight sits on top.
            const auto* style = es.HierarchyRowStyles ? world.Registry.try_get<HierarchyStyleComponent>(row.Entity) : nullptr;
            if (style && style->Color != 0 && style->FillMode != HierarchyStyleComponent::None && !style->Separator) {
                const float x0 = rp.x + row.Depth * indentW;
                const float x1 = wp.x + ImGui::GetWindowWidth();
                const ImU32 rgb = style->Color & 0x00FFFFFFu;
                if (style->FillMode == HierarchyStyleComponent::Flat) {
                    rowDl->AddRectFilled(ImVec2(x0, rp.y), ImVec2(x1, rp.y + rowH), rgb | 0x3A000000u, EditorTheme::Px(2.0f));
                } else {
                    const float mid = x0 + (x1 - x0) * 0.75f;
                    rowDl->AddRectFilledMultiColor(ImVec2(x0, rp.y), ImVec2(mid, rp.y + rowH),
                                                   rgb | 0x60000000u, rgb, rgb, rgb | 0x60000000u);
                }
            }

            // Tree lines: one vertical guide per ancestor level that still has rows below, plus
            // this row's own elbow. All from the ContinueMask FlattenHierarchyRows computed.
            if (drawTreeLines && row.Depth > 0) {
                const float chevC = ImGui::GetFontSize() * 0.55f; // centre of a row's chevron slot
                const float yTop = rp.y - ImGui::GetStyle().ItemSpacing.y;
                const float yMid = rp.y + rowH * 0.5f;
                const float yBot = rp.y + rowH;
                const ImU32 lc = EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Dim, 0.55f));
                const int depth = std::min(row.Depth, 32);
                for (int k = 0; k < depth - 1; ++k)
                    if (row.ContinueMask & (1u << k)) {
                        const float x = std::floor(rp.x + k * indentW + chevC) + 0.5f;
                        rowDl->AddLine(ImVec2(x, yTop), ImVec2(x, yBot), lc);
                    }
                const float x = std::floor(rp.x + (depth - 1) * indentW + chevC) + 0.5f;
                const bool more = (row.ContinueMask & (1u << (depth - 1))) != 0;
                rowDl->AddLine(ImVec2(x, yTop), ImVec2(x, more ? yBot : yMid), lc);
                rowDl->AddLine(ImVec2(x, std::floor(yMid) + 0.5f), ImVec2(rp.x + depth * indentW + ImGui::GetFontSize() * 0.2f, std::floor(yMid) + 0.5f), lc);
            }

            if (row.Depth > 0) ImGui::Indent(row.Depth * indentW);
            DrawHierarchyRowBody(world, assets, row.Entity, /*isFirstRow=*/i == 0);
            if (row.Depth > 0) ImGui::Unindent(row.Depth * indentW);

            for (size_t p = 0; p < pushed; ++p) ImGui::PopID();
        }
    }
    ImGui::PopStyleVar(); // ItemSpacing
    ImGui::PopStyleVar(); // FramePadding
    ImGui::PopStyleVar(); // IndentSpacing

    // Defect #51 — a genuinely empty scene rendered nothing at all: no icon, no message, no
    // action. Give it something to say, distinct from "the search matched nothing" (also blank
    // before this fix, per the audit's §3.3F finding).
    if (m_HierarchyVisibleBuild.empty()) {
        ImGui::Spacing();
        if (!sceneHasEntities) {
            ImGui::TextDisabled("Scene is empty");
            ImGui::Spacing();
            ImGui::TextUnformatted("Nothing has been added to this scene yet.");
            ImGui::Spacing();
            ImGui::TextDisabled("Tip: press " ICON_FA_KEYBOARD " Shift+A in the viewport to add an object.");
        } else {
            ImGui::TextDisabled("No matches");
            ImGui::Spacing();
            ImGui::TextUnformatted("No entity in this scene matches the current search.");
        }
    }

    // Auto-scroll while a row is being dragged near the panel's top/bottom edge — otherwise you
    // can only drop among the rows that happen to be on screen when the drag starts.
    if (const ImGuiPayload* drag = ImGui::GetDragDropPayload();
        drag && drag->IsDataType("HIERARCHY_ENTITY") && ImGui::GetScrollMaxY() > 0.0f) {
        const float my     = ImGui::GetIO().MousePos.y;
        const float top    = ImGui::GetWindowPos().y;
        const float bottom = top + ImGui::GetWindowSize().y;
        const float margin = 26.0f * m_UIScale;
        float over = 0.0f;
        if (my < top + margin)         over = my - (top + margin);      // negative -> scroll up
        else if (my > bottom - margin) over = my - (bottom - margin);   // positive -> scroll down
        if (over != 0.0f) {
            const float speed = 14.0f * m_UIScale; // px per frame at the edge, ramps with overshoot
            ImGui::SetScrollY(ImGui::GetScrollY() + (over > 0.0f ? 1.0f : -1.0f) *
                              std::min(std::abs(over) / margin, 2.0f) * speed);
        }
    }

    // Dropping onto empty space below the tree un-parents (Unity's "drag to the root") — and
    // right-clicking there opens the create/paste menu.
    // Dummy needs a real size (negative width isn't valid here), so this claims whatever space
    // is left below the tree as one big drop/right-click zone.
    ImVec2 remaining = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(remaining.x > 0.0f ? remaining.x : 1.0f, remaining.y > 0.0f ? remaining.y : 1.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
            entt::entity dragged = *(const entt::entity*)payload->Data;
            if (world.Registry.valid(dragged)) {
                // Dragging a row that's part of the current multi-selection un-parents the whole
                // selection, not just the one row the mouse happened to grab (#220).
                std::vector<entt::entity> toUnparent = IsSelected(dragged) ? GetSelectedItems()
                                                                            : std::vector<entt::entity>{dragged};
                StageUndo(world);
                for (entt::entity e : toUnparent) {
                    if (world.Registry.valid(e)) world.SetParent(e, entt::null);
                }
                CommitStagedUndo(world, "Reparent");
            }
        }
        // Model / prefab from the Asset Browser dropped below the tree -> instantiate at the root.
        const ImGuiPayload* mdl = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH");
        const ImGuiPayload* pfb = mdl ? nullptr : ImGui::AcceptDragDropPayload("ASSET_PREFAB_PATH");
        if (mdl || pfb) {
            InstantiateAssetDropInHierarchy(world, assets,
                mdl ? (const char*)mdl->Data : nullptr,
                pfb ? (const char*)pfb->Data : nullptr, entt::null);
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##HierarchyEmptyContext")) {
        DrawHierarchyContextMenu(world, assets, entt::null);
        ImGui::EndPopup();
    }

    // Publish the row order this pass built, for next frame's click handlers (and the Ctrl+A
    // check just below, which runs after every row is in).
    m_HierarchyVisibleOrder = m_HierarchyVisibleBuild;

    // Ctrl+A — select every visible row, matching Unity's Hierarchy shortcut. Gated on the
    // panel (or one of its children) being focused, and skipped while a text field here has
    // the keyboard (so Ctrl+A still means "select all text" in the search box).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        SelectAllVisibleInHierarchy();
    }
    // Editor Enhancers / vHierarchy hover keys (E, Shift+E, Ctrl+Shift+E, A, F, X, D) on the row
    // under the mouse - before type-to-select, which skips the frame when one of them acted.
    HandleHierarchyHoverKeys(world);
    // Arrow / Home / End / type-to-select nav over the same visible-row list.
    HandleHierarchyKeyboardNav(world);
    // ImGui::End() for the "Scene Hierarchy" window is the module's — it owns Begin() now.
}

bool EditorLayer::SceneSearchActive() const {
    return !m_HierarchyFilter.empty() || EditorSettings::Get().HierarchyTypeFilterMask != 0;
}

bool EditorLayer::MatchesHierarchyFilter(const World& world, entt::entity entity) const {
    // Phase 5 item 6 — the type-filter chips apply first and independently of the text filter
    // below; a row must satisfy both to show.
    const int typeMask = EditorSettings::Get().HierarchyTypeFilterMask;
    if (typeMask != 0 && (HierarchyKindMask(world.Registry, entity) & typeMask) == 0)
        return false;

    if (m_HierarchyFilter.empty()) return true;

    if (m_HierarchyFilter.rfind("t:", 0) == 0) {
        std::string wanted = m_HierarchyFilter.substr(2);
        if (wanted.empty()) return true;
        const auto* tag = world.Registry.try_get<TagComponent>(entity);
        return MatchesFilter(wanted, tag ? tag->Tag : std::string("Untagged"));
    }

    const auto* name = world.Registry.try_get<NameComponent>(entity);
    return MatchesFilter(m_HierarchyFilter, name ? name->Name : std::string());
}

void EditorLayer::FlattenHierarchyRows(World& world, entt::entity entity, int depth,
                                        std::vector<HierarchyFlatRow>& out, std::uint32_t continueMask) {
    if (!world.Registry.valid(entity)) return;
    out.push_back({entity, depth, continueMask});

    // Mirrors the pre-Defect-#45 recursive walk's one deliberate quirk: don't descend into a
    // row's children while it's being renamed, keeping the inline edit field stable for the one
    // frame it's open.
    if (entity == m_RenamingEntity) return;

    const auto* hier = world.Registry.try_get<HierarchyComponent>(entity);
    bool hasChildren = hier && !hier->Children.empty() &&
        m_HierarchyFilter.empty() && EditorSettings::Get().HierarchyTypeFilterMask == 0;
    if (!hasChildren) return;

    // Same PushID(entity)/"##node" lookup DrawHierarchyRowBody uses when it actually draws this
    // row — both sides must agree on the open/closed flag's ID.
    ImGui::PushID((int)entt::to_integral(entity));
    bool open = ImGui::GetStateStorage()->GetInt(ImGui::GetID("##node"), 0) != 0;
    if (open) {
        // Sorted by OrderComponent (not raw HierarchyComponent::Children insertion order) so
        // sibling reordering shows.
        std::vector<entt::entity> children = HierarchySiblingsInOrder(world, entity);
        // Phase 5 item 6 — display-only re-sort; HierarchySiblingsInOrder's own OrderComponent
        // order is left untouched, since ReorderHierarchySiblings and drag-drop key off it.
        ApplyHierarchyDisplaySort(world, children);
        // Tree-line mask for each child (Enhancers::TreeLineChildMask): the last child drawn ends
        // this level's guide.
        size_t lastValid = children.size();
        for (size_t c = children.size(); c-- > 0;)
            if (world.Registry.valid(children[c])) { lastValid = c; break; }
        for (size_t c = 0; c < children.size(); ++c) {
            if (!world.Registry.valid(children[c])) continue;
            FlattenHierarchyRows(world, children[c], depth + 1, out,
                                 Enhancers::TreeLineChildMask(continueMask, depth, c == lastValid));
        }
    }
    ImGui::PopID();
}

void EditorLayer::DrawHierarchyRowBody(World& world, AssetLibrary& assets, entt::entity entity,
                                        bool isFirstRow) {
    if (!world.Registry.valid(entity)) return;

    auto& name = world.Registry.get<NameComponent>(entity);
    bool selected = IsSelected(entity);
    bool inactive = world.Registry.all_of<InactiveTag>(entity);             // in hierarchy (greyed)
    bool selfDeactivated = world.Registry.all_of<DeactivatedTag>(entity);   // its own checkbox (#201)
    // #236 A2 — a prefab-instance root paints its name in prefab blue (amber-red when missing).
    const auto* prefabInst = world.Registry.try_get<PrefabInstanceComponent>(entity);
    // #236 B — SceneVis-lite: a row hidden in the Scene view reads like an inactive one (dimmed).
    bool sceneHiddenRow = world.Registry.all_of<HiddenInSceneTag>(entity);
    const auto* hier = world.Registry.try_get<HierarchyComponent>(entity);
    bool hasChildren = hier && !hier->Children.empty() &&
        m_HierarchyFilter.empty() && EditorSettings::Get().HierarchyTypeFilterMask == 0;

    // Defect #45 — the caller (DrawHierarchyTreeBody) already pushed this row's full
    // ancestor-to-self ID chain (ending with this entity's own PushID) before calling here, so
    // every ID below lands exactly where the old recursive DrawHierarchyNode would have left it.

    // The active-state eye toggle used to lead every row, so it indented with tree depth. It now
    // sits in a fixed right-hand column (drawn after the row below), Unity-style — one straight
    // file of eyes regardless of nesting.

    // Inline rename (F2 / context menu) replaces the row with an edit field in place.
    if (m_RenamingEntity == entity) {
        ImGui::SetNextItemWidth(-1.0f);
        if (m_EntityRenameJustStarted) {
            ImGui::SetKeyboardFocusHere();
            snprintf(m_EntityRenameBuffer, sizeof(m_EntityRenameBuffer), "%s", name.Name.c_str());
            m_EntityRenameJustStarted = false;
        }
        bool submitted = ImGui::InputText("##Rename", m_EntityRenameBuffer, sizeof(m_EntityRenameBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        // Captured now, while the InputText is still the last item — used below to anchor the
        // rejected-flash tooltip near the field itself rather than wherever the mouse happens to
        // be (the user just pressed Enter; the cursor is often nowhere near this row).
        const ImVec2 renameFieldMin = ImGui::GetItemRectMin();
        const ImVec2 renameFieldMax = ImGui::GetItemRectMax();
        bool rejected = false;
        if (submitted) {
            std::string sanitized = SanitizeEntityName(m_EntityRenameBuffer);
            if (sanitized.empty()) {
                // Blank (or whitespace-only) name — reject rather than silently leaving the
                // entity with an empty label (#38 B12). Keep the field open with the raw text
                // still in it and flash an inline error instead of committing or closing.
                rejected = true;
                m_EntityRenameRejectedFlash = 1.6f;
            } else {
                PushUndo(world, "Rename");
                name.Name = sanitized;
                m_RenamingEntity = entt::null;
                m_EntityRenameRejectedFlash = 0.0f;
            }
        }
        // Deactivated without the Enter branch above committing (blur / Escape) - cancel. Not
        // when this Enter press was the one that just got rejected, or the field would close
        // the same frame the error fires, undoing the "keep it open to fix" behaviour above.
        if (!rejected && ImGui::IsItemDeactivated() && m_RenamingEntity != entt::null) {
            m_RenamingEntity = entt::null;
            m_EntityRenameRejectedFlash = 0.0f;
        }
        if (m_EntityRenameRejectedFlash > 0.0f) {
            m_EntityRenameRejectedFlash -= ImGui::GetIO().DeltaTime;
            // Anchored to the rename field (just below it), not the mouse cursor: this fires
            // right after an Enter-key submit, when the mouse is frequently nowhere near the
            // row, unlike a normal hover tooltip. EditorUI::SetTooltip() isn't usable here either
            // way — it's gated on ImGui::IsItemHovered(), which this isn't.
            ImGui::SetNextWindowPos(ImVec2(renameFieldMin.x, renameFieldMax.y + 4.0f));
            ImGui::BeginTooltip();
            ImGui::TextColored(EditorTheme::Danger, "Name can't be blank.");
            ImGui::EndTooltip();
        }
        return; // children stay collapsed for the one frame a rename is open — deliberate, keeps the field stable
    }

    // Kind badge: ONE primary glyph (mesh > light > camera > empty priority) drawn in a fixed-
    // width leading slot so a name's left edge is identical on every row no matter how many kinds
    // it has (#153). A mesh that also carries a light still reads as both — the second kind shows
    // as a small badge inside that same slot (#27 P16) — without shifting the name; a rare third
    // kind is dropped (the Inspector lists every component anyway).
    const bool hasMesh   = world.Registry.all_of<RenderableComponent>(entity);
    const bool hasLight  = world.Registry.all_of<LightComponent>(entity);
    const bool hasCamera = world.Registry.all_of<CameraComponent>(entity);
    // Solid, literal glyphs beat the old abstract ones: a filled cube for a mesh (not a wireframe
    // polygon), a stacked layer-group for an empty that parents other rows (it's acting as a
    // folder), a plain bounding-box locator for a childless empty (not the org-chart node).
    const EditorSettings& es = EditorSettings::Get();
    // Editor Enhancers / vHierarchy row style: a custom icon replaces the kind glyph, a colour
    // tints it (the row wash itself was painted by DrawHierarchyTreeBody), and a separator row is
    // drawn as a section header with no glyph, minimap or toggles.
    const auto* rowStyle = es.HierarchyRowStyles ? world.Registry.try_get<HierarchyStyleComponent>(entity) : nullptr;
    const bool isSeparator = rowStyle && rowStyle->Separator;
    const char* customGlyph = (rowStyle && !rowStyle->Icon.empty()) ? Enhancers::FAIconGlyph(rowStyle->Icon.c_str()) : nullptr;
    const char* primaryGlyph =
        customGlyph ? customGlyph :
        hasMesh     ? ICON_FA_CUBE          :
        hasLight    ? ICON_FA_LIGHTBULB     :
        hasCamera   ? ICON_FA_VIDEO         :
        hasChildren ? ICON_FA_LAYER_GROUP   : ICON_FA_VECTOR_SQUARE;
    // Minimal mode: no glyph unless the user gave the row one.
    const bool drawGlyph = !isSeparator && (customGlyph || !es.HierarchyMinimal);
    const char* secondaryGlyph =
        (hasMesh && hasLight)   ? ICON_FA_LIGHTBULB :
        (hasMesh && hasCamera)  ? ICON_FA_VIDEO     :
        (hasLight && hasCamera) ? ICON_FA_VIDEO     : nullptr;
    // Muted per-kind tint so kinds separate at a glance without the panel turning to confetti —
    // amber light, blue camera (the #234 accent roles); mesh/empty stay near the text colour
    // since they're the bulk of every scene. Overridden to the disabled grey on inactive rows.
    const ImU32 kindCol = (rowStyle && rowStyle->Color != 0) ? (rowStyle->Color | 0xFF000000u) : EditorTheme::U32(
        hasLight  ? EditorTheme::KindLight :
        hasCamera ? EditorTheme::KindCamera :
        hasMesh   ? EditorTheme::KindMesh : EditorTheme::Secondary);

    // Never-named entities get a positional fallback instead of a wall of identical
    // "(unnamed)" rows (#21 P10).
    std::string shownName = name.Name;
    if (shownName.empty())
        shownName = "Object " + std::to_string(HierarchyCreationOrdinal(world, entity));

    // vHierarchy component minimap: the icons of this row's reflected components (up to the
    // preference's max, then "+N"), right-aligned just left of the eye/lock/active column. One
    // Has() per registered type for visible rows only - the clipper keeps that to ~50 rows.
    int miniIdx[12];
    int miniCount = 0, miniExtra = 0;
    if (es.HierarchyMinimap && !isSeparator) {
        const int maxIcons = std::clamp(es.HierarchyMinimapMax, 1, 12);
        const auto& all = ComponentRegistry::All();
        for (int ci = 0; ci < (int)all.size(); ++ci) {
            if (!all[(size_t)ci].Has(world.Registry, entity)) continue;
            if (miniCount < maxIcons) miniIdx[miniCount++] = ci;
            else ++miniExtra;
        }
    }

    // Only feeds the drag preview below — the row paints its own glyph + name after the node.
    std::string label = std::string(primaryGlyph) + "  " + shownName;

    // Every row is a Leaf (NoTreePushOnOpen) so ImGui never draws its chunky filled-triangle
    // arrow or auto-toggles — the disclosure control is a hand-drawn Font Awesome chevron below,
    // and open/closed state is our own int in the window's state storage (the same slot
    // SetHierarchyExpandedRecursive writes, so Expand/Collapse-all and the Alt cascade still work).
    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanAvailWidth |
        ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
        ImGuiTreeNodeFlags_AllowOverlap | // so the right-column eye below can take its own clicks
        (selected ? ImGuiTreeNodeFlags_Selected : 0);

    const ImGuiID nodeStateId = ImGui::GetID("##node");
    bool open = hasChildren && ImGui::GetStateStorage()->GetInt(nodeStateId, 0) != 0;

    // Empty label: the tree node owns the indent / full-row hitbox and every interaction handler
    // below; the chevron + glyph slot + name are painted afterward at a constant X.
    // NoNav (Defect #43): HandleHierarchyKeyboardNav() already owns arrow/Home/End/type-ahead for
    // this list — without this flag, ImGui's own keyboard nav would also try to move focus between
    // rows on the same keypress once NavEnableKeyboard is on.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::TreeNodeEx("##node", nodeFlags, "%s", "");
    ImGui::PopItemFlag();
    if (EditorTestProbeActive())
        if (const auto* tn = world.Registry.try_get<NameComponent>(entity)) EditorTestTag(("row:" + tn->Name).c_str());
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();

    // Keyboard nav asked to reveal this row last frame — bring it into view, once.
    if (entity == m_HierarchyScrollToEntity) {
        ImGui::SetScrollHereY(0.5f);
        m_HierarchyScrollToEntity = entt::null;
    }

    // The chevron's hit box: the leading ~1.1em of the row. A click there toggles this row's
    // subtree (Alt = cascade to every descendant); a click anywhere else selects.
    const float arrowSlotW = ImGui::GetFontSize() * 1.1f;
    const bool rowHovered = ImGui::IsItemHovered();
    const bool clickOnArrow = hasChildren && rowHovered &&
        ImGui::GetIO().MousePos.x < rowMin.x + arrowSlotW;
    // The row's right cluster — SceneVis eye + lock + the Active checkbox — is drawn below via
    // SetCursorScreenPos over the full-width node, so its InvisibleButtons and the node both see
    // the same click. Carve the cluster's whole X span out of the row's click / double-click
    // handling so a click anywhere on an icon is that icon's alone and never also selects (#236 B).
    const float rowIconsBandW = isSeparator ? 0.0f : ImGui::GetFontSize() * 3.4f + 20.0f * m_UIScale;
    // The minimap sits immediately left of that band; its icons take their own clicks too.
    const float miniCellW = ImGui::GetFontSize() * 1.05f;
    const float miniPlusW = miniExtra > 0 ? ImGui::GetFontSize() * 1.6f : 0.0f;
    const float miniW = miniCount > 0 ? miniCount * miniCellW + miniPlusW + EditorTheme::Px(4.0f) : 0.0f;
    const float miniRight = rowMax.x - rowIconsBandW - EditorTheme::Px(2.0f);
    const float miniLeft = miniRight - miniW;
    const float mouseX = ImGui::GetIO().MousePos.x;
    const bool overMinimap = rowHovered && miniCount > 0 && mouseX >= miniLeft && mouseX < miniRight;
    const bool overRowIcons = rowHovered && (mouseX > rowMax.x - rowIconsBandW || overMinimap);
    int hoveredMini = -1; // index into miniIdx, or miniCount for the "+N" chip
    if (overMinimap) {
        const float rel = mouseX - (miniLeft + EditorTheme::Px(4.0f));
        hoveredMini = rel < 0.0f ? -1 : std::min((int)(rel / miniCellW), miniCount);
    }
    if (rowHovered) m_HierarchyHoverEntity = entity; // hover keys act on this row

    if (ImGui::IsItemClicked() && clickOnArrow) {
        open = !open;
        ImGui::GetStateStorage()->SetInt(nodeStateId, open ? 1 : 0);
        if (ImGui::GetIO().KeyAlt) {
            for (entt::entity child : hier->Children) SetHierarchyExpandedRecursive(world, child, open);
        }
    } else if (ImGui::IsItemClicked() && overMinimap && hoveredMini >= 0 && hoveredMini < miniCount) {
        // Minimap icon: select the row and bring that component's Inspector section up; Alt
        // opens it in its own floating window instead (vHierarchy's mini inspector).
        const RegisteredComponent& rc = ComponentRegistry::All()[(size_t)miniIdx[hoveredMini]];
        if (ImGui::GetIO().KeyAlt) {
            OpenPinnedComponent(world, entity, rc.Meta.Name);
        } else {
            if (!IsSelected(entity) || HasGroupSelection()) SelectItem(entity, false);
            m_InspectorScrollToComponent = rc.Meta.Name;
            m_InspectorScrollToFrame = ImGui::GetFrameCount();
        }
    } else if (ImGui::IsItemClicked() && !overRowIcons) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift) {
            // Shift (or Ctrl+Shift) — contiguous range from the anchor to this row.
            SelectHierarchyRange(world, entity, io.KeyCtrl);
        } else {
            SelectItem(entity, io.KeyCtrl); // plain click replaces; Ctrl+Click toggles. Both re-anchor.
        }
        m_HierarchyRowHintDone = true; // learned the row interaction — stop showing the hint
    }
    if (ImGui::IsItemHovered() && !clickOnArrow && !overRowIcons && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        BeginRenameEntity(entity);
    }
    if (overMinimap && hoveredMini >= 0) {
        if (hoveredMini < miniCount) {
            EditorUI::SetTooltip("%s\nClick: show in the Inspector.  Alt+click: open in its own window.",
                                 ComponentRegistry::All()[(size_t)miniIdx[hoveredMini]].Meta.Name);
        } else {
            std::string rest;
            int skipped = 0;
            for (const auto& rc : ComponentRegistry::All()) {
                if (!rc.Has(world.Registry, entity)) continue;
                if (skipped++ < miniCount) continue;
                rest += rest.empty() ? "" : "\n";
                rest += rc.Meta.Name;
            }
            EditorUI::SetTooltip("%s", rest.c_str());
        }
    } else if (!m_HierarchyRowHintDone && ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        EditorUI::SetTooltip("Click to select (Ctrl+Click to add/remove, Shift+Click for a range, Ctrl+A for all).\nDouble-click or F2 to rename. Drag onto a row to parent it, or between rows to reorder.\nRight-click for more options.");
    }

    if (ImGui::BeginPopupContextItem("##RowContext")) {
        if (!IsSelected(entity)) SelectItem(entity, false);
        DrawHierarchyContextMenu(world, assets, entity);
        ImGui::EndPopup();
    }

    // Drag a row onto another row to re-parent it (Unity's core Hierarchy gesture).
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
        ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &entity, sizeof(entt::entity));
        // Dragging a row that's part of a multi-selection carries (and will re-parent) the whole
        // selection — the preview says so instead of naming just the one row under the mouse.
        size_t dragCount = IsSelected(entity) ? GetSelectedItems().size() : 1;
        if (dragCount > 1) ImGui::Text("%d objects", (int)dragCount);
        else ImGui::Text("%s", label.c_str());
        ImGui::EndDragDropSource();
    }
    // Row drop zone for reordering / reparenting. Deliberately NOT ImGui's BeginDragDropTarget
    // (that only reacts over the text line, leaving the padding around each name dead). One
    // row-pitch tall, centred on the name, so zones tile with no gaps or overlap:
    //   - middle 60% of the pitch = parent onto this row;
    //   - the 40% below it = a single "insert between this row and the next" strip, owned only
    //     by the upper row (so a boundary has ONE reactive spot, not the old two abutting ones);
    //   - the first visible row additionally gets an "insert above" strip so the very top is
    //     reachable.
    // The hovered strip fills with a solid block so the whole reactive area is visible.
    if (const ImGuiPayload* drag = ImGui::GetDragDropPayload();
        drag && drag->IsDataType("HIERARCHY_ENTITY")) {
        const float midY  = (rowMin.y + rowMax.y) * 0.5f;
        const float pitch = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
        const float half  = pitch * 0.5f;
        const float pInset = pitch * 0.30f;          // parent zone = +/- this around the name
        const float zoneTop = isFirstRow ? midY - half : midY - pInset;
        if (ImGui::IsMouseHoveringRect(ImVec2(rowMin.x, zoneTop), ImVec2(rowMax.x, midY + half), /*clip=*/false)) {
            const float my = ImGui::GetIO().MousePos.y;
            const int zone = my > midY + pInset ? 1                    // below the name -> insert after
                           : (isFirstRow && my < midY - pInset) ? -1  // above the first name -> insert before
                           : 0;                                        // on the name -> parent onto

            // #236 B — spring-loaded folders: hover the parent zone of a collapsed row with
            // children for ~0.5s while dragging and it opens itself, so you can drop deep in a
            // tree without a separate expand click.
            if (zone == 0 && hasChildren && !open) {
                const double now = ImGui::GetTime();
                if (m_HierarchySpringRow != entity) { m_HierarchySpringRow = entity; m_HierarchySpringSince = now; }
                else if (now - m_HierarchySpringSince > 0.5) {
                    ImGui::GetStateStorage()->SetInt(nodeStateId, 1);
                    open = true;
                    m_HierarchySpringRow = entt::null;
                }
            } else if (m_HierarchySpringRow == entity) {
                m_HierarchySpringRow = entt::null;
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 accent = EditorTheme::U32(EditorTheme::Accent);
            const ImU32 fill   = EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Accent, 0.16f));
            float y0, y1, edge;
            if (zone == 0)      { y0 = midY - pInset; y1 = midY + pInset; edge = -1.0f; }
            else if (zone > 0)  { y0 = midY + pInset; y1 = midY + half;   edge = y0 + 1.0f; }
            else                { y0 = midY - half;   y1 = midY - pInset; edge = y1 - 1.0f; }
            dl->AddRectFilled(ImVec2(rowMin.x, y0), ImVec2(rowMax.x, y1), fill);
            if (edge < 0.0f) dl->AddRect(ImVec2(rowMin.x, y0), ImVec2(rowMax.x, y1), accent, 3.0f, 0, 2.0f);
            else             dl->AddLine(ImVec2(rowMin.x, edge), ImVec2(rowMax.x, edge), accent, 2.0f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                entt::entity dragged = *(const entt::entity*)drag->Data;
                if (world.Registry.valid(dragged) && dragged != entity) {
                    // A multi-selected dragged row carries the whole selection (#220).
                    std::vector<entt::entity> moving = IsSelected(dragged) ? GetSelectedItems()
                                                                           : std::vector<entt::entity>{dragged};
                    if (zone == 0) {
                        StageUndo(world);
                        // SetParent refuses cycles and Collider-bearing children on its own; report the
                        // refusal rather than silently doing nothing, so the gesture never looks broken.
                        bool anyFailed = false;
                        for (entt::entity e : moving) {
                            if (!world.Registry.valid(e) || e == entity) continue;
                            if (!world.SetParent(e, entity)) anyFailed = true;
                        }
                        CommitStagedUndo(world, "Reparent");
                        if (anyFailed) {
                            Log::Warn("Can't parent that: level geometry has a collider that needs world-space "
                                      "coordinates, or the target is already a child of the dragged object.");
                        }
                    } else {
                        ReorderHierarchySiblings(world, moving, entity, zone > 0);
                    }
                }
            }
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        // Drop a model / prefab from the Asset Browser onto a row to instantiate it as a child
        // of that row (#236) — the Hierarchy counterpart of dragging into the viewport.
        const ImGuiPayload* mdl = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH");
        const ImGuiPayload* pfb = mdl ? nullptr : ImGui::AcceptDragDropPayload("ASSET_PREFAB_PATH");
        if (mdl || pfb) {
            InstantiateAssetDropInHierarchy(world, assets,
                mdl ? (const char*)mdl->Data : nullptr,
                pfb ? (const char*)pfb->Data : nullptr, entity);
        }
        // A material (slot 0), a texture (slot 0's albedo), a sound, an Animator Controller, a
        // weapon definition or a script onto the row's object - the viewport's drop, minus the
        // slot under the cursor (EditorLayer_AssetDrop.cpp).
        for (const char* type : {"ASSET_MATERIAL_PATH", "ASSET_TEXTURE_PATH", "ASSET_SOUND_PATH", "ASSET_FILE_PATH"}) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type))
                ApplyAssetDrop(world, assets, entity, type, (const char*)payload->Data, 0, glm::vec3(0.0f));
        }
        ImGui::EndDragDropTarget();
    }

    // Paint the kind glyph(s) + name at a fixed X off the row's left edge — see the primaryGlyph
    // comment above. Drawn straight into the window draw list so it never becomes "the last item"
    // and disturb the interaction handlers, selection rect or drag/drop that key off the node.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float fontSize = ImGui::GetFontSize();
        const float slotW    = fontSize * 1.45f;
        // Pull the kind glyph + name in toward the eye (#152 follow-up). Parent rows keep enough
        // lead for the disclosure chevron; leaf rows (no chevron) only need a hair of separation.
        const float labelX   = rowMin.x + (hasChildren ? fontSize * 1.15f : fontSize * 0.35f);
        ImU32 col = EditorTheme::U32((inactive || sceneHiddenRow) ? EditorTheme::Dim : EditorTheme::Text);
        if (prefabInst && !inactive && !sceneHiddenRow)
            col = EditorTheme::U32(prefabInst->Missing ? EditorTheme::KindPrefabBroken : EditorTheme::KindPrefab);
        // The selected row: an accent bar at its left edge over the theme's accent selection wash.
        if (selected)
            dl->AddRectFilled(ImVec2(ImGui::GetWindowPos().x, rowMin.y), ImVec2(ImGui::GetWindowPos().x + EditorTheme::Px(2.0f), rowMax.y),
                              EditorTheme::U32(EditorTheme::Accent));
        // The name never runs under the minimap or the eye / lock / active column on the right.
        dl->PushClipRect(ImVec2(rowMin.x, rowMin.y), ImVec2((miniCount > 0 ? miniLeft : rowMax.x - rowIconsBandW) - EditorTheme::Px(4.0f), rowMax.y), true);

        // Disclosure chevron — a light Font Awesome ">" / "v" (0.66em) centred in the leading
        // slot, in the dim text colour, brightening on arrow-hover. Replaces ImGui's chunky
        // filled triangle.
        if (hasChildren) {
            const float chSize = fontSize * 0.66f;
            const char* chev = open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT;
            const ImVec2 cm = ImGui::GetFont()->CalcTextSizeA(chSize, FLT_MAX, 0.0f, chev);
            const ImU32 chCol = EditorTheme::U32(clickOnArrow ? EditorTheme::Text : EditorTheme::Dim);
            dl->AddText(ImGui::GetFont(), chSize,
                        ImVec2(rowMin.x + (fontSize * 1.1f - cm.x) * 0.5f,
                               rowMin.y + (ImGui::GetFrameHeight() - cm.y) * 0.5f),
                        chCol, chev);
        }
        // Editor Enhancers / vHierarchy separator: a section header - the name in small caps-
        // tracked text centred between two hairlines, in the row colour when it has one.
        const float nameX = drawGlyph ? labelX + slotW : labelX;
        if (isSeparator) {
            const ImU32 sc = (rowStyle->Color != 0 && !inactive) ? (rowStyle->Color | 0xFF000000u) : EditorTheme::U32(EditorTheme::Secondary);
            std::string caps = shownName;
            for (char& c : caps) c = (char)std::toupper((unsigned char)c);
            EditorTheme::PushSmall();
            const ImVec2 ts = ImGui::CalcTextSize(caps.c_str());
            const float x0 = labelX, x1 = rowMax.x - EditorTheme::Px(6.0f);
            const float cx = std::max(x0, (x0 + x1 - ts.x) * 0.5f);
            const float cy = (rowMin.y + rowMax.y) * 0.5f;
            const float pad = EditorTheme::Px(6.0f);
            const ImU32 lineCol = (sc & 0x00FFFFFFu) | 0x70000000u;
            if (cx - pad > x0) dl->AddLine(ImVec2(x0, std::floor(cy) + 0.5f), ImVec2(cx - pad, std::floor(cy) + 0.5f), lineCol);
            if (cx + ts.x + pad < x1) dl->AddLine(ImVec2(cx + ts.x + pad, std::floor(cy) + 0.5f), ImVec2(x1, std::floor(cy) + 0.5f), lineCol);
            dl->AddText(ImVec2(cx, cy - ts.y * 0.5f), sc, caps.c_str());
            EditorTheme::PopFont();
        } else {
            if (drawGlyph) {
                dl->AddText(ImVec2(labelX, rowMin.y), inactive ? col : kindCol, primaryGlyph);
                if (secondaryGlyph && !customGlyph) {
                    const float sub = fontSize * 0.68f;
                    const ImU32 secBase = inactive ? col
                                        : EditorTheme::U32((hasMesh && hasLight) ? EditorTheme::KindLight : EditorTheme::KindCamera);
                    dl->AddText(ImGui::GetFont(), sub,
                                ImVec2(labelX + slotW - sub, rowMin.y + fontSize - sub),
                                (secBase & 0x00FFFFFFu) | 0xB4000000u, secondaryGlyph);
                }
            }
            dl->AddText(ImVec2(nameX, rowMin.y), col, shownName.c_str());
            // vHierarchy D: this row is the scene's default parent - new objects land under it.
            if (entity == m_HierDefaultParentNow) {
                const ImVec2 ns = ImGui::CalcTextSize(shownName.c_str());
                const float bs = fontSize * 0.72f;
                const ImVec2 bp(nameX + ns.x + fontSize * 0.35f, rowMin.y + (fontSize - bs) * 0.5f);
                dl->AddText(ImGui::GetFont(), bs, bp, EditorTheme::U32(EditorTheme::Accent), ICON_FA_ARROW_RIGHT_TO_BRACKET);
                if (ImGui::IsMouseHoveringRect(bp, ImVec2(bp.x + bs, bp.y + bs)))
                    EditorUI::SetTooltip("Default parent: new objects are created under this one (D over a row toggles it).");
            }
        }

        // Phase 5 item 8 — a badge glyph beside the name, not just the blue/red colour tint
        // above: on its own, that tint collides with the palette's blue=active role (the camera
        // kind glyph and the selection accent are both blue-family too), so colour alone doesn't
        // reliably say "this is a prefab instance."
        if (prefabInst && !inactive && !sceneHiddenRow && !isSeparator) {
            const ImVec2 nameSize = ImGui::CalcTextSize(shownName.c_str());
            const float badgeSize = fontSize * 0.72f;
            const ImVec2 badgePos(nameX + nameSize.x + fontSize * 0.35f,
                                   rowMin.y + (fontSize - badgeSize) * 0.5f);
            dl->AddText(ImGui::GetFont(), badgeSize, badgePos, col,
                        prefabInst->Missing ? ICON_FA_LINK_SLASH : ICON_FA_BOX_ARCHIVE);
            if (ImGui::IsMouseHoveringRect(badgePos, ImVec2(badgePos.x + badgeSize, badgePos.y + badgeSize)))
                EditorUI::SetTooltip(prefabInst->Missing ? "Prefab instance - link to its source is broken"
                                                          : "Prefab instance");
        }
        dl->PopClipRect();

        // vHierarchy component minimap, in the small font and the dim colour (brightening under
        // the cursor), so a row's makeup reads at a glance without competing with its name.
        if (miniCount > 0) {
            EditorTheme::PushSmall();
            const auto& all = ComponentRegistry::All();
            float x = miniLeft + EditorTheme::Px(4.0f);
            const float cy = (rowMin.y + rowMax.y) * 0.5f;
            for (int k = 0; k < miniCount; ++k) {
                const char* ic = all[(size_t)miniIdx[k]].Meta.Icon;
                if (!ic || !*ic) ic = ICON_FA_PUZZLE_PIECE;
                const ImVec2 is = ImGui::CalcTextSize(ic);
                const ImU32 c = EditorTheme::U32(hoveredMini == k ? EditorTheme::Text
                                                 : (inactive || sceneHiddenRow) ? EditorTheme::WithAlpha(EditorTheme::Dim, 0.6f)
                                                                                : EditorTheme::Dim);
                dl->AddText(ImVec2(x + (miniCellW - is.x) * 0.5f, cy - is.y * 0.5f), c, ic);
                x += miniCellW;
            }
            if (miniExtra > 0) {
                char plus[8];
                std::snprintf(plus, sizeof(plus), "+%d", miniExtra);
                const ImVec2 ps = ImGui::CalcTextSize(plus);
                dl->AddText(ImVec2(x + (miniPlusW - ps.x) * 0.5f, cy - ps.y * 0.5f),
                            EditorTheme::U32(hoveredMini == miniCount ? EditorTheme::Text : EditorTheme::Dim), plus);
            }
            EditorTheme::PopFont();
        }
    }

    // A separator row is a header: no eye / lock / active column.
    if (isSeparator) return;

    // Active-state eye, pinned to a fixed right-hand column so every row's eye lines up no matter
    // how deep it sits. Drawn after the row so a click on it never also selects the row.
    {
        // Phase 1 item 6 (hit-target-min 24x24): these must match the same floor ActiveToggle/
        // SceneVisToggle apply to their own invisible hit box (ImGui::GetFrameHeight(), already
        // >=24px at every scale/theme this editor ships), or this outer layout math reserves a
        // narrower slot than the widget actually occupies and the three toggles overlap.
        const float minHit = ImGui::GetFrameHeight();
        const float eyeW = ImMax(ImMax(ImGui::CalcTextSize(ICON_FA_EYE).x, ImGui::CalcTextSize(ICON_FA_EYE_SLASH).x) + 2.0f, minHit);
        const float gap  = 4.0f * m_UIScale;
        const float lockW = ImMax(ImMax(ImGui::CalcTextSize(ICON_FA_LOCK).x, ImGui::CalcTextSize(ICON_FA_LOCK_OPEN).x) + 2.0f, minHit);
        const float hideW = eyeW;

        // #236 B — SceneVis-lite: an eye (viewport visibility) + a padlock (viewport pickability),
        // always drawn just left of the Active checkbox. These never touch the object itself —
        // Game view, physics and saves are unaffected.
        const bool sceneHidden = world.Registry.all_of<HiddenInSceneTag>(entity);
        const bool sceneLocked = world.Registry.all_of<SceneLockedTag>(entity);
        // Like Unity's Scene visibility/pickability: a click applies to the object and everything
        // under it; Alt+click to change only this one.
        auto setOnSubtree = [&](auto* tagPtr, bool on) {
            using Tag = std::remove_pointer_t<decltype(tagPtr)>;
            std::vector<entt::entity> stack{entity};
            while (!stack.empty()) {
                const entt::entity e = stack.back();
                stack.pop_back();
                if (!world.Registry.valid(e)) continue;
                if (on) world.Registry.emplace_or_replace<Tag>(e);
                else    world.Registry.remove<Tag>(e);
                if (ImGui::GetIO().KeyAlt) break;
                if (const auto* h = world.Registry.try_get<HierarchyComponent>(e))
                    stack.insert(stack.end(), h->Children.begin(), h->Children.end());
            }
        };

        // Unity-style: the toggles only show while the row is hovered or selected, or when one is
        // away from its default (hidden / locked / inactive). Their columns stay reserved.
        const bool rowActive = selected ||
            (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(rowMin, rowMax));
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(rowMax.x - eyeW - gap - lockW - hideW - 2.0f * gap, rowMin.y));
        if (SceneVisToggle("##svhide", ICON_FA_EYE_SLASH, ICON_FA_EYE, sceneHidden, rowHovered,
                           sceneHidden ? "Hidden in the Scene view - click to show (Alt+click: this object only)"
                                       : "Hide in the Scene view with its children (still in the game, still collides,\n"
                                         "still saved). Alt+click: this object only.",
                           rowActive || sceneHidden)) {
            PushUndo(world, "Toggle Scene Visibility");
            setOnSubtree((HiddenInSceneTag*)nullptr, !sceneHidden);
        }
        ImGui::SameLine(0.0f, gap);
        if (SceneVisToggle("##svlock", ICON_FA_LOCK, ICON_FA_LOCK_OPEN, sceneLocked, rowHovered,
                           sceneLocked ? "Locked out of Scene-view clicks - click to unlock (Alt+click: this object only)"
                                       : "Lock with its children: can't be clicked in the Scene view (Hierarchy select\n"
                                         "still works). Alt+click: this object only.",
                           rowActive || sceneLocked)) {
            PushUndo(world, "Toggle Scene Lock");
            setOnSubtree((SceneLockedTag*)nullptr, !sceneLocked);
        }

        ImGui::SetCursorScreenPos(ImVec2(rowMax.x - eyeW - 4.0f * m_UIScale, rowMin.y));
        if (ActiveToggle("##rowactive", !selfDeactivated, rowHovered,
                         selfDeactivated ? "Inactive - click to enable"
                         : inactive      ? "Hidden because a parent is inactive - click to disable this one too"
                                         : "Active - click to disable",
                         /*alignTop=*/true, rowActive || selfDeactivated)) {
            PushUndo(world, "Toggle Active");
            if (selfDeactivated) world.Registry.remove<DeactivatedTag>(entity);
            else world.Registry.emplace<DeactivatedTag>(entity);
        }
    }

    // Defect #45 — children no longer recurse from here: FlattenHierarchyRows already expanded
    // this row's children (if any, and if open) into their own entries in the flat list that
    // DrawHierarchyTreeBody's clipped loop is iterating.
}

void EditorLayer::SetHierarchyExpandedRecursive(World& world, entt::entity entity, bool open) {
    if (!world.Registry.valid(entity)) return;
    ImGui::PushID((int)entt::to_integral(entity));
    ImGuiID nodeId = ImGui::GetID("##node");
    ImGui::GetStateStorage()->SetInt(nodeId, open ? 1 : 0);
    if (const auto* hier = world.Registry.try_get<HierarchyComponent>(entity)) {
        for (entt::entity child : hier->Children) {
            SetHierarchyExpandedRecursive(world, child, open);
        }
    }
    ImGui::PopID();
}

void EditorLayer::DrawHierarchyContextMenu(World& world, AssetLibrary& assets, entt::entity entity) {
    bool hasEntity = entity != entt::null && world.Registry.valid(entity);

    // Phase 5 item 7 (#7) — the flat 19-item menu grouped into labelled sections, so scanning it
    // doesn't mean reading every line; "Frame Selected" (previously missing here even though the
    // Scene viewport already has it) and a clearer name for the object-moving "Move To View" item
    // (see below) land in this pass too.
    EditorUIPrimitives::SectionHeader(ICON_FA_CUBES "  Create");
    if (ImGui::BeginMenu(ICON_FA_PLUS "  Create")) {
        // Same body as the toolbar Create menu and the Shift+A quick-add, so every "add an
        // object" entry point offers the same list. New objects spawn in front of the editor
        // camera (not as a child of the right-clicked row).
        if (m_EditorCameraPtr) DrawAddEntityItems(world, assets, *m_EditorCameraPtr);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(ICON_FA_DIAGRAM_PROJECT "  Create Empty Child", "Ctrl+Shift+N", false, hasEntity)) {
        CreateEmptyChild(world, entity);
    }

    EditorUIPrimitives::SectionHeader(ICON_FA_OBJECT_UNGROUP "  Select");
    const bool anyEntities = world.Registry.view<const NameComponent>().begin() != world.Registry.view<const NameComponent>().end();
    if (ImGui::MenuItem(ICON_FA_OBJECT_UNGROUP "  Select All", "Ctrl+A", false, anyEntities)) SelectAllEntities(world);
    if (ImGui::MenuItem(ICON_FA_BAN "  Deselect All", "Ctrl+Shift+A", false, HasAnySelection())) ClearSelection();
    if (ImGui::MenuItem(ICON_FA_RIGHT_LEFT "  Invert Selection", "Ctrl+I", false, anyEntities)) InvertSelection(world);
    // #178 - Unity's Select Children: adds every descendant of the selected objects.
    bool selectionHasChildren = false;
    for (entt::entity e : GetSelectedItems())
        if (const auto* h = world.Registry.try_get<HierarchyComponent>(e); h && !h->Children.empty()) selectionHasChildren = true;
    if (ImGui::MenuItem(ICON_FA_SITEMAP "  Select Children", nullptr, false, selectionHasChildren)) {
        std::vector<entt::entity> stack = GetSelectedItems();
        while (!stack.empty()) {
            const entt::entity e = stack.back();
            stack.pop_back();
            if (!world.Registry.valid(e)) continue;
            AddToSelectionIfAbsent(e);
            if (const auto* h = world.Registry.try_get<HierarchyComponent>(e))
                stack.insert(stack.end(), h->Children.begin(), h->Children.end());
        }
    }
    // #178 - Unity's Select Prefab Root: replaces each selected object with the outermost prefab
    // instance it belongs to (objects not inside any instance are dropped).
    std::vector<entt::entity> prefabRoots;
    for (entt::entity e : GetSelectedItems()) {
        entt::entity root = entt::null;
        for (entt::entity walk = e; walk != entt::null && world.Registry.valid(walk);) {
            if (world.Registry.all_of<PrefabInstanceComponent>(walk)) root = walk;
            const auto* h = world.Registry.try_get<HierarchyComponent>(walk);
            walk = h ? h->Parent : entt::null;
        }
        if (root != entt::null && std::find(prefabRoots.begin(), prefabRoots.end(), root) == prefabRoots.end())
            prefabRoots.push_back(root);
    }
    if (ImGui::MenuItem(ICON_FA_CUBES "  Select Prefab Root", nullptr, false, !prefabRoots.empty())) {
        ClearSelection();
        for (entt::entity root : prefabRoots) AddToSelectionIfAbsent(root);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        EditorUI::SetTooltip("Select the prefab instance each selected object is part of.");
    if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS_PLUS "  Frame Selected", "F", false, HasAnySelection()) && m_EditorCameraPtr) {
        FocusOnSelection(world, *m_EditorCameraPtr);
    }

    EditorUIPrimitives::SectionHeader(ICON_FA_SITEMAP "  Arrange");
    if (ImGui::MenuItem(ICON_FA_OBJECT_GROUP "  Group into Empty Parent", nullptr, false, HasAnySelection())) {
        CreateEmptyParentForSelection(world);
    }
    if (ImGui::IsItemHovered() && HasAnySelection()) {
        EditorUI::SetTooltip("Create a new Empty at the selection's center and parent every\nselected object under it. Positions are preserved.");
    }

    // Always-reachable un-parent (#220): dropping onto the empty space below the tree does the
    // same thing, but that drop zone can shrink to nothing once the tree fills the panel.
    bool selectionHasParent = false;
    for (entt::entity e : GetSelectedItems()) {
        if (!world.Registry.valid(e)) continue;
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        if (h && h->Parent != entt::null) { selectionHasParent = true; break; }
    }
    if (ImGui::MenuItem(ICON_FA_LINK_SLASH "  Unparent", nullptr, false, selectionHasParent)) {
        UnparentSelection(world);
    }
    if (ImGui::IsItemHovered() && selectionHasParent) {
        EditorUI::SetTooltip("Move the selection to the scene root.");
    }

    if (ImGui::MenuItem(ICON_FA_ANGLES_UP "  Set as First Sibling", nullptr, false, hasEntity))
        SetHierarchySiblingExtreme(world, entity, /*first=*/true);
    if (ImGui::MenuItem(ICON_FA_ANGLES_DOWN "  Set as Last Sibling", nullptr, false, hasEntity))
        SetHierarchySiblingExtreme(world, entity, /*first=*/false);
    // Editor Enhancers / vHierarchy: the scene's default parent (also D over a row).
    {
        const bool isDefault = hasEntity && entity == ResolveDefaultParent(world);
        if (ImGui::MenuItem(ICON_FA_ARROW_RIGHT_TO_BRACKET "  Default Parent", "D", isDefault, hasEntity))
            ToggleDefaultParent(world, entity);
        if (ImGui::IsItemHovered() && hasEntity)
            EditorUI::SetTooltip("New objects you create in this scene are placed under this one.");
    }
    // Editor Enhancers / vFavorites.
    if (hasEntity) {
        auto& us = Enhancers::EnhancerUserState::Get();
        const auto* o = world.Registry.try_get<OrderComponent>(entity);
        const auto* nm = world.Registry.try_get<NameComponent>(entity);
        const Enhancers::EditorRef ref = Enhancers::EditorRef::MakeEntity(CurrentSceneKey(), o ? o->Value : -1, nm ? nm->Name : std::string());
        const int favPage = o ? Enhancers::FindFavorite(us.FavoritePages, ref) : -1;
        if (ImGui::MenuItem(ICON_FA_STAR "  Favorite", "Ctrl+Alt+B", favPage >= 0, o != nullptr)) {
            if (favPage >= 0) Enhancers::RemoveFavorite(us.FavoritePages, ref);
            else Enhancers::AddFavorite(us.FavoritePages, m_FavPage, ref);
            us.MarkDirty();
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Hold Alt over the Asset Browser to see your favorites.");
    }

    EditorUIPrimitives::SectionHeader(ICON_FA_PALETTE "  Style");
    if (ImGui::BeginMenu(ICON_FA_PALETTE "  Row Style", hasEntity)) {
        DrawHierarchyStyleMenu(world);
        ImGui::EndMenu();
    }

    EditorUIPrimitives::SectionHeader(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT "  Transform");
    if (ImGui::MenuItem(ICON_FA_EYE "  Toggle Active State", "Alt+Shift+A", false, HasAnySelection()))
        ToggleSelectionActive(world);
    // Renamed from "Move To View" (Phase 5 item 7) — that name read like a camera action (framing
    // the view on the selection, what "Frame Selected" above actually does) when it's the
    // opposite: it MOVES the selected object's transform to sit in front of the camera. "Move to
    // Camera" says what it actually does to the object.
    if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS "  Move to Camera", nullptr, false, HasAnySelection()))
        MoveSelectionToView(world);
    if (ImGui::IsItemHovered() && HasAnySelection())
        EditorUI::SetTooltip("Move the selection to just in front of the editor camera.");
    if (hasEntity && world.Registry.all_of<CameraComponent>(entity)) {
        if (ImGui::MenuItem(ICON_FA_VIDEO "  Align With View") && m_EditorCameraPtr) {
            PushUndo(world, "Align Camera to View");
            glm::vec3 d = glm::normalize(m_EditorCameraPtr->Front());
            const float pitch = std::asin(glm::clamp(d.y, -1.0f, 1.0f));
            const float yaw = std::atan2(-d.x, -d.z);
            // #128 — the view pose is world space; SetWorldPose re-expresses it in the parent's
            // frame, so a parented camera lines up too (it used to get world values as local).
            world.SetWorldPose(entity, m_EditorCameraPtr->Position,
                               glm::quat_cast(glm::eulerAngleYXZ(yaw, pitch, 0.0f)));
            world.Registry.get<CameraComponent>(entity).FovDegrees = m_EditorCameraPtr->Fov;
        }
    }
    const bool isLight = hasEntity && world.Registry.all_of<LightComponent>(entity);
    if (isLight) {
        if (ImGui::MenuItem(ICON_FA_DOWN_LONG "  Drop Light to Surface")) {
            if (!DropLightToSurface(world, entity))
                Log::Info("Drop to surface: nothing directly below this light.");
        }
    }

    EditorUIPrimitives::SectionHeader(ICON_FA_COPY "  Edit");
    if (ImGui::MenuItem(ICON_FA_COPY "  Copy", "Ctrl+C", false, hasEntity)) CopySelection(world);
    if (ImGui::MenuItem(ICON_FA_SCISSORS "  Cut", "Ctrl+X", false, hasEntity)) {
        CopySelection(world);
        DeleteSelection(world);
    }
    if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", "Ctrl+V", false, !m_Clipboard.empty())) {
        PasteClipboard(world, assets);
    }
    if (ImGui::MenuItem(ICON_FA_CLONE "  Duplicate", "Ctrl+D", false, hasEntity)) {
        DuplicateSelection(world, assets);
    }
    if (ImGui::MenuItem(ICON_FA_TABLE_CELLS "  Duplicate Array\xE2\x80\xA6", nullptr, false, hasEntity)) {
        m_ShowArrayDuplicate = true;
    }
    if (ImGui::MenuItem(ICON_FA_PEN "  Rename", "F2", false, hasEntity)) BeginRenameEntity(entity);

    EditorUIPrimitives::SectionHeader(ICON_FA_BOX_ARCHIVE "  Prefab");
    if (ImGui::MenuItem(ICON_FA_BOX_ARCHIVE "  Save as Prefab...", nullptr, false, hasEntity)) {
        std::string path = FileDialog::SaveFile("Prefab Files\0*.prefab\0All Files\0*.*\0", "prefab", m_Window);
        if (!path.empty() && SceneSerializer::SavePrefab(world, entity, path)) {
            assets.RegisterPrefab(path);
        }
    }
    // #236 A2 — prefab-instance actions, only on an instance root.
    if (hasEntity && world.Registry.all_of<PrefabInstanceComponent>(entity)) {
        const auto& pi = world.Registry.get<PrefabInstanceComponent>(entity);
        if (ImGui::MenuItem(ICON_FA_LINK_SLASH "  Unpack Prefab Instance")) {
            PushUndo(world, "Unpack Prefab");
            world.Registry.remove<PrefabInstanceComponent>(entity);
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Break the link to %s.\nThe objects stay; they just stop tracking the prefab.",
                                 pi.SourcePath.c_str());
    }

    ImGui::Separator();
    // Danger-tinted, matching DangerIconButton's palette (EditorUIPrimitives::DangerColor) —
    // Delete is the one destructive, unrecoverable-without-undo action in this menu.
    ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::DangerColor());
    if (ImGui::MenuItem(ICON_FA_TRASH "  Delete", "Del", false, hasEntity)) DeleteSelection(world);
    ImGui::PopStyleColor();
}

void EditorLayer::BeginRenameEntity(entt::entity entity) {
    if (entity == entt::null) return;
    m_RenamingEntity = entity;
    m_EntityRenameJustStarted = true;
}

void EditorLayer::CreateEmptyParentForSelection(World& world) {
    std::vector<entt::entity> sel = GetSelectedItems();
    sel.erase(std::remove_if(sel.begin(), sel.end(),
        [&](entt::entity e) { return !world.Registry.valid(e); }), sel.end());
    if (sel.empty()) return;

    glm::vec3 center(0.0f);
    if (!GetSelectionCenter(world, center)) center = glm::vec3(0.0f);

    PushUndo(world, "Create Empty Parent");
    entt::entity parent = world.CreateEmptyEntity(center, glm::vec3(0.0f), glm::vec3(1.0f), UniqueNameFor(world, "Group"));

    // If every selected entity already shares one parent, slot the new group in under it so the
    // grouping doesn't yank the objects to the scene root.
    entt::entity commonParent = entt::null;
    bool sameParent = true;
    for (size_t i = 0; i < sel.size(); ++i) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(sel[i]);
        entt::entity p = h ? h->Parent : entt::null;
        if (i == 0) commonParent = p;
        else if (p != commonParent) { sameParent = false; break; }
    }
    if (sameParent && commonParent != entt::null) world.SetParent(parent, commonParent);

    int parented = 0;
    for (entt::entity e : sel) {
        if (world.SetParent(e, parent)) ++parented; // preserves world transform (#114: colliders too)
    }
    SelectItem(parent, false);
    Log::Info("Grouped " + std::to_string(parented) + " object(s) under a new Empty" +
              (parented < (int)sel.size() ? " (some couldn't be re-parented)." : "."));
}

void EditorLayer::UnparentSelection(World& world) {
    std::vector<entt::entity> sel = GetSelectedItems();
    StageUndo(world);
    for (entt::entity e : sel) {
        if (world.Registry.valid(e)) world.SetParent(e, entt::null);
    }
    CommitStagedUndo(world, "Reparent");
}

std::vector<entt::entity> EditorLayer::HierarchySiblingsInOrder(const World& world, entt::entity parent) const {
    std::vector<entt::entity> out;
    if (parent == entt::null) {
        for (entt::entity e : world.Registry.view<const NameComponent>()) {
            const auto* h = world.Registry.try_get<HierarchyComponent>(e);
            if (!h || h->Parent == entt::null) out.push_back(e);
        }
    } else if (const auto* h = world.Registry.try_get<HierarchyComponent>(parent)) {
        out = h->Children;
    }
    std::sort(out.begin(), out.end(), [&](entt::entity a, entt::entity b) {
        const auto* oa = world.Registry.try_get<OrderComponent>(a);
        const auto* ob = world.Registry.try_get<OrderComponent>(b);
        int va = oa ? oa->Value : 0, vb = ob ? ob->Value : 0;
        return va != vb ? va < vb : a < b;
    });
    return out;
}

// Phase 5 item 6 — the sort control's display-only re-sort. Mode 0 (Creation order) is a no-op
// since the caller's list already comes out of ViewInCreationOrder/HierarchySiblingsInOrder in
// that order; modes 1/2 stable_sort by name or kind on top of it, so ties (e.g. two meshes) keep
// their creation-order relative position. This never touches OrderComponent or Children — pure
// display, so drag-drop / ReorderHierarchySiblings behave exactly as before regardless of sort.
void EditorLayer::ApplyHierarchyDisplaySort(const World& world, std::vector<entt::entity>& rows) const {
    const int mode = EditorSettings::Get().HierarchySortMode;
    if (mode == 0 || rows.size() < 2) return;
    const bool desc = EditorSettings::Get().HierarchySortDesc;
    // Sort keys are computed once per row, not per comparison: the Name key for an unnamed row
    // ("Object 7") used to cost a full creation-order sort of the scene on every compare.
    std::unordered_map<entt::entity, int> ordinals;
    if (mode == 1) {
        const auto list = ViewInCreationOrder(world.Registry, world.Registry.view<const NameComponent>());
        for (size_t i = 0; i < list.size(); ++i) ordinals[list[i]] = (int)i + 1;
    }
    struct Keyed { entt::entity E; std::string Name; int Kind; };
    std::vector<Keyed> keyed;
    keyed.reserve(rows.size());
    for (entt::entity e : rows) {
        Keyed k{e, {}, 0};
        if (mode == 1) {
            const auto* nm = world.Registry.try_get<NameComponent>(e);
            k.Name = (nm && !nm->Name.empty()) ? nm->Name : "Object " + std::to_string(ordinals[e]);
        } else {
            k.Kind = HierarchyKindMask(world.Registry, e);
        }
        keyed.push_back(std::move(k));
    }
    auto less = [&](const Keyed& a, const Keyed& b) { return mode == 1 ? a.Name < b.Name : a.Kind < b.Kind; };
    std::stable_sort(keyed.begin(), keyed.end(), [&](const Keyed& a, const Keyed& b) {
        return desc ? less(b, a) : less(a, b);
    });
    for (size_t i = 0; i < rows.size(); ++i) rows[i] = keyed[i].E;
}

void EditorLayer::ReorderHierarchySiblings(World& world, const std::vector<entt::entity>& movingIn,
                                           entt::entity anchor, bool after, bool recordUndo) {
    if (!world.Registry.valid(anchor)) return;
    const auto* anchorHier = world.Registry.try_get<HierarchyComponent>(anchor);
    const entt::entity newParent = anchorHier ? anchorHier->Parent : entt::null;

    // Keep the dragged rows in their current visual order; drop the anchor itself and anything
    // that is an ancestor of the anchor (SetParent would reject the resulting cycle anyway).
    std::vector<entt::entity> moving;
    for (entt::entity e : movingIn) {
        if (!world.Registry.valid(e) || e == anchor) continue;
        bool ancestorOfAnchor = false;
        for (entt::entity w = newParent; w != entt::null; ) {
            if (w == e) { ancestorOfAnchor = true; break; }
            const auto* h = world.Registry.try_get<HierarchyComponent>(w);
            w = h ? h->Parent : entt::null;
        }
        if (!ancestorOfAnchor) moving.push_back(e);
    }
    if (moving.empty()) return;

    if (recordUndo) StageUndo(world);

    // Reparent any row not already under newParent (SetParent preserves world pose and keeps the
    // Children vectors consistent; a same-parent call is a no-op we skip explicitly).
    bool anyFailed = false;
    for (entt::entity e : moving) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        const entt::entity cur = h ? h->Parent : entt::null;
        if (cur != newParent && !world.SetParent(e, newParent)) anyFailed = true;
    }

    // Splice the moving rows back around the anchor, then renumber the whole group 0..N-1.
    std::vector<entt::entity> group = HierarchySiblingsInOrder(world, newParent);
    group.erase(std::remove_if(group.begin(), group.end(), [&](entt::entity e) {
        return std::find(moving.begin(), moving.end(), e) != moving.end();
    }), group.end());

    std::vector<entt::entity> rebuilt;
    rebuilt.reserve(group.size() + moving.size());
    for (entt::entity e : group) {
        if (e == anchor && !after) for (entt::entity m : moving) rebuilt.push_back(m);
        rebuilt.push_back(e);
        if (e == anchor && after) for (entt::entity m : moving) rebuilt.push_back(m);
    }
    // #118 — OrderComponent is also the entity's stable identity (joint ConnectedOrder, undo /
    // Play-Stop selection restore), so it must stay unique scene-wide. The old 0..N-1 renumber
    // collided with every other sibling group. Instead, hand this group's OWN existing values back
    // out in the new visual order (a permutation, so still unique and still sorted), give any
    // entity that somehow lacks one a fresh value, and repoint joints at their partner's new value.
    std::vector<int> values;
    values.reserve(rebuilt.size());
    for (entt::entity e : rebuilt)
        if (const auto* o = world.Registry.try_get<OrderComponent>(e)) values.push_back(o->Value);
    while (values.size() < rebuilt.size()) values.push_back(world.AllocateOrder());
    std::sort(values.begin(), values.end());
    std::unordered_map<int, int> remap; // old value -> new value
    for (int i = 0; i < (int)rebuilt.size(); ++i) {
        if (const auto* o = world.Registry.try_get<OrderComponent>(rebuilt[i])) remap[o->Value] = values[i];
    }
    for (int i = 0; i < (int)rebuilt.size(); ++i)
        world.Registry.emplace_or_replace<OrderComponent>(rebuilt[i], values[i]);
    for (auto [je, joint] : world.Registry.view<JointComponent>().each()) {
        auto it = remap.find(joint.ConnectedOrder);
        if (joint.ConnectedOrder >= 0 && it != remap.end()) joint.ConnectedOrder = it->second;
    }

    if (recordUndo) CommitStagedUndo(world, "Reorder");
    if (anyFailed)
        Log::Warn("Some rows couldn't be moved there (an object can't become a child of its own descendant).");
}

entt::entity EditorLayer::InstantiateAssetDropInHierarchy(World& world, AssetLibrary& assets,
                                                          const char* modelPath, const char* prefabPath,
                                                          entt::entity parent) {
    entt::entity e = entt::null;
    if (modelPath && *modelPath) {
        PushUndo(world, "Place Model");
        auto model = assets.InstantiateModel(modelPath);
        std::string name = std::filesystem::path(modelPath).stem().string();
        // #119 — a drop on the root lands in front of the Scene camera (like Create > ...), not at
        // the world origin; a drop onto a row still lands at that parent's origin (below).
        const bool toRoot = parent == entt::null || !world.Registry.valid(parent);
        const glm::vec3 pos = (toRoot && m_EditorCameraPtr) ? SafeSpawnInFrontOf(*m_EditorCameraPtr) : glm::vec3(0.0f);
        e = world.CreateModelEntity(model, pos, glm::vec3(0.0f), glm::vec3(1.0f),
                                    UniqueNameFor(world, name));
        if (auto* rc = world.Registry.try_get<RenderableComponent>(e); rc && rc->ModelRef)
            assets.ApplyMaterialRemap(*rc->ModelRef, rc->Materials); // its extracted .mat files
    } else if (prefabPath && *prefabPath) {
        PushUndo(world, "Place Prefab Instance");
        e = SceneSerializer::InstantiatePrefab(world, assets, prefabPath);
        if (e != entt::null) UniquifyName(world, e);
    }
    if (e != entt::null) {
        // SetParent re-expresses the transform into the parent's frame; a model was created at
        // the origin so it lands at the parent's origin, a prefab keeps its authored offset.
        if (parent != entt::null && world.Registry.valid(parent)) world.SetParent(e, parent);
        SelectItem(e, false);
    }
    return e;
}

entt::entity EditorLayer::CreateEmptyChild(World& world, entt::entity parent) {
    PushUndo(world, "Create Empty Child");
    entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f),
                                             UniqueNameFor(world, "Empty"));
    if (parent != entt::null && world.Registry.valid(parent)) world.SetParent(e, parent);
    SelectItem(e, false);
    return e;
}

void EditorLayer::SetHierarchySiblingExtreme(World& world, entt::entity entity, bool first) {
    if (!world.Registry.valid(entity)) return;
    const auto* h = world.Registry.try_get<HierarchyComponent>(entity);
    const entt::entity parent = h ? h->Parent : entt::null;
    std::vector<entt::entity> sibs = HierarchySiblingsInOrder(world, parent);
    if (sibs.size() < 2) return;
    const entt::entity anchor = first ? sibs.front() : sibs.back();
    if (anchor == entity) return; // already there
    ReorderHierarchySiblings(world, {entity}, anchor, /*after=*/!first);
}

void EditorLayer::MoveSelectionToView(World& world) {
    std::vector<entt::entity> sel = GetSelectedItems();
    if (sel.empty() || !m_EditorCameraPtr) return;
    const glm::vec3 target = SafeSpawnInFrontOf(*m_EditorCameraPtr);
    StageUndo(world);
    for (entt::entity e : sel) {
        if (!world.Registry.valid(e)) continue;
        auto* t = world.Registry.try_get<TransformComponent>(e);
        if (!t) continue;
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        if (hier && hier->Parent != entt::null && world.Registry.valid(hier->Parent)) {
            const glm::mat4 inv = glm::inverse(world.ComposeWorldTransform(hier->Parent));
            t->Position = glm::vec3(inv * glm::vec4(target, 1.0f));
        } else {
            t->Position = target;
        }
    }
    CommitStagedUndo(world, "Move To View");
}

void EditorLayer::ToggleSelectionActive(World& world) {
    std::vector<entt::entity> sel = GetSelectedItems();
    if (sel.empty()) return;
    bool anyActive = false;
    for (entt::entity e : sel)
        if (world.Registry.valid(e) && !world.Registry.all_of<DeactivatedTag>(e)) { anyActive = true; break; }
    StageUndo(world);
    for (entt::entity e : sel) {
        if (!world.Registry.valid(e)) continue;
        if (anyActive) world.Registry.emplace_or_replace<DeactivatedTag>(e); // mixed/all-active -> disable all
        else world.Registry.remove<DeactivatedTag>(e);
    }
    CommitStagedUndo(world, "Toggle Active");
}

entt::entity EditorLayer::CreateEmptyAt(World& world, Camera* editorCamera, const char* name, bool asLight) {
    PushUndo(world, std::string("Create ") + name);
    // Spawned in front of the camera when there is one (menu invoked from the viewport/toolbar),
    // else at the origin — the Hierarchy's own context menu has no camera to reference.
    glm::vec3 position = editorCamera ? SafeSpawnInFrontOf(*editorCamera) : glm::vec3(0.0f);
    entt::entity e = world.CreateEmptyEntity(position, glm::vec3(0.0f), glm::vec3(1.0f), UniqueNameFor(world, name));
    if (asLight) world.Registry.emplace<LightComponent>(e);
    ApplyDefaultParent(world, e); // vHierarchy D: world pose kept, so it still spawns in front of the camera
    SelectItem(e, false);
    Log::Info(std::string("Added ") + name + ".");
    return e;
}

// --- Editor Enhancers / vHierarchy: hover keys --------------------------------------------------
// Each acts on the selection when the row under the mouse is part of it, otherwise on that row
// alone (vHierarchy's rule). Runs at the end of DrawHierarchyTreeBody, inside the Hierarchy window
// with no extra IDs pushed, so node-state IDs line up with the rows just drawn. Ctrl+Shift+E works
// anywhere over the panel; the rest need a row under the mouse.
void EditorLayer::HandleHierarchyHoverKeys(World& world) {
    m_HierarchyHoverKeyUsed = false;
    if (!EditorSettings::Get().EnhancerHoverKeys) return;

    if (Shortcuts::Triggered("hierarchy.hover.collapseAll")) {
        HierarchyExpandAll(world, false);
        m_HierarchyHoverKeyUsed = true;
        return;
    }
    const entt::entity hov = m_HierarchyHoverEntity;
    if (hov == entt::null || !world.Registry.valid(hov)) return;
    const bool onSelection = IsSelected(hov);
    auto targets = [&]() { return onSelection ? GetSelectedItems() : std::vector<entt::entity>{hov}; };
    auto hasKids = [&](entt::entity e) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        return h && !h->Children.empty();
    };
    ImGuiStorage* st = ImGui::GetStateStorage();

    if (Shortcuts::Triggered("hierarchy.hover.expand")) {
        // Every target follows the hovered row's new state, so a mixed selection converges.
        const bool open = st->GetInt(HierarchyNodeStateId(world, hov, m_HierChainScratch), 0) == 0;
        for (entt::entity e : targets())
            if (world.Registry.valid(e) && hasKids(e)) st->SetInt(HierarchyNodeStateId(world, e, m_HierChainScratch), open ? 1 : 0);
        m_HierarchyHoverKeyUsed = true;
    } else if (Shortcuts::Triggered("hierarchy.hover.isolate")) {
        // Collapse everything, then reopen just the path down to the hovered row (and the row).
        HierarchyExpandAll(world, false);
        for (entt::entity w = hov; w != entt::null && world.Registry.valid(w);) {
            if (w != hov || hasKids(w)) st->SetInt(HierarchyNodeStateId(world, w, m_HierChainScratch), 1);
            const auto* h = world.Registry.try_get<HierarchyComponent>(w);
            w = h ? h->Parent : entt::null;
        }
        m_HierarchyScrollToEntity = hov;
        m_HierarchyHoverKeyUsed = true;
    } else if (Shortcuts::Triggered("hierarchy.hover.toggleActive")) {
        if (onSelection) {
            ToggleSelectionActive(world);
        } else {
            PushUndo(world, "Toggle Active");
            if (world.Registry.all_of<DeactivatedTag>(hov)) world.Registry.remove<DeactivatedTag>(hov);
            else world.Registry.emplace<DeactivatedTag>(hov);
        }
        m_HierarchyHoverKeyUsed = true;
    } else if (Shortcuts::Triggered("hierarchy.hover.focus")) {
        if (!onSelection) SelectItem(hov, false);
        if (m_EditorCameraPtr) FocusOnSelection(world, *m_EditorCameraPtr);
        m_HierarchyHoverKeyUsed = true;
    } else if (Shortcuts::Triggered("hierarchy.hover.delete")) {
        if (!onSelection) SelectItem(hov, false);
        DeleteSelection(world); // pushes its own undo
        m_HierarchyHoverEntity = entt::null;
        m_HierarchyHoverKeyUsed = true;
    } else if (Shortcuts::Triggered("hierarchy.hover.defaultParent")) {
        ToggleDefaultParent(world, hov);
        m_HierarchyHoverKeyUsed = true;
    }
}

// --- Editor Enhancers / vHierarchy: the row context menu's "Row Style" submenu ----------------
// Applies to every selected row (the context menu selected the right-clicked one first) as one
// undo step. A style that ends up empty removes the component, so unstyled rows cost nothing.
void EditorLayer::DrawHierarchyStyleMenu(World& world) {
    std::vector<entt::entity> targets = GetSelectedItems();
    targets.erase(std::remove_if(targets.begin(), targets.end(), [&](entt::entity e) { return !world.Registry.valid(e); }),
                  targets.end());
    if (targets.empty()) return;
    const HierarchyStyleComponent* cur = world.Registry.try_get<HierarchyStyleComponent>(targets.front());
    const HierarchyStyleComponent shown = cur ? *cur : HierarchyStyleComponent{};

    auto apply = [&](const char* label, auto&& mutate) {
        PushUndo(world, label);
        for (entt::entity e : targets) {
            auto& s = world.Registry.get_or_emplace<HierarchyStyleComponent>(e);
            mutate(s);
            if (s.IsEmpty()) world.Registry.remove<HierarchyStyleComponent>(e);
        }
    };

    if (targets.size() > 1) ImGui::TextDisabled("%d objects", (int)targets.size());
    if (!EditorSettings::Get().HierarchyRowStyles)
        ImGui::TextDisabled("Row styles are turned off in Settings > Editor Enhancers.");

    EditorUIPrimitives::SectionHeader("Colour");
    std::uint32_t color = shown.Color;
    if (Enhancers::PaletteColorRow("##hierRowColor", color))
        apply("Set Row Colour", [&](HierarchyStyleComponent& s) { s.Color = color; });
    {
        int fill = (int)shown.FillMode;
        static const char* kFills[] = {"Icon only", "Flat", "Gradient"};
        ImGui::SetNextItemWidth(EditorTheme::Px(200.0f));
        if (EditorUIPrimitives::Segmented("##hierFill", &fill, kFills, 3))
            apply("Set Row Fill", [&](HierarchyStyleComponent& s) { s.FillMode = (std::uint8_t)fill; });
    }

    EditorUIPrimitives::SectionHeader("Icon");
    if (ImGui::BeginMenu(shown.Icon.empty() ? ICON_FA_ICONS "  Choose icon..." : ICON_FA_ICONS "  Change icon...")) {
        std::string icon = shown.Icon;
        if (Enhancers::IconPickerGrid("##hierIcon", icon, m_HierStyleIconSearch, sizeof(m_HierStyleIconSearch), EditorTheme::Px(280.0f))) {
            apply("Set Row Icon", [&](HierarchyStyleComponent& s) { s.Icon = icon; });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    bool sep = shown.Separator;
    if (ImGui::MenuItem(ICON_FA_GRIP_LINES "  Separator Row", nullptr, &sep))
        apply(sep ? "Make Separator" : "Unmake Separator", [&](HierarchyStyleComponent& s) { s.Separator = sep; });
    if (ImGui::MenuItem(ICON_FA_ERASER "  Clear Style", nullptr, false, cur != nullptr))
        apply("Clear Row Style", [&](HierarchyStyleComponent& s) { s = HierarchyStyleComponent{}; });
}
