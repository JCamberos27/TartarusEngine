// Viewport interaction: the transform/group/view gizmos, light handles and gizmo overlays,
// entity icons, mouse picking, vertex snap/drag, the viewport drop target, and the camera
// framing / look-through navigation they share. Split out of EditorLayer.cpp (#179).

#include "EditorLayer.h"
#include "EditorSound.h"
#include "Enhancers/EnhancerCore.h"
#include "EditorPanels.h"
#include "EditorLayerInternal.h"
#include "EditorTheme.h"
#include "Glass/GlassMotion.h" // EaseProgress, CameraFrame
#include "FileDialog.h"
#include "AssetLibrary.h"
#include "World.h"
#include "Camera.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"
#include "AudioEngine.h"
#include "SceneSerializer.h"
#include "AABB.h"
#include "Log.h"
#include "EditorSettings.h"
#include "GizmoIconMath.h"
#include "Shortcuts.h" // vRuler: Shift+R is a held binding
#include "EditorUIHelpers.h"
#include "PhysicsWorld.h"   // #185 — mirror gizmo edits into the live actor while playing
#include "AssetImporterInspector.h"
#include "Profiler.h"
#include "ProjectPaths.h"
#include "LayerRegistry.h"
#include "GLStateCache.h"
#include "Framebuffer.h"
#include "gl.h"
#include "DecalAsset.h"

#include <imgui.h>
#include <imgui_internal.h> // ImMax/ImFloor, ImGuiWindow, and the item-flag helpers the panels use
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <IconsFontAwesome6.h>
#include <ImGuizmo.h>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4505) // ImViewGuizmo.h defines a couple of static helpers this TU doesn't call
#endif
#include <ImViewGuizmo.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

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
#include <cstdio>
#include <cctype>
#include <cstring>
#include <functional>
#include <cfloat>
#include <random>

using namespace EditorInternal;


namespace {

// #236 A1 — an entity is unclickable in the viewport when its layer's bit is set in the
// editor's pick-lock mask. Hierarchy selection is unaffected. Absent LayerComponent == layer 0.
inline bool ViewportPickLocked(const World& world, entt::entity e) {
    const auto* lc = world.Registry.try_get<LayerComponent>(e);
    const int layer = lc ? lc->Layer : 0;
    if (layer < 0 || layer >= 32) return false;
    return (EditorSettings::Get().LayerPickLockMask >> layer) & 1u;
}

// #236 B — SceneVis-lite: an entity that is hidden or locked in the Scene view is not
// selectable there by click or marquee. Hierarchy selection and, once selected, the gizmo still
// work — matching Unity's SceneVis lock.
inline bool NotSceneSelectable(const World& world, entt::entity e) {
    return world.Registry.all_of<HiddenInSceneTag>(e) || world.Registry.all_of<SceneLockedTag>(e);
}

// #236 A1 + B combined — skip an entity in any viewport pick loop when it's pick-locked by
// layer or hidden/locked by SceneVis.
inline bool ViewportUnpickable(const World& world, entt::entity e) {
    return ViewportPickLocked(world, e) || NotSceneSelectable(world, e);
}

// TransformComponent is LOCAL space once an entity has a parent, so anything that manipulates an
// entity in world space has to bracket the work with these two: read the world matrix, do the
// math there, then convert the result back into the parent's frame before writing it back.
// Unparented entities get an identity parent matrix, making both a no-op — exactly what
// World::SetParent does with glm::inverse(newParentWorld) * worldMatrix. (#224)
inline glm::mat4 ParentWorldMatrix(const World& world, entt::entity entity) {
    if (entity == entt::null || !world.Registry.valid(entity)) return glm::mat4(1.0f);
    const auto* hier = world.Registry.try_get<HierarchyComponent>(entity);
    if (!hier || hier->Parent == entt::null) return glm::mat4(1.0f);
    return world.ComposeWorldTransform(hier->Parent);
}

// True if any ancestor of `entity` is itself in the selection. Such an entity is already carried
// along by that ancestor's move (that is what parenting means), so a group operation that also
// applied its own world delta to the child would move it twice. (#224)
inline bool HasSelectedAncestor(const World& world, entt::entity entity, entt::entity primary,
                                const std::vector<entt::entity>& extras) {
    const auto* hier = world.Registry.try_get<HierarchyComponent>(entity);
    entt::entity walk = hier ? hier->Parent : entt::null;
    while (walk != entt::null && world.Registry.valid(walk)) {
        if (walk == primary) return true;
        for (entt::entity e : extras) if (e == walk) return true;
        const auto* parentHier = world.Registry.try_get<HierarchyComponent>(walk);
        walk = parentHier ? parentHier->Parent : entt::null;
    }
    return false;
}

// Writes `worldPosition` (an entity ORIGIN in world space) into an entity's local
// TransformComponent.Position. Rotation/scale are untouched, so this is only correct for pure
// translations — which is all the vertex drag and Snap to Ground ever do.
inline void SetWorldPosition(World& world, entt::entity entity, const glm::vec3& worldPosition) {
    auto& transform = world.Registry.get<TransformComponent>(entity);
    glm::mat4 toLocal = glm::inverse(ParentWorldMatrix(world, entity));
    transform.Position = glm::vec3(toLocal * glm::vec4(worldPosition, 1.0f));
}

// The gizmo drag and the Inspector's own number fields edit the same thing; give the History
// entry the same verb either way ("Move" / "Rotate" / "Scale") instead of a generic
// "Transform" from the gizmo path only (#19 P8).
// #116 / #117 — the editor's surface queries (click picking, drop-to-surface, measure, surface
// snap, Snap to Ground, drop light) all used to intersect each renderable's world-space AABB.
// That selected big/concave objects by clicking empty space inside their box, and — with the
// camera INSIDE a large object's box (a room, terrain) — returned t = 0 for it on every click,
// so nothing inside could be picked; surfaces were also "found" on bounding boxes. This does an
// AABB broadphase and then an exact test against the model's bind-pose triangles.
struct SurfaceHit {
    entt::entity Entity = entt::null;
    float T = 1e30f;
    glm::vec3 Normal{0.0f, 1.0f, 0.0f};
    int Mesh = -1; // index into the model's meshes = its material slot
};
template <class SkipFn>
SurfaceHit RaycastRenderables(const World& world, const glm::vec3& origin, const glm::vec3& dir, SkipFn skip,
                              float maxT = 1e30f) {
    SurfaceHit best;
    best.T = maxT;
    auto view = world.Registry.view<const RenderableComponent>(entt::exclude<InactiveTag>);
    for (auto entity : view) {
        if (skip(entity)) continue;
        const auto& r = view.get<const RenderableComponent>(entity);
        if (!r.ModelRef || r.ModelRef->MeshCount() == 0) continue;
        const glm::mat4 model = world.ComposeWorldTransform(entity);
        const AABB wb = AABB{r.ModelRef->BoundsMin(), r.ModelRef->BoundsMax()}.Transformed(model);
        float boxT;
        if (!wb.RayIntersect(origin, dir, boxT) || boxT >= best.T) continue; // broadphase
        float t;
        glm::vec3 n;
        int mesh = -1;
        if (r.ModelRef->RaycastTriangles(model, origin, dir, t, &n, 1e-4f, &mesh) && t < best.T) {
            best.Entity = entity;
            best.T = t;
            best.Normal = n;
            best.Mesh = mesh;
        }
    }
    return best;
}

// A decal's world rotation on a surface with normal `n`: +Z out of the surface, +X along `right`
// (the view's right) flattened onto it, so a placed decal reads upright from where it was placed.
glm::mat3 DecalSurfaceBasis(const glm::vec3& n, const glm::vec3& right) {
    glm::vec3 x = right - n * glm::dot(right, n);
    if (glm::dot(x, x) < 1e-6f) // looking straight down the normal: any in-plane axis will do
        x = glm::cross(std::abs(n.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0), n);
    x = glm::normalize(x);
    return glm::mat3(x, glm::cross(n, x), n);
}

// The decal's box (the unit cube under its Size) in world space.
glm::mat4 DecalBoxToWorld(const World& world, entt::entity e, const DecalComponent& d) {
    return world.ComposeWorldTransform(e) * glm::scale(glm::mat4(1.0f), d.Size);
}

// Ray parameter where the ray enters the decal's box (0 when it starts inside), or -1 for a miss.
float RayDecalBox(const glm::mat4& boxToWorld, const glm::vec3& origin, const glm::vec3& dir) {
    const glm::mat4 inv = glm::inverse(boxToWorld);
    const glm::vec3 o(inv * glm::vec4(origin, 1.0f)), d(inv * glm::vec4(dir, 0.0f));
    float t0 = 0.0f, t1 = 1e30f;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(d[a]) < 1e-12f) {
            if (o[a] < -0.5f || o[a] > 0.5f) return -1.0f;
            continue;
        }
        float ta = (-0.5f - o[a]) / d[a], tb = (0.5f - o[a]) / d[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return -1.0f;
    }
    return t0;
}

const char* GizmoOpUndoLabel(GizmoOp op) {
    switch (op) {
        case GizmoOp::Rotate:    return "Rotate";
        case GizmoOp::Scale:     return "Scale";
        case GizmoOp::Rect:      return "Edit Bounds";
        case GizmoOp::Universal: return "Transform";
        case GizmoOp::Translate:
        default:                 return "Move";
    }
}

// Motion v2 — gizmo hover springs. ImGuizmo draws the hovered / dragged handle in its SELECTION
// colour and every handle with the style's stroke sizes, so both are eased here for one Manipulate
// call and put back right after (the base values belong to the theme). The hovered handle comes
// from last frame's Manipulate, which is a frame late and invisible at a spring's pace. While the
// pointer is on the gizmo its strokes grow (GizmoHoverGrow), the handle under it crosses from its
// axis colour toward white, and the axis being dragged breathes slightly brighter at GizmoActiveHz.
// Nothing hovered and settled: no state and the style untouched.
int GizmoHandleAxis(ImGuizmo::MOVETYPE t) {
    switch (t) {
        case ImGuizmo::MT_MOVE_X: case ImGuizmo::MT_ROTATE_X: case ImGuizmo::MT_SCALE_X: return 0;
        case ImGuizmo::MT_MOVE_Y: case ImGuizmo::MT_ROTATE_Y: case ImGuizmo::MT_SCALE_Y: return 1;
        case ImGuizmo::MT_MOVE_Z: case ImGuizmo::MT_ROTATE_Z: case ImGuizmo::MT_SCALE_Z: return 2;
        default: return -1; // planes, screen-space rotate, uniform scale
    }
}

ImGuizmo::Style PushGizmoMotionStyle() {
    ImGuizmo::Style& st = ImGuizmo::GetStyle();
    const ImGuizmo::Style saved = st;
    Glass::ImAnimContext& motion = Glass::Motion();
    if (!motion.CategoryOn(Glass::MotionCategory::Viewport)) return saved;
    const ImGuizmo::MOVETYPE active = ImGuizmo::IsUsing() ? ImGuizmo::GetActiveHandleType() : ImGuizmo::MT_NONE;
    const ImGuizmo::MOVETYPE hot = active != ImGuizmo::MT_NONE ? active : ImGuizmo::GetHoveredHandleType();

    static const ImGuiID kGrowId = ImHashStr("##gizmoHoverGrow");
    const float grow = motion.Animate(kGrowId, hot != ImGuizmo::MT_NONE ? 1.0f : 0.0f, 0.0f, Glass::GizmoHover);
    if (grow != 0.0f) {
        const float k = 1.0f + Glass::GizmoHoverGrow * grow;
        st.TranslationLineThickness *= k;
        st.TranslationLineArrowSize *= k;
        st.ScaleLineThickness *= k;
        st.ScaleLineCircleSize *= k;
        st.CenterCircleSize *= k;
        st.RotationLineThickness *= k;
        st.RotationOuterLineThickness *= k;
    }
    if (hot != ImGuizmo::MT_NONE) {
        // One channel per handle, so sliding from one handle onto the next lights the new one from
        // its own colour instead of it arriving already white.
        static const ImGuiID kLitId = ImHashStr("##gizmoHandleLit");
        const float lit = ImClamp(motion.Animate(kLitId + (ImGuiID)hot, 1.0f, 0.0f, Glass::GizmoHover), 0.0f, 1.0f);
        const int axis = GizmoHandleAxis(hot);
        const ImVec4 base = axis >= 0 ? saved.Colors[ImGuizmo::DIRECTION_X + axis] : saved.Colors[ImGuizmo::SELECTION];
        float toWhite = 0.55f * lit;
        if (active != ImGuizmo::MT_NONE) toWhite += 0.14f * (0.5f + 0.5f * motion.Oscillate(Glass::GizmoActiveHz));
        ImVec4 c = Glass::MixOklab(base, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), ImClamp(toWhite, 0.0f, 1.0f));
        c.w = 1.0f;
        st.Colors[ImGuizmo::SELECTION] = c;
    }
    return saved;
}

void PopGizmoMotionStyle(const ImGuizmo::Style& saved) { ImGuizmo::GetStyle() = saved; }

} // namespace


bool EditorLayer::ComputeSelectionBounds(World& world, glm::vec3& outMin, glm::vec3& outMax) const {
    if (!HasAnySelection()) return false;

    glm::vec3 boundsMin(1e30f), boundsMax(-1e30f);
    bool any = false;
    // Every placeable-in-the-world entity used to have a real Renderable Model (former-boxes
    // included, via the cube primitive) — that stopped being true once lights and empties were
    // added, so this can no longer assume RenderableComponent exists (it used to call the
    // unchecked get<>(), which is undefined behavior — a crash in practice — the instant a
    // light/empty was selected, since this runs every frame via GetSelectionCenter). A
    // mesh-less entity contributes a small nominal box around its own origin instead, matching
    // the fallback DrawGizmo already uses for the same case, so focus/orbit still center on it
    // sensibly rather than crashing or silently contributing nothing.
    auto expand = [&](entt::entity entity) {
        if (!world.Registry.valid(entity)) return;
        glm::mat4 m = world.ComposeWorldTransform(entity);
        glm::vec3 localMin(-0.5f), localMax(0.5f);
        if (const auto* renderable = world.Registry.try_get<RenderableComponent>(entity);
            renderable && renderable->ModelRef && renderable->ModelRef->MeshCount() > 0) {
            localMin = renderable->ModelRef->BoundsMin();
            localMax = renderable->ModelRef->BoundsMax();
        }
        AABB bounds = AABB{localMin, localMax}.Transformed(m);
        boundsMin = glm::min(boundsMin, bounds.Min);
        boundsMax = glm::max(boundsMax, bounds.Max);
        any = true;
    };
    expand(m_Selected);
    for (entt::entity e : m_ExtraSelection) expand(e);
    if (!any) return false;

    outMin = boundsMin;
    outMax = boundsMax;
    return true;
}

bool EditorLayer::GetSelectionCenter(World& world, glm::vec3& outCenter) const {
    glm::vec3 boundsMin, boundsMax;
    if (!ComputeSelectionBounds(world, boundsMin, boundsMax)) return false;
    outCenter = (boundsMin + boundsMax) * 0.5f;
    return true;
}

bool EditorLayer::FrameFirstPersonBody(World& world, Camera& editorCamera) {
    auto bodies = world.Registry.view<FirstPersonBodyComponent>(entt::exclude<InactiveTag>);
    if (bodies.begin() == bodies.end()) return false;
    const glm::mat4 xf = world.ComposeWorldTransform(*bodies.begin());
    const glm::vec3 feet(xf[3]);
    glm::vec3 ahead(xf[2].x, 0.0f, xf[2].z); // the body faces its +Z
    ahead = glm::length(ahead) > 1e-4f ? glm::normalize(ahead) : glm::vec3(0.0f, 0.0f, 1.0f);
    editorCamera.Position = feet + ahead * 2.6f + glm::vec3(0.0f, 1.45f, 0.0f);
    const glm::vec3 look = glm::normalize(feet + glm::vec3(0.0f, 1.0f, 0.0f) - editorCamera.Position);
    editorCamera.Yaw = glm::degrees(std::atan2(look.z, look.x));
    editorCamera.Pitch = glm::degrees(std::asin(std::clamp(look.y, -1.0f, 1.0f)));
    return true;
}

void EditorLayer::FrameSceneBounds(World& world, Camera& editorCamera) {
    glm::vec3 mn, mx;
    if (!ComputeSceneBounds(world, mn, mx)) return;
    glm::vec3 center = (mn + mx) * 0.5f;
    float radius = std::max(glm::length(mx - mn) * 0.5f, 0.5f);
    float halfFov = glm::radians(editorCamera.Fov) * 0.5f;
    float distance = (radius / std::sin(halfFov)) * 1.35f;
    glm::vec3 targetPos = center - editorCamera.Front() * distance;
    // Last line of defence: a bounds value that still went non-finite (huge scene, overflow)
    // must not strand the camera at inf/NaN — leave it where it is instead.
    if (!std::isfinite(targetPos.x) || !std::isfinite(targetPos.y) || !std::isfinite(targetPos.z)) return;

    // Glide there rather than teleport — same eased transition FocusOnSelection uses.
    SettleViewMorph(editorCamera); // Motion v2: never inherit a half-finished ortho morph
    m_ViewTransition.Active = true;
    m_ViewTransition.T = 0.0f;
    m_ViewTransition.FromPos = editorCamera.Position;
    m_ViewTransition.FromYaw = editorCamera.Yaw;
    m_ViewTransition.FromPitch = editorCamera.Pitch;
    m_ViewTransition.FromOrthoHalfHeight = editorCamera.OrthoHalfHeight;
    m_ViewTransition.FromFov = m_ViewTransition.ToFov = editorCamera.Fov;
    m_ViewTransition.ToPos = targetPos;
    m_ViewTransition.ToYaw = editorCamera.Yaw;     // aim unchanged
    m_ViewTransition.ToPitch = editorCamera.Pitch;
    m_ViewTransition.ToOrthoHalfHeight = editorCamera.Orthographic
        ? (distance * std::tan(halfFov)) : editorCamera.OrthoHalfHeight;
}

void EditorLayer::FocusOnSelection(World& world, Camera& editorCamera) {
    glm::vec3 boundsMin, boundsMax;
    if (!ComputeSelectionBounds(world, boundsMin, boundsMax)) return;
    if (!m_QuietFrameSelection) EditorSound::Play(EditorSound::Event::FrameSelected);

    glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
    float radius = glm::length(boundsMax - boundsMin) * 0.5f;
    radius = std::max(radius, 0.5f); // guard against a degenerate/zero-size bounds parking the camera inside it

    // Keep the camera's current aim, just slide it back along that same ray until the
    // selection's bounding sphere fits inside the vertical field of view, with a margin.
    float halfFov = glm::radians(editorCamera.Fov) * 0.5f;
    float distance = (radius / std::sin(halfFov)) * 1.35f;
    glm::vec3 targetPos = center - editorCamera.Front() * distance;
    if (!std::isfinite(targetPos.x) || !std::isfinite(targetPos.y) || !std::isfinite(targetPos.z)) return;

    // Glide there rather than teleport — same eased transition SnapToView uses (audit follow-up).
    SettleViewMorph(editorCamera); // Motion v2: never inherit a half-finished ortho morph
    m_ViewTransition.Active = true;
    m_ViewTransition.T = 0.0f;
    m_ViewTransition.FromPos = editorCamera.Position;
    m_ViewTransition.FromYaw = editorCamera.Yaw;
    m_ViewTransition.FromPitch = editorCamera.Pitch;
    m_ViewTransition.FromOrthoHalfHeight = editorCamera.OrthoHalfHeight;
    m_ViewTransition.FromFov = m_ViewTransition.ToFov = editorCamera.Fov;
    m_ViewTransition.ToPos = targetPos;
    m_ViewTransition.ToYaw = editorCamera.Yaw;     // aim unchanged
    m_ViewTransition.ToPitch = editorCamera.Pitch;
    m_ViewTransition.ToOrthoHalfHeight = editorCamera.Orthographic
        ? (distance * std::tan(halfFov)) : editorCamera.OrthoHalfHeight;
}

