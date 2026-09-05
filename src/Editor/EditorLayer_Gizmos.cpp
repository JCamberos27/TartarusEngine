// Viewport interaction: the transform/group/view gizmos, light handles and gizmo overlays,
// entity icons, mouse picking, vertex snap/drag, the viewport drop target, and the camera
// framing / look-through navigation they share. Split out of EditorLayer.cpp (#179).

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
#include <cctype>
#include <cstring>
#include <functional>
#include <cfloat>

using namespace EditorInternal;


namespace {

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

// Euler angles in degrees (X,Y,Z as stored in TransformComponent::RotationEuler) from a matrix,
// using the SAME Ry*Rx*Rz decomposition ComposeTransform builds it with — so a gizmo-set
// rotation and an Inspector-typed one agree and compose(decompose(M)) == M (#108). ImGuizmo's
// own DecomposeMatrixToComponents uses a different order, which made objects jump when a gizmo
// rotate was followed by an Inspector nudge (or vice versa).
inline glm::vec3 EulerYXZFromMatrix(const glm::mat4& m) {
    glm::mat3 b(m);
    for (int c = 0; c < 3; ++c) {
        float len = glm::length(b[c]);
        if (len > 1e-8f) b[c] /= len;
    }
    if (glm::determinant(b) < 0.0f) b[0] = -b[0]; // mirrored basis (negative scale) has no clean Euler
    float ey, ex, ez;
    glm::extractEulerAngleYXZ(glm::mat4(b), ey, ex, ez);
    glm::vec3 d = glm::degrees(glm::vec3(ex, ey, ez));
    for (int i = 0; i < 3; ++i) if (std::fabs(d[i]) < 1.0e-4f) d[i] = 0.0f; // kill dust / -0.0
    return d;
}

// The gizmo drag and the Inspector's own number fields edit the same thing; give the History
// entry the same verb either way ("Move" / "Rotate" / "Scale") instead of a generic
// "Transform" from the gizmo path only (#19 P8).
const char* GizmoOpUndoLabel(GizmoOp op) {
    switch (op) {
        case GizmoOp::Rotate: return "Rotate";
        case GizmoOp::Scale:  return "Scale";
        case GizmoOp::Rect:   return "Edit Bounds";
        case GizmoOp::Translate:
        default:              return "Move";
    }
}

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

bool EditorLayer::CanSnapSelectionToGround(World& world) const {
    return m_Selected != entt::null && world.Registry.valid(m_Selected) &&
        world.Registry.all_of<RenderableComponent>(m_Selected);
}

void EditorLayer::SnapSelectionToGround(World& world) {
    if (!CanSnapSelectionToGround(world)) return;
    PushUndo(world, "Snap to Ground");

    auto& renderable = world.Registry.get<RenderableComponent>(m_Selected);
    // World matrix, not ComposeTransform(transform): for a parented entity the local matrix would
    // put the bounds in the PARENT's frame, and subtracting that from Position drops the object to
    // the parent's Y=0 instead of the world's. Compute the drop in world space, then convert the
    // resulting world origin back to local before writing it. (#224)
    glm::mat4 m = world.GetCachedWorldTransform(m_Selected);
    float dropY;
    if (world.Registry.all_of<LevelGeometryTag>(m_Selected)) {
        AABB worldBounds = AABB{renderable.ModelRef->BoundsMin(), renderable.ModelRef->BoundsMax()}.Transformed(m);
        dropY = worldBounds.Min.y;
    } else {
        dropY = renderable.ModelRef->LowestVertexWorldY(m);
    }
    glm::vec3 worldOrigin = glm::vec3(m[3]);
    SetWorldPosition(world, m_Selected, {worldOrigin.x, worldOrigin.y - dropY, worldOrigin.z});
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
    if (!any) return false;
    outMin = boundsMin;
    outMax = boundsMax;
    return true;
}

void EditorLayer::ComputeViewPivot(World& world, Camera& editorCamera, glm::vec3& outPivot,
                                   float& outFrameRadius) const {
    outFrameRadius = 0.0f;

    if (GetSelectionCenter(world, outPivot)) return;

    float t;
    if (world.Raycast(editorCamera.Position, editorCamera.Front(), 1000.0f, t) != entt::null) {
        outPivot = editorCamera.Position + editorCamera.Front() * t;
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

    editorCamera.Orthographic = orthographic; // switches immediately; only angle/position/size animate
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
        m_ViewTransition.Active = false;
        return;
    }

    const float kDuration = 0.28f;
    m_ViewTransition.T = std::min(1.0f, m_ViewTransition.T + dt / kDuration);
    float t = m_ViewTransition.T;
    float eased = t * t * (3.0f - 2.0f * t); // smoothstep

    auto lerpAngle = [](float from, float to, float f) {
        float delta = fmodf(to - from + 540.0f, 360.0f) - 180.0f; // shortest path, wrapped to [-180,180)
        return from + delta * f;
    };

    editorCamera.Yaw = lerpAngle(m_ViewTransition.FromYaw, m_ViewTransition.ToYaw, eased);
    editorCamera.Pitch = m_ViewTransition.FromPitch + (m_ViewTransition.ToPitch - m_ViewTransition.FromPitch) * eased;
    editorCamera.Position = glm::mix(m_ViewTransition.FromPos, m_ViewTransition.ToPos, eased);
    editorCamera.OrthoHalfHeight = m_ViewTransition.FromOrthoHalfHeight +
        (m_ViewTransition.ToOrthoHalfHeight - m_ViewTransition.FromOrthoHalfHeight) * eased;
    editorCamera.Fov = m_ViewTransition.FromFov +
        (m_ViewTransition.ToFov - m_ViewTransition.FromFov) * eased;

    if (m_ViewTransition.T >= 1.0f) m_ViewTransition.Active = false;
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
        m_LookThroughStartRot = tf0.RotationEuler;
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
        glm::vec3 newRot = EulerYXZFromMatrix(invParent *
            glm::inverse(glm::lookAt(glm::vec3(0.0f), fwd, up)));

        bool moved = glm::length(newPos - tf.Position) > 1e-4f ||
                     glm::length(newRot - tf.RotationEuler) > 1e-3f;
        if (moved) {
            if (!m_LookThroughMoved) {           // first real move -> one undo point, light still at entry pose
                PushUndo(world, "Reposition Light (Look Through)");
                m_LookThroughMoved = true;
            }
            tf.Position = newPos;
            tf.RotationEuler = newRot;
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
    dl->AddRectFilled(bmin, bmax, IM_COL32(20, 22, 28, 225), 5.0f);
    dl->AddRect(bmin, bmax, IM_COL32(255, 210, 90, 220), 5.0f);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y), IM_COL32(240, 240, 245, 255), buf);
}

