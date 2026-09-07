// Hierarchy panel: the entity tree, selection, rename, grouping/parenting, and the
// Add-entity menu body. Split out of EditorLayer.cpp for build time (#179).

#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "FileDialog.h"
#include "AssetLibrary.h"
#include "World.h"
#include "Camera.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"
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
    lc.Shadow.Enabled = true; // a freshly added sun casts shadows by default
    world.Registry.get<TransformComponent>(e).RotationEuler = glm::vec3(-36.25f, 53.13f, 0.0f);
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

// 1-based position of `entity` within its kind's creation-ordered list — the number behind the
// "Box 3" / "Object 7" fallback shown for entities the user never named (#21 P10).
int CreationOrdinal(const entt::registry& reg, entt::entity entity) {
    auto list = ViewInCreationOrder(reg, reg.view<const NameComponent>());
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == entity) return static_cast<int>(i) + 1;
    return 0;
}

} // namespace


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
    m_Selected = entt::null;
    m_ExtraSelection.clear();
    m_SelectionAnchor = entt::null;
    m_RenamingEntity = entt::null;
    m_LightHandleArmedFor = entt::null;
}

void EditorLayer::SelectItem(entt::entity entity, bool addToSelection) {
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
    // inside DrawHierarchyNode's PushID stack — and rows nest, so that stack is PushID(root) …
    // PushID(entity), one per ancestor. Rebuild the whole chain here or the id won't match for
    // any row below the top level (which is why Left/Right did nothing on nested rows).
    auto nodeId = [&](entt::entity e) {
        std::vector<entt::entity> chain;
        for (entt::entity w = e; w != entt::null; ) {
            chain.push_back(w);
            const auto* h = world.Registry.try_get<HierarchyComponent>(w);
            w = h ? h->Parent : entt::null;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            ImGui::PushID((int)entt::to_integral(*it));
        ImGuiID id = ImGui::GetID("##node");
        for (size_t i = 0; i < chain.size(); ++i) ImGui::PopID();
        return id;
    };
    auto nodeOpen    = [&](entt::entity e) { return ImGui::GetStateStorage()->GetInt(nodeId(e), 1) != 0; };
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
    if (!io.KeyCtrl && !io.KeyAlt && io.InputQueueCharacters.Size > 0) {
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
void EditorLayer::DuplicateSelection(World& world, AssetLibrary& assets) {
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
    if (fragment.empty() || !SceneSerializer::AppendEntitiesFromString(world, assets, fragment, created) || created.empty()) {
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
        if (auto* transform = world.Registry.try_get<TransformComponent>(e)) {
            transform->Position += glm::vec3(1.0f, 0.0f, 1.0f);
        }
        if (auto* name = world.Registry.try_get<NameComponent>(e)) {
            existingNames.erase(name->Name);
            name->Name = NextDuplicateName(existingNames, name->Name.empty() ? "Object" : name->Name);
        }
    }

    // Select the new duplicates instead of the originals, so you can immediately drag them
    // into place without having to re-pick them from the Hierarchy.
    ClearSelection();
    for (size_t i = 0; i < created.size(); ++i) {
        if (i == 0) m_Selected = created[i];
        else m_ExtraSelection.push_back(created[i]);
    }
    Log::Info("Duplicated " + std::to_string(created.size()) + (created.size() == 1 ? " object." : " objects."));
}
// The Add-menu body, shared verbatim by the File-menu-bar "Add" menu and the Shift+A quick-add
// popup (ImGui::MenuItem works inside BeginMenu and BeginPopup alike).
void EditorLayer::DrawAddEntityItems(World& world, AssetLibrary& assets, Camera& editorCamera) {
    auto spawnPrimitive = [&](const char* kind, const char* displayName) {
        PushUndo(world, std::string("Create ") + displayName);
        auto model = assets.CreatePrimitive(kind);
        glm::vec3 position = SafeSpawnInFrontOf(editorCamera);
        entt::entity e = world.CreateModelEntity(model, position, glm::vec3(0.0f), glm::vec3(1.0f), UniqueNameFor(world, displayName));
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

    ImGui::SeparatorText("Objects");
    if (ImGui::MenuItem(ICON_FA_DIAGRAM_PROJECT "  Empty")) {
        CreateEmptyAt(world, &editorCamera, "Empty", false);
    }
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
    if (ImGui::MenuItem(ICON_FA_VIDEO "  Camera")) {
        entt::entity e = CreateEmptyAt(world, &editorCamera, "Camera", false);
        world.Registry.emplace<CameraComponent>(e);
        // Aim it back at the world origin so its Game-view preview isn't just black —
        // ComposeTransform rotates Y(yaw) then X(pitch), local -Z is forward.
        auto& t = world.Registry.get<TransformComponent>(e);
        glm::vec3 d = t.Position;
        if (glm::dot(d, d) > 1.0e-4f) {
            d = glm::normalize(-d); // direction from the camera toward the origin
            t.RotationEuler = glm::vec3(
                glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))),
                glm::degrees(std::atan2(-d.x, -d.z)),
                0.0f);
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
    // Accumulates as each row is drawn (DrawHierarchyNode); published to m_HierarchyVisibleOrder
    // just before this function returns. See the header for why the two buffers are separate.
    m_HierarchyVisibleBuild.clear();

    // #236 B — clear the spring-load dwell target whenever there's no row drag in flight, so a
    // stale entity can't auto-expand a folder on the next unrelated drag.
    if (const ImGuiPayload* d = ImGui::GetDragDropPayload(); !d || !d->IsDataType("HIERARCHY_ENTITY"))
        m_HierarchySpringRow = entt::null;

    const bool filtering = !m_HierarchyFilter.empty();

    // One flat list — every entity in creation order, no "Level Geometry" / "Objects" split.
    // A box, a model, a light and an empty are all just entities; the old grouping only ever
    // added a header to scroll past. LevelGeometryTag survives as an invisible serialization /
    // built-in-collider detail, nothing the Hierarchy shows.
    //
    // Tighter per-level indent than the editor-wide default — this panel is narrow, so a few
    // levels of nesting otherwise push names off the right edge fast (#153).
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 13.0f * m_UIScale);
    // Bento (#234 layer 3): a fixed, tight row gap so every row is the same height regardless of
    // content (the SaaS-list look). Other themes keep the editor-wide ItemSpacing.
    const bool bentoRows = UseBentoLayout();
    if (bentoRows) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                       ImVec2(ImGui::GetStyle().ItemSpacing.x, 3.0f * m_UIScale));
    for (auto entity : ViewInCreationOrder(world.Registry, world.Registry.view<const NameComponent>())) {
        if (!MatchesHierarchyFilter(world, entity)) continue;
        // A parented entity draws nested under its parent, not as a sibling — except while
        // filtering, where the parent may be filtered out, so matches are shown flat instead.
        if (!filtering) {
            const auto* hier = world.Registry.try_get<HierarchyComponent>(entity);
            if (hier && hier->Parent != entt::null) continue;
        }
        DrawHierarchyNode(world, assets, entity, world.Registry.all_of<LevelGeometryTag>(entity));
    }
    if (bentoRows) ImGui::PopStyleVar();
    ImGui::PopStyleVar(); // IndentSpacing

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
    // Arrow / Home / End / type-to-select nav over the same visible-row list.
    HandleHierarchyKeyboardNav(world);
    // ImGui::End() for the "Scene Hierarchy" window is the module's — it owns Begin() now.
}

bool EditorLayer::MatchesHierarchyFilter(const World& world, entt::entity entity) const {
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

void EditorLayer::DrawHierarchyNode(World& world, AssetLibrary& assets, entt::entity entity, bool isLevelGeometry) {
    if (!world.Registry.valid(entity)) return;

    // This row is about to be drawn — record it in visible top-to-bottom order for Ctrl+A and
    // Shift+Click. Children append themselves in the recursive calls below, and only when this
    // node is expanded, so the list mirrors exactly what the user sees.
    m_HierarchyVisibleBuild.push_back(entity);

    auto& name = world.Registry.get<NameComponent>(entity);
    bool selected = IsSelected(entity);
    bool inactive = world.Registry.all_of<InactiveTag>(entity);
    // #236 B — SceneVis-lite: hidden rows read like inactive ones (dimmed name); locked rows
    // aren't dimmed (they're still visible) but the row's lock glyph stays lit.
    bool sceneHiddenRow = world.Registry.all_of<HiddenInSceneTag>(entity);
    const auto* hier = world.Registry.try_get<HierarchyComponent>(entity);
    bool hasChildren = hier && !hier->Children.empty() && m_HierarchyFilter.empty();

    ImGui::PushID((int)entt::to_integral(entity));

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
        if (ImGui::InputText("##Rename", m_EntityRenameBuffer, sizeof(m_EntityRenameBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            PushUndo(world, "Rename");
            name.Name = SanitizeEntityName(m_EntityRenameBuffer);
            m_RenamingEntity = entt::null;
        }
        if (ImGui::IsItemDeactivated()) m_RenamingEntity = entt::null;
        ImGui::PopID();
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
    const char* primaryGlyph =
        hasMesh     ? ICON_FA_CUBE          :
        hasLight    ? ICON_FA_LIGHTBULB     :
        hasCamera   ? ICON_FA_VIDEO         :
        hasChildren ? ICON_FA_LAYER_GROUP   : ICON_FA_VECTOR_SQUARE;
    const char* secondaryGlyph =
        (hasMesh && hasLight)   ? ICON_FA_LIGHTBULB :
        (hasMesh && hasCamera)  ? ICON_FA_VIDEO     :
        (hasLight && hasCamera) ? ICON_FA_VIDEO     : nullptr;
    // Muted per-kind tint so kinds separate at a glance without the panel turning to confetti —
    // amber light, blue camera (the #234 accent roles); mesh/empty stay near the text colour
    // since they're the bulk of every scene. Overridden to the disabled grey on inactive rows.
    const ImU32 kindCol =
        hasLight  ? IM_COL32(232, 196, 104, 255) :
        hasCamera ? IM_COL32( 91, 157, 249, 255) :
        hasMesh   ? IM_COL32(214, 214, 218, 255) : IM_COL32(148, 148, 156, 255);

    // Never-named entities get a positional fallback instead of a wall of identical
    // "(unnamed)" rows (#21 P10).
    std::string shownName = name.Name;
    if (shownName.empty())
        shownName = "Object " + std::to_string(CreationOrdinal(world.Registry, entity));

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
    bool open = hasChildren && ImGui::GetStateStorage()->GetInt(nodeStateId, 1) != 0;

    // Empty label: the tree node owns the indent / full-row hitbox and every interaction handler
    // below; the chevron + glyph slot + name are painted afterward at a constant X.
    ImGui::TreeNodeEx("##node", nodeFlags, "%s", "");
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
    // The active-state eye lives in a fixed right column (drawn below) that the full-width node
    // overlaps. Carve its X band out of the row's own click/select/rename handling so a click —
    // or double-click — on the eye is the eye's alone.
    const float eyeBandW = ImGui::GetFontSize() * 1.6f + 8.0f * m_UIScale;
    const bool overEye = rowHovered && ImGui::GetIO().MousePos.x > rowMax.x - eyeBandW;

    if (ImGui::IsItemClicked() && clickOnArrow) {
        open = !open;
        ImGui::GetStateStorage()->SetInt(nodeStateId, open ? 1 : 0);
        if (ImGui::GetIO().KeyAlt) {
            for (entt::entity child : hier->Children) SetHierarchyExpandedRecursive(world, child, open);
        }
    } else if (ImGui::IsItemClicked() && !overEye) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift) {
            // Shift (or Ctrl+Shift) — contiguous range from the anchor to this row.
            SelectHierarchyRange(world, entity, io.KeyCtrl);
        } else {
            SelectItem(entity, io.KeyCtrl); // plain click replaces; Ctrl+Click toggles. Both re-anchor.
        }
        m_HierarchyRowHintDone = true; // learned the row interaction — stop showing the hint
    }
    if (ImGui::IsItemHovered() && !clickOnArrow && !overEye && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        BeginRenameEntity(entity);
    }
    if (!m_HierarchyRowHintDone && ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
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
        const bool  isFirstRow = m_HierarchyVisibleBuild.size() == 1;
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
            const ImU32 accent = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
            const ImU32 fill   = (accent & 0x00FFFFFFu) | 0x44000000u; // same hue, ~27% alpha block
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
        // Existing behavior: dropping a texture from the Asset Browser assigns it as Albedo.
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE_PATH")) {
            std::string texPath((const char*)payload->Data);
            if (auto* renderable = world.Registry.try_get<RenderableComponent>(entity)) {
                PushUndo(world, "Set Albedo Map");
                Model* model = renderable->ModelRef.get();
                auto override_ = model->MaterialOverride();
                if (!override_) {
                    override_ = std::make_shared<Material>();
                    if (model->MeshCount() > 0) {
                        const Material& imported = model->MeshMaterial(0);
                        override_->NormalMap = imported.NormalMap;
                        override_->MetallicMap = imported.MetallicMap;
                        override_->RoughnessMap = imported.RoughnessMap;
                        override_->AOMap = imported.AOMap;
                        override_->EmissiveMap = imported.EmissiveMap;
                    }
                    model->SetMaterialOverride(override_);
                }
                override_->AlbedoMap = assets.LoadTexture(texPath);
            }
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
        const ImU32 col = ImGui::GetColorU32((inactive || sceneHiddenRow) ? ImGuiCol_TextDisabled : ImGuiCol_Text);

        // Disclosure chevron — a light Font Awesome ">" / "v" (0.66em) centred in the leading
        // slot, in the dim text colour, brightening on arrow-hover. Replaces ImGui's chunky
        // filled triangle.
        if (hasChildren) {
            const float chSize = fontSize * 0.66f;
            const char* chev = open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT;
            const ImVec2 cm = ImGui::GetFont()->CalcTextSizeA(chSize, FLT_MAX, 0.0f, chev);
            const ImU32 chCol = ImGui::GetColorU32(clickOnArrow ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            dl->AddText(ImGui::GetFont(), chSize,
                        ImVec2(rowMin.x + (fontSize * 1.1f - cm.x) * 0.5f,
                               rowMin.y + (ImGui::GetFrameHeight() - cm.y) * 0.5f),
                        chCol, chev);
        }
        dl->AddText(ImVec2(labelX, rowMin.y), inactive ? col : kindCol, primaryGlyph);
        if (secondaryGlyph) {
            const float sub = fontSize * 0.68f;
            const ImU32 secBase = inactive ? col
                                : (hasMesh && hasLight) ? IM_COL32(232, 196, 104, 255)
                                                        : IM_COL32(91, 157, 249, 255);
            dl->AddText(ImGui::GetFont(), sub,
                        ImVec2(labelX + slotW - sub, rowMin.y + fontSize - sub),
                        (secBase & 0x00FFFFFFu) | 0xB4000000u, secondaryGlyph);
        }
        dl->AddText(ImVec2(labelX + slotW, rowMin.y), col, shownName.c_str());
    }

    // Active-state eye, pinned to a fixed right-hand column so every row's eye lines up no matter
    // how deep it sits. Drawn after the row so a click on it never also selects the row.
    {
        const float eyeW = ImMax(ImGui::CalcTextSize(ICON_FA_EYE).x, ImGui::CalcTextSize(ICON_FA_EYE_SLASH).x) + 2.0f;
        const float gap  = 4.0f * m_UIScale;
        const float lockW = ImMax(ImGui::CalcTextSize(ICON_FA_LOCK).x, ImGui::CalcTextSize(ICON_FA_LOCK_OPEN).x) + 2.0f;
        const float hideW = ImMax(ImGui::CalcTextSize(ICON_FA_GHOST).x, ImGui::CalcTextSize(ICON_FA_EYE_LOW_VISION).x) + 2.0f;

        // #236 B — SceneVis-lite: hide (viewport-only) + lock (unpickable) glyphs, hover-reveal,
        // just left of the Active eye. These never touch the object itself — Game view, physics
        // and saves are unaffected.
        const bool sceneHidden = world.Registry.all_of<HiddenInSceneTag>(entity);
        const bool sceneLocked = world.Registry.all_of<SceneLockedTag>(entity);

        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(rowMax.x - eyeW - gap - lockW - hideW - 2.0f * gap, rowMin.y));
        if (SceneVisToggle("##svhide", ICON_FA_GHOST, ICON_FA_EYE_LOW_VISION, sceneHidden, rowHovered,
                           sceneHidden ? "Hidden in the Scene view - click to show"
                                       : "Hide in the Scene view (still in the game, still collides, still saved)")) {
            PushUndo(world, "Toggle Scene Visibility");
            if (sceneHidden) world.Registry.remove<HiddenInSceneTag>(entity);
            else             world.Registry.emplace<HiddenInSceneTag>(entity);
        }
        ImGui::SameLine(0.0f, gap);
        if (SceneVisToggle("##svlock", ICON_FA_LOCK, ICON_FA_LOCK_OPEN, sceneLocked, rowHovered,
                           sceneLocked ? "Locked out of Scene-view clicks - click to unlock"
                                       : "Lock: can't be clicked in the Scene view (Hierarchy select still works)")) {
            PushUndo(world, "Toggle Scene Lock");
            if (sceneLocked) world.Registry.remove<SceneLockedTag>(entity);
            else             world.Registry.emplace<SceneLockedTag>(entity);
        }

        ImGui::SetCursorScreenPos(ImVec2(rowMax.x - eyeW - 4.0f * m_UIScale, rowMin.y));
        if (ActiveToggle("##rowactive", !inactive, rowHovered,
                         inactive ? "Inactive - click to enable" : "Active - click to disable",
                         /*alignTop=*/true)) {
            PushUndo(world, "Toggle Active");
            if (inactive) world.Registry.remove<InactiveTag>(entity);
            else world.Registry.emplace<InactiveTag>(entity);
        }
    }

    if (hasChildren && open) {
        // Every row is NoTreePushOnOpen now, so ImGui no longer auto-indents children — do it
        // here (IndentSpacing is the tightened 13*uiScale pushed by DrawHierarchyTreeBody).
        ImGui::Indent(ImGui::GetStyle().IndentSpacing);
        // Sorted by OrderComponent (not raw HierarchyComponent::Children insertion order) so
        // sibling reordering shows, and it's a fresh copy anyway — a re-parent or delete from a
        // child's own context menu would otherwise mutate Children mid-iteration.
        std::vector<entt::entity> children = HierarchySiblingsInOrder(world, entity);
        for (entt::entity child : children) {
            if (world.Registry.valid(child)) DrawHierarchyNode(world, assets, child, isLevelGeometry);
        }
        ImGui::Unindent(ImGui::GetStyle().IndentSpacing);
    }

    ImGui::PopID();
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

    ImGui::Separator();
    const bool anyEntities = world.Registry.view<const NameComponent>().begin() != world.Registry.view<const NameComponent>().end();
    if (ImGui::MenuItem(ICON_FA_OBJECT_UNGROUP "  Select All", "Ctrl+A", false, anyEntities)) SelectAllEntities(world);
    if (ImGui::MenuItem(ICON_FA_BAN "  Deselect All", "Ctrl+Shift+A", false, HasAnySelection())) ClearSelection();
    if (ImGui::MenuItem(ICON_FA_RIGHT_LEFT "  Invert Selection", "Ctrl+I", false, anyEntities)) InvertSelection(world);
    ImGui::Separator();
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
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_EYE "  Toggle Active State", "Alt+Shift+A", false, HasAnySelection()))
        ToggleSelectionActive(world);
    if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS "  Move To View", nullptr, false, HasAnySelection()))
        MoveSelectionToView(world);
    if (ImGui::IsItemHovered() && HasAnySelection())
        EditorUI::SetTooltip("Move the selection to just in front of the editor camera.");
    if (hasEntity && world.Registry.all_of<CameraComponent>(entity)) {
        if (ImGui::MenuItem(ICON_FA_VIDEO "  Align With View") && m_EditorCameraPtr) {
            PushUndo(world, "Align Camera to View");
            auto& t = world.Registry.get<TransformComponent>(entity);
            t.Position = m_EditorCameraPtr->Position;
            glm::vec3 d = glm::normalize(m_EditorCameraPtr->Front());
            t.RotationEuler = glm::vec3(glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))),
                                        glm::degrees(std::atan2(-d.x, -d.z)), 0.0f);
            world.Registry.get<CameraComponent>(entity).FovDegrees = m_EditorCameraPtr->Fov;
        }
    }
    ImGui::Separator();

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

    const bool isLight = hasEntity && world.Registry.all_of<LightComponent>(entity);
    if (isLight) {
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_DOWN_LONG "  Drop Light to Surface")) {
            if (!DropLightToSurface(world, entity))
                Log::Info("Drop to surface: nothing directly below this light.");
        }
    }
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_PEN "  Rename", "F2", false, hasEntity)) BeginRenameEntity(entity);
    if (ImGui::MenuItem(ICON_FA_BOX_ARCHIVE "  Save as Prefab...", nullptr, false, hasEntity)) {
        std::string path = FileDialog::SaveFile("Prefab Files\0*.prefab\0All Files\0*.*\0", "prefab", m_Window);
        if (!path.empty() && SceneSerializer::SavePrefab(world, entity, path)) {
            assets.RegisterPrefab(path);
        }
    }
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_TRASH "  Delete", "Del", false, hasEntity)) DeleteSelection(world);
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

    // Nothing with a Box Collider can be re-parented yet (SetParent refuses it) — check up front
    // so we don't create a stray "Group" empty that ends up with no children.
    bool anyReparentable = false;
    for (entt::entity e : sel) {
        if (!world.Registry.all_of<ColliderComponent>(e)) { anyReparentable = true; break; }
    }
    if (!anyReparentable) {
        Log::Warn("Couldn't group: objects with a Box Collider can't be re-parented yet.");
        return;
    }

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
        if (world.SetParent(e, parent)) ++parented; // preserves world transform; refuses colliders
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