// #236 E — Hand tool (Q): a left-drag that starts inside the Scene viewport pans the editor
// camera parallel to the view plane, the same motion as a middle-mouse drag, with no picking or
// gizmo interaction while the tool is active.
void EditorLayer::HandleHandToolPan(Camera& editorCamera) {
    ImGuiIO& io = ImGui::GetIO();
    const bool overViewport =
        m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f &&
        io.MousePos.x >= m_ViewportPos.x && io.MousePos.x <= m_ViewportPos.x + m_ViewportSize.x &&
        io.MousePos.y >= m_ViewportPos.y && io.MousePos.y <= m_ViewportPos.y + m_ViewportSize.y;

    if (overViewport && !WantsCaptureMouse()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_HandPanActive = overViewport && !WantsCaptureMouse() && !m_ViewGizmoBlocking;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { m_HandPanActive = false; return; }
    if (!m_HandPanActive) return;

    // Same fixed rate as the middle-mouse pan in UpdateEditorCamera. Horizontal grabs the scene
    // (drag right -> view moves left); vertical is inverted from that on purpose (drag down ->
    // camera moves up), matching how the user expects the Hand tool to feel here.
    const float kPanSpeed = 0.01f;
    editorCamera.Position +=
        (editorCamera.Up() * io.MouseDelta.y - editorCamera.Right() * io.MouseDelta.x) * kPanSpeed;
}

// #236 E — Lock View to Selected (Shift+F): each frame, shift the camera position by however
// much the selection's centroid moved since last frame, so the camera rides along with a moving
// selection without changing its orientation or zoom. Any manual reframe (F) or an empty
// selection re-arms the tracking so re-acquiring a target never snaps the view.
void EditorLayer::UpdateLockViewToSelection(World& world, Camera& editorCamera) {
    if (!m_LockViewToSelection || m_ViewTransition.Active) { m_LockViewHasCentroid = false; return; }

    glm::vec3 bmin, bmax;
    if (!ComputeSelectionBounds(world, bmin, bmax)) { m_LockViewHasCentroid = false; return; }
    const glm::vec3 centroid = (bmin + bmax) * 0.5f;

    // Only the selection moving carries the camera, not a different selection: picking another object jumped the
    // camera by the distance between the two.
    thread_local std::vector<entt::entity> selection;
    selection.clear();
    selection.push_back(m_Selected);
    selection.insert(selection.end(), m_ExtraSelection.begin(), m_ExtraSelection.end());
    if (selection != m_LockViewSelection) {
        m_LockViewSelection = selection;
        m_LockViewHasCentroid = false;
    }

    if (m_LockViewHasCentroid) {
        const glm::vec3 delta = centroid - m_LockViewCentroid;
        if (std::isfinite(delta.x) && std::isfinite(delta.y) && std::isfinite(delta.z))
            editorCamera.Position += delta;
    }
    m_LockViewCentroid = centroid;
    m_LockViewHasCentroid = true;
}

bool EditorLayer::CanSnapSelectionToGround(World& world) const {
    return m_Selected != entt::null && world.Registry.valid(m_Selected) &&
        world.Registry.all_of<RenderableComponent>(m_Selected);
}

void EditorLayer::SnapSelectionToGround(World& world) {
    if (!CanSnapSelectionToGround(world)) return;
    PushUndo(world, "Snap to Ground");

    // #117 — every selected object (not just the primary), each dropped onto the SURFACE below
    // it (was: always world Y = 0, so on an upper floor / table / terrain it sank or jumped).
    // Falls back to Y = 0 only when there's nothing underneath. A child of another selected
    // object rides along with its parent instead of being moved twice.
    std::vector<entt::entity> sel = GetSelectedItems();
    auto isSelectedOrUnder = [&](entt::entity e) {
        for (entt::entity c = e; c != entt::null; ) {
            if (IsSelected(c)) return true;
            const auto* h = world.Registry.try_get<HierarchyComponent>(c);
            c = h ? h->Parent : entt::null;
        }
        return false;
    };
    int snapped = 0;
    for (entt::entity e : sel) {
        if (!world.Registry.valid(e) || !world.Registry.all_of<RenderableComponent>(e)) continue;
        if (HasSelectedAncestor(world, e, m_Selected, m_ExtraSelection) && e != m_Selected) continue;
        auto& renderable = world.Registry.get<RenderableComponent>(e);
        if (!renderable.ModelRef) continue;
        ++snapped;
        // World matrix, not ComposeTransform(transform): the drop is computed in world space and
        // converted back to local on write (#224).
        const glm::mat4 m = world.ComposeWorldTransform(e);
        const AABB worldBounds = AABB{renderable.ModelRef->BoundsMin(), renderable.ModelRef->BoundsMax()}.Transformed(m);
        const float lowestY = world.Registry.all_of<LevelGeometryTag>(e)
            ? worldBounds.Min.y : renderable.ModelRef->LowestVertexWorldY(m);
        const glm::vec3 c = (worldBounds.Min + worldBounds.Max) * 0.5f;
        // Cast from just above the object's lowest point, straight down, ignoring the selection.
        const SurfaceHit hit = RaycastRenderables(world, glm::vec3(c.x, lowestY + 0.01f, c.z),
                                                  glm::vec3(0.0f, -1.0f, 0.0f), isSelectedOrUnder);
        const float groundY = hit.Entity != entt::null ? (lowestY + 0.01f - hit.T) : 0.0f;
        const glm::vec3 worldOrigin = glm::vec3(m[3]);
        SetWorldPosition(world, e, {worldOrigin.x, worldOrigin.y + (groundY - lowestY), worldOrigin.z});
    }
    // Nothing had a model to drop (e.g. a failed import): a refusal, not a landing.
    EditorSound::Play(snapped > 0 ? EditorSound::Event::SnapGround : EditorSound::Event::Blocked);
}

bool EditorLayer::ComputeSceneBounds(World& world, glm::vec3& outMin, glm::vec3& outMax) const {
    glm::vec3 boundsMin(1e30f), boundsMax(-1e30f);
    bool any = false;
    for (auto entity : world.Registry.view<TransformComponent, RenderableComponent>()) {
        auto& renderable = world.Registry.get<RenderableComponent>(entity);
        // A model that failed to import contributes no geometry and carries a degenerate
        // (inverted-sentinel) bounds — folding it in poisons the whole scene AABB with
        // ±1e30, which then overflows to inf when FrameSceneBounds takes its length. Skip it.
        if (!renderable.ModelRef || renderable.ModelRef->MeshCount() == 0) continue;
        glm::mat4 m = world.ComposeWorldTransform(entity);
        AABB bounds = AABB{renderable.ModelRef->BoundsMin(), renderable.ModelRef->BoundsMax()}.Transformed(m);
        boundsMin = glm::min(boundsMin, bounds.Min);
        boundsMax = glm::max(boundsMax, bounds.Max);
        any = true;
    }
    // No meshes (lights, empties, cameras only): Frame All used to do nothing at all, while Frame
    // Selected framed the same objects fine. Fall back to where the objects are.
    if (!any) {
        for (auto entity : world.Registry.view<TransformComponent, NameComponent>()) {
            const glm::vec3 p(world.ComposeWorldTransform(entity)[3]);
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
            boundsMin = glm::min(boundsMin, p - glm::vec3(0.5f));
            boundsMax = glm::max(boundsMax, p + glm::vec3(0.5f));
            any = true;
        }
    }
    if (!any) return false;
    outMin = boundsMin;
    outMax = boundsMax;
    return true;
}

void EditorLayer::ComputeViewPivot(World& world, Camera& editorCamera, glm::vec3& outPivot,
                                   float& outFrameRadius) const {
    outFrameRadius = 0.0f;

    if (GetSelectionCenter(world, outPivot)) return;

    if (const SurfaceHit h = RaycastRenderables(world, editorCamera.Position, editorCamera.Front(),
            [](entt::entity) { return false; }, 1000.0f); h.Entity != entt::null) {
        outPivot = editorCamera.Position + editorCamera.Front() * h.T;
        return;
    }

    // Nothing selected and not pointed at anything — frame the whole scene rather than orbiting
    // a fixed point 15 units ahead (which, if the camera had drifted off, left the viewport
    // black with no recovery).
    glm::vec3 sceneMin, sceneMax;
    if (ComputeSceneBounds(world, sceneMin, sceneMax)) {
        outPivot = (sceneMin + sceneMax) * 0.5f;
        outFrameRadius = std::max(glm::length(sceneMax - sceneMin) * 0.5f, 0.5f);
        return;
    }

    const float kDefaultFocusDistance = 15.0f;
    outPivot = editorCamera.Position + editorCamera.Front() * kDefaultFocusDistance;
}

void EditorLayer::SnapToView(World& world, Camera& editorCamera, float yaw, float pitch, bool orthographic) {
    EditorSound::Play(EditorSound::Event::ViewPreset);
    SettleViewMorph(editorCamera); // a retarget mid-morph starts from a real projection
    glm::vec3 pivot;
    float frameRadius = 0.0f;
    ComputeViewPivot(world, editorCamera, pivot, frameRadius);

    // A "distance to pivot" that means the same thing regardless of the CURRENT projection mode.
    // In orthographic mode the camera's literal position is decoupled from zoom level (scroll
    // only changes OrthoHalfHeight, never Position — see UpdateEditorCamera in main.cpp), so
    // computing distance from Position here would use a stale, arbitrary number; derive the
    // perspective-equivalent distance from the current ortho size instead.
    float halfFovTan = tan(glm::radians(editorCamera.Fov) * 0.5f);
    float distance;
    if (frameRadius > 0.0f) {
        // Whole-scene fallback (no selection, not pointed at anything): don't keep the old
        // distance — it could be anywhere. Pull back far enough to fit the scene's bounding
        // sphere in view, same formula FocusOnSelection uses.
        distance = (frameRadius / std::sin(glm::radians(editorCamera.Fov) * 0.5f)) * 1.35f;
    } else {
        distance = editorCamera.Orthographic
            ? editorCamera.OrthoHalfHeight / halfFovTan
            : glm::length(editorCamera.Position - pivot);
    }
    distance = std::max(distance, 0.5f);

    glm::vec3 newFront;
    newFront.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    newFront.y = sin(glm::radians(pitch));
    newFront.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    newFront = glm::normalize(newFront);

    SettleViewMorph(editorCamera); // Motion v2: never inherit a half-finished ortho morph
    m_ViewTransition.Active = true;
    m_ViewTransition.T = 0.0f;
    m_ViewTransition.FromPos = editorCamera.Position;
    m_ViewTransition.FromYaw = editorCamera.Yaw;
    m_ViewTransition.FromPitch = editorCamera.Pitch;
    m_ViewTransition.FromOrthoHalfHeight = editorCamera.OrthoHalfHeight;
    m_ViewTransition.FromFov = m_ViewTransition.ToFov = editorCamera.Fov;
    m_ViewTransition.ToYaw = yaw;
    m_ViewTransition.ToPitch = pitch;
    m_ViewTransition.ToPos = pivot - newFront * distance;
    // Keep apparent scale continuous across a projection-mode switch instead of an arbitrary
    // jump in how big everything suddenly looks.
    m_ViewTransition.ToOrthoHalfHeight = orthographic ? (distance * halfFovTan) : editorCamera.OrthoHalfHeight;

    // Motion v2: orbit the pivot rather than cutting a chord through it, and morph the projection
    // instead of popping it: toward ortho the field of view narrows while the camera pulls back
    // (the pivot keeps its size) and the projection goes flat on landing; from ortho it starts
    // nearly flat, far back, and widens in. Viewport motion off: the projection switches at once.
    m_ViewOrbit = ViewOrbit{};
    if (Glass::Motion().CategoryOn(Glass::MotionCategory::Viewport)) {
        m_ViewOrbit.On = true;
        m_ViewOrbit.Stamp = m_ViewTransition.FromPos;
        m_ViewOrbit.Pivot = pivot;
        m_ViewOrbit.FromDist = editorCamera.Orthographic ? editorCamera.OrthoHalfHeight / halfFovTan
                                                         : glm::length(editorCamera.Position - pivot);
        m_ViewOrbit.ToDist = distance;
        m_ViewOrbit.Fov = editorCamera.Fov;
        if (orthographic != editorCamera.Orthographic) {
            m_ViewOrbit.Morph = orthographic ? 1 : -1;
            m_ViewOrbit.FromHalfH = m_ViewOrbit.FromDist * halfFovTan;
            m_ViewOrbit.ToHalfH = distance * halfFovTan;
            if (!orthographic) {
                editorCamera.Orthographic = false; // perspective now, but nearly flat (see Apply)
                ApplyViewTransition(editorCamera, 0.0f, 0.0f);
            }
            return; // toward ortho, the projection flips when the morph lands
        }
    }
    editorCamera.Orthographic = orthographic; // switches immediately; only angle/position/size animate
}

bool EditorLayer::ViewOrbitLive() const {
    return m_ViewOrbit.On && m_ViewTransition.Active && m_ViewOrbit.Stamp == m_ViewTransition.FromPos;
}

// The tangent of half the morph's field of view at morph progress `u` (0 the camera's own fov, 1
// nearly flat), log-spaced so the flattening reads evenly. Capped so the pulled-back camera stays
// well inside the far plane.
static float MorphTanHalf(const Camera& cam, float halfH, float fovDeg, float u) {
    const float base = std::tan(glm::radians(fovDeg) * 0.5f);
    float flat = std::tan(glm::radians(Glass::ViewMorphMinFovDeg) * 0.5f);
    flat = std::max(flat, halfH / std::max(1.0f, cam.FarPlane * 0.35f));
    flat = std::min(flat, base);
    return base * std::pow(flat / base, glm::clamp(u, 0.0f, 1.0f));
}

void EditorLayer::ApplyViewTransition(Camera& editorCamera, float eased, float easedLinear) {
    auto lerpAngle = [](float from, float to, float f) {
        float delta = fmodf(to - from + 540.0f, 360.0f) - 180.0f; // shortest path, wrapped to [-180,180)
        return from + delta * f;
    };
    editorCamera.Yaw = lerpAngle(m_ViewTransition.FromYaw, m_ViewTransition.ToYaw, eased);
    editorCamera.Pitch = m_ViewTransition.FromPitch + (m_ViewTransition.ToPitch - m_ViewTransition.FromPitch) * eased;

    if (!ViewOrbitLive()) {
        editorCamera.Position = glm::mix(m_ViewTransition.FromPos, m_ViewTransition.ToPos, eased);
        editorCamera.OrthoHalfHeight = m_ViewTransition.FromOrthoHalfHeight +
            (m_ViewTransition.ToOrthoHalfHeight - m_ViewTransition.FromOrthoHalfHeight) * eased;
        editorCamera.Fov = m_ViewTransition.FromFov + (m_ViewTransition.ToFov - m_ViewTransition.FromFov) * eased;
        return;
    }
    editorCamera.Pitch = glm::clamp(editorCamera.Pitch, -89.0f, 89.0f); // a spring's overshoot past a pole
    const glm::vec3 front = editorCamera.Front();
    if (m_ViewOrbit.Morph == 0) {
        const float dist = m_ViewOrbit.FromDist + (m_ViewOrbit.ToDist - m_ViewOrbit.FromDist) * easedLinear;
        editorCamera.Position = m_ViewOrbit.Pivot - front * dist;
        editorCamera.OrthoHalfHeight = m_ViewTransition.FromOrthoHalfHeight +
            (m_ViewTransition.ToOrthoHalfHeight - m_ViewTransition.FromOrthoHalfHeight) * easedLinear;
        return;
    }
    const float halfH = m_ViewOrbit.FromHalfH + (m_ViewOrbit.ToHalfH - m_ViewOrbit.FromHalfH) * easedLinear;
    const float u = m_ViewOrbit.Morph > 0 ? easedLinear : 1.0f - easedLinear;
    const float tanHalf = MorphTanHalf(editorCamera, halfH, m_ViewOrbit.Fov, u);
    editorCamera.Fov = glm::degrees(2.0f * std::atan(tanHalf));
    editorCamera.Position = m_ViewOrbit.Pivot - front * (halfH / tanHalf);
    editorCamera.OrthoHalfHeight = halfH;
}

void EditorLayer::SettleViewMorph(Camera& editorCamera) {
    // Whatever follows (a new transition, manual control) owns the camera: the orbit is done.
    if (!ViewOrbitLive() || m_ViewOrbit.Morph == 0) { m_ViewOrbit.On = false; return; }
    const float tanBase = std::tan(glm::radians(m_ViewOrbit.Fov) * 0.5f);
    const float halfH = editorCamera.OrthoHalfHeight; // ApplyViewTransition keeps it current
    editorCamera.Fov = m_ViewOrbit.Fov;
    if (m_ViewOrbit.Morph > 0) {
        editorCamera.Orthographic = true;
    } else {
        editorCamera.Position = m_ViewOrbit.Pivot - editorCamera.Front() * (halfH / tanBase);
    }
    m_ViewOrbit.On = false;
}

void EditorLayer::ToggleOrthographic(World& world, Camera& editorCamera) {
    // Same angle, just flips projection — SnapToView still handles the position/scale
    // conversion so perspective<->orthographic round-trips don't drift.
    SnapToView(world, editorCamera, editorCamera.Yaw, editorCamera.Pitch, !editorCamera.Orthographic);
}

void EditorLayer::UpdateViewTransition(Camera& editorCamera, float dt) {
    if (!m_ViewTransition.Active) return;

    // Any manual camera input hands control back to the user immediately instead of fighting
    // the animation to completion — the same feel as canceling the nav gizmo's own axis-snap
    // animation by moving the mouse mid-flight.
    ImGuiIO& io = ImGui::GetIO();
    bool manualInput = io.MouseWheel != 0.0f ||
        ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
        (io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ||
        (!io.WantCaptureKeyboard && (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_A) ||
            ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_D) ||
            ImGui::IsKeyDown(ImGuiKey_Q) || ImGui::IsKeyDown(ImGuiKey_E)));
    if (manualInput) {
        SettleViewMorph(editorCamera); // never leave the camera mid-morph (a narrow fov)
        m_ViewTransition.Active = false;
        return;
    }

    // Lands the transition exactly on its target, finishing a projection morph.
    auto land = [&]() {
        const bool orbit = ViewOrbitLive();
        const int morph = orbit ? m_ViewOrbit.Morph : 0;
        editorCamera.Yaw = m_ViewTransition.ToYaw;
        editorCamera.Pitch = m_ViewTransition.ToPitch;
        editorCamera.Position = m_ViewTransition.ToPos;
        editorCamera.OrthoHalfHeight = morph < 0 ? m_ViewOrbit.ToHalfH : m_ViewTransition.ToOrthoHalfHeight;
        editorCamera.Fov = morph != 0 ? m_ViewOrbit.Fov : m_ViewTransition.ToFov;
        if (morph > 0) editorCamera.Orthographic = true;
        m_ViewOrbit.On = false;
        m_ViewTransition.Active = false;
    };

    // Phase 6 item 12 — Reduce Motion snaps straight to the target instead of easing over
    // kDuration below.
    if (EditorSettings::Get().ReduceMotion || Glass::Motion().ReduceMotion()) {
        land();
        return;
    }

    // The Glass "smooth" curve (Glass::CameraFrame), so the camera lands like the UI does. A longer
    // trip takes a little longer, capped so framing never feels slow. A nav-gizmo click turns the
    // view on the ViewCubeTurn spring instead (the cube visibly swings into place and settles).
    // Stepped on the motion clock (Glass::Motion().Dt()), so the speed preference applies.
    (void)dt;
    const bool spring = ViewOrbitLive() && m_ViewOrbit.Spring;
    const float dist = glm::length(m_ViewTransition.ToPos - m_ViewTransition.FromPos);
    const float kDuration = spring ? Glass::ViewCubeTurn.durationMs * 0.001f
        : std::min(Glass::CameraFrameMaxMs,
                   Glass::CameraFrame.durationMs + Glass::CameraFramePerOctaveMs * std::log2(1.0f + dist)) * 0.001f;
    m_ViewTransition.T = std::min(1.0f, m_ViewTransition.T + Glass::Motion().Dt() / kDuration);
    const float t = m_ViewTransition.T;
    const float linear = Glass::EaseProgress(Glass::CameraFrame, t);
    const float eased = spring ? Glass::EaseProgress(Glass::ViewCubeTurn, t) : linear;
    if (t >= 1.0f) { land(); return; }
    ApplyViewTransition(editorCamera, eased, linear);
}

// --- Look through light (#140 phase 4) -------------------------------------------------------
void EditorLayer::LookThroughLight(World& world, Camera& editorCamera, entt::entity light) {
    if (light == entt::null || !world.Registry.valid(light) ||
        !world.Registry.all_of<TransformComponent, LightComponent>(light))
        return;

    // Stash the camera only on the FIRST entry — hopping between lights keeps the original.
    if (!m_LookThroughActive) {
        m_LookThroughRestore = {editorCamera.Position, editorCamera.Yaw, editorCamera.Pitch,
                                editorCamera.Fov, editorCamera.OrthoHalfHeight, editorCamera.Orthographic};
    }
    // Remember this light's pose so the first frame the flown camera diverges from it counts as
    // a real reposition and gets its own undo entry. Re-armed per light when hopping.
    {
        const auto& tf0 = world.Registry.get<const TransformComponent>(light);
        m_LookThroughStartPos = tf0.Position;
        m_LookThroughStartRot = tf0.EulerDegrees();
        m_LookThroughMoved = false;
    }

    const auto& lc = world.Registry.get<const LightComponent>(light);
    glm::mat4 m = world.ComposeWorldTransform(light);
    glm::vec3 pos = glm::vec3(m[3]);
    glm::vec3 aim = glm::normalize(glm::vec3(m * glm::vec4(0, 0, -1, 0)));

    float yaw = glm::degrees(std::atan2(aim.z, aim.x));
    float pitch = glm::degrees(std::asin(std::clamp(aim.y, -1.0f, 1.0f)));

    bool ortho = lc.Kind == LightComponent::Type::Directional;
    float fov = lc.Kind == LightComponent::Type::Spot
                    ? std::clamp(lc.SpotAngleDegrees * 2.0f, 10.0f, 150.0f)
                : lc.Kind == LightComponent::Type::Point ? 90.0f
                : editorCamera.Fov;

    SettleViewMorph(editorCamera); // Motion v2: never inherit a half-finished ortho morph
    m_ViewTransition.Active = true;
    m_ViewTransition.T = 0.0f;
    m_ViewTransition.FromPos = editorCamera.Position;
    m_ViewTransition.FromYaw = editorCamera.Yaw;
    m_ViewTransition.FromPitch = editorCamera.Pitch;
    m_ViewTransition.FromOrthoHalfHeight = editorCamera.OrthoHalfHeight;
    m_ViewTransition.FromFov = editorCamera.Fov;
    m_ViewTransition.ToPos = pos;
    m_ViewTransition.ToYaw = yaw;
    m_ViewTransition.ToPitch = pitch;
    m_ViewTransition.ToFov = fov;
    // Perspective near a light POV is fine as-is; for a directional, size the ortho box to its
    // range if it's a spot/point (n/a) — just pick a readable default for the sun.
    m_ViewTransition.ToOrthoHalfHeight = ortho ? 12.0f : editorCamera.OrthoHalfHeight;
    editorCamera.Orthographic = ortho; // matches SnapToView: switches now, the rest animates

    m_LookThroughActive = true;
    m_LookThroughLight = light;
    SelectItem(light, false);
}

void EditorLayer::ExitLookThrough(Camera& editorCamera) {
    if (!m_LookThroughActive) return;
    const CameraPose& r = m_LookThroughRestore;
    SettleViewMorph(editorCamera); // Motion v2: never inherit a half-finished ortho morph
    m_ViewTransition.Active = true;
    m_ViewTransition.T = 0.0f;
    m_ViewTransition.FromPos = editorCamera.Position;
    m_ViewTransition.FromYaw = editorCamera.Yaw;
    m_ViewTransition.FromPitch = editorCamera.Pitch;
    m_ViewTransition.FromOrthoHalfHeight = editorCamera.OrthoHalfHeight;
    m_ViewTransition.FromFov = editorCamera.Fov;
    m_ViewTransition.ToPos = r.Pos;
    m_ViewTransition.ToYaw = r.Yaw;
    m_ViewTransition.ToPitch = r.Pitch;
    m_ViewTransition.ToFov = r.Fov;
    m_ViewTransition.ToOrthoHalfHeight = r.OrthoHalfHeight;
    editorCamera.Orthographic = r.Ortho;
    m_LookThroughActive = false;
    m_LookThroughLight = entt::null;
}

void EditorLayer::UpdateLookThrough(World& world, Camera& editorCamera) {
    if (!m_LookThroughActive) return;

    // Light deleted / scene swapped out from under us — just bail (camera stays where it is).
    if (m_LookThroughLight == entt::null || !world.Registry.valid(m_LookThroughLight) ||
        !world.Registry.all_of<LightComponent>(m_LookThroughLight)) {
        m_LookThroughActive = false;
        m_LookThroughLight = entt::null;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ExitLookThrough(editorCamera);
        return;
    }

    // Live-drive: once the entry glide has settled (or the user cancelled it by flying), the
    // light rides with the editor camera — WASD/look moves and re-aims it from its own POV.
    // main.cpp has already applied this frame's camera nav by the time Draw() calls us.
    if (!m_ViewTransition.Active && world.Registry.all_of<TransformComponent>(m_LookThroughLight)) {
        auto& tf = world.Registry.get<TransformComponent>(m_LookThroughLight);

        entt::entity parent = entt::null;
        if (auto* h = world.Registry.try_get<HierarchyComponent>(m_LookThroughLight)) parent = h->Parent;
        glm::mat4 invParent = parent != entt::null
            ? glm::inverse(world.ComposeWorldTransform(parent)) : glm::mat4(1.0f);

        glm::vec3 newPos = glm::vec3(invParent * glm::vec4(editorCamera.Position, 1.0f));
        glm::vec3 fwd = editorCamera.Front();
        glm::vec3 up = std::fabs(fwd.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        glm::quat newRotation = QuaternionFromMatrix(invParent *
            glm::inverse(glm::lookAt(glm::vec3(0.0f), fwd, up)));

        bool moved = glm::length(newPos - tf.Position) > 1e-4f ||
                     !SameRotation(newRotation, tf.Rotation);
        if (moved) {
            if (!m_LookThroughMoved) {           // first real move -> one undo point, light still at entry pose
                PushUndo(world, "Reposition Light (Look Through)");
                m_LookThroughMoved = true;
            }
            tf.Position = newPos;
            tf.SetRotationQuaternion(newRotation);
        }
    }

    // Banner across the top of the viewport.
    const char* nm = "light";
    if (const auto* n = world.Registry.try_get<const NameComponent>(m_LookThroughLight))
        if (!n->Name.empty()) nm = n->Name.c_str();
    char buf[160];
    snprintf(buf, sizeof(buf),
             ICON_FA_EYE "  Looking through \"%s\"  —  fly to move / re-aim it  ·  Esc to exit", nm);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ts = ImGui::CalcTextSize(buf);
    float padX = 14.0f * m_UIScale, padY = 6.0f * m_UIScale;
    ImVec2 c(m_ViewportPos.x + m_ViewportSize.x * 0.5f, m_ViewportPos.y + 14.0f * m_UIScale);
    ImVec2 bmin(c.x - ts.x * 0.5f - padX, c.y - padY);
    ImVec2 bmax(c.x + ts.x * 0.5f + padX, c.y + ts.y + padY);
    dl->AddRectFilled(bmin, bmax, EditorUIPrimitives::kHudPlateColor, 4.0f * m_UIScale);
    dl->AddRect(bmin, bmax, EditorTheme::U32(EditorTheme::Accent), 4.0f * m_UIScale);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y), EditorUIPrimitives::kHudTextColor, buf);
}

