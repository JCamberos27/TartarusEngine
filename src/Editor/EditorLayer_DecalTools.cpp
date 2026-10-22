// The decal editor tools (docs/DECALS.md, "Editor tools"): Place Decal (a ghost that follows the
// cursor, a stamp on click), the Decal Brush (a stroke of seeded dabs, Ctrl erases, one undo step a
// stroke), the auto-grime pass, and the cursor ring / stamp rings / hint plate they draw. The
// Decal Library panel and the state these share are in EditorLayer_DecalLibrary.cpp; the maths
// (stroke stepping, room probing) is in DecalTools.cpp.
//
// The ghost is the real decal: DecalSystem draws it (a DecalComponent held in DecalEditorState) at
// the cursor on the editor's own views, so it shows exactly how the decal will look on that wall.
// Everything animated goes through Glass::Motion() in the Viewport category and holds nothing at
// rest; the per-frame paths here allocate nothing (stack arrays, state vectors that keep capacity).
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorIcons.h"
#include "EditorSettings.h"
#include "EditorSound.h"
#include "EditorTheme.h"
#include "EditorUIPrimitives.h"
#include "DecalEditorState.h"
#include "Glass/GlassMotion.h"
#include "Camera.h"
#include "DecalAsset.h"
#include "Log.h"
#include "Model.h"
#include "ProjectPaths.h"
#include "World.h"
#include "AABB.h"

#include <imgui.h>
#include <imgui_internal.h> // ImHashStr
#include <IconsFontAwesome6.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace EditorInternal;
using namespace DecalEdit;