void EditorLayer::ReorderHierarchySiblings(World& world, const std::vector<entt::entity>& movingIn,
                                           entt::entity anchor, bool after) {
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

    StageUndo(world);

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
    for (int i = 0; i < (int)rebuilt.size(); ++i)
        world.Registry.emplace_or_replace<OrderComponent>(rebuilt[i], i);

    CommitStagedUndo(world, "Reorder");
    if (anyFailed)
        Log::Warn("Some rows couldn't be moved there — level geometry with a collider can't be parented.");
}

entt::entity EditorLayer::InstantiateAssetDropInHierarchy(World& world, AssetLibrary& assets,
                                                          const char* modelPath, const char* prefabPath,
                                                          entt::entity parent) {
    entt::entity e = entt::null;
    if (modelPath && *modelPath) {
        PushUndo(world, "Place Model");
        auto model = assets.InstantiateModel(modelPath);
        std::string name = std::filesystem::path(modelPath).stem().string();
        e = world.CreateModelEntity(model, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f),
                                    UniqueNameFor(world, name));
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
        if (world.Registry.valid(e) && !world.Registry.all_of<InactiveTag>(e)) { anyActive = true; break; }
    StageUndo(world);
    for (entt::entity e : sel) {
        if (!world.Registry.valid(e)) continue;
        if (anyActive) world.Registry.emplace_or_replace<InactiveTag>(e); // mixed/all-active -> disable all
        else world.Registry.remove<InactiveTag>(e);
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
    SelectItem(e, false);
    Log::Info(std::string("Added ") + name + ".");
    return e;
}