// --- Drop light to surface (#140 phase 4) --------------------------------------------------
bool EditorLayer::DropLightToSurface(World& world, entt::entity light) {
    if (light == entt::null || !world.Registry.valid(light) ||
        !world.Registry.all_of<TransformComponent, LightComponent>(light))
        return false;

    glm::vec3 origin = glm::vec3(world.ComposeWorldTransform(light)[3]);
    const glm::vec3 down(0.0f, -1.0f, 0.0f);

    // #116 — land on the real surface below, not the top of a bounding box.
    const SurfaceHit h = RaycastRenderables(world, origin, down, [&](entt::entity ent) { return ent == light; });
    if (h.Entity == entt::null) return false;
    const float bestT = h.T;

    PushUndo(world, "Drop Light to Surface");
    glm::vec3 landing = origin + down * bestT + glm::vec3(0.0f, 0.1f, 0.0f);

    // Write the LOCAL translation, so a parented light lands on the surface in world space.
    entt::entity parent = entt::null;
    if (auto* hier = world.Registry.try_get<HierarchyComponent>(light)) parent = hier->Parent;
    glm::mat4 parentWorld = parent != entt::null ? world.ComposeWorldTransform(parent) : glm::mat4(1.0f);
    world.Registry.get<TransformComponent>(light).Position =
        glm::vec3(glm::inverse(parentWorld) * glm::vec4(landing, 1.0f));
    return true;
}
glm::vec3 EditorLayer::ComputeDropRayPosition(World& world, Camera& editorCamera) const {
    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);
    glm::mat4 invVP = glm::inverse(proj * view);
    ImVec2 mousePos = ImGui::GetMousePos();
    float ndcX = (2.0f * (mousePos.x - m_ViewportPos.x)) / m_ViewportSize.x - 1.0f;
    float ndcY = 1.0f - (2.0f * (mousePos.y - m_ViewportPos.y)) / m_ViewportSize.y;
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farP = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;
    glm::vec3 origin = glm::vec3(nearP);
    glm::vec3 dir = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));

    // Landing point: the nearest mesh surface (#116 — was the nearest collider's axis-aligned
    // box, so drops onto rotated geometry floated/embedded and collider-less models couldn't be
    // dropped onto), else the Y=0 ground plane, else a fixed distance out.
    float bestT = 1e30f;
    if (const SurfaceHit h = RaycastRenderables(world, origin, dir, [](entt::entity) { return false; }, 500.0f);
        h.Entity != entt::null) {
        bestT = h.T;
    }
    if (std::abs(dir.y) > 1e-5f) {
        float groundT = -origin.y / dir.y;
        if (groundT > 0.0f && groundT < bestT) bestT = groundT;
    }
    if (bestT >= 1e30f) bestT = 8.0f;
    glm::vec3 position = origin + dir * bestT;

    // Same grid-snap concept as the transform gizmo (m_GridSnapEnabled, Ctrl inverts it
    // momentarily) - XZ only, since Y is about to be re-derived from where the object actually
    // sits (either here directly, for a prefab, or bottom-aligned in ComputeModelDropPosition).
    bool snapActive = m_GridSnapEnabled != ImGui::GetIO().KeyCtrl;
    float step = EditorSettings::Get().GridMinorSpacing;
    if (snapActive && step > 0.0001f) {
        position.x = std::round(position.x / step) * step;
        position.z = std::round(position.z / step) * step;
    }
    return position;
}

glm::vec3 EditorLayer::ComputeModelDropPosition(World& world, Model& model, Camera& editorCamera) const {
    glm::vec3 position = ComputeDropRayPosition(world, editorCamera);

    // Rest the model's own bottom on the hit point instead of its (possibly arbitrary) pivot -
    // same technique SnapSelectionToGround() uses for an already-placed object: evaluate the
    // lowest vertex's world Y at a candidate transform, then shift by however far that missed
    // the target height. See SnapSelectionToGround's comment for the general formula this is a
    // special case of (targetY happens to equal the candidate's own Y here).
    float targetY = position.y;
    glm::mat4 candidate = ComposeTransform(position, glm::vec3(0.0f), glm::vec3(1.0f));
    float lowestY = model.LowestVertexWorldY(candidate);
    position.y += (targetY - lowestY);
    return position;
}

entt::entity EditorLayer::ObjectUnderMouse(World& world, Camera& editorCamera, int& outSlot) const {
    outSlot = -1;
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return entt::null;
    const glm::mat4 invVP = glm::inverse(editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) *
                                         editorCamera.ViewMatrix());
    const ImVec2 mouse = ImGui::GetMousePos();
    const float ndcX = (2.0f * (mouse.x - m_ViewportPos.x)) / m_ViewportSize.x - 1.0f;
    const float ndcY = 1.0f - (2.0f * (mouse.y - m_ViewportPos.y)) / m_ViewportSize.y;
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farP = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;
    const SurfaceHit h = RaycastRenderables(world, glm::vec3(nearP), glm::normalize(glm::vec3(farP - nearP)),
                                            [](entt::entity) { return false; }, 5000.0f);
    outSlot = h.Mesh;
    return h.Entity;
}

void EditorLayer::RestoreMaterialDropPreview(World& world) {
    auto& p = m_MaterialDropPreview;
    if (!p.Active) return;
    p.Active = false;
    if (!world.Registry.valid(p.Entity)) return;
    auto* rc = world.Registry.try_get<RenderableComponent>(p.Entity);
    if (!rc || p.Slot < 0 || p.Slot >= (int)rc->Materials.size()) return;
    rc->Materials[p.Slot] = p.Original;
    if (rc->Materials.size() > p.OriginalSlotCount) rc->Materials.resize(p.OriginalSlotCount);
}

void EditorLayer::DrawViewportDropTarget(World& world, AssetLibrary& assets, Camera& editorCamera) {
    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
    bool isModelDrag = peek && peek->IsDataType("ASSET_MODEL_PATH");
    bool isPrefabDrag = peek && peek->IsDataType("ASSET_PREFAB_PATH");
    // A .decal lands on the surface under the cursor, or swaps the look of a decal it's over.
    const bool isDecalDrag = peek && peek->IsDataType("ASSET_DECAL_PATH");
    // Everything else the Asset Browser drags applies to what's under the cursor (or, for an
    // .hdr / a sound, to the scene) - see ApplyAssetDrop.
    const char* applyType = nullptr;
    for (const char* t : {"ASSET_MATERIAL_PATH", "ASSET_TEXTURE_PATH", "ASSET_SOUND_PATH", "ASSET_FILE_PATH"})
        if (peek && peek->IsDataType(t)) applyType = t;
    // Only active while an asset is actually being dragged, so this fullscreen overlay never
    // otherwise sits over the viewport intercepting camera input.
    if ((!isModelDrag && !isPrefabDrag && !isDecalDrag && !applyType) || m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) {
        m_DragPreview.Active = false;
        RestoreMaterialDropPreview(world); // the drag ended somewhere else, or was cancelled
        return;
    }

    // Cover exactly the Scene viewport rect (not the whole window) and sit ON TOP of the "Scene"
    // panel for the duration of the drag. "Scene" is a normal docked window now — it used to be
    // the dockspace's passthru central node, which let mouse events fall through it to a
    // fullscreen catcher behind. A bottom-layer catcher is simply occluded by "Scene" and never
    // becomes g.HoveredWindowUnderMovingWindow, so BeginDragDropTarget() below refuses the drop.
    // Restricting the window to the viewport rect keeps drops over the side panels routing to
    // those panels' own drop targets.
    ImGui::SetNextWindowPos(ImVec2(m_ViewportPos.x, m_ViewportPos.y));
    ImGui::SetNextWindowSize(ImVec2(m_ViewportSize.x, m_ViewportSize.y));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##ViewportDropTarget", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoDocking);
    // Only submitted while a placeable asset is mid-drag (see the early-out above), so forcing
    // it to the front just parks the catcher over the Scene image for that drag.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    ImGui::InvisibleButton("##viewport_drop_zone", ImGui::GetContentRegionAvail());
    // Plain IsItemHovered() defaults to false here for the ENTIRE drag: the Asset Browser's
    // drag-source cell stays the "active" item the whole time the mouse button is held, and
    // IsItemHovered() normally excludes hover on anything else while a different item is active
    // (to stop drag interactions from also triggering hover-only affordances underneath). That
    // flag is exactly why this only lit up after release before - the active item cleared at
    // that point, so hover only ever registered on the very last frame.
    bool hoveringViewport = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // Live preview: recomputed every frame while a model is being dragged over the viewport, so
    // main.cpp can render a translucent ghost at the exact spot that would be committed if the
    // mouse were released right now (TintOverlayRenderer, driven by GetDragPreview()). Prefabs
    // don't get a ghost - they aren't necessarily a single Model with bounds to preview.
    if (isModelDrag && hoveringViewport) {
        std::string path((const char*)peek->Data);
        auto model = assets.LoadModel(path); // cache hit - already imported to appear in the browser
        if (model) {
            m_DragPreview.Active = true;
            m_DragPreview.ModelRef = model;
            m_DragPreview.Position = ComputeModelDropPosition(world, *model, editorCamera);
        } else {
            m_DragPreview.Active = false;
        }
    } else {
        m_DragPreview.Active = false;
    }

    // What the drop would do, on a plate beside the cursor.
    auto drawDropHint = [&](const std::string& hint) {
        if (hint.empty()) return;
        const ImVec2 m = ImGui::GetMousePos();
        const ImVec2 at(m.x + 18.0f * m_UIScale, m.y + 22.0f * m_UIScale);
        const ImVec2 ts = ImGui::CalcTextSize(hint.c_str());
        const ImVec2 pad(6.0f * m_UIScale, 3.0f * m_UIScale);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(ImVec2(at.x - pad.x, at.y - pad.y), ImVec2(at.x + ts.x + pad.x, at.y + ts.y + pad.y),
                          EditorUIPrimitives::kHudPlateColor, 4.0f * m_UIScale);
        fg->AddText(at, EditorUIPrimitives::kHudTextColor, hint.c_str());
    };

    DecalSpot decalSpot;
    if (isDecalDrag && hoveringViewport) {
        decalSpot = DecalSpotUnderMouse(world, editorCamera);
        if (decalSpot.Decal != entt::null || decalSpot.OnSurface)
            drawDropHint(AssetDropHint(world, decalSpot.Decal, "ASSET_DECAL_PATH", (const char*)peek->Data, -1));
    }

    // Material / texture / sound / controller / script / HDRI: find what's under the cursor, show
    // what the drop would do beside it, and preview a material on the hovered part (Unity does).
    entt::entity applyTarget = entt::null;
    int applySlot = -1;
    if (applyType && hoveringViewport) {
        const std::string path((const char*)peek->Data);
        applyTarget = ObjectUnderMouse(world, editorCamera, applySlot);
        const std::string hint = AssetDropHint(world, applyTarget, applyType, path, applySlot);

        auto& pv = m_MaterialDropPreview;
        const bool wantPreview = !hint.empty() && std::string(applyType) == "ASSET_MATERIAL_PATH";
        int slot = applySlot;
        auto* rc = wantPreview ? world.Registry.try_get<RenderableComponent>(applyTarget) : nullptr;
        if (rc && rc->ModelRef && (slot < 0 || slot >= rc->ModelRef->MeshCount())) slot = 0;
        const bool samePreview = pv.Active && pv.Entity == applyTarget && pv.Slot == slot && pv.Path == path;
        if (!samePreview) {
            RestoreMaterialDropPreview(world);
            if (rc && rc->ModelRef) {
                if (auto mat = assets.LoadMaterial(path); mat && !mat->Missing) {
                    pv = {applyTarget, slot, slot < (int)rc->Materials.size() ? rc->Materials[slot] : nullptr,
                          rc->Materials.size(), path, true};
                    if ((int)rc->Materials.size() <= slot) rc->Materials.resize(slot + 1);
                    rc->Materials[slot] = mat;
                }
            }
        }

        drawDropHint(hint);
    } else {
        RestoreMaterialDropPreview(world);
    }

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_DECAL_PATH")) {
            const std::string path((const char*)p->Data);
            if (decalSpot.Decal != entt::null)
                ApplyAssetDrop(world, assets, decalSpot.Decal, "ASSET_DECAL_PATH", path, -1, decalSpot.Point);
            else if (decalSpot.OnSurface)
                PlaceDecal(world, path, decalSpot.Point, decalSpot.Normal, editorCamera.Right());
        }
        if (applyType) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(applyType)) {
                const std::string path((const char*)p->Data);
                RestoreMaterialDropPreview(world); // so the undo step records the real "before"
                ApplyAssetDrop(world, assets, applyTarget, applyType, path, applySlot,
                               ComputeDropRayPosition(world, editorCamera));
            }
        }
        const ImGuiPayload* modelPayload = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH");
        const ImGuiPayload* prefabPayload = modelPayload ? nullptr : ImGui::AcceptDragDropPayload("ASSET_PREFAB_PATH");

        if (modelPayload || prefabPayload) {
            std::string path((const char*)(modelPayload ? modelPayload->Data : prefabPayload->Data));
            PushUndo(world, modelPayload ? "Place Model" : "Place Prefab Instance");

            if (modelPayload) {
                auto model = assets.InstantiateModel(path);
                glm::vec3 position = ComputeModelDropPosition(world, *model, editorCamera);
                std::string name = std::filesystem::path(path).stem().string();
                entt::entity e = world.CreateModelEntity(model, position, glm::vec3(0.0f), glm::vec3(1.0f), UniqueNameFor(world, name));
                if (auto* rc = world.Registry.try_get<RenderableComponent>(e); rc && rc->ModelRef)
                    assets.ApplyMaterialRemap(*rc->ModelRef, rc->Materials); // its extracted .mat files
                SelectItem(e, false);
            } else {
                glm::vec3 position = ComputeDropRayPosition(world, editorCamera);
                // A prefab carries its own authored transform, so the instance is created first
                // and then moved to the drop point (children ride along, being local-space).
                entt::entity e = SceneSerializer::InstantiatePrefab(world, assets, path);
                if (e != entt::null) {
                    UniquifyName(world, e);
                    if (auto* transform = world.Registry.try_get<TransformComponent>(e)) {
                        transform->Position = position;
                    }
                    SelectItem(e, false);
                }
            }
            m_DragPreview.Active = false;
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// The box-select drag edge (BoxSelectStart plays once per drag); file-static, there is one EditorLayer.
static bool s_BoxSelectDragging = false;

void EditorLayer::HandleViewportPicking(World& world, Camera& editorCamera) {
    ImGuiIO& io = ImGui::GetIO();
    bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool leftPressed = leftDown && !m_PrevLeftMouseDown;
    m_PrevLeftMouseDown = leftDown;

    // All ray/screen math below is done in the actual viewport rect (the dockspace's central
    // node), not the full window — so picking, box-select, and gizmos stay correctly aligned
    // as the Hierarchy/Inspector/Asset Browser panels are resized around it.
    glm::vec2 vpPos = m_ViewportPos, vpSize = m_ViewportSize;
    float w = vpSize.x, h = vpSize.y;
    if (w <= 0 || h <= 0) return;

    // Eyedropper (#236 R2): armed by a colour field. The next viewport click captures the
    // pixel for main.cpp to sample off the tonemapped scene FBO; right-click / Esc cancels.
    if (EyedropperArmed() && !WantsCaptureMouse() && !m_ViewGizmoBlocking) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            CancelEyedropper();
            EditorSound::Play(EditorSound::Event::ClickCancel);
            return;
        }
        if (leftPressed) {
            m_EyedropperClickPos = { io.MousePos.x, io.MousePos.y };
            m_EyedropperSampleRequested = true;
            return;
        }
        return; // armed — swallow every viewport click
    }

    // Graffiti Designer: Spray in Scene view owns the left button (a stroke that started here keeps
    // it even while the cursor crosses a panel, so its release always ends the stroke).
    if (GraffitiViewportSprayActive() &&
        HandleGraffitiViewportSpray(world, editorCamera, leftPressed && !WantsCaptureMouse() && !m_ViewGizmoBlocking, leftDown))
        return;

    // Measure / ruler tool (#236 R2, Phase 3 item 7): clicks drop endpoints instead of
    // selecting — never falls through to picking / box-select below. Each left-click APPENDS a
    // point, chaining segments end to end (used to reset back to a single fresh segment after
    // the second click); right-click (or Esc, handled in the shortcut block) clears the chain.
    // vRuler (hold Shift+R): a click shows the measured object's size instead of selecting.
    if (RulerHeldNow() && !WantsCaptureMouse() && !m_ViewGizmoBlocking) {
        if (leftPressed) m_RulerBounds = m_RulerBounds == m_RulerHitEntity ? entt::null : m_RulerHitEntity;
        return;
    }
    // Screen x of the cursor, as Play's pan.
    const float cursorX01 = io.DisplaySize.x > 0.0f ? io.MousePos.x / io.DisplaySize.x : -1.0f;
    if (m_MeasureTool && !WantsCaptureMouse() && !m_ViewGizmoBlocking && !m_GizmoEngaged) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (!m_MeasurePoints.empty()) EditorSound::Play(EditorSound::Event::MeasureClear, cursorX01);
            m_MeasurePoints.clear();
            return;
        }
        if (leftPressed) {
            glm::vec3 hit;
            if (RaycastViewportSurface(world, editorCamera, {io.MousePos.x, io.MousePos.y}, hit)) {
                m_MeasurePoints.push_back(hit);
                EditorSound::Play(EditorSound::Event::MeasurePoint, cursorX01);
            } else {
                EditorSound::Play(EditorSound::Event::Blocked, cursorX01); // nothing under the cursor to measure to
            }
            return;
        }
        return; // tool active — never fall through to selection / box-select
    }

    // Place Decal / Decal Brush: clicks place and paint instead of selecting. The update also runs with
    // the cursor off the viewport, to close a stroke whose button came up out there.
    if (m_DecalTool != DecalToolMode::None && !m_Radial.Open) {
        const bool free = !WantsCaptureMouse() && !m_ViewGizmoBlocking && !m_GizmoEngaged && !io.KeyAlt &&
                          !ImGui::IsAnyItemHovered();
        UpdateDecalTool(world, editorCamera, free, leftPressed, leftDown);
        if (free) return; // never fall through to selection / box-select
    }

    const float kDragThreshold = 6.0f; // pixels of movement before a press-drag-release counts as a box select rather than a click

    if (leftPressed) {
        // ImGui panel, the transform gizmo, or the nav gizmo (rotate ring / dolly / pan
        // buttons) already owns this click; Alt+Left-drag is reserved for orbiting the camera
        // around the current selection (see main.cpp's UpdateEditorCamera).
        // The left tool palette is a child window of "Scene", so WantsCaptureMouse() reads the
        // cursor as over the viewport; a click on one of its buttons also picked whatever sat
        // behind it. Any hovered ImGui item owns the click (the viewport image isn't an item).
        m_BoxSelectActive = !WantsCaptureMouse() && !m_GizmoEngaged && !m_ViewGizmoBlocking &&
                            !m_LightHandleEngaged && !io.KeyAlt && !ImGui::IsAnyItemHovered();
        m_BoxSelectStart = {io.MousePos.x, io.MousePos.y};
        s_BoxSelectDragging = false;
        return; // click vs. drag is only decided on release, below
    }

    if (!m_BoxSelectActive) return;
    glm::vec2 current(io.MousePos.x, io.MousePos.y);
    bool isDragging = glm::length(current - m_BoxSelectStart) > kDragThreshold;

    if (leftDown) {
        if (isDragging) {
            if (!s_BoxSelectDragging) EditorSound::Play(EditorSound::Event::BoxSelectStart, cursorX01);
            s_BoxSelectDragging = true;
            ImVec2 a(m_BoxSelectStart.x, m_BoxSelectStart.y), b(current.x, current.y);
            ImGui::GetForegroundDrawList()->AddRectFilled(a, b, EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Accent, 0.12f)));
            ImGui::GetForegroundDrawList()->AddRect(a, b, EditorTheme::U32(EditorTheme::Accent));
        }
        return; // still held: nothing selected yet, just drawing the marquee
    }

    // Released this frame — commit.
    m_BoxSelectActive = false;
    s_BoxSelectDragging = false;
    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix((float)w / (float)h);

    if (!isDragging) {
        // Plain click: single-object raycast pick straight through the cursor, same as before.
        glm::mat4 invVP = glm::inverse(proj * view);
        float ndcX = (2.0f * (current.x - vpPos.x)) / w - 1.0f;
        float ndcY = 1.0f - (2.0f * (current.y - vpPos.y)) / h;
        glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
        glm::vec4 farP = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
        nearP /= nearP.w;
        farP /= farP.w;
        glm::vec3 origin = glm::vec3(nearP);
        glm::vec3 dir = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));

        // #116 — exact triangle picking (see RaycastRenderables).
        const SurfaceHit pick = RaycastRenderables(world, origin, dir, [&](entt::entity entity) {
            return ViewportPickLocked(world, entity) || NotSceneSelectable(world, entity); // #236 B
        });
        float bestT = pick.T;
        entt::entity best = pick.Entity;

        // Mesh-less entities (lights, empties) have no geometry to hit, so they're picked by
        // proximity to their on-screen icon instead. The icon is a screen-space overlay drawn
        // ON TOP of everything, so a click that lands on it selects that entity even when a mesh
        // is in front — that's what you see, and what you want when deliberately clicking a
        // light's icon (#140). Grab the mesh by clicking it away from the icon. Nearest icon to
        // the cursor wins when several overlap.
        {
            const float kIconPickPixels = 12.0f * m_UIScale;
            glm::mat4 viewProj = proj * view;
            float bestPixelDist = kIconPickPixels;
            entt::entity iconHit = entt::null;
            float iconDepth = 1e30f;
            for (auto entity : world.Registry.view<const TransformComponent>(entt::exclude<RenderableComponent, InactiveTag>)) {
                if (ViewportPickLocked(world, entity)) continue;
                if (NotSceneSelectable(world, entity)) continue; // #236 B
                glm::mat4 model = world.ComposeWorldTransform(entity);
                glm::vec3 worldPos = glm::vec3(model[3]);
                glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0f);
                if (clip.w <= 0.0001f) continue;

                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                glm::vec2 screen(vpPos.x + (ndc.x * 0.5f + 0.5f) * w,
                                 vpPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
                float pixelDist = glm::length(screen - current);
                if (pixelDist > bestPixelDist) continue;

                bestPixelDist = pixelDist;
                iconHit = entity;
                iconDepth = glm::length(worldPos - origin);
            }
            if (iconHit != entt::null) { // an icon under the cursor beats any mesh behind it
                best = iconHit;
                bestT = iconDepth;
            }
        }

        if (best != entt::null) SelectItem(best, io.KeyCtrl);
        else if (!io.KeyCtrl) ClearSelection(); // clicked empty space -> deselect (unless Ctrl-clicking to preserve a group)

        // Grab handles arm only when the click resolved to a light IN THE VIEWPORT (#140
        // follow-up). SelectItem() cleared m_LightHandleArmedFor a moment ago; re-set it here.
        m_LightHandleArmedFor =
            (best != entt::null && world.Registry.all_of<LightComponent>(best)) ? best : entt::null;
        return;
    }

    // Box select: everything whose projected screen-space bounds overlap the drag rectangle.
    glm::vec2 rectMin = glm::min(m_BoxSelectStart, current);
    glm::vec2 rectMax = glm::max(m_BoxSelectStart, current);
    glm::mat4 viewProj = proj * view;

    auto projectedScreenRect = [&](const AABB& bounds, glm::vec2& outMin, glm::vec2& outMax) {
        outMin = glm::vec2(1e30f);
        outMax = glm::vec2(-1e30f);
        bool any = false;
        for (int c = 0; c < 8; ++c) {
            glm::vec3 corner(
                (c & 1) ? bounds.Max.x : bounds.Min.x,
                (c & 2) ? bounds.Max.y : bounds.Min.y,
                (c & 4) ? bounds.Max.z : bounds.Min.z);
            glm::vec4 clip = viewProj * glm::vec4(corner, 1.0f);
            if (clip.w <= 0.0001f) continue; // behind the camera
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            glm::vec2 screen = vpPos + glm::vec2((ndc.x * 0.5f + 0.5f) * w, (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
            outMin = glm::min(outMin, screen);
            outMax = glm::max(outMax, screen);
            any = true;
        }
        return any;
    };
    auto rectsOverlap = [](glm::vec2 aMin, glm::vec2 aMax, glm::vec2 bMin, glm::vec2 bMax) {
        return aMin.x <= bMax.x && aMax.x >= bMin.x && aMin.y <= bMax.y && aMax.y >= bMin.y;
    };

    if (!io.KeyCtrl) ClearSelection();

    auto entityView = world.Registry.view<const RenderableComponent>(entt::exclude<InactiveTag>);
    for (auto entity : entityView) {
        if (ViewportPickLocked(world, entity)) continue;
        if (NotSceneSelectable(world, entity)) continue; // #236 B
        const auto& renderable = entityView.get<const RenderableComponent>(entity);
        glm::mat4 model = world.ComposeWorldTransform(entity);
        AABB bounds = AABB{renderable.ModelRef->BoundsMin(), renderable.ModelRef->BoundsMax()}.Transformed(model);
        glm::vec2 pMin, pMax;
        if (projectedScreenRect(bounds, pMin, pMax) && rectsOverlap(pMin, pMax, rectMin, rectMax)) {
            AddToSelectionIfAbsent(entity);
        }
    }

    // Mesh-less entities (lights, cameras, empties) have no AABB to project — marquee-select
    // them the same way a plain click does: test their on-screen icon position (same math as
    // the icon-proximity pick above) against the drag rectangle.
    for (auto entity : world.Registry.view<const TransformComponent>(entt::exclude<RenderableComponent, InactiveTag>)) {
        if (ViewportPickLocked(world, entity)) continue;
        if (NotSceneSelectable(world, entity)) continue; // #236 B
        glm::mat4 model = world.ComposeWorldTransform(entity);
        glm::vec3 worldPos = glm::vec3(model[3]);
        glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0f);
        if (clip.w <= 0.0001f) continue; // behind the camera

        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        glm::vec2 screen(vpPos.x + (ndc.x * 0.5f + 0.5f) * w,
                         vpPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
        if (screen.x >= rectMin.x && screen.x <= rectMax.x && screen.y >= rectMin.y && screen.y <= rectMax.y) {
            AddToSelectionIfAbsent(entity);
        }
    }

    // Same frame as the selection change, so SelectionFrame stays quiet; higher for a bigger catch.
    const int boxCount = (int)GetSelectedItems().size();
    EditorSound::Play(EditorSound::Event::BoxSelectEnd, cursorX01,
                      boxCount > 0 ? 0.9f + 0.25f * (float)std::min(boxCount, 16) / 16.0f : 0.85f);
}

bool EditorLayer::FindVertexUnderCursor(World& world, Camera& editorCamera, glm::vec3& outLocalPos) const {
    if (!IsVertexDraggable(world, m_Selected)) return false;
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return false;

    const auto& renderable = world.Registry.get<RenderableComponent>(m_Selected);
    // World matrix: the picked vertices are projected against a world-space view-projection, and
    // the snap-target loop in UpdateVertexDrag already evaluates other models in world space —
    // using the local matrix here tested the wrong screen positions for a parented model. (#224)
    glm::mat4 model = world.GetCachedWorldTransform(m_Selected);
    glm::mat4 viewProj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * editorCamera.ViewMatrix();

    ImVec2 mouse = ImGui::GetIO().MousePos;
    glm::vec2 viewportMouse(mouse.x - m_ViewportPos.x, mouse.y - m_ViewportPos.y);
    return renderable.ModelRef->FindNearestVertexToScreenPoint(model, viewProj, {viewportMouse.x, viewportMouse.y},
        m_ViewportSize.x, m_ViewportSize.y, m_VertexPickPixels, outLocalPos);
}

void EditorLayer::UpdateVertexDrag(World& world, Camera& editorCamera) {
    if (!IsVertexDraggable(world, m_Selected)) {
        m_VertexDragActive = false;
        return;
    }
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    // Cast a ray through the current mouse position and intersect it with a camera-facing
    // plane fixed at the grabbed vertex's world position when the drag started — a standard
    // "grab" drag (Blender's G key): the object slides freely across the screen at constant
    // depth rather than needing an axis-constrained gizmo handle.
    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);
    glm::mat4 invVP = glm::inverse(proj * view);

    ImVec2 mouse = ImGui::GetIO().MousePos;
    float ndcX = (2.0f * (mouse.x - m_ViewportPos.x)) / m_ViewportSize.x - 1.0f;
    float ndcY = 1.0f - (2.0f * (mouse.y - m_ViewportPos.y)) / m_ViewportSize.y;
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farP = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;
    glm::vec3 rayOrigin = glm::vec3(nearP);
    glm::vec3 rayDir = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));

    glm::vec3 planeNormal = editorCamera.Front();
    float denom = glm::dot(rayDir, planeNormal);
    if (std::abs(denom) < 1e-5f) return; // looking edge-on along the drag plane; hold position this frame

    float t = glm::dot(m_VertexDragPlanePoint - rayOrigin, planeNormal) / denom;
    if (t < 0.0f) return; // plane point is behind the camera

    glm::vec3 grabbedWorld = rayOrigin + rayDir * t;
    glm::vec3 candidatePosition = grabbedWorld + m_VertexDragOffset;

    // Vertex snap: search every model OUTSIDE the current selection for whichever vertex sits
    // closest to the CURSOR ON SCREEN (other group members are excluded — they're moving
    // rigidly along with the grabbed vertex, so their relative distance to it never actually
    // changes, and "snapping" onto one would just lock the group to itself) and, if one is
    // within the pick radius, nudge the object so the grabbed vertex lands exactly on it.
    //
    // This is screen-space (pixels), matching the SAME m_VertexPickPixels threshold that
    // decides whether the yellow hover circle shows up in the first place — deliberately, so
    // "the circle is showing" and "this will snap" always agree. It used to be a fixed
    // world-space radius (m_VertexSnapRadius), which meant a vertex that looked perfectly
    // aligned on screen (because the camera was zoomed out) could still refuse to snap purely
    // because it was far away in 3D units — a mismatch between what you see and what the
    // check actually measured.
    auto isExcluded = [&](entt::entity entity) {
        if (entity == m_Selected) return true;
        for (entt::entity e : m_ExtraSelection) if (e == entity) return true;
        return false;
    };

    glm::mat4 viewProjSnap = proj * view;
    glm::vec2 cursorScreen(mouse.x - m_ViewportPos.x, mouse.y - m_ViewportPos.y);

    // Boxes were never valid snap targets before (only other models) — excluded here the same
    // way, via LevelGeometryTag.
    float bestPixelDist = m_VertexPickPixels;
    glm::vec3 bestTarget{};
    bool found = false;
    auto snapView = world.Registry.view<const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    for (auto entity : snapView) {
        if (isExcluded(entity)) continue;
        const auto& otherRenderable = snapView.get<const RenderableComponent>(entity);
        glm::mat4 otherMatrix = world.ComposeWorldTransform(entity);
        glm::vec3 candidateLocal;
        // Passing the current best-so-far as this call's own cutoff narrows every subsequent
        // candidate model to "closer than whatever's already winning," so the loop converges
        // on the single nearest-on-screen vertex across ALL candidate models, not just the
        // nearest one within each model considered in isolation.
        if (!otherRenderable.ModelRef->FindNearestVertexToScreenPoint(otherMatrix, viewProjSnap, cursorScreen,
                m_ViewportSize.x, m_ViewportSize.y, bestPixelDist, candidateLocal)) {
            continue;
        }
        glm::vec4 clip = viewProjSnap * otherMatrix * glm::vec4(candidateLocal, 1.0f);
        if (clip.w <= 0.0001f) continue; // behind the camera
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        glm::vec2 screen((ndc.x * 0.5f + 0.5f) * m_ViewportSize.x, (1.0f - (ndc.y * 0.5f + 0.5f)) * m_ViewportSize.y);
        float pixelDist = glm::length(screen - cursorScreen);
        if (pixelDist < bestPixelDist) {
            bestPixelDist = pixelDist;
            bestTarget = glm::vec3(otherMatrix * glm::vec4(candidateLocal, 1.0f));
            found = true;
        }
    }
    if (found) {
        candidatePosition += (bestTarget - grabbedWorld);
    }

    // Move the primary by however much this frame actually resolved to (drag + snap), then
    // carry every other selected object along by that exact same delta so the whole group
    // moves together while only the primary's vertex does the snapping. Boxes and models both
    // just have a TransformComponent now, so there's no more per-kind branch needed here either.
    //
    // candidatePosition is a WORLD-space origin (the drag plane and the snap targets are both
    // world-space), so the delta carried to the rest of the selection is a world delta and each
    // object's new world origin has to be converted back into its own parent's frame before it
    // lands in TransformComponent. Every world matrix is read BEFORE any write, so moving a
    // parent doesn't change the reading for a sibling that has already been sampled. (#224)
    glm::vec3 primaryWorld = glm::vec3(world.GetCachedWorldTransform(m_Selected)[3]);
    glm::vec3 delta = candidatePosition - primaryWorld;

    std::vector<std::pair<entt::entity, glm::vec3>> moves;
    moves.emplace_back(m_Selected, candidatePosition);
    for (entt::entity e : m_ExtraSelection) {
        if (!world.Registry.valid(e)) continue;
        // A child of another selected object is already carried along by its parent's move —
        // moving it a second time would double the delta.
        if (HasSelectedAncestor(world, e, m_Selected, m_ExtraSelection)) continue;
        moves.emplace_back(e, glm::vec3(world.GetCachedWorldTransform(e)[3]) + delta);
    }
    for (const auto& [entity, worldPos] : moves) SetWorldPosition(world, entity, worldPos);
}