namespace {

constexpr Glass::MotionCategory kViewport = Glass::MotionCategory::Viewport;

ImU32 White(float a) { return IM_COL32(255, 255, 255, (int)(ImClamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)); }
ImU32 Black(float a) { return IM_COL32(0, 0, 0, (int)(ImClamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)); }

const ImGuiID kPopId = ImHashStr("##decalPop");
const ImGuiID kGhostFadeId = ImHashStr("##decalGhostFade");
const ImGuiID kHudFadeId = ImHashStr("##decalHudFade");

constexpr float kMaxScale = 12.0f;

// A decal's rotation on a surface with normal `n`: +Z out of the surface, +X along `right` flattened
// onto it (any in-plane axis when they line up), then spun about +Z by `spin` radians.
glm::mat3 SurfaceBasis(const glm::vec3& n, const glm::vec3& right, float spin) {
    glm::vec3 x = right - n * glm::dot(right, n);
    if (glm::dot(x, x) < 1e-6f) x = glm::cross(std::abs(n.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0), n);
    x = glm::normalize(x);
    const glm::vec3 y = glm::cross(n, x);
    const float c = std::cos(spin), s = std::sin(spin);
    const glm::vec3 x2 = x * c + y * s;
    return glm::mat3(x2, glm::cross(n, x2), n);
}

const DecalEditorState::AssetInfo& InfoFor(DecalEditorState& ed, const std::string& absPath) {
    auto it = ed.Assets.find(absPath);
    if (it != ed.Assets.end()) return it->second;
    DecalEditorState::AssetInfo info;
    if (const auto asset = DecalAsset::Load(absPath); asset && !asset->Missing) {
        info.Size = asset->DefaultSize;
        info.Ok = true;
    }
    return ed.Assets.emplace(absPath, info).first->second;
}

std::string DisplayName(const std::string& path) {
    std::string name = std::filesystem::u8path(path).stem().u8string();
    if (name.rfind("DA_", 0) == 0 && name.size() > 3) name = name.substr(3);
    return name.empty() ? std::string("Decal") : name;
}

// The group the Decal Brush's decals go under (made when the first lands).
entt::entity PaintGroup(World& world, DecalEditorState& ed) {
    if (ed.PaintGroup != entt::null && world.Registry.valid(ed.PaintGroup) && world.Registry.all_of<NameComponent>(ed.PaintGroup))
        return ed.PaintGroup;
    ed.PaintGroup = entt::null;
    for (auto [e, name] : world.Registry.view<const NameComponent>().each()) {
        if (name.Name != "Decal Paint") continue;
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        if (h && h->Parent != entt::null) continue;
        ed.PaintGroup = e;
        return e;
    }
    ed.PaintGroup = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Decal Paint");
    return ed.PaintGroup;
}

// One decal on a surface. `parent` (or null) keeps it with a moving receiver or under a group.
entt::entity SpawnDecal(World& world, DecalEditorState& ed, const std::string& path, const glm::vec3& point, const glm::vec3& normal,
                        const glm::vec3& right, float spin, float scale, float opacity, const glm::vec3& tint, int seed,
                        entt::entity parent) {
    const DecalEditorState::AssetInfo& info = InfoFor(ed, path);
    const entt::entity e = world.CreateEmptyEntity(point, glm::vec3(0.0f), glm::vec3(1.0f), DisplayName(path));
    world.Registry.get<TransformComponent>(e).SetRotationQuaternion(glm::quat_cast(SurfaceBasis(glm::normalize(normal), right, spin)));
    DecalComponent dc;
    dc.AssetPath = ProjectPaths::Relativize(path); // project-relative, like every scene asset reference
    dc.Size = (info.Ok ? info.Size : dc.Size) * scale;
    dc.Tint = tint;
    dc.Opacity = opacity;
    dc.Seed = seed;
    world.Registry.emplace<DecalComponent>(e, dc);
    if (parent != entt::null) world.SetParent(e, parent);
    return e;
}

// A world point to the screen; false behind the camera.
struct Projector {
    glm::mat4 VP;
    ImVec2 Pos, Size;
    bool operator()(const glm::vec3& p, ImVec2& out) const {
        const glm::vec4 c = VP * glm::vec4(p, 1.0f);
        if (c.w <= 1e-4f) return false;
        const glm::vec3 ndc = glm::vec3(c) / c.w;
        out = ImVec2(Pos.x + (ndc.x * 0.5f + 0.5f) * Size.x, Pos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * Size.y);
        return true;
    }
};

// A circle lying on a surface (centre, normal, a right axis), as a screen polyline. Fine at 48 points;
// nothing is drawn if any of it is behind the camera.
void RingOnSurface(ImDrawList* dl, const Projector& project, const glm::vec3& centre, const glm::vec3& normal, const glm::vec3& right,
                   float radius, ImU32 col, float thickness) {
    constexpr int N = 48;
    ImVec2 pts[N];
    const glm::vec3 x = glm::normalize(right - normal * glm::dot(right, normal));
    const glm::vec3 y = glm::cross(normal, x);
    for (int i = 0; i < N; ++i) {
        const float a = 2.0f * IM_PI * (float)i / (float)N;
        if (!project(centre + (x * std::cos(a) + y * std::sin(a)) * radius, pts[i])) return;
    }
    dl->AddPolyline(pts, N, col, ImDrawFlags_Closed, thickness);
}

} // namespace

// --- Update -------------------------------------------------------------------------------------

void EditorLayer::EndDecalStroke(World& world) {
    DecalEditorState& ed = DecalEd();
    if (!ed.Stroking) return;
    ed.Stroking = false;
    char label[48];
    std::snprintf(label, sizeof(label), ed.Erasing ? "Erase Decals (%d)" : "Paint Decals (%d)", ed.StrokeCount);
    CommitStagedUndo(world, label);
}