// --- Drop light to surface (#140 phase 4) --------------------------------------------------
bool EditorLayer::DropLightToSurface(World& world, entt::entity light) {
    if (light == entt::null || !world.Registry.valid(light) ||
        !world.Registry.all_of<TransformComponent, LightComponent>(light))
        return false;

    glm::vec3 origin = glm::vec3(world.ComposeWorldTransform(light)[3]);
    const glm::vec3 down(0.0f, -1.0f, 0.0f);

    float bestT = 1e30f;
    bool hit = false;
    for (auto ent : world.Registry.view<const RenderableComponent>()) {
        if (ent == light) continue;
        const auto& r = world.Registry.get<const RenderableComponent>(ent);
        if (!r.ModelRef) continue;
        glm::mat4 model = world.ComposeWorldTransform(ent);
        AABB wb = AABB{r.ModelRef->BoundsMin(), r.ModelRef->BoundsMax()}.Transformed(model);
        float t;
        if (wb.RayIntersect(origin, down, t) && t > 1e-3f && t < bestT) { bestT = t; hit = true; }
    }
    if (!hit) return false;

    PushUndo(world, "Drop Light to Surface");
    glm::vec3 landing = origin + down * bestT + glm::vec3(0.0f, 0.1f, 0.0f);

    // Write the LOCAL translation, so a parented light lands on the surface in world space.
    entt::entity parent = entt::null;
    if (auto* h = world.Registry.try_get<HierarchyComponent>(light)) parent = h->Parent;
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

    // Landing point: the nearest existing box collider, else the Y=0 ground plane, else a fixed
    // distance out (pointing at the sky / parallel to the ground, where neither hits).
    float bestT = 1e30f;
    float boxDist;
    if (world.Raycast(origin, dir, 500.0f, boxDist) != entt::null) {
        bestT = boxDist;
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

void EditorLayer::DrawViewportDropTarget(World& world, AssetLibrary& assets, Camera& editorCamera) {
    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
    bool isModelDrag = peek && peek->IsDataType("ASSET_MODEL_PATH");
    bool isPrefabDrag = peek && peek->IsDataType("ASSET_PREFAB_PATH");
    // Only active while a placeable asset is actually being dragged, so this fullscreen overlay
    // never otherwise sits over the viewport intercepting camera input.
    if (!isModelDrag && !isPrefabDrag) {
        m_DragPreview.Active = false;
        return;
    }

    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) {
        m_DragPreview.Active = false;
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

    if (ImGui::BeginDragDropTarget()) {
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

    const float kDragThreshold = 6.0f; // pixels of movement before a press-drag-release counts as a box select rather than a click

    if (leftPressed) {
        // ImGui panel, the transform gizmo, or the nav gizmo (rotate ring / dolly / pan
        // buttons) already owns this click; Alt+Left-drag is reserved for orbiting the camera
        // around the current selection (see main.cpp's UpdateEditorCamera).
        m_BoxSelectActive = !WantsCaptureMouse() && !m_GizmoEngaged && !m_ViewGizmoBlocking &&
                            !m_LightHandleEngaged && !io.KeyAlt;
        m_BoxSelectStart = {io.MousePos.x, io.MousePos.y};
        return; // click vs. drag is only decided on release, below
    }

    if (!m_BoxSelectActive) return;
    glm::vec2 current(io.MousePos.x, io.MousePos.y);
    bool isDragging = glm::length(current - m_BoxSelectStart) > kDragThreshold;

    if (leftDown) {
        if (isDragging) {
            ImVec2 a(m_BoxSelectStart.x, m_BoxSelectStart.y), b(current.x, current.y);
            ImGui::GetForegroundDrawList()->AddRectFilled(a, b, IM_COL32(255, 217, 77, 35));
            ImGui::GetForegroundDrawList()->AddRect(a, b, IM_COL32(255, 217, 77, 220));
        }
        return; // still held: nothing selected yet, just drawing the marquee
    }

    // Released this frame — commit.
    m_BoxSelectActive = false;
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

        float bestT = 1e30f;
        entt::entity best = entt::null;
        auto pickView = world.Registry.view<const RenderableComponent>(entt::exclude<InactiveTag>);
        for (auto entity : pickView) {
            const auto& renderable = pickView.get<const RenderableComponent>(entity);
            glm::mat4 model = world.ComposeWorldTransform(entity);
            AABB worldBounds = AABB{renderable.ModelRef->BoundsMin(), renderable.ModelRef->BoundsMax()}.Transformed(model);
            float t;
            if (worldBounds.RayIntersect(origin, dir, t) && t < bestT) {
                bestT = t; best = entity;
            }
        }

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

void EditorLayer::DrawEntityIcons(World& world, Camera& editorCamera) {
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    glm::mat4 viewProj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * editorCamera.ViewMatrix();

    // Draw into the Scene window's own draw list (clipped to the viewport rect), NOT the
    // foreground list — the foreground list renders on top of every panel, so light/empty
    // icons would punch through the Inspector, Preferences, any window overlapping the
    // viewport. Appended to the Scene window's list, they sit at its z-order and a panel on
    // top correctly covers them.
    ImGuiWindow* sceneWin = ImGui::FindWindowByName("Scene");
    ImDrawList* draw = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    const ImVec2 clipMin(m_ViewportPos.x, m_ViewportPos.y);
    const ImVec2 clipMax(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y);
    draw->PushClipRect(clipMin, clipMax, true);

    // Mesh-less entities would otherwise be invisible in the viewport — a light you can't see
    // is a light you can't select or aim.
    auto view = world.Registry.view<const TransformComponent>(entt::exclude<RenderableComponent>);
    for (auto entity : view) {
        glm::mat4 model = world.ComposeWorldTransform(entity);
        glm::vec4 clip = viewProj * glm::vec4(glm::vec3(model[3]), 1.0f);
        if (clip.w <= 0.0001f) continue; // behind the camera

        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        ImVec2 screen(m_ViewportPos.x + (ndc.x * 0.5f + 0.5f) * m_ViewportSize.x,
                      m_ViewportPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * m_ViewportSize.y);

        bool selected = IsSelected(entity);
        bool inactive = world.Registry.all_of<InactiveTag>(entity);
        const auto* light = world.Registry.try_get<LightComponent>(entity);
        const bool isCamera = world.Registry.all_of<CameraComponent>(entity);

        // One glyph per kind (#159), and per light type so point / spot / sun read apart at a
        // glance — matching the Add menu's glyphs.
        const char* glyph;
        if (light) {
            glyph = light->Kind == LightComponent::Type::Spot        ? ICON_FA_BULLSEYE
                  : light->Kind == LightComponent::Type::Directional ? ICON_FA_SUN
                                                                     : ICON_FA_LIGHTBULB; // Point
        } else {
            glyph = isCamera ? ICON_FA_VIDEO : ICON_FA_DIAGRAM_PROJECT;
        }

        ImU32 color;
        if (inactive) {
            color = IM_COL32(140, 140, 140, 170);
        } else if (light) {
            // Tinted with the light's own colour so the marker previews what it casts.
            glm::vec3 c = glm::clamp(light->Color, 0.0f, 1.0f) * 255.0f;
            color = IM_COL32((int)c.r, (int)c.g, (int)c.b, selected ? 255 : 235);
        } else {
            color = selected ? IM_COL32(230, 238, 245, 255) : IM_COL32(190, 200, 210, 220);
        }

        ImFont* font = ImGui::GetFont();
        const float iconPx = 16.0f * m_UIScale;
        const ImVec2 gs = font->CalcTextSizeA(iconPx, FLT_MAX, 0.0f, glyph);
        draw->AddText(font, iconPx, ImVec2(screen.x - gs.x * 0.5f, screen.y - gs.y * 0.5f), color, glyph);

        // Selected state = a subtle ring, not extra geometry inside the marker.
        if (selected)
            draw->AddCircle(screen, iconPx * 0.72f, IM_COL32(255, 150, 30, 230), 0, 1.5f * m_UIScale);
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
    ImGuiWindow* sceneWin = ImGui::FindWindowByName("Scene");
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

    ImGuiWindow* sceneWin = ImGui::FindWindowByName("Scene");
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
                    xf.RotationEuler = EulerYXZFromMatrix(glm::inverse(parentWorld) * rot);
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
        draw->AddCircleFilled(sp, r, hot ? IM_COL32(255, 200, 60, fillA) : IM_COL32(245, 245, 245, fillA));
        draw->AddCircle(sp, r, IM_COL32(0, 0, 0, lineA), 0, 1.5f);
    }

    draw->PopClipRect();
}

// Shared setup behind DrawGizmo() (single-object) and DrawGroupGizmo() (multi-select). See the
// declaration in EditorLayer.h for the contract; the comments on the individual calls below
// explain why each one is needed.
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

    // ImGuizmo hit-tests against its own draw-list window (this NoInputs overlay), which is never
    // ImGui's g.HoveredWindow — so without this, hovering the actual "Scene" panel makes
    // IsHoveringWindow() return false and the handles draw but never grab. Registering "Scene" as
    // the alternative window is ImGuizmo's supported way to say "the user hovers there, not here".
    ImGuizmo::SetAlternativeWindow(ImGui::FindWindowByName("Scene"));

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
    if (HasGroupSelection()) {
        DrawGroupGizmo(world, editorCamera);
        return;
    }

    if (m_Selected == entt::null || !world.Registry.valid(m_Selected)) {
        m_GizmoEngaged = false;
        return;
    }

    auto& transform = world.Registry.get<TransformComponent>(m_Selected);
    // Lights and empties have no mesh, so this can legitimately be null — everything below that
    // needs mesh extents (Rect-tool bounds, Center pivot) falls back to a small unit box.
    auto* renderablePtr = world.Registry.try_get<RenderableComponent>(m_Selected);
    bool registryHasRenderable = renderablePtr != nullptr;

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

    glm::mat4 matrix = parentWorld * ComposeTransform(transform);

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

    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, gizmoMode,
        glm::value_ptr(matrix), nullptr, snapActive ? snapValues : nullptr, boundsPtr);
    m_GizmoEngaged = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

    bool isUsingNow = ImGuizmo::IsUsing();
    if (isUsingNow && !m_GizmoWasUsing) {
        // Drag just started this frame: snapshot the still-unmodified transform (pos/rot/scale
        // below haven't been written yet) so undo restores to exactly where the drag began.
        PushUndo(world, GizmoOpUndoLabel(m_GizmoOp));
    }
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
        // Rotation via the ComposeTransform-matching order, NOT ImGuizmo's decompose (#108).
        glm::vec3 newRot = EulerYXZFromMatrix(newLocal);

        // Write back only the channel this gizmo actually drives — ImGuizmo's decompose leaks
        // float noise into the other two, and a pure translate drag was nudging Rotation
        // 0.000 -> -0.000 every time, accumulating over many drags (#12 P1).
        switch (m_GizmoOp) {
            case GizmoOp::Translate: transform.Position = newPos;      break;
            case GizmoOp::Rotate:    transform.RotationEuler = newRot;  break;
            case GizmoOp::Scale:     transform.Scale = newScale;        break;
            default: // Rect / bounds edit resizes from a handle — position and scale both move
                transform.Position = newPos;
                transform.RotationEuler = newRot;
                transform.Scale = newScale;
                break;
        }
    }

    EndGizmoOverlay();
}

void EditorLayer::DrawViewGizmo(World& world, Camera& editorCamera) {
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    int w, h;
    glfwGetWindowSize(m_Window, &w, &h);
    if (w <= 0 || h <= 0) return;

    // The library's own defaults (256px rotate ring, 50px tool buttons) are sized for a full
    // editor viewport; shrink them to sit unobtrusively in the corner instead.
    ImViewGuizmo::Style& style = ImViewGuizmo::GetStyle();
    style.scale = m_UIScale * 0.5f;

    float gizmoRadius = 128.0f * style.scale; // half of the library's fixed 256px rotate-ring box
    float toolRadius = style.toolButtonRadius * style.scale;
    float margin = 14.0f * m_UIScale;
    float spacing = 8.0f * m_UIScale;

    // Rotate's `position` param is the ring's CENTER, but Dolly/Pan's is the TOP-LEFT of their
    // button box (confirmed by reading ImViewGuizmo.h — the two widget kinds don't agree on
    // that convention despite the doc comments implying otherwise). Laying out from a shared
    // center point and converting only for Dolly/Pan keeps the whole cluster symmetric.
    ImVec2 rotateCenter(m_ViewportPos.x + m_ViewportSize.x - margin - gizmoRadius,
        m_ViewportPos.y + margin + gizmoRadius);
    float toolCenterY = rotateCenter.y + gizmoRadius + spacing + toolRadius;
    ImVec2 dollyCenter(rotateCenter.x - spacing * 0.5f - toolRadius, toolCenterY);
    ImVec2 panCenter(rotateCenter.x + spacing * 0.5f + toolRadius, toolCenterY);

    ImVec2 rotatePos = rotateCenter;
    ImVec2 dollyPos(dollyCenter.x - toolRadius, dollyCenter.y - toolRadius);
    ImVec2 panPos(panCenter.x - toolRadius, panCenter.y - toolRadius);

    // Contrast-adaptive tint for the dolly / pan tool buttons and the projection label: sample the
    // scene luminance behind the cluster and steer the glyphs white-on-dark / dark-on-light — the
    // same readback the corner engine mark and the viewport Play button use (throttled ~10 Hz,
    // eased per frame). The rotate ball keeps its own red/green/blue axis colours untouched.
    {
        m_NavGizmoSampleAccum += ImGui::GetIO().DeltaTime;
        if (m_NavGizmoSampleAccum >= 0.1f) {
            m_NavGizmoSampleAccum = 0.0f;
            float lum = SampleSceneLuminance(m_NavGizmoReadback, ImVec2(rotateCenter.x, toolCenterY), 48.0f * m_UIScale);
            if (lum >= 0.0f) {
                float t = (lum - 0.30f) / (0.62f - 0.30f);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                m_NavGizmoContrastTarget = 1.0f - t * t * (3.0f - 2.0f * t); // 1 = white on dark, 0 = black on light
            }
        }
        float k = 1.0f - expf(-ImGui::GetIO().DeltaTime / 0.15f);
        m_NavGizmoContrastLum += (m_NavGizmoContrastTarget - m_NavGizmoContrastLum) * k;
    }
    int navV = (int)(m_NavGizmoContrastLum * 255.0f + 0.5f);
    navV = navV < 0 ? 0 : (navV > 255 ? 255 : navV);
    const int navInv = 255 - navV;
    style.toolButtonIconColor    = IM_COL32(navV, navV, navV, 235);
    style.toolButtonColor        = IM_COL32(navInv, navInv, navInv, 40);
    style.toolButtonHoveredColor = IM_COL32(navInv, navInv, navInv, 64);

    // Same fullscreen-transparent-overlay trick as DrawGizmo(): the library hit-tests against
    // raw mouse position within ImGui::GetWindowDrawList()'s owning window, so it needs a real
    // hoverable window as the "current window"; NoInputs keeps it from stealing
    // WantCaptureMouse everywhere else (which would otherwise block the editor fly-camera).
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##ViewGizmoOverlay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
    // See DrawGizmo's identical call for why this is needed - without it, "Scene" (newer, so
    // higher in ImGui's window stack) covers this NoInputs overlay instead of the other way
    // around.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

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

    bool modified = false;
    modified |= ImViewGuizmo::Rotate(camPos, camRot, pivot, rotatePos);
    modified |= ImViewGuizmo::Dolly(camPos, camRot, dollyPos);
    modified |= ImViewGuizmo::Pan(camPos, camRot, panPos);
    m_ViewGizmoBlocking = ImViewGuizmo::IsOver() || ImViewGuizmo::IsUsing();

    // Tooltips - the tool buttons above give no feedback on their own (they're manually
    // hit-tested inside the vendored gizmo library, not real ImGui widgets, so
    // ImGui::IsItemHovered() can't see them); read the library's own hover state instead.
    const auto& gizmoCtx = ImViewGuizmo::GetContext();
    if (gizmoCtx.hoveredAxisID == 6) {
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
    } else if (gizmoCtx.isZoomButtonHovered) {
        EditorUI::SetTooltip("Click and drag to dolly the view closer to/further from the pivot");
    } else if (gizmoCtx.isPanButtonHovered) {
        EditorUI::SetTooltip("Click and drag to pan the view");
    }

    // "Persp"/"Iso" label under the gizmo, mirroring Unity's own - click to switch the editor
    // camera between perspective and orthographic (isometric) projection without changing the
    // current viewing angle. Manually hit-tested against the raw mouse position, same as the
    // tool buttons above, since this overlay window is ImGuiWindowFlags_NoInputs.
    {
        // Name the view when the camera is aligned to a canonical axis (Front/Right/Top/...),
        // as set by the nav-gizmo axis handles or the numpad views — falling back to
        // Persp/Iso only when it's a free angle (#25 P14).
        const char* isoLabel = [&]() -> const char* {
            const glm::vec3 f = editorCamera.Front();
            const float k = 0.999f; // within ~2.5 degrees of dead-on
            if (f.z < -k) return "Front";
            if (f.z >  k) return "Back";
            if (f.x < -k) return "Right";
            if (f.x >  k) return "Left";
            if (f.y < -k) return "Top";
            if (f.y >  k) return "Bottom";
            return editorCamera.Orthographic ? "Iso" : "Persp";
        }();
        ImFont* font = ImGui::GetFont();
        float labelFontSize = ImGui::GetFontSize();
        ImVec2 textSize = font->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, isoLabel);
        ImVec2 textPos(rotateCenter.x - textSize.x * 0.5f, toolCenterY + toolRadius + spacing);
        ImVec2 padding(4.0f * m_UIScale, 2.0f * m_UIScale);
        ImVec2 hitMin(textPos.x - padding.x, textPos.y - padding.y);
        ImVec2 hitMax(textPos.x + textSize.x + padding.x, textPos.y + textSize.y + padding.y);
        ImVec2 mouse = ImGui::GetIO().MousePos;
        bool isoHovered = mouse.x >= hitMin.x && mouse.x <= hitMax.x && mouse.y >= hitMin.y && mouse.y <= hitMax.y;
        ImDrawList* labelDl = ImGui::GetWindowDrawList();
        if (isoHovered) {
            labelDl->AddRectFilled(hitMin, hitMax, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), 3.0f * m_UIScale);
            EditorUI::SetTooltip("Switch between Perspective and Isometric (orthographic) view");
            m_ViewGizmoBlocking = true;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ToggleOrthographic(world, editorCamera);
            }
        }
        // Same contrast-adaptive grey as the tool buttons above (full-strength on hover).
        ImU32 isoTextColor = IM_COL32(navV, navV, navV, isoHovered ? 255 : 200);
        labelDl->AddText(font, labelFontSize, textPos, isoTextColor, isoLabel);
    }

    if (modified) {
        glm::vec3 forward = glm::normalize(camRot * glm::vec3(0.0f, 0.0f, -1.0f));
        editorCamera.Pitch = glm::clamp(glm::degrees(asinf(glm::clamp(forward.y, -1.0f, 1.0f))), -89.0f, 89.0f);
        editorCamera.Yaw = glm::degrees(atan2f(forward.z, forward.x));
        editorCamera.Position = camPos;
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
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
    if (refs.empty()) { m_GizmoEngaged = false; return; }

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

    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, gizmoMode,
        glm::value_ptr(m_GroupGizmoMatrix), nullptr, snapActive ? snapValues : nullptr);
    m_GizmoEngaged = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

    bool isUsingNow = ImGuizmo::IsUsing();
    if (isUsingNow && !m_GizmoWasUsing) {
        PushUndo(world, GizmoOpUndoLabel(m_GizmoOp));
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
            transform.RotationEuler = EulerYXZFromMatrix(newLocal); // ComposeTransform order, not ImGuizmo's (#108)
            transform.Scale = {ns[0], ns[1], ns[2]};
        }
    }
    m_GizmoWasUsing = isUsingNow;

    EndGizmoOverlay();
}