bool EditorLayer::IsSelectedOrUnderSelected(const World& world, entt::entity entity) const {
    return IsSelected(entity) || HasSelectedAncestor(world, entity, m_Selected, m_ExtraSelection);
}

void EditorLayer::DrawEntityIcons(World& world, Camera& editorCamera) {
    const EditorSettings& prefs = EditorSettings::Get();
    if (prefs.EntityIconsMode >= 2) return; // None
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    glm::mat4 viewProj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * editorCamera.ViewMatrix();

    // Draw into the Scene window's own draw list (clipped to the viewport rect), NOT the
    // foreground list — the foreground list renders on top of every panel, so light/empty
    // icons would punch through the Inspector, Preferences, any window overlapping the
    // viewport. Appended to the Scene window's list, they sit at its z-order and a panel on
    // top correctly covers them.
    ImGuiWindow* sceneWin = ImGui::FindWindowByName(EditorPanels::Scene);
    ImDrawList* draw = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    const ImVec2 clipMin(m_ViewportPos.x, m_ViewportPos.y);
    const ImVec2 clipMax(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y);
    draw->PushClipRect(clipMin, clipMax, true);

    // Pass 1: every mesh-less entity that wants an icon — a light you can't see is a light you
    // can't select or aim.
    struct Item {
        entt::entity Entity;
        const char* Glyph;
        ImVec2 Screen;
        glm::vec3 Pos;
        float Dist;
        bool Selected, Inactive, IsLight, IsCamera;
        glm::vec3 Light; // the light's own colour (IsLight only)
    };
    std::vector<Item> items;
    const bool allKinds = prefs.EntityIconsMode == 0;
    const glm::vec3 camPos = editorCamera.Position;
    auto view = world.Registry.view<const TransformComponent>(entt::exclude<RenderableComponent>);
    const unsigned layerVisible = prefs.LayerVisibleMask;
    for (auto entity : view) {
        // #184 — an entity hidden in the Scene view (SceneVis, or its layer switched off) gets no
        // icon either; picking already skipped it, so the icon was unclickable.
        if (world.Registry.all_of<HiddenInSceneTag>(entity)) continue;
        {
            const auto* lc = world.Registry.try_get<LayerComponent>(entity);
            const int layer = lc ? lc->Layer : 0; // same rule as SceneRenderer's layer cull
            if (layer >= 0 && layer < 32 && !((layerVisible >> layer) & 1u)) continue;
        }
        const bool selected = IsSelected(entity);
        const auto* light = world.Registry.try_get<LightComponent>(entity);
        const bool isCamera = world.Registry.all_of<CameraComponent>(entity);
        // Lights & cameras mode: everything else only shows once it is selected.
        if (!allKinds && !selected && !light && !isCamera) continue;

        glm::mat4 model = world.ComposeWorldTransform(entity);
        const glm::vec3 wp = glm::vec3(model[3]);
        glm::vec4 clip = viewProj * glm::vec4(wp, 1.0f);
        if (clip.w <= 0.0001f) continue; // behind the camera

        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        ImVec2 screen(m_ViewportPos.x + (ndc.x * 0.5f + 0.5f) * m_ViewportSize.x,
                      m_ViewportPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * m_ViewportSize.y);

        // One glyph per kind (#159), and per light type so point / spot / sun read apart at a
        // glance — matching the Add menu's glyphs. The rarer kinds only show in All mode.
        const char* glyph;
        const auto* nameC = world.Registry.try_get<NameComponent>(entity);
        const auto* colC = world.Registry.try_get<ColliderComponent>(entity);
        if (light) {
            glyph = light->Kind == LightComponent::Type::Spot        ? ICON_FA_BULLSEYE
                  : light->Kind == LightComponent::Type::Directional ? ICON_FA_SUN
                                                                     : ICON_FA_LIGHTBULB; // Point
        } else if (isCamera) {
            glyph = ICON_FA_VIDEO;
        } else if (world.Registry.all_of<ReflectionProbeComponent>(entity)) {
            glyph = ICON_FA_GLOBE;
        } else if (world.Registry.all_of<DecalComponent>(entity)) {
            glyph = ICON_FA_SPRAY_CAN;
        } else if (world.Registry.all_of<PostProcessVolumeComponent>(entity)) {
            glyph = ICON_FA_SLIDERS;
        } else if (world.Registry.all_of<AudioSourceComponent>(entity)) {
            glyph = ICON_FA_VOLUME_HIGH;
        } else if (world.Registry.all_of<NpcSpawnComponent>(entity) ||
                   (nameC && nameC->Name.find("Spawn") != std::string::npos)) {
            glyph = ICON_FA_LOCATION_DOT;
        } else if (colC && colC->IsTrigger) {
            glyph = ICON_FA_VECTOR_SQUARE;
        } else {
            glyph = ICON_FA_DIAGRAM_PROJECT;
        }

        items.push_back({entity, glyph, screen, wp, glm::length(wp - camPos), selected,
                         world.Registry.all_of<InactiveTag>(entity), light != nullptr, isCamera,
                         light ? glm::clamp(light->Color, 0.0f, 1.0f) : glm::vec3(1.0f)});
    }

    // Pass 2: occlusion. One ray from the camera to each icon (the editor's triangle-exact
    // RaycastRenderables, which also works outside Play), refreshed stalest-first within a
    // per-frame budget; an icon never checked is treated as visible. Selected icons never hide.
    ++m_IconOcclusionFrame;
    {
        constexpr size_t kRaysPerFrame = 24;
        std::vector<std::pair<std::uint32_t, size_t>> stale; // (frame last checked, item index)
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].Selected) continue;
            auto it = m_IconOcclusion.find(items[i].Entity);
            stale.emplace_back(it == m_IconOcclusion.end() ? 0u : it->second.FrameChecked, i);
        }
        const size_t n = std::min(stale.size(), kRaysPerFrame);
        std::partial_sort(stale.begin(), stale.begin() + (std::ptrdiff_t)n, stale.end());
        for (size_t k = 0; k < n; ++k) {
            const Item& it = items[stale[k].second];
            // The icon's own ancestors (a lamp fixture it hangs from) must not hide it.
            std::vector<entt::entity> skipChain;
            if (const auto* hier = world.Registry.try_get<HierarchyComponent>(it.Entity)) {
                entt::entity walk = hier->Parent;
                while (walk != entt::null && world.Registry.valid(walk) && skipChain.size() < 32) {
                    skipChain.push_back(walk);
                    const auto* ph = world.Registry.try_get<HierarchyComponent>(walk);
                    walk = ph ? ph->Parent : entt::null;
                }
            }
            const glm::vec3 toIcon = it.Pos - camPos;
            const float dist = std::max(it.Dist, 1e-4f);
            const SurfaceHit hit = RaycastRenderables(
                world, camPos, toIcon / dist,
                [&](entt::entity e) { return std::find(skipChain.begin(), skipChain.end(), e) != skipChain.end(); },
                std::max(dist - 0.3f, 0.0f));
            IconOcclusion& slot = m_IconOcclusion[it.Entity];
            slot.Occluded = hit.Entity != entt::null;
            slot.FrameChecked = m_IconOcclusionFrame;
        }
        // Forget entities that stopped being candidates (deleted, mode changed) every few seconds.
        if ((m_IconOcclusionFrame & 511u) == 0u) {
            for (auto it = m_IconOcclusion.begin(); it != m_IconOcclusion.end();) {
                if (m_IconOcclusionFrame - it->second.FrameChecked > 600u) it = m_IconOcclusion.erase(it);
                else ++it;
            }
        }
    }

    // Pass 3: fade, drop what hides behind geometry (dim it instead through walls), declutter.
    const float baseIconPx = 16.0f * m_UIScale;
    std::vector<GizmoIconMath::IconCandidate> cands;
    std::vector<float> alphaMul(items.size(), 1.0f);
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        float a = GizmoIconMath::DistanceAlpha(it.Dist, it.Selected);
        if (!it.Selected) {
            auto o = m_IconOcclusion.find(it.Entity);
            if (o != m_IconOcclusion.end() && o->second.Occluded) {
                if (!prefs.EntityIconsThroughWalls) continue;
                a *= 0.2f;
            }
        }
        if (a <= 0.01f) continue;
        alphaMul[i] = a;
        GizmoIconMath::IconCandidate c;
        c.Id = (int)i;
        c.X = it.Screen.x;
        c.Y = it.Screen.y;
        c.Priority = it.Selected ? 0 : it.IsLight ? 1 : it.IsCamera ? 2 : 3;
        c.Distance = it.Dist;
        cands.push_back(c);
    }
    GizmoIconMath::Declutter(cands, 0.6f * baseIconPx);

    // Pass 4: draw, far to near so a nearer icon overlaps a farther one.
    ImFont* font = ImGui::GetFont();
    const ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    for (size_t k = cands.size(); k-- > 0;) {
        if (!cands[k].Keep) continue;
        const Item& it = items[(size_t)cands[k].Id];
        const float iconPx = baseIconPx * GizmoIconMath::DistanceScale(it.Dist, it.Selected);

        float rgb[3];
        float alpha;
        if (it.Inactive) {
            rgb[0] = rgb[1] = rgb[2] = 140.0f / 255.0f;
            alpha = 0.67f;
        } else if (it.IsLight) {
            // The light's own colour, pulled 40% toward grey so a rack of lights stays calm.
            const float lum = 0.299f * it.Light.r + 0.587f * it.Light.g + 0.114f * it.Light.b;
            for (int c = 0; c < 3; ++c) rgb[c] = it.Light[c] + (lum - it.Light[c]) * 0.4f;
            alpha = it.Selected ? 1.0f : 0.7f;
        } else {
            rgb[0] = textCol.x; rgb[1] = textCol.y; rgb[2] = textCol.z;
            alpha = it.Selected ? 1.0f : 0.8f;
        }
        const ImU32 color = IM_COL32((int)(rgb[0] * 255.0f), (int)(rgb[1] * 255.0f), (int)(rgb[2] * 255.0f),
                                     (int)(std::clamp(alpha * alphaMul[(size_t)cands[k].Id], 0.0f, 1.0f) * 255.0f));

        // Centre the glyph's ink (what you actually see), not its advance box, then snap to whole
        // pixels so the marker doesn't shimmer as the camera moves.
        ImVec2 ink;
        {
            unsigned cp = 0;
            ImTextCharFromUtf8(&cp, it.Glyph, nullptr);
            const ImFontGlyph* g = font->GetFontBaked(iconPx)->FindGlyph((ImWchar)cp);
            if (g) {
                ink = ImVec2((g->X0 + g->X1) * 0.5f, (g->Y0 + g->Y1) * 0.5f);
            } else {
                const ImVec2 gs = font->CalcTextSizeA(iconPx, FLT_MAX, 0.0f, it.Glyph);
                ink = ImVec2(gs.x * 0.5f, gs.y * 0.5f);
            }
        }
        const ImVec2 screen(std::floor(it.Screen.x + 0.5f), std::floor(it.Screen.y + 0.5f));
        draw->AddText(font, iconPx, ImVec2(std::floor(screen.x - ink.x + 0.5f), std::floor(screen.y - ink.y + 0.5f)),
                      color, it.Glyph);

        // Selected state = a subtle ring, not extra geometry inside the marker.
        // Motion v2: it sweeps on clockwise from the top like the mesh outline does.
        if (it.Selected) {
            const float reveal = SelectionOutlineReveal();
            if (reveal >= 1.0f) {
                draw->AddCircle(screen, iconPx * 0.72f, EditorTheme::U32(EditorTheme::Accent), 0, 1.5f * m_UIScale);
            } else if (reveal > 0.0f) {
                const float a0 = -IM_PI * 0.5f;
                draw->PathArcTo(screen, iconPx * 0.72f, a0, a0 + reveal * 2.0f * IM_PI, 32);
                draw->PathStroke(EditorTheme::U32(EditorTheme::Accent), ImDrawFlags_None, 1.5f * m_UIScale);
            }
        }
    }

    draw->PopClipRect();
}