void EditorLayer::UpdateDecalTool(World& world, Camera& cam, bool free, bool leftPressed, bool leftDown) {
    DecalEditorState& ed = DecalEd();
    ScanStep(ed); // keep indexing with the panel closed
    ed.GhostView.Active = false;
    ed.Cur.Valid = false;
    ImGuiIO& io = ImGui::GetIO();
    Glass::ImAnimContext& motion = Glass::Motion();
    const bool animate = motion.CategoryOn(kViewport);

    // A stroke whose button came up (perhaps off the viewport) closes here.
    if (ed.Stroking && !leftDown) EndDecalStroke(world);
    if (!free && !ed.Stroking) return;

    // The surface under the cursor: the ghost's place, the brush's centre.
    const DecalSpot spot = DecalSpotUnderMouse(world, cam);
    if (spot.OnSurface) {
        ed.Cur.Valid = true;
        ed.Cur.Point = spot.Point;
        ed.Cur.Normal = glm::normalize(spot.Normal);
        ed.Cur.Right = cam.Right();
        ed.Cur.Receiver = spot.Receiver;
    }
    const float x01 = io.DisplaySize.x > 0.0f ? io.MousePos.x / io.DisplaySize.x : -1.0f;

    // The wheel: Place turns the ghost (Shift: sizes it); the brush sizes its radius (Shift: its decals).
    float wheel = 0.0f;
    if (free && DecalToolOwnsWheel()) wheel = io.MouseWheel;
    if (m_DecalTool == DecalToolMode::Place) {
        if (wheel != 0.0f) {
            if (io.KeyShift) ed.PlaceScale = ImClamp(ed.PlaceScale * std::pow(1.1f, wheel), 0.1f, kMaxScale);
            else ed.PlaceSpin += glm::radians(wheel * 7.5f);
        }
        if (ed.PlacePath.empty() || !ed.Cur.Valid || !free) return;
        const DecalEditorState::AssetInfo& info = InfoFor(ed, ed.PlacePath);
        if (!info.Ok) return;

        // The ghost: the decal as it will land, breathing; a click presses it in (it settles from a
        // little large while its opacity comes down to the ghost level).
        float scale = ed.PlaceScale, alpha = Glass::DecalGhostAlpha;
        if (animate) alpha *= 1.0f + Glass::DecalGhostBreath * motion.Oscillate(Glass::DecalGhostBreathHz);
        if (ed.LastPop >= 0 && motion.PulseActive(kPopId + (ImGuiID)ed.LastPop)) {
            const float p = motion.PulseProgress(kPopId + (ImGuiID)ed.LastPop);
            scale *= ImLerp(Glass::DecalStampFromScale, 1.0f, Glass::EaseProgress(Glass::DecalStampPop, p));
            alpha = ImLerp(1.0f, alpha, ImClamp(Glass::EaseProgress(Glass::DecalRingFlash, p), 0.0f, 1.0f));
        }
        DecalComponent& g = ed.GhostView.Comp;
        g = DecalComponent();
        g.AssetPath = ed.PlacePath;
        g.Size = info.Size * scale;
        g.Seed = ed.PlaceSeed;
        g.Opacity = ImClamp(alpha, 0.0f, 1.0f);
        const glm::mat3 basis = SurfaceBasis(ed.Cur.Normal, ed.Cur.Right, ed.PlaceSpin);
        ed.GhostView.World = glm::translate(glm::mat4(1.0f), ed.Cur.Point) * glm::mat4(basis);
        ed.GhostView.Active = true;

        if (leftPressed) {
            PushUndo(world, "Place Decal"); // plays DecalPlace
            entt::entity parent = IsDynamicDecalReceiver(world, ed.Cur.Receiver) ? ed.Cur.Receiver : entt::null;
            SpawnDecal(world, ed, ed.PlacePath, ed.Cur.Point, ed.Cur.Normal, ed.Cur.Right, ed.PlaceSpin, ed.PlaceScale, 1.0f,
                       glm::vec3(1.0f), ed.PlaceSeed, parent);
            NoteRecent(ed, ed.PlacePath);
            ed.PlaceSeed = (int)(DecalTools::Mix((uint32_t)ed.PlaceSeed, (uint32_t)ImGui::GetFrameCount()) & 0x7FFFFFFF);
            DecalEditorState::Pop& pop = ed.Pops[ed.NextPop];
            pop.Point = ed.Cur.Point;
            pop.Normal = ed.Cur.Normal;
            pop.Right = basis[0];
            pop.RadiusM = 0.5f * std::max(info.Size.x, info.Size.y) * ed.PlaceScale;
            pop.Live = true;
            ed.LastPop = ed.NextPop;
            if (animate) motion.Pulse(kPopId + (ImGuiID)ed.NextPop, Glass::DecalRingFlash.durationMs * 0.001f);
            ed.NextPop = (ed.NextPop + 1) % DecalEditorState::kPops;
        }
        return;
    }

    // --- Decal Brush ----------------------------------------------------------------------------
    DecalTools::BrushSettings& brush = ed.Brush;
    if (wheel != 0.0f) {
        if (io.KeyShift) brush.Size = ImClamp(brush.Size * std::pow(1.1f, wheel), 0.1f, 4.0f);
        else brush.Radius = ImClamp(brush.Radius * std::pow(1.1f, wheel), 0.02f, 3.0f);
        SaveToolSettings(ed);
    }
    if (leftPressed && free && ed.Cur.Valid && !ed.Stroking) {
        StageUndo(world); // the whole stroke is one undo step; EndDecalStroke labels it
        ed.Stroking = true;
        ed.Erasing = io.KeyCtrl;
        ed.StrokeCount = 0;
        ed.StrokeSeed = DecalTools::Mix(ed.StrokeSeed, (uint32_t)(ImGui::GetTime() * 1000.0) ^ (uint32_t)ImGui::GetFrameCount());
        ed.Stroke.Begin(ed.StrokeSeed, brush);
    }
    if (!ed.Stroking || !ed.Cur.Valid) return;

    if (ed.Erasing) {
        // Every decal whose centre is under the brush goes (a few a frame, so a slow sweep is smooth).
        ed.Scratch.clear();
        const float reach = std::max(brush.Radius, 0.1f);
        for (auto [e, dc] : world.Registry.view<const DecalComponent>().each()) {
            (void)dc;
            const glm::vec3 centre(world.ComposeWorldTransform(e)[3]);
            if (glm::length(centre - ed.Cur.Point) <= reach) ed.Scratch.push_back(e);
            if (ed.Scratch.size() >= 16) break;
        }
        for (entt::entity e : ed.Scratch) {
            if (!world.Registry.valid(e)) continue;
            if (m_Selected == e) ClearSelection();
            world.DestroyEntityAndChildren(e);
        }
        if (!ed.Scratch.empty()) {
            ed.StrokeCount += (int)ed.Scratch.size();
            EditorSound::Play(EditorSound::Event::DecalErase, x01);
        }
        return;
    }

    DecalTools::BrushDab dabs[DecalTools::BrushStroke::kMaxPerAdvance];
    const int n = ed.Stroke.Advance(ed.Cur.Point, ed.Cur.Normal, dabs, DecalTools::BrushStroke::kMaxPerAdvance);
    if (n == 0) return;
    const int source = SourceKind(ed);
    if (source == 1) RefreshPool(ed);
    const DecalPalette* palette = source == 2 ? ed.Palettes[ed.ActivePalette].get() : nullptr;
    int placed = 0;
    for (int i = 0; i < n; ++i) {
        const DecalTools::BrushDab& dab = dabs[i];
        // What this dab paints, and how the palette dresses it.
        std::string path;
        float scale = dab.Scale, opacity = dab.Opacity;
        glm::vec3 tint(1.0f);
        DecalTools::Rng rng(DecalTools::Mix(dab.Seed, 0x51u));
        if (palette) {
            const int pick = palette->Pick(dab.PickRoll);
            if (pick < 0) continue;
            const DecalPaletteEntry& e = palette->Entries[pick];
            path = ProjectPaths::Resolve(e.DecalPath);
            scale *= rng.Range(e.Scale.x, e.Scale.y);
            opacity *= rng.Range(e.Opacity.x, e.Opacity.y);
            tint = e.Tint;
        } else if (source == 1) {
            if (ed.CategoryPool.empty()) continue;
            path = ed.Entries[ed.CategoryPool[std::min((size_t)(dab.PickRoll * (float)ed.CategoryPool.size()), ed.CategoryPool.size() - 1)]].Path;
        } else {
            path = ed.PlacePath;
        }
        if (path.empty() || !InfoFor(ed, path).Ok) continue;

        // The dab was scattered on the stroke's plane: find the real surface under it.
        glm::vec3 hit, hn;
        entt::entity receiver = entt::null;
        if (!CastDecalRay(world, dab.Point + dab.Normal * 0.4f, -dab.Normal, 0.9f, hit, hn, &receiver)) continue;
        const glm::vec3 normal = brush.AlignToSurface ? hn : dab.Normal;
        const entt::entity parent = IsDynamicDecalReceiver(world, receiver) ? receiver : PaintGroup(world, ed);
        SpawnDecal(world, ed, path, hit, normal, dab.Right, dab.Spin, scale, opacity, tint, (int)dab.Seed, parent);
        ++placed;
    }
    if (placed > 0) {
        ed.StrokeCount += placed;
        EditorSound::Play(EditorSound::Event::DecalBrushTick, x01, 0.94f + 0.12f * (float)(ed.StrokeCount % 5) / 4.0f);
    }
}