void EditorLayer::DrawLightGizmos(World& world, Camera& editorCamera) {
    const EditorSettings& prefs = EditorSettings::Get();
    if (!prefs.ShowLightGizmos) return;
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    const glm::mat4 view = editorCamera.ViewMatrix();
    const glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);
    const glm::mat4 viewProj = proj * view;

    // Same draw target + clipping as DrawEntityIcons: the Scene window's own list, bounded to
    // the viewport rect, so panels over the viewport cover the shapes instead of them bleeding.
    ImGuiWindow* sceneWin = ImGui::FindWindowByName(EditorPanels::Scene);
    ImDrawList* draw = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    const ImVec2 clipMin(m_ViewportPos.x, m_ViewportPos.y);
    const ImVec2 clipMax(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y);
    draw->PushClipRect(clipMin, clipMax, true);

    auto project = [&](const glm::vec3& wp, ImVec2& out) -> bool {
        glm::vec4 clip = viewProj * glm::vec4(wp, 1.0f);
        if (clip.w <= 0.0001f) return false;               // at/behind the eye
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out = ImVec2(m_ViewportPos.x + (ndc.x * 0.5f + 0.5f) * m_ViewportSize.x,
                     m_ViewportPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * m_ViewportSize.y);
        return true;
    };
    // Project a world-space polyline and stroke it, dropping any segment with an endpoint that
    // failed to project (behind the camera) rather than drawing a wild line across the screen.
    auto stroke = [&](const std::vector<glm::vec3>& pts, bool closed, ImU32 col, float thick) {
        const int n = (int)pts.size();
        for (int i = 0; i < n - (closed ? 0 : 1); ++i) {
            ImVec2 a, b;
            if (project(pts[i], a) && project(pts[(i + 1) % n], b)) draw->AddLine(a, b, col, thick);
        }
    };
    auto circle = [](const glm::vec3& c, const glm::vec3& u, const glm::vec3& v, float r, int seg) {
        std::vector<glm::vec3> pts;
        pts.reserve(seg);
        for (int i = 0; i < seg; ++i) {
            float t = (float)i / (float)seg * 6.28318530718f;
            pts.push_back(c + (cosf(t) * u + sinf(t) * v) * r);
        }
        return pts;
    };
    auto basis = [](const glm::vec3& dir, glm::vec3& u, glm::vec3& v) {
        glm::vec3 up = std::fabs(dir.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        u = glm::normalize(glm::cross(dir, up));
        v = glm::cross(dir, u);
    };

    const float gscale = std::max(prefs.LightGizmoScale, 0.05f);

    for (auto entity : world.Registry.view<const TransformComponent, const LightComponent>()) {
        if (world.Registry.all_of<InactiveTag>(entity)) continue;
        const bool selected = IsSelected(entity);
        if (prefs.LightGizmoSelectedOnly && !selected) continue;

        const auto& light = world.Registry.get<const LightComponent>(entity);
        const glm::mat4 model = world.ComposeWorldTransform(entity);
        const glm::vec3 pos = glm::vec3(model[3]);
        const glm::vec3 dir = glm::normalize(glm::vec3(model * glm::vec4(0, 0, -1, 0)));

        glm::vec3 c = glm::clamp(light.Color, 0.0f, 1.0f);
        int a = (int)(std::clamp(prefs.LightGizmoOpacity, 0.0f, 1.0f) * 200.0f) + 25;
        if (selected) a = std::min(255, a + 100);
        const ImU32 col = IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), a);
        const float thick = selected ? 2.0f : 1.3f;

        if (light.Kind == LightComponent::Type::Point) {
            // One clean view-facing ring for the range (like Unity), plus a faint ground circle
            // so it still reads as a sphere sitting in space rather than a flat disc.
            float r = std::max(light.Range, 0.01f);
            stroke(circle(pos, editorCamera.Right(), editorCamera.Up(), r, 56), true, col, thick);
            stroke(circle(pos, glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), r, 48), true,
                   IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), a / 3), 1.0f);
        } else if (light.Kind == LightComponent::Type::Spot) {
            float range = std::max(light.Range, 0.05f);
            float half = glm::radians(std::clamp(light.SpotAngleDegrees, 1.0f, 89.0f));
            glm::vec3 u, v; basis(dir, u, v);
            glm::vec3 apex = pos;
            glm::vec3 center = pos + dir * range;
            float rimR = range * tanf(half);
            stroke(circle(center, u, v, rimR, 48), true, col, thick);
            // inner falloff cone, dimmer — matches main.cpp's 0.9 * SpotAngle inner cutoff
            float innerR = range * tanf(half * 0.9f);
            stroke(circle(center, u, v, innerR, 40), true, IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), a / 2), 1.0f);
            for (int k = 0; k < 4; ++k) {
                float t = (float)k * 1.57079632679f;
                std::vector<glm::vec3> edge = { apex, center + (cosf(t) * u + sinf(t) * v) * rimR };
                stroke(edge, false, col, thick);
            }
        } else { // Directional
            float L = 2.6f * gscale;
            glm::vec3 u, v; basis(dir, u, v);
            // two parallel shafts to read as parallel rays, each with a small chevron head
            for (int s = -1; s <= 1; s += 2) {
                glm::vec3 o = pos + u * (0.5f * gscale * (float)s);
                glm::vec3 tip = o + dir * L;
                std::vector<glm::vec3> shaft = { o, tip };
                stroke(shaft, false, col, thick);
                std::vector<glm::vec3> head1 = { tip, tip - dir * (0.5f * gscale) + u * (0.28f * gscale) };
                std::vector<glm::vec3> head2 = { tip, tip - dir * (0.5f * gscale) - u * (0.28f * gscale) };
                stroke(head1, false, col, thick);
                stroke(head2, false, col, thick);
            }
            stroke(circle(pos, u, v, 0.38f * gscale, 32), true, col, thick); // sun disc
        }

        if (selected) {
            ImVec2 sp;
            if (project(pos, sp)) {
                char buf[96];
                if (light.Kind == LightComponent::Type::Spot)
                    snprintf(buf, sizeof(buf), "%.0f deg  |  range %.1f", light.SpotAngleDegrees, light.Range);
                else if (light.Kind == LightComponent::Type::Point)
                    snprintf(buf, sizeof(buf), "range %.1f", light.Range);
                else
                    snprintf(buf, sizeof(buf), "sun  %.2f deg", light.AngularSizeDegrees);
                ImVec2 tp(sp.x + 12.0f * m_UIScale, sp.y - 6.0f * m_UIScale);
                draw->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 180), buf);
                draw->AddText(tp, IM_COL32(255, 255, 255, 235), buf);
            }
        }
    }

    draw->PopClipRect();
}

void EditorLayer::DrawReflectionProbeGizmos(World& world, Camera& editorCamera) {
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    const glm::mat4 view      = editorCamera.ViewMatrix();
    const glm::mat4 proj      = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);
    const glm::mat4 viewProj  = proj * view;

    ImGuiWindow* sceneWin = ImGui::FindWindowByName(EditorPanels::Scene);
    ImDrawList* draw = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    const ImVec2 clipMin(m_ViewportPos.x, m_ViewportPos.y);
    const ImVec2 clipMax(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y);
    draw->PushClipRect(clipMin, clipMax, true);

    auto project = [&](const glm::vec3& wp, ImVec2& out) -> bool {
        glm::vec4 clip = viewProj * glm::vec4(wp, 1.0f);
        if (clip.w <= 0.0001f) return false;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out = ImVec2(m_ViewportPos.x + (ndc.x * 0.5f + 0.5f) * m_ViewportSize.x,
                     m_ViewportPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * m_ViewportSize.y);
        return true;
    };
    auto strokeEdge = [&](const glm::vec3& a, const glm::vec3& b, ImU32 col, float thick) {
        ImVec2 sa, sb;
        if (project(a, sa) && project(b, sb))
            draw->AddLine(sa, sb, col, thick);
    };

    const ImU32 kProbeColor    = IM_COL32(100, 200, 255, 200);
    const ImU32 kProbeSelected = EditorTheme::U32(EditorTheme::AccentBright);
    const float kThick = 1.5f;

    // Probes, post-process volumes and decals each have their own switch; by default only the
    // selected one (or the ones under it) draws, so a placed-out scene stays clean.
    const EditorSettings& vprefs = EditorSettings::Get();
    auto showVolume = [&](entt::entity e) { return !vprefs.VolumeGizmosSelectedOnly || IsSelectedOrUnderSelected(world, e); };

    auto probeView = world.Registry.view<const TransformComponent, const ReflectionProbeComponent>();
    for (auto [entity, tc, rp] : probeView.each()) {
        if (!vprefs.ShowProbeGizmos || !showVolume(entity)) continue;
        glm::mat4 wt     = world.GetCachedWorldTransform(entity);
        glm::vec3 center = glm::vec3(wt[3]);
        glm::vec3 scale  = glm::vec3(glm::length(glm::vec3(wt[0])),
                                     glm::length(glm::vec3(wt[1])),
                                     glm::length(glm::vec3(wt[2])));
        glm::vec3 hs     = rp.Size * scale * 0.5f;

        bool selected = IsSelected(entity);
        ImU32 col = selected ? kProbeSelected : kProbeColor;

        // 8 box corners
        glm::vec3 c[8];
        for (int i = 0; i < 8; ++i)
            c[i] = center + glm::vec3(
                (i & 1) ? hs.x : -hs.x,
                (i & 2) ? hs.y : -hs.y,
                (i & 4) ? hs.z : -hs.z);

        // 12 box edges
        int edges[12][2] = {
            {0,1},{2,3},{4,5},{6,7}, // along X
            {0,2},{1,3},{4,6},{5,7}, // along Y
            {0,4},{1,5},{2,6},{3,7}, // along Z
        };
        for (auto& e : edges) strokeEdge(c[e[0]], c[e[1]], col, kThick);
    }

    // #162 / #203 - local Post-process Volumes: their (oriented) box.
    const ImU32 kVolumeColor = IM_COL32(120, 230, 150, 170);
    auto volumeView = world.Registry.view<const TransformComponent, const PostProcessVolumeComponent>();
    for (auto [entity, tc, vol] : volumeView.each()) {
        if (!vprefs.ShowVolumeGizmos || !showVolume(entity)) continue;
        if (vol.Global || world.Registry.all_of<InactiveTag>(entity)) continue;
        const glm::mat4 wt = world.GetCachedWorldTransform(entity);
        const glm::vec3 h = glm::abs(vol.Size) * 0.5f;
        glm::vec3 c[8];
        for (int i = 0; i < 8; ++i)
            c[i] = glm::vec3(wt * glm::vec4((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z, 1.0f));
        const int edges[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
        const ImU32 col = IsSelected(entity) ? kProbeSelected : kVolumeColor;
        for (auto& e : edges) strokeEdge(c[e[0]], c[e[1]], col, kThick);
    }

    // Decals: the projection box (its Size; the Scale gizmo edits it). The +Z face (out of the
    // surface) is drawn solid and the far -Z face faint, with an arrow down the projection axis
    // and a tick on the top edge, so which way it projects and which way is up both read at a
    // glance. Unselected decals only get a faint outline.
    const ImU32 kDecalColor = IM_COL32(230, 200, 150, 110);
    auto decalView = world.Registry.view<const TransformComponent, const DecalComponent>();
    for (auto [entity, tc, dc] : decalView.each()) {
        if (!vprefs.ShowDecalGizmos || !showVolume(entity)) continue;
        if (world.Registry.all_of<InactiveTag>(entity)) continue;
        const bool selected = IsSelected(entity);
        const glm::mat4 box = world.GetCachedWorldTransform(entity) * glm::scale(glm::mat4(1.0f), dc.Size);
        auto at = [&](float x, float y, float z) { return glm::vec3(box * glm::vec4(x, y, z, 1.0f)); };
        glm::vec3 c[8];
        for (int i = 0; i < 8; ++i) c[i] = at((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f);
        const ImU32 col = selected ? kProbeSelected : kDecalColor;
        const ImU32 faint = selected ? IM_COL32(255, 255, 255, 90) : IM_COL32(230, 200, 150, 45);
        const int front[4][2] = {{4,5},{6,7},{4,6},{5,7}};   // z = +0.5
        const int back[4][2]  = {{0,1},{2,3},{0,2},{1,3}};   // z = -0.5
        const int sides[4][2] = {{0,4},{1,5},{2,6},{3,7}};
        for (auto& e : front) strokeEdge(c[e[0]], c[e[1]], col, kThick);
        for (auto& e : sides) strokeEdge(c[e[0]], c[e[1]], faint, 1.0f);
        for (auto& e : back) strokeEdge(c[e[0]], c[e[1]], faint, 1.0f);
        if (!selected) continue;
        // Projection arrow: front-face centre to the far face, with a small head.
        const glm::vec3 a0 = at(0, 0, 0.5f), a1 = at(0, 0, -0.5f);
        strokeEdge(a0, a1, col, kThick);
        strokeEdge(a1, at(0.08f, 0, -0.38f), col, kThick);
        strokeEdge(a1, at(-0.08f, 0, -0.38f), col, kThick);
        strokeEdge(at(0, 0.5f, 0.5f), at(0, 0.38f, 0.5f), col, kThick); // up tick
    }

    draw->PopClipRect();
}

void EditorLayer::UpdateLightHandles(World& world, Camera& editorCamera) {
    m_LightHandleEngaged = false;
    const EditorSettings& prefs = EditorSettings::Get();
    // #162 — de-conflict with the transform gizmo instead of hard-vanishing the dots.
    // While the gizmo is actually being dragged, the dots go away entirely. While it's only
    // hovered (m_GizmoEngaged carries last frame's ImGuizmo::IsOver() || IsUsing()), keep the
    // dots on screen but faded and non-interactive so the gizmo always wins the cursor.
    const bool gizmoDragging = ImGuizmo::IsUsing();
    const bool gizmoHot = m_GizmoEngaged;
    if (!prefs.ShowLightGizmos || m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f ||
        gizmoDragging || HasGroupSelection()) {
        m_LightHandleDragging = false;
        m_HotLightHandle = LightHandle::None;
        return;
    }
    entt::entity e = m_Selected;
    // Only for a light that was clicked in the viewport (its icon) — not one selected from the
    // Hierarchy / Lights panel / box-select. m_LightHandleArmedFor is set by HandleViewportPicking
    // and cleared by every other selection path (#140 follow-up).
    if (e == entt::null || e != m_LightHandleArmedFor || !world.Registry.valid(e) ||
        !world.Registry.all_of<LightComponent>(e)) {
        m_LightHandleDragging = false;
        m_HotLightHandle = LightHandle::None;
        return;
    }

    auto& light = world.Registry.get<LightComponent>(e);
    auto& xf = world.Registry.get<TransformComponent>(e);
    entt::entity parent = entt::null;
    if (auto* h = world.Registry.try_get<HierarchyComponent>(e)) parent = h->Parent;
    glm::mat4 parentWorld = parent != entt::null ? world.ComposeWorldTransform(parent) : glm::mat4(1.0f);
    glm::mat4 model = world.ComposeWorldTransform(e);
    glm::vec3 pos = glm::vec3(model[3]);
    glm::vec3 dir = glm::normalize(glm::vec3(model * glm::vec4(0, 0, -1, 0)));

    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);
    glm::mat4 vp = proj * view;
    auto project = [&](const glm::vec3& w, ImVec2& out) -> bool {
        glm::vec4 c = vp * glm::vec4(w, 1.0f);
        if (c.w <= 0.0001f) return false;
        glm::vec3 n = glm::vec3(c) / c.w;
        out = ImVec2(m_ViewportPos.x + (n.x * 0.5f + 0.5f) * m_ViewportSize.x,
                     m_ViewportPos.y + (1.0f - (n.y * 0.5f + 0.5f)) * m_ViewportSize.y);
        return true;
    };
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    glm::mat4 invVP = glm::inverse(vp);
    float ndcX = 2.0f * (mouse.x - m_ViewportPos.x) / m_ViewportSize.x - 1.0f;
    float ndcY = 1.0f - 2.0f * (mouse.y - m_ViewportPos.y) / m_ViewportSize.y;
    glm::vec4 rn = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f); rn /= rn.w;
    glm::vec4 rf = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);  rf /= rf.w;
    glm::vec3 rayO = glm::vec3(rn);
    glm::vec3 rayD = glm::normalize(glm::vec3(rf) - glm::vec3(rn));

    // Parameter t of the point on the line (P0 + A*t, A unit) closest to the cursor ray.
    auto lineParamNearestRay = [&](const glm::vec3& P0, const glm::vec3& A) -> float {
        glm::vec3 w0 = P0 - rayO;
        float b = glm::dot(A, rayD);
        float d = glm::dot(A, w0);
        float ee = glm::dot(rayD, w0);
        float denom = 1.0f - b * b;                 // a*c - b*b, with a=c=1
        if (std::fabs(denom) < 1e-5f) return -d;    // ray ~parallel to the axis
        return (b * ee - d) / denom;
    };

    // Build this light's dot list: {kind, world position, slide axis}.
    struct Dot { LightHandle kind; glm::vec3 world; glm::vec3 axis; };
    std::vector<Dot> dots;
    float range = std::max(light.Range, 0.05f);
    float halfDeg = std::clamp(light.SpotAngleDegrees, 1.0f, 89.0f);
    glm::vec3 u, v;
    {
        glm::vec3 up = std::fabs(dir.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        u = glm::normalize(glm::cross(dir, up));
        v = glm::cross(dir, u);
    }
    if (light.Kind == LightComponent::Type::Point) {
        // Four dots on the view-facing range ring (matches the single ring the gizmo now draws).
        const glm::vec3 rr = editorCamera.Right(), uu = editorCamera.Up();
        dots.push_back({LightHandle::Range, pos + rr * range, rr});
        dots.push_back({LightHandle::Range, pos - rr * range, -rr});
        dots.push_back({LightHandle::Range, pos + uu * range, uu});
        dots.push_back({LightHandle::Range, pos - uu * range, -uu});
    } else if (light.Kind == LightComponent::Type::Spot) {
        glm::vec3 center = pos + dir * range;
        float rimR = range * tanf(glm::radians(halfDeg));
        dots.push_back({LightHandle::Range, center, dir});
        dots.push_back({LightHandle::SpotAngle, center + u * rimR, u});
        dots.push_back({LightHandle::SpotAngle, center - u * rimR, -u});
        dots.push_back({LightHandle::SpotAngle, center + v * rimR, v});
        dots.push_back({LightHandle::SpotAngle, center - v * rimR, -v});
        dots.push_back({LightHandle::Aim, pos + dir * std::clamp(range * 0.4f, 1.0f, 6.0f), dir});
    } else { // Directional
        dots.push_back({LightHandle::Aim, pos + dir * (2.6f * std::max(prefs.LightGizmoScale, 0.05f)), dir});
    }

    ImGuiWindow* sceneWin = ImGui::FindWindowByName(EditorPanels::Scene);
    ImDrawList* draw = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    draw->PushClipRect(ImVec2(m_ViewportPos.x, m_ViewportPos.y),
                       ImVec2(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y), true);

    const bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (m_LightHandleDragging) {
        m_LightHandleEngaged = true;
        if (!leftDown) {
            m_LightHandleDragging = false;
            m_HotLightHandle = LightHandle::None;
        } else if (m_HotLightHandle == LightHandle::Range) {
            float t = lineParamNearestRay(pos, m_LightHandleGrabAxis);
            light.Range = std::clamp(t, 0.05f, 100000.0f);
        } else if (m_HotLightHandle == LightHandle::SpotAngle) {
            glm::vec3 center = pos + dir * std::max(light.Range, 0.05f);
            float t = lineParamNearestRay(center, m_LightHandleGrabAxis);
            float newHalf = glm::degrees(atanf(std::max(t, 1e-4f) / std::max(light.Range, 0.05f)));
            light.SpotAngleDegrees = std::clamp(newHalf, 1.0f, 89.0f);
        } else if (m_HotLightHandle == LightHandle::Aim) {
            glm::vec3 handlePos = pos + dir * m_LightHandleGrabParam;
            glm::vec3 nrm = -editorCamera.Front();
            float denom = glm::dot(rayD, nrm);
            if (std::fabs(denom) > 1e-5f) {
                float s = glm::dot(handlePos - rayO, nrm) / denom;
                glm::vec3 fwd = (rayO + rayD * s) - pos;
                if (glm::length(fwd) > 1e-4f) {
                    fwd = glm::normalize(fwd);
                    glm::vec3 up = std::fabs(fwd.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
                    glm::mat4 rot = glm::inverse(glm::lookAt(glm::vec3(0.0f), fwd, up));
                    xf.SetRotationQuaternion(QuaternionFromMatrix(glm::inverse(parentWorld) * rot));
                }
            }
        }
    } else {
        m_HotLightHandle = LightHandle::None;
        m_HotLightHandleIndex = -1;
        float best = 11.0f * m_UIScale;
        // #162 — the gizmo owns the cursor while it's hovered; don't let a dot under one of
        // its arrows claim the hover/click.
        for (int i = 0; !gizmoHot && i < (int)dots.size(); ++i) {
            ImVec2 sp;
            if (!project(dots[i].world, sp)) continue;
            float d = std::sqrt((sp.x - mouse.x) * (sp.x - mouse.x) + (sp.y - mouse.y) * (sp.y - mouse.y));
            if (d < best) { best = d; m_HotLightHandleIndex = i; m_HotLightHandle = dots[i].kind; }
        }
        if (m_HotLightHandleIndex >= 0) {
            m_LightHandleEngaged = true; // hovering a dot claims the click (no deselect / box-select)
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !WantsCaptureMouse()) {
                const Dot& g = dots[m_HotLightHandleIndex];
                m_LightHandleDragging = true;
                m_LightHandleGrabAxis = g.axis;
                if (g.kind == LightHandle::Aim) m_LightHandleGrabParam = glm::length(g.world - pos);
                PushUndo(world, g.kind == LightHandle::Aim ? "Aim Light"
                              : g.kind == LightHandle::SpotAngle ? "Set Spot Angle" : "Set Light Range");
            }
        }
    }

    // #162 — dots draw here, before DrawGizmo() runs later this frame into the same Scene
    // draw list, so the transform gizmo always paints on top. Two fades keep them from
    // fighting the gizmo visually: a flat dim while the gizmo is hovered, and a radial
    // falloff for any dot sitting within ~one gizmo-arm's length of the light's origin.
    ImVec2 originSP;
    const bool haveOrigin = project(pos, originSP);
    const float gizmoPx = std::max(m_GizmoSize * m_ViewportSize.y * 0.5f, 1.0f);
    const float baseAlpha = gizmoHot ? 0.28f : 1.0f;
    for (int i = 0; i < (int)dots.size(); ++i) {
        ImVec2 sp;
        if (!project(dots[i].world, sp)) continue;
        bool hot = m_LightHandleDragging
                       ? (dots[i].kind == m_HotLightHandle &&
                          glm::dot(dots[i].axis, m_LightHandleGrabAxis) > 0.999f)
                       : (i == m_HotLightHandleIndex);
        float alpha = baseAlpha;
        if (haveOrigin && !hot) {
            float dOrigin = std::sqrt((sp.x - originSP.x) * (sp.x - originSP.x) +
                                      (sp.y - originSP.y) * (sp.y - originSP.y));
            if (dOrigin < gizmoPx)
                alpha *= std::clamp(0.12f + 0.88f * (dOrigin / gizmoPx), 0.0f, 1.0f);
        }
        if (hot) alpha = 1.0f; // the dot you're dragging stays fully legible
        float r = (hot ? 6.0f : 4.0f) * m_UIScale;
        int fillA = (int)((hot ? 255.0f : 225.0f) * alpha);
        int lineA = (int)(190.0f * alpha);
        draw->AddCircleFilled(sp, r, hot ? EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Accent, fillA / 255.0f)) : IM_COL32(245, 245, 245, fillA));
        draw->AddCircle(sp, r, IM_COL32(0, 0, 0, lineA), 0, 1.5f);
    }

    draw->PopClipRect();
}

// The transform gizmo and the nav compass each live in a fullscreen NoInputs overlay that
// forces itself to the display front every frame (so the Scene image can't cover it). That
// also parked it on top of the centered Preferences / Project Settings windows. Re-fronting
// those specific floating windows right after puts them back above the overlay — they're the
// only editor windows that float free over the viewport rather than dock beside it.
static void KeepFloatingWindowsAboveOverlay() {
    // Re-front EVERY free-floating panel/modal — not a hardcoded list — so the gizmo overlay
    // never paints over Preferences, Lighting, Project Settings, or any modal. Docked panels
    // (Hierarchy / Inspector / Asset Browser / Console) stay put: they're part of the main
    // dockspace and don't overlap the viewport. Internal overlays/hosts all have "##" names,
    // which this skips.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    // BringWindowToDisplayFront reorders g.Windows, so work from a snapshot: iterating the live
    // list skipped or revisited windows, and a floating panel fronted after a popup ended up
    // above it (the Window menu opened behind a floating Physics Debug / Settings window).
    // Floating panels go first, then popups/tooltips, so the latter always finish on top.
    ImVector<ImGuiWindow*> floating, popups;
    for (ImGuiWindow* w : g.Windows) {
        if (!w->WasActive || w->Hidden) continue;
        // Popups, context menus and tooltips (a Combo's dropdown list, a right-click menu, a
        // hover hint) must sit above the overlay no matter which window opened them — otherwise
        // a Combo opened from Preferences renders behind this transparent fullscreen overlay and
        // looks like it never opened. They carry ImGui's "##" auto-id names and a ParentWindow,
        // so they'd be skipped by the floating-window filters below.
        if (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) {
            popups.push_back(w);
            continue;
        }
        if (w->ParentWindow != nullptr) continue;                 // child of another window
        if (w->Flags & ImGuiWindowFlags_ChildWindow) continue;
        if (w->DockNode != nullptr) continue;                     // docked — not floating over the viewport
        if (w->Name[0] == '#' && w->Name[1] == '#') continue;     // ##GizmoOverlay / ##DockHost / ...
        if (std::strcmp(w->Name, EditorPanels::Scene) == 0 || std::strcmp(w->Name, EditorPanels::Game) == 0) continue;
        floating.push_back(w);
    }
    for (ImGuiWindow* w : floating) ImGui::BringWindowToDisplayFront(w);
    for (ImGuiWindow* w : popups) ImGui::BringWindowToDisplayFront(w);
}