// --- Overlay ------------------------------------------------------------------------------------

void EditorLayer::DrawDecalToolOverlay(World& world, Camera& cam) {
    (void)world;
    if (!m_DecalEd) return;
    DecalEditorState& ed = *m_DecalEd;
    Glass::ImAnimContext& motion = Glass::Motion();
    const bool animate = motion.CategoryOn(kViewport);
    const bool toolOn = m_DecalTool != DecalToolMode::None;
    if (m_ViewportSize.x <= 1.0f || m_ViewportSize.y <= 1.0f) return;

    // The hint plate fades in and out with the tool (a toggle: no state at rest).
    const float hud = animate ? motion.AnimateToggle(kHudFadeId, toolOn, ed.HudWas, Glass::DecalGhostFade, Glass::DecalGhostFade) : (toolOn ? 1.0f : 0.0f);
    ed.HudWas = toolOn;

    // Stamp rings go on after the tool exits, so check them before bailing.
    bool anyPop = false;
    for (int i = 0; i < DecalEditorState::kPops; ++i) {
        DecalEditorState::Pop& pop = ed.Pops[i];
        if (!pop.Live) continue;
        if (!animate || !motion.PulseActive(kPopId + (ImGuiID)i)) pop.Live = false;
        else anyPop = true;
    }
    if (hud <= 0.0f && !anyPop && !toolOn) return;

    const Projector project{cam.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * cam.ViewMatrix(),
                            ImVec2(m_ViewportPos.x, m_ViewportPos.y), ImVec2(m_ViewportSize.x, m_ViewportSize.y)};
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 vpMin(m_ViewportPos.x, m_ViewportPos.y), vpMax(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y);
    dl->PushClipRect(vpMin, vpMax, true);

    // Stamp rings: a ring on the surface that opens out from the decal and fades.
    for (int i = 0; i < DecalEditorState::kPops; ++i) {
        const DecalEditorState::Pop& pop = ed.Pops[i];
        if (!pop.Live) continue;
        const float p = motion.PulseProgress(kPopId + (ImGuiID)i);
        const float e = ImClamp(Glass::EaseProgress(Glass::DecalRingFlash, p), 0.0f, 1.0f);
        RingOnSurface(dl, project, pop.Point, pop.Normal, pop.Right, pop.RadiusM * (0.45f + 0.75f * e), White(0.9f * (1.0f - e)),
                      ImLerp(2.4f, 0.8f, e) * m_UIScale);
    }

    if (toolOn && ed.Cur.Valid) {
        const float breath = animate ? 1.0f + 0.02f * motion.Oscillate(Glass::DecalGhostBreathHz) : 1.0f;
        if (m_DecalTool == DecalToolMode::Brush) {
            const bool erasing = ed.Stroking ? ed.Erasing : ImGui::GetIO().KeyCtrl;
            const float r = std::max(ed.Brush.Radius, 0.02f) * breath;
            RingOnSurface(dl, project, ed.Cur.Point, ed.Cur.Normal, ed.Cur.Right, r, White(erasing ? 0.95f : 0.8f),
                          (erasing ? 2.4f : 1.6f) * m_UIScale);
            RingOnSurface(dl, project, ed.Cur.Point, ed.Cur.Normal, ed.Cur.Right, r * 0.55f, White(0.22f), 1.0f * m_UIScale);
            ImVec2 c;
            if (project(ed.Cur.Point, c)) {
                dl->AddCircleFilled(c, 2.5f * m_UIScale, White(0.9f));
                if (erasing) {
                    const float k = 6.0f * m_UIScale;
                    dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), White(0.95f), 1.6f * m_UIScale);
                    dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), White(0.95f), 1.6f * m_UIScale);
                }
            }
        } else if (ed.GhostView.Active) {
            // Corner brackets round the ghost's box, and a short tick out of the surface.
            const float fade = animate ? motion.AnimateToggle(kGhostFadeId, true, ed.GhostWas, Glass::DecalGhostFade, Glass::DecalGhostFade) : 1.0f;
            const glm::mat4& w = ed.GhostView.World;
            const glm::vec3 half = ed.GhostView.Comp.Size * 0.5f;
            const glm::vec3 ax(w[0]), ay(w[1]), c(w[3]);
            ImVec2 s[4];
            const glm::vec3 corners[4] = {c - ax * half.x - ay * half.y, c + ax * half.x - ay * half.y, c + ax * half.x + ay * half.y,
                                          c - ax * half.x + ay * half.y};
            bool ok = true;
            for (int i = 0; i < 4; ++i) ok = project(corners[i], s[i]) && ok;
            if (ok) {
                const ImU32 col = White(0.7f * fade);
                for (int i = 0; i < 4; ++i) {
                    const ImVec2 a = s[i], b = s[(i + 1) % 4], d = s[(i + 3) % 4];
                    const float lb = 0.22f, ld = 0.22f;
                    dl->AddLine(a, ImVec2(a.x + (b.x - a.x) * lb, a.y + (b.y - a.y) * lb), col, 1.6f * m_UIScale);
                    dl->AddLine(a, ImVec2(a.x + (d.x - a.x) * ld, a.y + (d.y - a.y) * ld), col, 1.6f * m_UIScale);
                }
            }
            ImVec2 c0, c1;
            if (project(c, c0) && project(c + ed.Cur.Normal * 0.12f, c1)) {
                dl->AddLine(c0, c1, White(0.6f * fade), 1.2f * m_UIScale);
                dl->AddCircleFilled(c0, 2.2f * m_UIScale, White(0.85f * fade));
            }
        }
    }
    ed.GhostWas = toolOn && m_DecalTool == DecalToolMode::Place && ed.GhostView.Active;
    dl->PopClipRect();

    // The hint plate, top centre of the viewport.
    if (hud > 0.0f) {
        char line[200];
        if (m_DecalTool == DecalToolMode::Place) {
            const std::string name = ed.PlacePath.empty() ? std::string("pick a decal in the Decal Library") : DisplayName(ed.PlacePath);
            std::snprintf(line, sizeof(line), ICON_FA_STAMP "  Place Decal   %s   %.0f deg   %.2fx      Click stamps   Scroll turns   Shift+Scroll sizes   Esc exits",
                          name.c_str(), std::fmod(glm::degrees(ed.PlaceSpin), 360.0f), ed.PlaceScale);
        } else {
            const int src = SourceKind(ed);
            const std::string what = src == 2 ? ed.Palettes[ed.ActivePalette]->Name
                                   : src == 1 ? EditorSettings::Get().DecalSourceCategory
                                   : ed.PlacePath.empty() ? std::string("no decal") : DisplayName(ed.PlacePath);
            std::snprintf(line, sizeof(line), ICON_FA_SPRAY_CAN "  Decal Brush   %s   radius %.2f m      Drag paints   Ctrl erases   Scroll radius   Shift+Scroll size   Space picks   Esc exits",
                          what.c_str(), ed.Brush.Radius);
        }
        EditorTheme::PushSmall();
        const ImVec2 ts = ImGui::CalcTextSize(line);
        const ImVec2 pad(10.0f * m_UIScale, 5.0f * m_UIScale);
        const float cx = m_ViewportPos.x + m_ViewportSize.x * 0.5f;
        const ImVec2 mn(cx - ts.x * 0.5f - pad.x, m_ViewportPos.y + 10.0f * m_UIScale);
        const ImVec2 mx(cx + ts.x * 0.5f + pad.x, mn.y + ts.y + pad.y * 2.0f);
        const float rise = (1.0f - hud) * 6.0f * m_UIScale;
        dl->AddRectFilled(ImVec2(mn.x, mn.y - rise), ImVec2(mx.x, mx.y - rise), Black(0.86f * hud), 6.0f * m_UIScale);
        dl->AddRect(ImVec2(mn.x, mn.y - rise), ImVec2(mx.x, mx.y - rise), White(0.12f * hud), 6.0f * m_UIScale);
        dl->AddText(ImVec2(mn.x + pad.x, mn.y + pad.y - rise), White(0.88f * hud), line);
        EditorTheme::PopFont();
    }
}