// Shared setup behind DrawGizmo() (single-object) and DrawGroupGizmo() (multi-select). See the
// declaration in EditorLayer.h for the contract; the comments on the individual calls below
// explain why each one is needed.
//
// Phase 3 item 6's third clause ("add depth-test-off gizmo rendering so it's never hidden behind
// geometry", audit #5) is already true by construction here, not something this pass needed to
// add: ImGuizmo draws into this fullscreen ImGui overlay's draw list, which BringWindowToDisplayFront
// below forces in front of "Scene" every frame — a flat 2D composite painted AFTER the 3D scene
// resolves to a texture, with zero depth-buffer participation. Same story for the nav gizmo
// (DrawViewGizmo), light gizmos and entity icons (DrawLightGizmos/DrawEntityIcons) — all project
// world space into an ImDrawList by hand rather than rendering real 3D geometry. The one thing
// that IS depth-tested, the collider wireframe (main.cpp, colliderGizmo.Draw), is deliberately so
// per its own #185 PR 2 comment — "depth-tested so scene geometry occludes it" is the intended
// reading (a collider behind a wall should look behind the wall), not a bug this item should undo.
bool EditorLayer::BeginGizmoOverlay(Camera& editorCamera, const char* overlayName) {
    int w, h;
    glfwGetWindowSize(m_Window, &w, &h);
    if (w <= 0 || h <= 0) return false;

    // ImGuizmo's own hover/click hit-testing needs a real, hoverable ImGui window as the
    // "current window" — calling Manipulate() with no window active draws fine but never
    // registers clicks. A fullscreen transparent overlay gives it that context without
    // visually intruding or stealing focus from the other panels.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    // NoInputs keeps this fullscreen window out of ImGui's own hover/capture bookkeeping —
    // otherwise it would make WantCaptureMouse true everywhere on screen and block the
    // editor fly-camera. ImGuizmo does its own hit-testing against raw mouse position, so
    // it isn't affected by this window's own input flags.
    ImGui::Begin(overlayName, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
    // NoInputs means this window can never be clicked to bring itself to front, so its z-order
    // is otherwise whatever position it happened to land in ImGui's window stack the first time
    // it was ever created - which put it BEHIND "Scene" once that became a real window (Scene is
    // newer, so it was appended in front). Forced to the front explicitly, every frame, so the
    // gizmo actually draws on top of the Scene image instead of being invisibly covered by it.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    KeepFloatingWindowsAboveOverlay(); // ...but not on top of Preferences / Project Settings

    // ImGuizmo hit-tests against its own draw-list window (this NoInputs overlay), which is never
    // ImGui's g.HoveredWindow — so without this, hovering the actual "Scene" panel makes
    // IsHoveringWindow() return false and the handles draw but never grab. Registering "Scene" as
    // the alternative window is ImGuizmo's supported way to say "the user hovers there, not here".
    ImGuizmo::SetAlternativeWindow(ImGui::FindWindowByName(EditorPanels::Scene));

    ImGuizmo::SetOrthographic(editorCamera.Orthographic); // #107 — ortho views used to offset the handles
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(m_ViewportPos.x, m_ViewportPos.y, m_ViewportSize.x, m_ViewportSize.y);
    ImGuizmo::SetGizmoSizeClipSpace(m_GizmoSize);
    return true;
}

void EditorLayer::EndGizmoOverlay() {
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void EditorLayer::DrawGizmo(World& world, Camera& editorCamera) {
    // Motion v2: the radial menu is up (it's about to change the tool anyway); the handles step
    // aside so they neither cover it nor grab its click.
    if (m_Radial.Open) {
        m_GizmoEngaged = false;
        m_GizmoUsing = false;
        return;
    }
    if (HasGroupSelection()) {
        DrawGroupGizmo(world, editorCamera);
        return;
    }

    if (m_Selected == entt::null || !world.Registry.valid(m_Selected)) {
        m_GizmoEngaged = false;
        m_GizmoUsing = false;
        return;
    }

    auto& transform = world.Registry.get<TransformComponent>(m_Selected);
    // Lights and empties have no mesh, so this can legitimately be null — everything below that
    // needs mesh extents (Rect-tool bounds, Center pivot) falls back to a small unit box.
    auto* renderablePtr = world.Registry.try_get<RenderableComponent>(m_Selected);
    bool registryHasRenderable = renderablePtr != nullptr;
    // A decal's box is its Size, not the Transform's scale (kept at 1): the gizmo works on the box
    // (Size folded into the matrix it manipulates) and a scale / bounds drag writes Size back.
    DecalComponent* decalPtr = registryHasRenderable ? nullptr : world.Registry.try_get<DecalComponent>(m_Selected);

    // A parented entity's TransformComponent is local space, but ImGuizmo has to manipulate a
    // world-space matrix (it's drawn and dragged against the world-space view/proj below) — so
    // the gizmo works in world space and the result gets converted back to local afterward.
    entt::entity parent = entt::null;
    if (auto* hier = world.Registry.try_get<HierarchyComponent>(m_Selected)) parent = hier->Parent;
    glm::mat4 parentWorld = parent != entt::null ? world.ComposeWorldTransform(parent) : glm::mat4(1.0f);

    glm::vec3 nativeBoundsMin = registryHasRenderable ? renderablePtr->ModelRef->BoundsMin() : glm::vec3(-0.5f);
    glm::vec3 nativeBoundsMax = registryHasRenderable ? renderablePtr->ModelRef->BoundsMax() : glm::vec3(0.5f);

    if (!BeginGizmoOverlay(editorCamera, "##GizmoOverlay")) return;

    ImGuizmo::MODE gizmoMode = m_GizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (m_GizmoOp == GizmoOp::Rotate) op = ImGuizmo::ROTATE;
    else if (m_GizmoOp == GizmoOp::Scale) op = ImGuizmo::SCALE;
    else if (m_GizmoOp == GizmoOp::Rect) op = ImGuizmo::OPERATION(ImGuizmo::TRANSLATE | ImGuizmo::BOUNDS);
    else if (m_GizmoOp == GizmoOp::Universal) op = ImGuizmo::UNIVERSAL; // #236 E — one gizmo, all three

    glm::mat4 matrix = parentWorld * ComposeTransform(transform);
    if (decalPtr) matrix = matrix * glm::scale(glm::mat4(1.0f), decalPtr->Size);

    // Pivot/Center: in Center mode the gizmo is drawn at the bounding-box center instead of the
    // object's own origin. Only the gizmo's displayed frame shifts — the delta it produces is
    // applied back to the real transform below, so the object doesn't move when the mode flips.
    glm::vec3 centerOffset(0.0f);
    if (m_GizmoPivotCenter && registryHasRenderable) {
        AABB nativeBounds{nativeBoundsMin, nativeBoundsMax};
        AABB worldBounds = nativeBounds.Transformed(matrix);
        glm::vec3 worldCenter = (worldBounds.Min + worldBounds.Max) * 0.5f;
        centerOffset = worldCenter - glm::vec3(matrix[3]);
        matrix[3] += glm::vec4(centerOffset, 0.0f);
    }

    // Ctrl held inverts the checkbox for the duration of the drag — matches Blender's
    // momentary-snap convention while still giving snapping a persistent on/off default.
    bool snapActive = m_GridSnapEnabled != ImGui::GetIO().KeyCtrl;
    float snapValues[3] = {m_SnapTranslation, m_SnapTranslation, m_SnapTranslation};
    if (m_GizmoOp == GizmoOp::Rotate) snapValues[0] = snapValues[1] = snapValues[2] = m_SnapRotationDeg;
    else if (m_GizmoOp == GizmoOp::Scale) snapValues[0] = snapValues[1] = snapValues[2] = m_SnapScale;

    float bounds[6] = {nativeBoundsMin.x, nativeBoundsMin.y, nativeBoundsMin.z,
                        nativeBoundsMax.x, nativeBoundsMax.y, nativeBoundsMax.z};
    const float* boundsPtr = (m_GizmoOp == GizmoOp::Rect) ? bounds : nullptr;

    const glm::mat4 matrixBeforeDrag = matrix;
    const ImGuizmo::Style gizmoStyle = PushGizmoMotionStyle(); // Motion v2: hover springs
    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, gizmoMode,
        glm::value_ptr(matrix), nullptr, snapActive ? snapValues : nullptr, boundsPtr);
    PopGizmoMotionStyle(gizmoStyle);
    m_GizmoEngaged = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    m_GizmoUsing = ImGuizmo::IsUsing();
    EditorSound::GizmoFrame(m_GizmoUsing, matrix != matrixBeforeDrag, snapActive);

    bool isUsingNow = ImGuizmo::IsUsing();
    const bool dragStarted = isUsingNow && !m_GizmoWasUsing;
    if (isUsingNow && !m_GizmoWasUsing) {
        // Drag just started this frame: snapshot the still-unmodified transform (pos/rot/scale
        // below haven't been written yet) so undo restores to exactly where the drag began, and
        // so the live readout below can show a delta from here (#236 E).
        PushUndo(world, GizmoOpUndoLabel(m_GizmoOp));
        m_GizmoDragStartPos = transform.Position;
        m_GizmoDragStartRot = transform.EulerDegrees();
        m_GizmoDragStartScale = transform.Scale;
    }
    if (!isUsingNow && m_GizmoWasUsing && snapActive) GlassNotifySnap(); // Glass theme: the snap ring
    m_GizmoWasUsing = isUsingNow;

    if (isUsingNow) {
        // Undo the Center-mode display shift before reading the result back, so the object's
        // real origin moves by the drag delta rather than jumping onto its bounds center.
        glm::mat4 adjusted = matrix;
        adjusted[3] -= glm::vec4(centerOffset, 0.0f);
        // Manipulate() edited the world-space matrix — convert back to local before writing it
        // to TransformComponent (a no-op conversion when unparented, since parentWorld is then
        // identity).
        glm::mat4 newLocal = glm::inverse(parentWorld) * adjusted;
        float nt[3], nr[3], ns[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(newLocal), nt, nr, ns);
        glm::vec3 newPos{nt[0], nt[1], nt[2]};
        glm::vec3 newScale{ns[0], ns[1], ns[2]};
        // Store the manipulated orientation directly. Euler is only an Inspector/readout view,
        // so the gizmo never passes through a gimbal-locked decomposition (#123).
        const glm::quat newRotation = QuaternionFromMatrix(newLocal);
        // The scale channel's owner: a decal's Size (the matrix carried it), else the Transform.
        auto writeScale = [&]() {
            if (!decalPtr) { transform.Scale = newScale; return; }
            const glm::vec3 ts = glm::max(glm::abs(transform.Scale), glm::vec3(1e-4f));
            decalPtr->Size = glm::max(glm::abs(newScale) / ts, glm::vec3(0.001f));
        };

        // Write back only the channel this gizmo actually drives — ImGuizmo's decompose leaks
        // float noise into the other two, and a pure translate drag was nudging Rotation
        // 0.000 -> -0.000 every time, accumulating over many drags (#12 P1).
        switch (m_GizmoOp) {
            case GizmoOp::Translate: transform.Position = newPos;      break;
            case GizmoOp::Rotate:    transform.SetRotationQuaternion(newRotation); break;
            case GizmoOp::Scale:     writeScale();                      break;
            default: // Rect / Universal / bounds edit — position and scale both move
                transform.Position = newPos;
                transform.SetRotationQuaternion(newRotation);
                writeScale();
                break;
        }

        // #236 E — surface drag-snapping: while translating with the toggle on (hold Shift to
        // invert), override the axis-handle result: drop the object where the cursor ray hits
        // another surface, optionally aligning to that face's normal.
        if (m_GizmoOp == GizmoOp::Translate && (m_SurfaceSnap != ImGui::GetIO().KeyShift)) {
            ApplySurfaceSnap(world, editorCamera, m_Selected, parentWorld);
            const ImVec2 mp = ImGui::GetIO().MousePos;
            ImGui::GetForegroundDrawList()->AddText(ImVec2(mp.x + 18.0f, mp.y - 20.0f),
                IM_COL32(120, 220, 160, 255),
                m_SurfaceSnapAlign ? "surface + align" : "surface");
        }
        const glm::vec3& shownScale = decalPtr ? decalPtr->Size : transform.Scale;
        DrawGizmoDragReadout(transform.Position, transform.EulerDegrees(), shownScale);
        PushGizmoEditToPhysics(m_Selected, parentWorld * ComposeTransform(transform)); // #185
        if (snapActive) {
            // Motion v2: a tick for each snap step crossed, measured on the readout's own value.
            if (m_GizmoOp == GizmoOp::Rotate)     NoteGizmoSnapStep(transform.EulerDegrees() - m_GizmoDragStartRot, dragStarted, 0, "\xC2\xB0");
            else if (m_GizmoOp == GizmoOp::Scale) NoteGizmoSnapStep(shownScale, dragStarted, 2, nullptr);
            else                                  NoteGizmoSnapStep(transform.Position - m_GizmoDragStartPos, dragStarted, 2, nullptr);
        }
    }
    DrawSnapTicks();

    EndGizmoOverlay();
}

// #185 — push a transform-gizmo edit into the live PhysX actor during Play so the drag holds
// (the sim would otherwise write the old pose straight back over it next step).
void EditorLayer::PushGizmoEditToPhysics(entt::entity e, const glm::mat4& worldMatrix) {
    if (!m_InPlayMode || !PhysicsWorld::IsActive() || e == entt::null) return;
    const glm::vec3 p(worldMatrix[3]);
    const glm::quat rotation = QuaternionFromMatrix(worldMatrix);
    const float pf[3] = { p.x, p.y, p.z };
    const float rf[4] = { rotation.x, rotation.y, rotation.z, rotation.w };
    PhysicsWorld::SetActorPose((unsigned)entt::to_integral(e), pf, rf, /*zeroVelocity=*/true);
}


// #236 E — a small readout near the cursor while a gizmo is dragging: the delta from where the
// drag started, in the units that match the active tool. Motion v2: each number rolls like a
// mechanical counter as it changes (Glass Odometer, category RollingNumbers; off = plain digits).
void EditorLayer::DrawGizmoDragReadout(const glm::vec3& pos, const glm::vec3& rot, const glm::vec3& scale) {
    const char* tag = "T";
    glm::vec3 v = pos - m_GizmoDragStartPos;
    int decimals = 2;
    const char* suffix = nullptr;
    bool showLength = true;
    switch (m_GizmoOp) {
        case GizmoOp::Rotate: tag = "R"; v = rot - m_GizmoDragStartRot; decimals = 1; suffix = " deg"; showLength = false; break;
        case GizmoOp::Scale:  tag = "S"; v = scale; decimals = 3; showLength = false; break;
        default: break; // Translate / Rect / Universal
    }
    Glass::ImAnimContext& motion = Glass::Motion();
    const float gap = ImGui::CalcTextSize("  ").x;
    const float lenW = showLength ? ImGui::CalcTextSize("  (||)").x + Glass::ImAnimContext::OdometerWidth(glm::length(v), decimals) : 0.0f;
    float w = ImGui::CalcTextSize(tag).x + lenW;
    for (int a = 0; a < 3; ++a) w += gap + Glass::ImAnimContext::OdometerWidth(v[a], decimals, a == 2 ? suffix : nullptr);

    const ImVec2 mp = ImGui::GetIO().MousePos;
    const ImVec2 at(mp.x + 18.0f, mp.y + 18.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    dl->AddRectFilled(ImVec2(at.x - 5.0f, at.y - 3.0f), ImVec2(at.x + w + 5.0f, at.y + lineH + 3.0f),
                      EditorUIPrimitives::kHudPlateColor, 3.0f * m_UIScale);
    const ImU32 col = EditorUIPrimitives::kHudTextColor;
    float x = at.x;
    dl->AddText(ImVec2(x, at.y), col, tag);
    x += ImGui::CalcTextSize(tag).x;
    static const ImGuiID kIds[4] = {ImHashStr("##gzReadX"), ImHashStr("##gzReadY"), ImHashStr("##gzReadZ"), ImHashStr("##gzReadL")};
    for (int a = 0; a < 3; ++a) {
        x += gap;
        x += motion.Odometer(kIds[a], dl, ImVec2(x, at.y), v[a], decimals, col, a == 2 ? suffix : nullptr);
    }
    if (showLength) {
        dl->AddText(ImVec2(x, at.y), col, "  (|");
        x += ImGui::CalcTextSize("  (|").x;
        x += motion.Odometer(kIds[3], dl, ImVec2(x, at.y), glm::length(v), decimals, col);
        dl->AddText(ImVec2(x, at.y), col, "|)");
    }
}

// Ray from a screen pixel into the scene; returns the nearest renderable-AABB hit, or a point
// ~20 units out along the ray when nothing is hit (so the ruler still gets a usable endpoint
// over empty space). Shared by the Measure tool (#236 R2).
bool EditorLayer::RaycastViewportSurface(World& world, Camera& cam, const glm::vec2& screenPx,
                                         glm::vec3& outHit) const {
    const float w = m_ViewportSize.x, h = m_ViewportSize.y;
    if (w <= 1.0f || h <= 1.0f) return false;
    const glm::mat4 invVP = glm::inverse(cam.ProjectionMatrix(w / h) * cam.ViewMatrix());
    const float ndcX = (2.0f * (screenPx.x - m_ViewportPos.x)) / w - 1.0f;
    const float ndcY = 1.0f - (2.0f * (screenPx.y - m_ViewportPos.y)) / h;
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farP  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);
    nearP /= nearP.w; farP /= farP.w;
    const glm::vec3 origin = glm::vec3(nearP);
    const glm::vec3 dir = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));

    // #116 — measure against real surfaces, not bounding boxes.
    const SurfaceHit surf = RaycastRenderables(world, origin, dir, [](entt::entity) { return false; });
    outHit = origin + dir * (surf.Entity != entt::null ? surf.T : 20.0f);
    return true;
}

// EditorSettings::MeasureFeet ? feet/inches : metric — the scene's native unit is always meters, so this
// only affects display formatting, never the stored point data. Shared with vRuler through
// Enhancers::FormatLength (adaptive mm/cm/m/km; ft' in" / mi).
static void FormatMeasureLength(float meters, bool feet, char* buf, size_t n) {
    Enhancers::FormatLength(meters, feet, buf, n);
}

namespace {
// vRuler / Measure drawing: theme colours, Px-scaled lines over a dark halo so they read on a bright
// sky, dashes anchored to the segment start (they don't crawl as it moves), points as a dot with a
// ring, and labels on HUD plates that stay inside the viewport and step aside instead of overlapping.
constexpr int kBoxEdges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};

ImU32 RulerAlpha(ImU32 c, float a) {
    const float ca = (float)((c >> IM_COL32_A_SHIFT) & 0xFF) * a;
    return (c & ~IM_COL32_A_MASK) | ((ImU32)ca << IM_COL32_A_SHIFT);
}
ImU32 RulerHalo(ImU32 c) { return RulerAlpha(IM_COL32(0, 0, 0, 160), (float)((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f); }

void RulerLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thick) {
    dl->AddLine(a, b, RulerHalo(col), thick + EditorTheme::Px(2.5f));
    dl->AddLine(a, b, col, thick);
}

void RulerDashes(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thick) {
    const ImVec2 d(b.x - a.x, b.y - a.y);
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1.0f) return;
    const ImVec2 u(d.x / len, d.y / len);
    const float dash = EditorTheme::Px(6.0f), step = dash + EditorTheme::Px(4.0f);
    for (int pass = 0; pass < 2; ++pass) // every halo first, so no halo covers a dash
        for (float t = 0.0f; t < len; t += step) {
            const float t2 = std::min(t + dash, len);
            dl->AddLine(ImVec2(a.x + u.x * t, a.y + u.y * t), ImVec2(a.x + u.x * t2, a.y + u.y * t2),
                        pass == 0 ? RulerHalo(col) : col, pass == 0 ? thick + EditorTheme::Px(2.5f) : thick);
        }
}

// A measured segment: a 2px line (or dashes), with end ticks when it is a real measurement.
void RulerSegment(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, bool dashed, bool ticks) {
    const ImVec2 d(b.x - a.x, b.y - a.y);
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1.0f) return;
    const float thick = EditorTheme::Px(2.0f);
    if (dashed) RulerDashes(dl, a, b, col, thick); else RulerLine(dl, a, b, col, thick);
    if (!ticks) return;
    const ImVec2 n(-d.y / len * EditorTheme::Px(5.0f), d.x / len * EditorTheme::Px(5.0f));
    RulerLine(dl, ImVec2(a.x - n.x, a.y - n.y), ImVec2(a.x + n.x, a.y + n.y), col, thick);
    RulerLine(dl, ImVec2(b.x - n.x, b.y - n.y), ImVec2(b.x + n.x, b.y + n.y), col, thick);
}

void RulerPoint(ImDrawList* dl, ImVec2 p, ImU32 col) {
    const float r = EditorTheme::Px(3.5f), ring = EditorTheme::Px(7.5f);
    dl->AddCircle(p, ring, RulerHalo(col), 0, EditorTheme::Px(3.5f));
    dl->AddCircle(p, ring, RulerAlpha(col, 0.7f), 0, EditorTheme::Px(1.5f));
    dl->AddCircleFilled(p, r + EditorTheme::Px(1.5f), RulerHalo(col));
    dl->AddCircleFilled(p, r, col);
}

// Labels placed one after another: each is clamped into the viewport and, when it would overlap
// one already placed, steps along `push` (away from its line) until it is clear.
struct RulerLabels {
    ImVec2 ClipMin, ClipMax;
    std::vector<std::pair<ImVec2, ImVec2>> Placed;
    void Draw(ImDrawList* dl, ImVec2 at, ImVec2 push, const char* text, ImU32 col) {
        EditorTheme::PushMonoSmall();
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        const ImVec2 ts = ImGui::CalcTextSize(text);
        ImGui::PopFont();
        const float px = EditorTheme::Px(6.0f), py = EditorTheme::Px(3.0f), margin = EditorTheme::Px(4.0f);
        const ImVec2 half(std::floor(ts.x * 0.5f + px), std::floor(ts.y * 0.5f + py));
        const float pl = std::sqrt(push.x * push.x + push.y * push.y);
        push = pl < 1e-3f ? ImVec2(0.0f, 1.0f) : ImVec2(push.x / pl, push.y / pl);
        const float stepLen = half.y * 2.0f + EditorTheme::Px(2.0f);
        ImVec2 c = at;
        for (int tries = 0; tries < 6; ++tries) {
            c.x = std::max(ClipMin.x + half.x + margin, std::min(c.x, ClipMax.x - half.x - margin));
            c.y = std::max(ClipMin.y + half.y + margin, std::min(c.y, ClipMax.y - half.y - margin));
            bool overlaps = false;
            for (const auto& r : Placed)
                if (c.x - half.x < r.second.x && c.x + half.x > r.first.x && c.y - half.y < r.second.y && c.y + half.y > r.first.y) { overlaps = true; break; }
            if (!overlaps) break;
            c.x += push.x * stepLen; c.y += push.y * stepLen;
        }
        const ImVec2 mn(std::floor(c.x - half.x), std::floor(c.y - half.y)), mx(mn.x + half.x * 2.0f, mn.y + half.y * 2.0f);
        Placed.push_back({mn, mx});
        EditorUIPrimitives::DrawHudPlate(dl, mn, mx, EditorTheme::Px(4.0f));
        dl->AddRect(mn, mx, RulerAlpha(col, 0.35f), EditorTheme::Px(4.0f), 0, EditorTheme::Px(1.0f));
        dl->AddText(font, fs, ImVec2(mn.x + px, mn.y + py), col, text);
    }
};
} // namespace