// --- Auto-grime ---------------------------------------------------------------------------------

namespace {

struct CastContext {
    const EditorLayer* Layer;
    World* W;
};

bool CastThunk(void* user, const glm::vec3& origin, const glm::vec3& dir, float maxT, DecalTools::GrimeHit& out) {
    const CastContext* c = static_cast<const CastContext*>(user);
    glm::vec3 p, n;
    if (!c->Layer->CastDecalRay(*c->W, origin, dir, maxT, p, n)) return false;
    out.Point = p;
    out.Normal = n;
    out.T = glm::length(p - origin);
    return true;
}

// The world bounds of `root` and everything under it that renders.
bool SubtreeBounds(World& world, entt::entity root, glm::vec3& mn, glm::vec3& mx, bool& any) {
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        if (!world.Registry.valid(e)) continue;
        if (const auto* r = world.Registry.try_get<RenderableComponent>(e); r && r->ModelRef && r->ModelRef->MeshCount() > 0) {
            const AABB b = AABB{r->ModelRef->BoundsMin(), r->ModelRef->BoundsMax()}.Transformed(world.ComposeWorldTransform(e));
            mn = any ? glm::min(mn, b.Min) : b.Min;
            mx = any ? glm::max(mx, b.Max) : b.Max;
            any = true;
        }
        if (const auto* h = world.Registry.try_get<HierarchyComponent>(e))
            for (entt::entity c : h->Children) stack.push_back(c);
    }
    return any;
}

} // namespace