void EditorLayer::DrawMeasurement(Camera& cam) {
    if (m_MeasurePoints.empty()) return;
    const float w = m_ViewportSize.x, h = m_ViewportSize.y;
    if (w <= 1.0f || h <= 1.0f) return;
    const glm::mat4 vp = cam.ProjectionMatrix(w / h) * cam.ViewMatrix();
    auto toScreen = [&](const glm::vec3& p, ImVec2& out) -> bool {
        glm::vec4 c = vp * glm::vec4(p, 1.0f);
        if (c.w <= 1e-4f) return false;
        glm::vec3 ndc = glm::vec3(c) / c.w;
        out = ImVec2(m_ViewportPos.x + (ndc.x * 0.5f + 0.5f) * w,
                     m_ViewportPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
        return true;
    };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 col = EditorTheme::U32(EditorTheme::Info);
    RulerLabels labels{ImVec2(m_ViewportPos.x, m_ViewportPos.y), ImVec2(m_ViewportPos.x + w, m_ViewportPos.y + h), {}};

    // Phase 3 item 7 — chained segments: every consecutive pair of points is its own dashed leg
    // with its own length label, and the total across the whole chain feeds the HUD below.
    std::vector<ImVec2> screenPts(m_MeasurePoints.size());
    std::vector<bool> valid(m_MeasurePoints.size());
    for (size_t i = 0; i < m_MeasurePoints.size(); ++i) valid[i] = toScreen(m_MeasurePoints[i], screenPts[i]);

    float totalMeters = 0.0f;
    for (size_t i = 0; i + 1 < m_MeasurePoints.size(); ++i) {
        totalMeters += glm::length(m_MeasurePoints[i + 1] - m_MeasurePoints[i]);
        if (valid[i] && valid[i + 1]) RulerSegment(dl, screenPts[i], screenPts[i + 1], col, true, false);
    }
    for (size_t i = 0; i < m_MeasurePoints.size(); ++i)
        if (valid[i]) RulerPoint(dl, screenPts[i], col);
    // Labels last, over every line: above their leg, hidden on legs too short to carry one.
    for (size_t i = 0; i + 1 < m_MeasurePoints.size(); ++i) {
        if (!valid[i] || !valid[i + 1]) continue;
        const ImVec2 s0 = screenPts[i], s1 = screenPts[i + 1];
        const ImVec2 d(s1.x - s0.x, s1.y - s0.y);
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < EditorTheme::Px(24.0f)) continue;
        ImVec2 n(-d.y / len, d.x / len);
        if (n.y > 0.0f) n = ImVec2(-n.x, -n.y);
        char segBuf[32];
        FormatMeasureLength(glm::length(m_MeasurePoints[i + 1] - m_MeasurePoints[i]), EditorSettings::Get().MeasureFeet, segBuf, sizeof(segBuf));
        const ImVec2 mid((s0.x + s1.x) * 0.5f, (s0.y + s1.y) * 0.5f);
        labels.Draw(dl, ImVec2(mid.x + n.x * EditorTheme::Px(16.0f), mid.y + n.y * EditorTheme::Px(16.0f)), n, segBuf, col);
    }

    // Persistent HUD (audit's "Clear/Copy/unit toggle") — a real ImGui window, not just draw-list
    // text, so it can host buttons. Shown whenever there's at least one point, same as the chain
    // itself (m_MeasureTool || !m_MeasurePoints.empty() at the call site), so the reading stays
    // on screen after switching to another tool, until explicitly cleared.
    {
        const float barBottomMargin = 14.0f * m_UIScale; // above the viewport's bottom edge
        ImGui::SetNextWindowPos(ImVec2(m_ViewportPos.x + m_ViewportSize.x * 0.5f,
                                       m_ViewportPos.y + m_ViewportSize.y - barBottomMargin),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorUIPrimitives::kHudPlateColor);
        ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::kHudTextColor);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * m_UIScale, 6.0f * m_UIScale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * m_UIScale);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::Begin("##MeasureHud", nullptr, flags);

        char totalBuf[48];
        FormatMeasureLength(totalMeters, EditorSettings::Get().MeasureFeet, totalBuf, sizeof(totalBuf));
        EditorTheme::PushMono();
        if (m_MeasurePoints.size() > 2) {
            ImGui::Text(ICON_FA_RULER "  Total: %s  (%d segments)", totalBuf, (int)m_MeasurePoints.size() - 1);
        } else {
            ImGui::Text(ICON_FA_RULER "  %s", totalBuf);
        }
        ImGui::PopFont();
        ImGui::SameLine();

        if (ActionButton(ICON_FA_COPY, "Copy the total distance") && !m_MeasurePoints.empty()) {
            ImGui::SetClipboardText(totalBuf);
        }
        ImGui::SameLine();
        if (ActionButton(EditorSettings::Get().MeasureFeet ? "ft" : "m", "Toggle meters / feet (the Shift+R ruler uses the same units)")) {
            EditorSettings::Get().MeasureFeet = !EditorSettings::Get().MeasureFeet;
            EditorSettings::Save();
        }
        ImGui::SameLine();
        bool clearClicked = false;
        {
            EditorSoundEvents::ScopedClickSilence silence; // MeasureClear instead of the generic click
            clearClicked = ActionButton(ICON_FA_XMARK, "Clear (right-click in the viewport does the same)");
        }
        if (clearClicked) {
            EditorSound::Play(m_MeasurePoints.empty() ? EditorSound::Event::ClickCancel : EditorSound::Event::MeasureClear,
                              EditorSound::LastItemScreenX());
            m_MeasurePoints.clear();
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }
}

// --- Editor Enhancers / vRuler ------------------------------------------------------------
// Hold Shift+R over the Scene view. The surface under the cursor is measured along its normal to
// the next surface (and through its own object to the far side); the wheel steps to the next
// object behind along the ray; a click shows that object's bounds. Shares the Measure tool's
// ray, surface query and units (EditorSettings::MeasureFeet, Enhancers::FormatLength).
bool EditorLayer::RulerHeldNow() const {
    return EditorSettings::Get().Ruler && Shortcuts::Held("viewport.ruler");
}

bool EditorLayer::ViewportRay(Camera& cam, const glm::vec2& screenPx, glm::vec3& origin, glm::vec3& dir) const {
    const float w = m_ViewportSize.x, h = m_ViewportSize.y;
    if (w <= 1.0f || h <= 1.0f) return false;
    const glm::mat4 invVP = glm::inverse(cam.ProjectionMatrix(w / h) * cam.ViewMatrix());
    const float ndcX = (2.0f * (screenPx.x - m_ViewportPos.x)) / w - 1.0f;
    const float ndcY = 1.0f - (2.0f * (screenPx.y - m_ViewportPos.y)) / h;
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farP  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);
    nearP /= nearP.w; farP /= farP.w;
    origin = glm::vec3(nearP);
    dir = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));
    return true;
}

namespace {
constexpr int kRulerMaxDepth = 8;      // objects collected along the cursor ray
constexpr float kRulerReach = 10000.0f; // farthest surface the normal probe looks for

// World-space bounds of a renderable (model bounds through its world transform); false if none.
bool RenderableWorldBounds(const World& world, entt::entity e, AABB& out) {
    const auto* r = world.Registry.valid(e) ? world.Registry.try_get<RenderableComponent>(e) : nullptr;
    if (!r || !r->ModelRef || r->ModelRef->MeshCount() == 0) return false;
    out = AABB{r->ModelRef->BoundsMin(), r->ModelRef->BoundsMax()}.Transformed(world.ComposeWorldTransform(e));
    return true;
}
} // namespace

void EditorLayer::DrawRuler(World& world, Camera& cam) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool overViewport = mouse.x >= m_ViewportPos.x && mouse.x < m_ViewportPos.x + m_ViewportSize.x &&
                              mouse.y >= m_ViewportPos.y && mouse.y < m_ViewportPos.y + m_ViewportSize.y;
    m_RulerHeld = overViewport && RulerHeldNow();
    if (!m_RulerHeld) { m_RulerDepth = 0; m_RulerBounds = entt::null; m_RulerWheel = 0.0f; m_RulerHeldSince = -1.0; m_RulerOutlineValid = false; return; }
    const bool feet = EditorSettings::Get().MeasureFeet;

    glm::vec3 origin, dir;
    if (!ViewportRay(cam, {mouse.x, mouse.y}, origin, dir)) return;
    const float w = m_ViewportSize.x, h = m_ViewportSize.y;
    const glm::mat4 vp = cam.ProjectionMatrix(w / h) * cam.ViewMatrix();
    auto toScreen = [&](const glm::vec3& p, ImVec2& out) -> bool {
        const glm::vec4 c = vp * glm::vec4(p, 1.0f);
        if (c.w <= 1e-4f) return false;
        const glm::vec3 ndc = glm::vec3(c) / c.w;
        out = ImVec2(m_ViewportPos.x + (ndc.x * 0.5f + 0.5f) * w, m_ViewportPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
        return true;
    };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(ImVec2(m_ViewportPos.x, m_ViewportPos.y), ImVec2(m_ViewportPos.x + w, m_ViewportPos.y + h), true);
    const ImU32 info = EditorTheme::U32(EditorTheme::Info), warn = EditorTheme::U32(EditorTheme::Warning);
    RulerLabels labels{ImVec2(m_ViewportPos.x, m_ViewportPos.y), ImVec2(m_ViewportPos.x + w, m_ViewportPos.y + h), {}};
    struct PendingLabel { ImVec2 At, Push; std::string Text; ImU32 Col; };
    std::vector<PendingLabel> pending; // drawn last, over every line
    // A measured segment; its label sits off to one side (away from `awayFrom` when given) and is
    // hidden when the segment is too short on screen to carry one.
    auto segment = [&](const glm::vec3& a, const glm::vec3& b, ImU32 col, bool dashed, bool ticks, const char* text, const ImVec2* awayFrom) {
        ImVec2 sa, sb;
        if (!toScreen(a, sa) || !toScreen(b, sb)) return;
        RulerSegment(dl, sa, sb, col, dashed, ticks);
        const ImVec2 d(sb.x - sa.x, sb.y - sa.y);
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (!text || len < EditorTheme::Px(24.0f)) return;
        ImVec2 n(-d.y / len, d.x / len);
        const ImVec2 mid((sa.x + sb.x) * 0.5f, (sa.y + sb.y) * 0.5f);
        if (awayFrom ? n.x * (mid.x - awayFrom->x) + n.y * (mid.y - awayFrom->y) < 0.0f : n.x < 0.0f) n = ImVec2(-n.x, -n.y);
        pending.push_back({ImVec2(mid.x + n.x * EditorTheme::Px(16.0f), mid.y + n.y * EditorTheme::Px(16.0f)), n, text, col});
    };
    // A box: the three edges meeting at the corner farthest from the camera are behind it, so they
    // are dashed and dim.
    auto box = [&](const glm::vec3& mn, const glm::vec3& mx, ImU32 col, float thick) {
        const glm::vec3 c[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
                                {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
        int farIdx = 0;
        float farD = -1.0f;
        for (int k = 0; k < 8; ++k) {
            const glm::vec3 off = c[k] - cam.Position;
            const float dd = glm::dot(off, off);
            if (dd > farD) { farD = dd; farIdx = k; }
        }
        for (const auto& e : kBoxEdges) {
            ImVec2 a, bb;
            if (!toScreen(c[e[0]], a) || !toScreen(c[e[1]], bb)) continue;
            if (e[0] == farIdx || e[1] == farIdx) RulerDashes(dl, a, bb, RulerAlpha(col, 0.4f), thick);
            else RulerLine(dl, a, bb, col, thick);
        }
    };

    // Every object along the ray, nearest first.
    SurfaceHit hits[kRulerMaxDepth];
    int count = 0;
    while (count < kRulerMaxDepth) {
        const SurfaceHit hit = RaycastRenderables(world, origin, dir, [&](entt::entity e) {
            for (int i = 0; i < count; ++i) if (hits[i].Entity == e) return true;
            return false;
        });
        if (hit.Entity == entt::null) break;
        hits[count++] = hit;
    }

    // The wheel picks which of them is measured (one notch, one object).
    m_RulerWheel += io.MouseWheel;
    if (count > 0) {
        if (m_RulerWheel <= -1.0f) { m_RulerDepth = (m_RulerDepth + 1) % count; m_RulerWheel = 0.0f; }
        else if (m_RulerWheel >= 1.0f) { m_RulerDepth = (m_RulerDepth + count - 1) % count; m_RulerWheel = 0.0f; }
    }
    m_RulerDepth = count > 0 ? std::clamp(m_RulerDepth, 0, count - 1) : 0;
    char buf[48], buf2[48];
    std::string objName, primary, thick;
    bool open = false;

    if (count > 0) {
        const SurfaceHit& hit = hits[m_RulerDepth];
        const glm::vec3 p = origin + dir * hit.T;
        glm::vec3 n = glm::length(hit.Normal) > 1e-6f ? glm::normalize(hit.Normal) : glm::vec3(0, 1, 0);
        if (glm::dot(n, dir) > 0.0f) n = -n; // face the camera
        const float eps = 1e-3f;
        // The measured object, outlined faintly when there is a choice (wheel to change it); the
        // outline eases from the last object to the next.
        AABB ob;
        if (count > 1 && RenderableWorldBounds(world, hit.Entity, ob)) {
            const float k = m_RulerOutlineValid ? 1.0f - std::exp(-io.DeltaTime * 18.0f) : 1.0f;
            m_RulerOutlineMin = glm::mix(m_RulerOutlineMin, ob.Min, k);
            m_RulerOutlineMax = glm::mix(m_RulerOutlineMax, ob.Max, k);
            m_RulerOutlineValid = true;
            box(m_RulerOutlineMin, m_RulerOutlineMax, RulerAlpha(info, 0.45f), EditorTheme::Px(1.0f));
        } else {
            m_RulerOutlineValid = false;
        }
        // Outward: this surface to the next one along its normal (floor -> ceiling, wall -> wall).
        const SurfaceHit out = RaycastRenderables(world, p + n * eps, n, [](entt::entity) { return false; }, kRulerReach);
        // Inward: through this object to its far side (its thickness here).
        const SurfaceHit in = RaycastRenderables(world, p - n * eps, -n, [&](entt::entity e) { return e != hit.Entity; }, kRulerReach);
        if (out.Entity != entt::null) {
            Enhancers::FormatLength(out.T + eps, feet, buf, sizeof(buf));
            segment(p, p + n * (out.T + eps), info, false, true, buf, nullptr);
            primary = buf;
        } else {
            segment(p, p + n * 0.5f, RulerAlpha(info, 0.45f), true, false, nullptr, nullptr);
            primary = "open";
            open = true;
        }
        if (in.Entity != entt::null) {
            Enhancers::FormatLength(in.T + eps, feet, buf2, sizeof(buf2));
            segment(p, p - n * (in.T + eps), warn, true, true, buf2, nullptr);
            thick = buf2;
        }
        ImVec2 sp;
        if (toScreen(p, sp)) RulerPoint(dl, sp, info);
        const auto* nm = world.Registry.try_get<NameComponent>(hit.Entity);
        objName = nm && !nm->Name.empty() ? nm->Name : "object";
    } else {
        m_RulerOutlineValid = false;
    }

    // Clicked object's bounds: its three sizes on three different edges, so the labels don't pile
    // onto one corner: width along the front bottom edge, height up its far end, depth back along
    // the near side. Labels sit outside the box.
    AABB b;
    if (m_RulerBounds != entt::null && RenderableWorldBounds(world, m_RulerBounds, b)) {
        const glm::vec3 size = b.Max - b.Min;
        const glm::vec3 center = (b.Min + b.Max) * 0.5f;
        const float x0 = cam.Position.x > center.x ? b.Max.x : b.Min.x;
        const float z0 = cam.Position.z > center.z ? b.Max.z : b.Min.z;
        const float x1 = x0 == b.Max.x ? b.Min.x : b.Max.x;
        const float z1 = z0 == b.Max.z ? b.Min.z : b.Max.z;
        box(b.Min, b.Max, warn, EditorTheme::Px(1.5f));
        ImVec2 cs;
        const ImVec2* away = toScreen(center, cs) ? &cs : nullptr;
        Enhancers::FormatLength(size.x, feet, buf, sizeof(buf));
        segment(glm::vec3(x1, b.Min.y, z0), glm::vec3(x0, b.Min.y, z0), warn, false, true, buf, away);
        Enhancers::FormatLength(size.y, feet, buf, sizeof(buf));
        segment(glm::vec3(x1, b.Min.y, z0), glm::vec3(x1, b.Max.y, z0), warn, false, true, buf, away);
        Enhancers::FormatLength(size.z, feet, buf, sizeof(buf));
        segment(glm::vec3(x0, b.Min.y, z0), glm::vec3(x0, b.Min.y, z1), warn, false, true, buf, away);
    } else {
        m_RulerBounds = entt::null;
    }
    for (const PendingLabel& l : pending) labels.Draw(dl, l.At, l.Push, l.Text.c_str(), l.Col);
    dl->PopClipRect();

    // Read-out card by the cursor: the object, the main distance large and mono, one secondary
    // line; the usage hint only for the first two seconds of a hold.
    const double now = ImGui::GetTime();
    if (m_RulerHeldSince < 0.0) m_RulerHeldSince = now;
    const bool showHint = now - m_RulerHeldSince < 2.0;
    std::string title = count > 0 ? objName : std::string("No surface under the cursor");
    if (count > 1) title += "   " + std::to_string(m_RulerDepth + 1) + "/" + std::to_string(count);
    std::string second = thick.empty() ? std::string() : "thick " + thick;
    if (count > 1) second += std::string(second.empty() ? "" : "   ") + "wheel: next object";
    const std::string hint = std::string("Click: object size   ") + (feet ? "feet / inches" : "metric");
    const float pad = EditorTheme::Px(10.0f), gap = EditorTheme::Px(3.0f);
    const ImVec2 titleSz = ImGui::CalcTextSize(title.c_str());
    ImVec2 primSz(0.0f, 0.0f), secSz(0.0f, 0.0f), hintSz(0.0f, 0.0f);
    if (count > 0) { ImGui::PushFont(EditorTheme::MonoFont(), EditorTheme::Px(20.0f)); primSz = ImGui::CalcTextSize(primary.c_str()); ImGui::PopFont(); }
    if (!second.empty()) { EditorTheme::PushMonoSmall(); secSz = ImGui::CalcTextSize(second.c_str()); ImGui::PopFont(); }
    if (showHint) hintSz = ImGui::CalcTextSize(hint.c_str());
    const float cw = std::floor(std::max(std::max(titleSz.x, primSz.x), std::max(secSz.x, hintSz.x)) + pad * 2.0f);
    const float ch = std::floor(pad * 2.0f + titleSz.y + (primSz.y > 0.0f ? gap + primSz.y : 0.0f) +
                                (secSz.y > 0.0f ? gap + secSz.y : 0.0f) + (hintSz.y > 0.0f ? gap * 2.0f + hintSz.y : 0.0f));
    ImVec2 cp(mouse.x + EditorTheme::Px(18.0f), mouse.y + EditorTheme::Px(18.0f));
    cp.x = std::floor(std::max(m_ViewportPos.x + EditorTheme::Px(8.0f), std::min(cp.x, m_ViewportPos.x + w - cw - EditorTheme::Px(8.0f))));
    cp.y = std::floor(std::max(m_ViewportPos.y + EditorTheme::Px(8.0f), std::min(cp.y, m_ViewportPos.y + h - ch - EditorTheme::Px(8.0f))));
    const ImVec2 cmx(cp.x + cw, cp.y + ch);
    EditorUIPrimitives::DrawSoftShadow(dl, cp, cmx, EditorTheme::Px(6.0f), EditorTheme::Px(8.0f), 0.5f);
    EditorUIPrimitives::DrawHudPlate(dl, cp, cmx, EditorTheme::Px(6.0f));
    dl->AddRect(cp, cmx, EditorTheme::U32(EditorTheme::Hairline), EditorTheme::Px(6.0f), 0, EditorTheme::Px(1.0f));
    float y = cp.y + pad;
    dl->AddText(ImVec2(cp.x + pad, y), EditorTheme::U32(EditorTheme::Secondary), title.c_str());
    y += titleSz.y;
    if (primSz.y > 0.0f) {
        y += gap;
        ImGui::PushFont(EditorTheme::MonoFont(), EditorTheme::Px(20.0f));
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(cp.x + pad, y), open ? EditorTheme::U32(EditorTheme::Dim) : info, primary.c_str());
        ImGui::PopFont();
        y += primSz.y;
    }
    if (secSz.y > 0.0f) {
        y += gap;
        EditorTheme::PushMonoSmall();
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(cp.x + pad, y), thick.empty() ? EditorTheme::U32(EditorTheme::Dim) : warn, second.c_str());
        ImGui::PopFont();
        y += secSz.y;
    }
    if (hintSz.y > 0.0f) dl->AddText(ImVec2(cp.x + pad, y + gap * 2.0f), EditorTheme::U32(EditorTheme::Dim), hint.c_str());

    m_RulerHitEntity = count > 0 ? hits[m_RulerDepth].Entity : entt::null;
}

void EditorLayer::ApplySurfaceSnap(World& world, Camera& editorCamera, entt::entity sel,
                                   const glm::mat4& parentWorld) {
    const float w = m_ViewportSize.x, h = m_ViewportSize.y;
    if (w <= 1.0f || h <= 1.0f || !world.Registry.valid(sel)) return;

    const glm::mat4 view = editorCamera.ViewMatrix();
    const glm::mat4 proj = editorCamera.ProjectionMatrix(w / h);
    const glm::mat4 invVP = glm::inverse(proj * view);
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float ndcX = (2.0f * (m.x - m_ViewportPos.x)) / w - 1.0f;
    const float ndcY = 1.0f - (2.0f * (m.y - m_ViewportPos.y)) / h;
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farP  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);
    nearP /= nearP.w; farP /= farP.w;
    const glm::vec3 origin = glm::vec3(nearP);
    const glm::vec3 dir = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));

    // Never snap to the moving object itself or anything parented under it.
    auto isSelfOrChild = [&](entt::entity e) {
        for (entt::entity c = e; c != entt::null; ) {
            if (c == sel) return true;
            const auto* hp = world.Registry.try_get<HierarchyComponent>(c);
            c = hp ? hp->Parent : entt::null;
        }
        return false;
    };

    // #116 — the real surface under the cursor and its real face normal (was the hit object's
    // AABB and the nearest AABB face, so "align to normal" on a slope aligned to a box face).
    const SurfaceHit hit = RaycastRenderables(world, origin, dir, isSelfOrChild);
    if (hit.Entity == entt::null) return;
    const glm::vec3 p = origin + dir * hit.T;
    const glm::vec3 n = hit.Normal;

    // Lift the object so its footprint rests on the surface rather than its origin sinking to it.
    glm::vec3 dropWorld = p;
    const glm::mat4 selWorld = world.ComposeWorldTransform(sel);
    if (const auto* selR = world.Registry.try_get<RenderableComponent>(sel)) {
        const AABB sw = AABB{selR->ModelRef->BoundsMin(), selR->ModelRef->BoundsMax()}.Transformed(selWorld);
        const glm::vec3 selCtr = (sw.Min + sw.Max) * 0.5f;
        const glm::vec3 selOrigin = glm::vec3(selWorld[3]);
        const float halfAlong = 0.5f * std::abs(glm::dot(sw.Max - sw.Min, glm::abs(n)));
        const float originToCtr = glm::dot(selCtr - selOrigin, n);
        dropWorld = p + n * (halfAlong - originToCtr);
    }

    auto& transform = world.Registry.get<TransformComponent>(sel);
    transform.Position = glm::vec3(glm::inverse(parentWorld) * glm::vec4(dropWorld, 1.0f));

    if (m_SurfaceSnapAlign) {
        glm::mat4 rot(1.0f);
        if (world.Registry.all_of<DecalComponent>(sel)) {
            rot = glm::mat4(DecalSurfaceBasis(n, editorCamera.Right())); // a decal faces out of the surface (+Z)
        } else {
            const glm::vec3 up = n;
            const glm::vec3 ref = (std::abs(up.y) > 0.99f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 fwd = glm::normalize(glm::cross(ref, up));
            const glm::vec3 right = glm::cross(up, fwd);
            rot[0] = glm::vec4(right, 0.0f);
            rot[1] = glm::vec4(up, 0.0f);
            rot[2] = glm::vec4(fwd, 0.0f);
        }
        transform.SetRotationQuaternion(QuaternionFromMatrix(glm::inverse(parentWorld) * rot));
    }
}

// --- Decals (docs/DECALS.md) -------------------------------------------------------------------
EditorLayer::DecalSpot EditorLayer::DecalSpotUnderMouse(World& world, Camera& editorCamera) const {
    DecalSpot spot;
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return spot;
    const glm::mat4 invVP = glm::inverse(editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) *
                                         editorCamera.ViewMatrix());
    const ImVec2 mouse = ImGui::GetMousePos();
    const float ndcX = (2.0f * (mouse.x - m_ViewportPos.x)) / m_ViewportSize.x - 1.0f;
    const float ndcY = 1.0f - (2.0f * (mouse.y - m_ViewportPos.y)) / m_ViewportSize.y;
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farP = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;
    const glm::vec3 origin(nearP);
    const glm::vec3 dir = glm::normalize(glm::vec3(farP) - origin);

    const SurfaceHit h = RaycastRenderables(world, origin, dir, [](entt::entity) { return false; }, 5000.0f);
    if (h.Entity != entt::null) {
        spot.OnSurface = true;
        spot.Receiver = h.Entity;
        spot.Point = origin + dir * h.T;
        spot.Normal = glm::dot(h.Normal, dir) > 0.0f ? -h.Normal : h.Normal; // the side facing the camera
    }
    // A placed decal's box straddles its surface, so the ray enters the box before the surface:
    // the nearest box entered no later than the surface hit is the decal under the cursor.
    float best = h.Entity != entt::null ? h.T : 1e30f;
    for (auto [e, d] : world.Registry.view<const DecalComponent>(entt::exclude<InactiveTag>).each()) {
        const float t = RayDecalBox(DecalBoxToWorld(world, e, d), origin, dir);
        if (t >= 0.0f && t <= best) { best = t; spot.Decal = e; }
    }
    return spot;
}

entt::entity EditorLayer::PlaceDecal(World& world, const std::string& path, const glm::vec3& point,
                                     const glm::vec3& normal, const glm::vec3& viewRight) {
    PushUndo(world, "Place Decal");
    // "DA_Tag_Crown" places as "Tag_Crown": the prefix names the file type, not the object.
    std::string name = std::filesystem::u8path(path).stem().u8string();
    if (name.rfind("DA_", 0) == 0 && name.size() > 3) name = name.substr(3);
    const entt::entity e = world.CreateEmptyEntity(point, glm::vec3(0.0f), glm::vec3(1.0f),
                                                   UniqueNameFor(world, name.empty() ? "Decal" : name));
    world.Registry.get<TransformComponent>(e).SetRotationQuaternion(
        QuaternionFromMatrix(glm::mat4(DecalSurfaceBasis(glm::normalize(normal), viewRight))));
    DecalComponent dc;
    dc.AssetPath = path;
    if (const auto asset = DecalAsset::Load(path); asset && !asset->Missing) dc.Size = asset->DefaultSize;
    // Every placement gets its own seed, so a run of the same decal doesn't repeat its jitter.
    static std::mt19937 rng{std::random_device{}()};
    dc.Seed = (int)(rng() & 0x7FFFFFFF);
    world.Registry.emplace<DecalComponent>(e, dc);
    SelectItem(e, false);
    return e;
}

bool EditorLayer::FitDecalToSurface(World& world, entt::entity entity) {
    auto* d = world.Registry.try_get<DecalComponent>(entity);
    if (!d || !world.Registry.all_of<TransformComponent>(entity)) return false;
    const glm::mat4 w = world.ComposeWorldTransform(entity);
    const glm::vec3 centre(w[3]);
    const glm::vec3 z = glm::normalize(glm::vec3(w[2])), x = glm::normalize(glm::vec3(w[0]));
    // From in front of the box along -Z (the projection direction), far enough to find the
    // surface whether the decal floats off it or has sunk through it.
    const float reach = std::max(2.0f, d->Size.z * 4.0f);
    const glm::vec3 origin = centre + z * (reach * 0.5f);
    const SurfaceHit h = RaycastRenderables(world, origin, -z, [](entt::entity) { return false; }, reach);
    if (h.Entity == entt::null) return false;
    const glm::vec3 n = glm::dot(h.Normal, z) < 0.0f ? -h.Normal : h.Normal; // out of the surface, toward +Z

    entt::entity parent = entt::null;
    if (const auto* hier = world.Registry.try_get<HierarchyComponent>(entity)) parent = hier->Parent;
    const glm::mat4 parentInv = glm::inverse(parent != entt::null ? world.ComposeWorldTransform(parent) : glm::mat4(1.0f));
    PushUndo(world, "Fit Decal to Surface");
    auto& t = world.Registry.get<TransformComponent>(entity);
    t.Position = glm::vec3(parentInv * glm::vec4(origin - z * h.T, 1.0f));
    t.SetRotationQuaternion(QuaternionFromMatrix(parentInv * glm::mat4(DecalSurfaceBasis(n, x))));
    return true;
}

bool EditorLayer::CastDecalRay(World& world, const glm::vec3& origin, const glm::vec3& dir, float maxT, glm::vec3& point,
                               glm::vec3& normal, entt::entity* entity) const {
    const SurfaceHit h = RaycastRenderables(world, origin, dir, [](entt::entity) { return false; }, maxT);
    if (h.Entity == entt::null) return false;
    point = origin + dir * h.T;
    normal = glm::dot(h.Normal, dir) > 0.0f ? -h.Normal : h.Normal; // the side facing the ray
    if (entity) *entity = h.Entity;
    return true;
}

bool EditorLayer::IsDynamicDecalReceiver(World& world, entt::entity entity) const {
    if (entity == entt::null || !world.Registry.valid(entity) || world.Registry.all_of<StaticTag>(entity)) return false;
    const auto* r = world.Registry.try_get<RenderableComponent>(entity);
    return (r && r->ModelRef && r->ModelRef->HasBones()) || world.Registry.all_of<RigidbodyComponent>(entity);
}

void EditorLayer::DrawViewGizmo(World& world, Camera& editorCamera) {
    m_ViewGizmoBlocking = false;
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    // The library's own defaults (256px rotate ring, 50px tool buttons) are sized for a full
    // editor viewport; scaled down to sit in the corner instead of taking it over. Phase 3 item
    // 6 (audit #5) — was halved (0.5) to ~64px radius, which the audit called out as too small
    // alongside the unlabelled negative axes fixed in ImViewGuizmo.h; 0.7 (~90px) keeps it a
    // corner overlay, not a takeover, while actually being readable.
    ImViewGuizmo::Style& style = ImViewGuizmo::GetStyle();
    style.scale = m_UIScale * 0.7f;
    style.axisColors[0] = EditorTheme::U32(EditorTheme::AxisX);
    style.axisColors[1] = EditorTheme::U32(EditorTheme::AxisY);
    style.axisColors[2] = EditorTheme::U32(EditorTheme::AxisZ);
    style.highlightColor = EditorTheme::U32(EditorTheme::Accent);

    float gizmoRadius = 128.0f * style.scale; // half of the library's fixed 256px rotate-ring box
    float margin = 14.0f * m_UIScale;

    ImVec2 rotatePos(m_ViewportPos.x + m_ViewportSize.x - margin - gizmoRadius,
        m_ViewportPos.y + margin + gizmoRadius);

    // Append to Scene itself: docked tabs and floating panels occlude this layer naturally.
    if (!ImGui::Begin(EditorPanels::Scene)) {
        m_ViewGizmoBlocking = false;
        ImGui::End();
        UpdateRadialMenu(world, editorCamera, false); // hidden Scene: the menu closes
        return;
    }
    glm::vec3 camPos = editorCamera.Position;
    glm::quat camRot = glm::quatLookAt(editorCamera.Front(), glm::vec3(0.0f, 1.0f, 0.0f));
    // Orbit pivot for the Rotate ring: the current selection's bounds center when there is one,
    // so dragging to orbit or clicking an axis handle to snap keeps the selected object(s)
    // centered instead of spinning around an arbitrary point. Falls back to a fixed distance in
    // front of the camera (the old behavior) when nothing's selected — the editor camera is a
    // plain fly-camera with no scene pivot of its own otherwise.
    glm::vec3 pivot;
    glm::vec3 selBoundsMin, selBoundsMax;
    if (ComputeSelectionBounds(world, selBoundsMin, selBoundsMax)) {
        pivot = (selBoundsMin + selBoundsMax) * 0.5f;
    } else {
        pivot = camPos + editorCamera.Front() * 6.0f;
    }

    // Motion v2: an axis click no longer runs the library's own snap tween (a fixed 0.5 s ease-out
    // quad on wall-clock time). The library snaps instantly, and that target becomes an orbiting
    // view transition on the ViewCubeTurn spring, so the cube swings into place and settles, the
    // speed preference applies, and Reduce Motion lands it at once.
    style.animateSnap = false;
    const bool wasOrbitDrag = ImViewGuizmo::GetContext().activeTool == ImViewGuizmo::TOOL_GIZMO;
    // Smoke-test screenshots leave the nav gizmo out (the radial below still runs).
    bool modified = !m_CleanShots && ImViewGuizmo::Rotate(camPos, camRot, pivot, rotatePos);
    if (modified && !wasOrbitDrag && ImViewGuizmo::GetContext().activeTool == ImViewGuizmo::TOOL_NONE) {
        const glm::vec3 forward = glm::normalize(camRot * glm::vec3(0.0f, 0.0f, -1.0f));
        SettleViewMorph(editorCamera);
        m_ViewTransition.Active = true;
        m_ViewTransition.T = 0.0f;
        m_ViewTransition.FromPos = editorCamera.Position;
        m_ViewTransition.FromYaw = editorCamera.Yaw;
        m_ViewTransition.FromPitch = editorCamera.Pitch;
        m_ViewTransition.FromOrthoHalfHeight = m_ViewTransition.ToOrthoHalfHeight = editorCamera.OrthoHalfHeight;
        m_ViewTransition.FromFov = m_ViewTransition.ToFov = editorCamera.Fov;
        m_ViewTransition.ToPos = camPos;
        m_ViewTransition.ToPitch = glm::clamp(glm::degrees(asinf(glm::clamp(forward.y, -1.0f, 1.0f))), -89.0f, 89.0f);
        // Straight up / down has no heading of its own: keep the current one rather than spinning
        // to whatever the degenerate atan2 gives.
        m_ViewTransition.ToYaw = std::fabs(forward.y) > 0.999f ? editorCamera.Yaw : glm::degrees(atan2f(forward.z, forward.x));
        m_ViewOrbit = ViewOrbit{};
        if (Glass::Motion().CategoryOn(Glass::MotionCategory::Viewport)) {
            m_ViewOrbit.On = true;
            m_ViewOrbit.Stamp = m_ViewTransition.FromPos;
            m_ViewOrbit.Pivot = pivot;
            m_ViewOrbit.FromDist = glm::length(editorCamera.Position - pivot);
            m_ViewOrbit.ToDist = glm::length(camPos - pivot);
            m_ViewOrbit.Fov = editorCamera.Fov;
            m_ViewOrbit.Spring = true;
        }
        EditorSound::Play(EditorSound::Event::ViewPreset);
        modified = false; // the transition moves the camera from here
    } else if (modified && m_ViewTransition.Active) {
        SettleViewMorph(editorCamera); // dragging the ring takes the camera back
        m_ViewTransition.Active = false;
    }
    m_ViewGizmoBlocking = !m_CleanShots && (ImViewGuizmo::IsOver() || ImViewGuizmo::IsUsing());

    // Tooltips - the rotate ring gives no feedback on its own (it's manually hit-tested inside
    // the vendored gizmo library, not a real ImGui widget, so ImGui::IsItemHovered() can't see
    // it); read the library's own hover state instead. The Dolly/Pan tool buttons and the
    // Persp/Iso label that used to live below the ring were removed (redundant with the tool
    // palette's own draw-mode/ortho controls and the editor's existing zoom/pan camera input) —
    // only the axis-handle and orbit tooltips are left to read.
    const auto& gizmoCtx = ImViewGuizmo::GetContext();
    if (m_CleanShots) {
    } else if (gizmoCtx.hoveredAxisID == 6) {
        EditorUI::SetTooltip("Drag to orbit the view");
    } else if (gizmoCtx.hoveredAxisID >= 0 && gizmoCtx.hoveredAxisID <= 5) {
        static const char* kAxisTooltips[6] = {
            "Click to look along +X",
            "Click to look along -X",
            "Click to look straight down (+Y)",
            "Click to look straight up (-Y)",
            "Click to look along +Z",
            "Click to look along -Z",
        };
        EditorUI::SetTooltip("%s", kAxisTooltips[gizmoCtx.hoveredAxisID]);
    }

    if (modified) {
        glm::vec3 forward = glm::normalize(camRot * glm::vec3(0.0f, 0.0f, -1.0f));
        editorCamera.Pitch = glm::clamp(glm::degrees(asinf(glm::clamp(forward.y, -1.0f, 1.0f))), -89.0f, 89.0f);
        editorCamera.Yaw = glm::degrees(atan2f(forward.z, forward.x));
        editorCamera.Position = camPos;
    }

    // Motion v2: the hold-Space radial menu needs the Scene window hovered (child windows too, so
    // the tool rail counts) and the pointer over the 3D view itself.
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool sceneHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        mp.x >= m_ViewportPos.x && mp.y >= m_ViewportPos.y &&
        mp.x < m_ViewportPos.x + m_ViewportSize.x && mp.y < m_ViewportPos.y + m_ViewportSize.y;
    ImGui::End();
    const bool radialWasOpen = m_Radial.Open;
    UpdateRadialMenu(world, editorCamera, sceneHovered);
    // Its clicks pick an item, not the scene (including the click that closes it).
    if (radialWasOpen || m_Radial.Open) m_ViewGizmoBlocking = true;
}

void EditorLayer::DrawGroupGizmo(World& world, Camera& editorCamera) {
    // The gizmo's delta is world-space, so each member is tracked by its WORLD matrix (and its
    // parent's, to convert the result back to local when writing). The single-object gizmo has
    // always done this — DrawGizmo's `parentWorld * ComposeTransform(transform)` — while this one
    // fed raw local TransformComponents into a world-space delta, so the same drag behaved
    // differently depending on how many objects were selected. (#224)
    struct Ref { entt::entity entity; glm::mat4 world; glm::mat4 parentWorld; };
    std::vector<Ref> refs;
    auto addRef = [&](entt::entity entity) {
        if (entity == entt::null || !world.Registry.valid(entity)) return;
        if (!world.Registry.all_of<TransformComponent>(entity)) return;
        // A child of another selected object rides along with its parent already; transforming it
        // a second time here would apply the delta twice.
        if (entity != m_Selected && HasSelectedAncestor(world, entity, m_Selected, m_ExtraSelection)) return;
        refs.push_back({entity, world.GetCachedWorldTransform(entity), ParentWorldMatrix(world, entity)});
    };
    addRef(m_Selected);
    for (entt::entity e : m_ExtraSelection) addRef(e);
    if (refs.empty()) { m_GizmoEngaged = false; m_GizmoUsing = false; return; }

    // Same fullscreen-overlay approach as the single-object gizmo (see BeginGizmoOverlay) — needed
    // so ImGuizmo's hit-testing has a real window to test hover/click against.
    if (!BeginGizmoOverlay(editorCamera, "##GroupGizmoOverlay")) return;

    ImGuizmo::MODE gizmoMode = m_GizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);

    // GizmoOp::Rect falls through to plain TRANSLATE here (no bounds-handle case below, unlike
    // DrawGizmo) — a group's members don't share one native mesh extent, so there's no single
    // coherent bounding box to hang scale handles on; translating the whole group still works.
    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (m_GizmoOp == GizmoOp::Rotate) op = ImGuizmo::ROTATE;
    else if (m_GizmoOp == GizmoOp::Scale) op = ImGuizmo::SCALE;
    else if (m_GizmoOp == GizmoOp::Universal) op = ImGuizmo::UNIVERSAL; // #236 E

    // Only re-center the pivot on the group's current average position when a drag ISN'T in
    // progress — while one is, m_GroupGizmoMatrix is the evolving frame of reference and
    // recomputing it from the (already partway-moved) objects would fight the drag.
    if (!m_GizmoWasUsing) {
        glm::vec3 pivot(0.0f);
        for (auto& r : refs) pivot += glm::vec3(r.world[3]); // world origins, not local Positions (#224)
        pivot /= (float)refs.size();
        m_GroupGizmoMatrix = glm::translate(glm::mat4(1.0f), pivot);
    }
    glm::mat4 matrixBefore = m_GroupGizmoMatrix;

    bool snapActive = m_GridSnapEnabled != ImGui::GetIO().KeyCtrl;
    float snapValues[3] = {m_SnapTranslation, m_SnapTranslation, m_SnapTranslation};
    if (m_GizmoOp == GizmoOp::Rotate) snapValues[0] = snapValues[1] = snapValues[2] = m_SnapRotationDeg;
    else if (m_GizmoOp == GizmoOp::Scale) snapValues[0] = snapValues[1] = snapValues[2] = m_SnapScale;

    const ImGuizmo::Style gizmoStyle = PushGizmoMotionStyle(); // Motion v2: hover springs
    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, gizmoMode,
        glm::value_ptr(m_GroupGizmoMatrix), nullptr, snapActive ? snapValues : nullptr);
    PopGizmoMotionStyle(gizmoStyle);
    m_GizmoEngaged = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    m_GizmoUsing = ImGuizmo::IsUsing();
    EditorSound::GizmoFrame(m_GizmoUsing, m_GroupGizmoMatrix != matrixBefore, snapActive);

    bool isUsingNow = ImGuizmo::IsUsing();
    const bool dragStarted = isUsingNow && !m_GizmoWasUsing;
    if (isUsingNow && !m_GizmoWasUsing) {
        PushUndo(world, GizmoOpUndoLabel(m_GizmoOp));
        m_GroupSnapStartPos = glm::vec3(matrixBefore[3]);
    }
    if (isUsingNow && snapActive) {
        // Motion v2: snap ticks. The pivot frame starts as a pure translation, so its decomposed
        // rotation and scale are the drag's own delta angle and scale.
        float nt[3], nr[3], ns[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(m_GroupGizmoMatrix), nt, nr, ns);
        if (m_GizmoOp == GizmoOp::Rotate)     NoteGizmoSnapStep(glm::vec3(nr[0], nr[1], nr[2]), dragStarted, 0, "\xC2\xB0");
        else if (m_GizmoOp == GizmoOp::Scale) NoteGizmoSnapStep(glm::vec3(ns[0], ns[1], ns[2]), dragStarted, 2, nullptr);
        else NoteGizmoSnapStep(glm::vec3(nt[0], nt[1], nt[2]) - m_GroupSnapStartPos, dragStarted, 2, nullptr);
    }

    if (isUsingNow) {
        // The pivot moved by `delta` this frame — apply that same rigid transform to every
        // selected object's own world matrix, so the group rotates/scales around the shared
        // pivot instead of each object spinning in place around its own center.
        glm::mat4 delta = m_GroupGizmoMatrix * glm::inverse(matrixBefore);
        for (auto& r : refs) {
            // delta is world-space, so it composes onto the object's WORLD matrix; the result is
            // then re-expressed in the parent's frame before it goes back into the (local)
            // TransformComponent — identical to what DrawGizmo and World::SetParent do. Both
            // matrices are identity-parented no-ops for an unparented object. (#224)
            glm::mat4 newWorld = delta * r.world;
            glm::mat4 newLocal = glm::inverse(r.parentWorld) * newWorld;
            float nt[3], nr[3], ns[3];
            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(newLocal), nt, nr, ns);
            auto& transform = world.Registry.get<TransformComponent>(r.entity);
            transform.Position = {nt[0], nt[1], nt[2]};
            transform.SetRotationQuaternion(QuaternionFromMatrix(newLocal));
            transform.Scale = {ns[0], ns[1], ns[2]};
            PushGizmoEditToPhysics(r.entity, newWorld); // #185
        }
    }
    if (!isUsingNow && m_GizmoWasUsing && snapActive) GlassNotifySnap(); // Glass theme: the snap ring
    m_GizmoWasUsing = isUsingNow;
    DrawSnapTicks();

    EndGizmoOverlay();
}

// Scene-view hover pre-highlight: which mesh entity the cursor is over, for the faint outline
// main.cpp draws. Reuses the click pick's exact-triangle raycast; runs only when the cursor moved
// (at most one raycast per frame) and stands down whenever the mouse belongs to something else.
void EditorLayer::UpdateHoverPick(World& world, Camera& editorCamera) {
    ImGuiIO& io = ImGui::GetIO();
    const bool idle = !m_InPlayMode && !m_MeasureTool && m_DecalTool == DecalToolMode::None &&
                      !m_GizmoEngaged && !m_ViewGizmoBlocking && !m_LightHandleEngaged &&
                      !ImGuizmo::IsUsing() && !ImGuizmo::IsOver() &&
                      !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
                      !ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !io.KeyAlt &&
                      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
                      IsMouseOverSceneViewport() && !WantsCaptureMouse() && !ImGui::IsAnyItemHovered();
    if (!idle) {
        m_HoverEntity = entt::null;
        m_HoverMouse = glm::vec2(-1.0f);
        return;
    }
    if (m_HoverEntity != entt::null && !world.Registry.valid(m_HoverEntity)) m_HoverEntity = entt::null;
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
    if (mouse == m_HoverMouse) return; // not moved: keep what we had
    m_HoverMouse = mouse;

    const float w = m_ViewportSize.x, h = m_ViewportSize.y;
    if (w <= 0.0f || h <= 0.0f) { m_HoverEntity = entt::null; return; }
    const glm::mat4 invVP = glm::inverse(editorCamera.ProjectionMatrix(w / h) * editorCamera.ViewMatrix());
    const float ndcX = (2.0f * (mouse.x - m_ViewportPos.x)) / w - 1.0f;
    const float ndcY = 1.0f - (2.0f * (mouse.y - m_ViewportPos.y)) / h;
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farP = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;
    const SurfaceHit hit = RaycastRenderables(world, glm::vec3(nearP), glm::normalize(glm::vec3(farP) - glm::vec3(nearP)),
                                              [&](entt::entity entity) { return ViewportUnpickable(world, entity); });
    m_HoverEntity = hit.Entity;
}