void EditorLayer::ClearAutoGrime(World& world) {
    std::vector<entt::entity> groups;
    for (auto [e, name] : world.Registry.view<const NameComponent>().each()) {
        if (name.Name.rfind("Grime (auto)", 0) != 0) continue;
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        if (h && h->Parent != entt::null) continue;
        groups.push_back(e);
    }
    if (groups.empty()) return;
    PushUndo(world, "Clear Auto Grime");
    for (entt::entity e : groups) {
        if (m_Selected == e) ClearSelection();
        world.DestroyEntityAndChildren(e);
    }
    EditorSound::Play(EditorSound::Event::DecalErase);
    DecalEd().GrimeStatus = "Cleared.";
}

void EditorLayer::RunAutoGrime(World& world) {
    DecalEditorState& ed = DecalEd();
    BeginScan(ed);
    ScanStep(ed);
    if (!HasAnySelection()) { ed.GrimeStatus = "Select a room first."; EditorSound::Play(EditorSound::Event::Blocked); return; }

    // The palette: the active one, else the first the project has.
    const DecalPalette* palette = ed.ActivePalette >= 0 ? ed.Palettes[ed.ActivePalette].get()
                                : ed.Palettes.empty() ? nullptr : ed.Palettes.front().get();
    if (!palette || palette->Entries.empty()) { ed.GrimeStatus = "No decal palette: pick one on the strip."; EditorSound::Play(EditorSound::Event::Blocked); return; }

    glm::vec3 mn(0.0f), mx(0.0f);
    bool any = false;
    for (entt::entity e : GetSelectedItems()) SubtreeBounds(world, e, mn, mx, any);
    if (!any || glm::length(mx - mn) < 1.0f) { ed.GrimeStatus = "The selection has no room-sized geometry."; EditorSound::Play(EditorSound::Event::Blocked); return; }

    EditorSound::Play(EditorSound::Event::AutoGrimeStart);
    PushUndo(world, "Auto Grime");
    if (ed.GrimeReplace) {
        std::vector<entt::entity> old;
        for (auto [e, name] : world.Registry.view<const NameComponent>().each()) {
            if (name.Name.rfind("Grime (auto)", 0) != 0) continue;
            const auto* h = world.Registry.try_get<HierarchyComponent>(e);
            if (!h || h->Parent == entt::null) old.push_back(e);
        }
        for (entt::entity e : old) world.DestroyEntityAndChildren(e);
    }

    const CastContext ctx{this, &world};
    DecalTools::GrimeProbe probe;
    probe.Cast = &CastThunk;
    probe.User = const_cast<CastContext*>(&ctx);
    DecalTools::GrimeSettings gs = ed.Grime;
    gs.Kinds = ed.GrimeKinds;
    gs.Density *= palette->Density; // a heavy palette (Squat) asks for more sites
    const DecalTools::GrimeSummary sum = DecalTools::PlanGrime(probe, mn, mx, gs, (uint32_t)ed.GrimeSeed, ed.GrimeSites);

    const entt::entity group = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Grime (auto)");
    int placed = 0, byKind[8] = {};
    for (size_t i = 0; i < ed.GrimeSites.size(); ++i) {
        const DecalTools::GrimeSite& site = ed.GrimeSites[i];
        DecalTools::Rng rng(DecalTools::Mix((uint32_t)ed.GrimeSeed, 0x4000u + (uint32_t)i));
        const int pick = palette->Pick(rng.Unit(), site.Kind);
        if (pick < 0) continue;
        const DecalPaletteEntry& entry = palette->Entries[pick];
        const std::string path = ProjectPaths::Resolve(entry.DecalPath);
        if (!InfoFor(ed, path).Ok) continue;
        // Floor, ceiling and open wall spin freely; a base, drip or frame keeps near upright.
        const bool free = site.Kind & (DecalSites::Floor | DecalSites::Ceiling | DecalSites::Wall);
        const float spin = rng.Signed() * (free ? IM_PI : site.Kind == DecalSites::Corner ? 0.5f : 0.26f);
        SpawnDecal(world, ed, path, site.Point, site.Normal, site.Along, spin, rng.Range(entry.Scale.x, entry.Scale.y) * site.Scale,
                   rng.Range(entry.Opacity.x, entry.Opacity.y), entry.Tint, (int)(rng.Next() & 0x7FFFFFFF), group);
        ++placed;
        for (int k = 0; k < 8; ++k) byKind[k] += site.Kind == (1u << k);
    }
    SelectItem(group, false);

    char status[200];
    std::snprintf(status, sizeof(status), "%d decals from %s: %d bases, %d walls, %d corners, %d windows, %d doors, %d pipes, %d ceiling, %d floor (%d rays, floor %.2f m, ceiling %.2f m).",
                  placed, palette->Name.c_str(), byKind[1], byKind[0], byKind[2], byKind[3], byKind[4], byKind[5], byKind[6], byKind[7],
                  sum.Rays, sum.FloorY, sum.CeilingY);
    ed.GrimeStatus = status;
    Log::Info(std::string("Auto-grime: ") + status);
    EditorSound::Play(EditorSound::Event::AutoGrimeDone);
}
