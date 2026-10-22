// Motion v2, group B: the viewport's own motion. The selection outline's sweep (read by main.cpp's
// outline pass and by the icon rings), the snap ticks a gizmo drag leaves at the cursor, and the
// hold-Space radial menu. The gizmo hover springs, the orbiting view transitions and the ortho
// morph live with the code they change, in EditorLayer_Gizmos.cpp.
//
// Host only (Glass headers). Everything goes through Glass::Motion() and is gated on the Viewport
// category: off, or Reduce Motion, and it all reads as the static look (the outline whole, no
// ticks drawn, the menu appearing at once). Nothing here holds an entry at rest.

#include "EditorLayer.h"
#include "EditorIcons.h"
#include "EditorSound.h"
#include "World.h"
#include "Camera.h"
#include "Glass/GlassMotion.h"
#include "Glass/GlassTheme.h" // Px, colours

#include <imgui.h>
#include <imgui_internal.h> // ImHashStr, BringWindowToDisplayFront
#include <IconsFontAwesome6.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr Glass::MotionCategory kViewport = Glass::MotionCategory::Viewport;

ImU32 White(float a) { return IM_COL32(255, 255, 255, (int)(ImClamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)); }
ImU32 Tone(ImVec4 c, float a) { c.w *= ImClamp(a, 0.0f, 1.0f); return ImGui::ColorConvertFloat4ToU32(c); }

// The radial menu's items, clockwise from the top: the tools across the top, the toggles down the
// right, the view down the left.
enum RadialItem : int { RMove, RRotate, RScale, RRect, RSnap, RSpace, RFrame, RProjection, RShading, RHand, RCount };

// A full circle of radius `r` about the origin, starting at the left and running clockwise on
// screen (`cw`) or counter-clockwise, defined once per radius (in pixels: a UI-scale or font
// change asks for a new radius and gets a new path). Labels on the top half ride the clockwise
// ring and labels on the bottom half the counter-clockwise one, so both read left to right.
ImGuiID RingPath(float r, bool cw) {
    const ImGuiID id = ImHashStr(cw ? "##radialRingCW" : "##radialRingCCW") + (ImGuiID)std::lround(r * 4.0f);
    if (!Glass::ImAnimContext::PathDefined(id)) {
        const float k = 0.5522847f * r; // quarter-circle cubic handles
        using S = Glass::PathSeg;
        const S cwSegs[4] = {
            {S::Cubic, ImVec2(-r, -k), ImVec2(-k, -r), ImVec2(0, -r)},
            {S::Cubic, ImVec2(k, -r),  ImVec2(r, -k),  ImVec2(r, 0)},
            {S::Cubic, ImVec2(r, k),   ImVec2(k, r),   ImVec2(0, r)},
            {S::Cubic, ImVec2(-k, r),  ImVec2(-r, k),  ImVec2(-r, 0)},
        };
        const S ccwSegs[4] = {
            {S::Cubic, ImVec2(-r, k),  ImVec2(-k, r),  ImVec2(0, r)},
            {S::Cubic, ImVec2(k, r),   ImVec2(r, k),   ImVec2(r, 0)},
            {S::Cubic, ImVec2(r, -k),  ImVec2(k, -r),  ImVec2(0, -r)},
            {S::Cubic, ImVec2(-k, -r), ImVec2(-r, -k), ImVec2(-r, 0)},
        };
        Glass::ImAnimContext::DefinePath(id, ImVec2(-r, 0), cw ? cwSegs : ccwSegs, 4);
    }
    return id;
}

float Wrap2Pi(float a) {
    a = std::fmod(a, 2.0f * IM_PI);
    return a < 0.0f ? a + 2.0f * IM_PI : a;
}

} // namespace

// --- Selection outline sweep -----------------------------------------------------------------------

float EditorLayer::SelectionOutlineReveal() {
    Glass::ImAnimContext& motion = Glass::Motion();
    static const ImGuiID kSweepId = ImHashStr("##outlineSweep");
    const int frame = ImGui::GetFrameCount();
    if (frame != m_OutlineSweepFrame) {
        m_OutlineSweepFrame = frame;
        const uint64_t sig = m_Selected == entt::null ? 0u
            : ((uint64_t)entt::to_integral(m_Selected) + 1u) | ((uint64_t)m_ExtraSelection.size() << 32);
        if (sig != m_OutlineSweepSig) {
            m_OutlineSweepSig = sig;
            if (sig != 0u && motion.CategoryOn(kViewport)) motion.Pulse(kSweepId, Glass::OutlineSweep.durationMs * 0.001f);
        }
    }
    if (!motion.CategoryOn(kViewport) || !motion.PulseActive(kSweepId)) return 1.0f;
    return ImClamp(Glass::EaseProgress(Glass::OutlineSweep, motion.PulseProgress(kSweepId)), 0.0f, 1.0f);
}

// --- Snap ticks -------------------------------------------------------------------------------------

static const ImGuiID kSnapTickId = ImHashStr("##snapTick");

void EditorLayer::NoteGizmoSnapStep(const glm::vec3& value, bool started, int decimals, const char* suffix) {
    if (started) { m_SnapTickLast = value; return; }
    const glm::vec3 d = value - m_SnapTickLast;
    int axis = 0;
    for (int a = 1; a < 3; ++a) if (std::fabs(d[a]) > std::fabs(d[axis])) axis = a;
    // A snapped drag only moves in whole steps, so any change is a step (or several in one frame).
    if (std::fabs(d[axis]) <= 1e-3f) return;
    m_SnapTickLast = value;

    const ImVec2 mp = ImGui::GetIO().MousePos;
    const double nowMs = ImGui::GetTime() * 1000.0;
    if (nowMs - m_SnapTickSoundAt >= (double)Glass::SnapTickMinMs) {
        m_SnapTickSoundAt = nowMs;
        const float x01 = mp.x / ImMax(1.0f, ImGui::GetIO().DisplaySize.x);
        EditorSound::Play(EditorSound::Event::MotionSnapTick, x01, d[axis] > 0.0f ? 1.06f : 0.94f);
    }
    Glass::ImAnimContext& motion = Glass::Motion();
    if (!motion.CategoryOn(kViewport)) return;
    const int slot = m_SnapTickNext;
    m_SnapTickNext = (m_SnapTickNext + 1) % kSnapTickSlots;
    SnapTick& tick = m_SnapTicks[slot];
    tick.X = mp.x;
    tick.Y = mp.y;
    // Scale reads as a factor (no sign); deltas carry theirs.
    if (suffix && suffix[0] == 'x') std::snprintf(tick.Text, sizeof(tick.Text), "%.*f%s", decimals, value[axis], suffix);
    else std::snprintf(tick.Text, sizeof(tick.Text), "%+.*f%s", decimals, value[axis], suffix ? suffix : "");
    motion.Pulse(kSnapTickId + (ImGuiID)slot, Glass::SnapPop.durationMs * 0.001f);
}

void EditorLayer::DrawSnapTicks() {
    Glass::ImAnimContext& motion = Glass::Motion();
    if (!motion.CategoryOn(kViewport)) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    for (int i = 0; i < kSnapTickSlots; ++i) {
        const ImGuiID id = kSnapTickId + (ImGuiID)i;
        if (!motion.PulseActive(id)) continue;
        const float p = motion.PulseProgress(id);
        if (p >= 1.0f) continue;
        // The end-of-drag snap pop, small: a ring springing out from 60% and fading, and the snapped
        // number lifting away beside it.
        const SnapTick& tick = m_SnapTicks[i];
        const float s = Glass::EaseProgress(Glass::SnapPop, p);
        const float a = 1.0f - Glass::EaseProgress(Glass::HoverIn, p);
        const float r = Glass::Px(Glass::SnapTickRingPx) * (Glass::SnapRingFromScale + (Glass::SnapRingToScale - Glass::SnapRingFromScale) * s);
        const ImVec2 c(tick.X, tick.Y);
        dl->AddCircle(c, r, White(0.85f * a), 0, Glass::Px(1.5f));
        const ImVec2 tp(c.x + r + Glass::Px(4.0f), c.y - lineH - Glass::Px(6.0f) * s);
        dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(0, 0, 0, (int)(160.0f * a)), tick.Text);
        dl->AddText(tp, White(a), tick.Text);
    }
}

// --- The radial menu -----------------------------------------------------------------------------
// Hold Space over the Scene view (edit mode) and a ring of the viewport's quick actions blooms
// around the pointer: the tools, snap, local / world, frame, projection and wireframe. Point at a
// wedge and let go of Space (or click) to run it, exactly as its shortcut or rail button would;
// let go in the centre, or press Esc, to cancel. A quick tap does nothing. Space is free in the
// Scene view; text fields (the Script IDE's Ctrl+Space among them) and Play mode never see this.

void EditorLayer::RunRadialItem(World& world, Camera& editorCamera, int item) {
    switch (item) {
        case RMove:   m_GizmoOp = GizmoOp::Translate; m_HandTool = false; m_MeasureTool = false; m_DecalTool = DecalToolMode::None; break;
        case RRotate: m_GizmoOp = GizmoOp::Rotate;    m_HandTool = false; m_MeasureTool = false; m_DecalTool = DecalToolMode::None; break;
        case RScale:  m_GizmoOp = GizmoOp::Scale;     m_HandTool = false; m_MeasureTool = false; m_DecalTool = DecalToolMode::None; break;
        case RRect:   m_GizmoOp = GizmoOp::Rect;      m_HandTool = false; m_MeasureTool = false; m_DecalTool = DecalToolMode::None; break;
        case RHand:   m_HandTool = true; m_MeasureTool = false; m_DecalTool = DecalToolMode::None; break;
        case RSnap:   m_GridSnapEnabled = !m_GridSnapEnabled; break;
        case RSpace:  SetGizmoLocalSpace(!GizmoLocalSpace()); break;
        case RFrame:  if (HasAnySelection()) FocusOnSelection(world, editorCamera); break;
        case RProjection: ToggleOrthographic(world, editorCamera); break;
        case RShading: SetShadingModeIndex(ShadingModeIndex() == 1 ? 0 : 1); break; // Shaded <-> Wireframe
        default: break;
    }
}

void EditorLayer::UpdateRadialMenu(World& world, Camera& editorCamera, bool sceneHovered) {
    ImGuiIO& io = ImGui::GetIO();
    Glass::ImAnimContext& motion = Glass::Motion();
    const bool animate = motion.CategoryOn(kViewport);
    static const ImGuiID kBloomId = ImHashStr("##radialBloom");
    static const ImGuiID kStaggerId = ImHashStr("##radialStagger");
    static const ImGuiID kWashId = ImHashStr("##radialWash");

    const float R = Glass::Px(Glass::RadialRadiusPx);
    const float rIn = R * 0.38f;                 // inside this: cancel
    const float rOut = R + Glass::Px(20.0f);
    const float rIcon = R - Glass::Px(16.0f);
    const float rLabel = R + Glass::Px(4.0f);
    const double now = ImGui::GetTime();
    const bool spaceDown = ImGui::IsKeyDown(ImGuiKey_Space);
    const bool eligible = !m_InPlayMode && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper &&
                          !ImGuizmo::IsUsing() && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsAnyItemActive();

    // Arm on the press; open once held. A release before then was a tap: nothing happens.
    if (!m_Radial.Armed && !m_Radial.Open && sceneHovered && eligible && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        m_Radial.Armed = true;
        m_Radial.ArmedAt = now;
    }
    if (m_Radial.Armed && !m_Radial.Open) {
        if (!spaceDown || !eligible) {
            m_Radial.Armed = false;
        } else if ((now - m_Radial.ArmedAt) * 1000.0 >= (double)Glass::RadialHoldMs) {
            m_Radial.Open = true;
            m_Radial.Hovered = -1;
            m_Radial.Picked = -1;
            // With the Decal Brush active the ring is its sources instead: palettes, categories, recents.
            m_Radial.Decals = m_DecalTool == DecalToolMode::Brush && (m_Radial.Count = DecalRadialBuild()) >= 2;
            if (!m_Radial.Decals) m_Radial.Count = RCount;
            // Centred on the pointer, nudged in so the ring fits the view when it can.
            const float fit = rOut + Glass::Px(Glass::Space1);
            ImVec2 c = io.MousePos;
            if (m_ViewportSize.x > fit * 2.0f) c.x = ImClamp(c.x, m_ViewportPos.x + fit, m_ViewportPos.x + m_ViewportSize.x - fit);
            if (m_ViewportSize.y > fit * 2.0f) c.y = ImClamp(c.y, m_ViewportPos.y + fit, m_ViewportPos.y + m_ViewportSize.y - fit);
            m_Radial.Center = c;
            const float x01 = c.x / ImMax(1.0f, io.DisplaySize.x);
            EditorSound::Play(EditorSound::Event::RadialOpen, x01);
            if (animate) {
                const float total = Glass::StaggerDelay(m_Radial.Count - 1, Glass::RadialStaggerMs, m_Radial.Count) + Glass::RadialOpen.durationMs * 0.001f;
                motion.Pulse(kStaggerId, total);
            }
        }
    }

    // The ring's size is fixed while it is open (its decal slots are built when it opens).
    const int count = m_Radial.Count;
    const float step = 2.0f * IM_PI / (float)count;
    const bool decalRing = m_Radial.Decals;
    auto itemEnabled = [&](int i) { return decalRing || i != RFrame || HasAnySelection(); };

    if (m_Radial.Open) {
        const ImVec2 d(io.MousePos.x - m_Radial.Center.x, io.MousePos.y - m_Radial.Center.y);
        const float dist = std::sqrt(d.x * d.x + d.y * d.y);
        m_Radial.Hovered = -1;
        if (dist >= rIn) { // past the rim still counts: aim, don't steer
            const float rel = Wrap2Pi(std::atan2(d.y, d.x) + IM_PI * 0.5f + step * 0.5f);
            m_Radial.Hovered = ImClamp((int)(rel / step), 0, count - 1);
        }
        const bool cancel = m_InPlayMode || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        const bool commit = !spaceDown || ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (cancel || commit) {
            m_Radial.Open = false;
            m_Radial.Armed = false;
            const float x01 = m_Radial.Center.x / ImMax(1.0f, io.DisplaySize.x);
            const int pick = cancel ? -1 : m_Radial.Hovered;
            if (pick >= 0 && itemEnabled(pick)) {
                m_Radial.Picked = pick;
                EditorSound::Play(EditorSound::Event::RadialPick, x01);
                if (decalRing) RunDecalRadialItem(pick);
                else RunRadialItem(world, editorCamera, pick);
            } else {
                m_Radial.Picked = -1;
                EditorSound::Play(EditorSound::Event::RadialClose, x01);
            }
        }
    }

    // --- Draw: blooms open on the RadialOpen spring (scale + alpha), fades out quickly ------------
    const bool open = m_Radial.Open;
    const bool wasOpen = m_Radial.WasOpen;
    m_Radial.WasOpen = open;
    const float t = animate ? motion.AnimateToggle(kBloomId, open, wasOpen, Glass::RadialOpen, Glass::RadialItem) : (open ? 1.0f : 0.0f);
    if (t <= 0.0f) return;

    const ImVec2 c = m_Radial.Center;
    const float pad = Glass::Px(Glass::Space2);
    ImGui::SetNextWindowPos(ImVec2(c.x - rOut - pad, c.y - rOut - pad));
    ImGui::SetNextWindowSize(ImVec2((rOut + pad) * 2.0f, (rOut + pad) * 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##RadialMenu", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int layer = Glass::ImAnimContext::BeginLayer(dl);

    // The plate: glass in the HUDs' opaque fallback (the frosted blur belongs to the Glass HUD pass),
    // a soft shadow, the 15% white rim and a hairline round the cancel hub.
    for (int i = 3; i >= 1; --i)
        dl->AddCircleFilled(ImVec2(c.x, c.y + Glass::Px(2.0f)), rOut + Glass::Px(3.0f) * (float)i,
                            Tone(Glass::Shadow, 0.10f * (float)(4 - i)), 64);
    dl->AddCircleFilled(c, rOut, Tone(Glass::GlassFallback, 0.94f), 64);
    dl->AddCircle(c, rOut, Tone(Glass::GlassEdge, 1.0f), 64, 1.0f);
    dl->AddCircle(c, rIn, Tone(Glass::Separator, 1.0f), 48, 1.0f);
    for (int i = 0; i < count; ++i) { // wedge dividers
        const float a = -IM_PI * 0.5f + ((float)i + 0.5f) * step;
        const ImVec2 u(std::cos(a), std::sin(a));
        dl->AddLine(ImVec2(c.x + u.x * (rIn + Glass::Px(4.0f)), c.y + u.y * (rIn + Glass::Px(4.0f))),
                    ImVec2(c.x + u.x * (rOut - Glass::Px(4.0f)), c.y + u.y * (rOut - Glass::Px(4.0f))), White(0.05f), 1.0f);
    }
    // The hub: an X, lit while the pointer rests there (letting go there cancels).
    {
        const bool hubHot = open && m_Radial.Hovered < 0;
        const ImVec2 xs = ImGui::CalcTextSize(ICON_FA_XMARK);
        dl->AddText(ImVec2(c.x - xs.x * 0.5f, c.y - xs.y * 0.5f), Tone(hubHot ? Glass::TextPrimary : Glass::TextTertiary, 1.0f), ICON_FA_XMARK);
    }

    const float staggerElapsed = animate ? motion.PulseElapsed(kStaggerId, 1.0e3f) : 1.0e3f;
    const float fs = ImGui::GetFontSize();
    const ImGuiID pathCW = RingPath(rLabel - fs * 0.3f, true);
    const ImGuiID pathCCW = RingPath(rLabel + fs * 0.3f, false);
    const float lenCW = 2.0f * IM_PI * (rLabel - fs * 0.3f), lenCCW = 2.0f * IM_PI * (rLabel + fs * 0.3f);
    const int gizmoOp = (int)m_GizmoOp;

    for (int i = 0; i < count; ++i) {
        const char* icon = "";
        const char* label = "";
        bool on = false;
        if (decalRing) on = DecalRadialItem(i, icon, label);
        else switch (i) {
            case RMove:   icon = EDITOR_ICON_TRANSLATE; label = "Move";   on = !m_HandTool && gizmoOp == 0; break;
            case RRotate: icon = EDITOR_ICON_ROTATE;    label = "Rotate"; on = !m_HandTool && gizmoOp == 1; break;
            case RScale:  icon = EDITOR_ICON_SCALE;     label = "Scale";  on = !m_HandTool && gizmoOp == 2; break;
            case RRect:   icon = EDITOR_ICON_RECT_TOOL; label = "Rect";   on = !m_HandTool && gizmoOp == 3; break;
            case RSnap:   icon = EDITOR_ICON_SNAP_TO_GRID; label = "Snap"; on = m_GridSnapEnabled; break;
            case RSpace:  icon = m_GizmoLocalSpace ? EDITOR_ICON_LOCAL_SPACE : EDITOR_ICON_WORLD_SPACE;
                          label = m_GizmoLocalSpace ? "Local" : "World"; break;
            case RFrame:  icon = ICON_FA_CROSSHAIRS; label = "Frame"; break;
            case RProjection: icon = editorCamera.Orthographic ? EDITOR_ICON_ORTHOGRAPHIC : EDITOR_ICON_PERSPECTIVE;
                              label = editorCamera.Orthographic ? "Ortho" : "Persp"; on = editorCamera.Orthographic; break;
            case RShading: icon = EDITOR_ICON_WIREFRAME_MODE; label = "Wire"; on = ShadingModeIndex() == 1; break;
            case RHand:   icon = EDITOR_ICON_HAND_TOOL; label = "Hand"; on = m_HandTool; break;
            default: break;
        }
        const bool enabled = itemEnabled(i);
        const float ang = -IM_PI * 0.5f + (float)i * step;
        const ImVec2 u(std::cos(ang), std::sin(ang));

        // Items bloom out one after another (scale out from the hub + fade).
        float p = 1.0f;
        if (staggerElapsed < 1.0e2f) {
            const float delay = Glass::StaggerDelay(i, Glass::RadialStaggerMs, count);
            p = Glass::EaseProgress(Glass::RadialOpen, (staggerElapsed - delay) / (Glass::RadialOpen.durationMs * 0.001f));
        }
        const float pa = ImClamp(p, 0.0f, 1.0f);

        // The hovered wedge: a white wash easing in (RadialItem); the picked one holds it as the
        // menu fades.
        const bool hot = (open && m_Radial.Hovered == i && enabled) || (!open && m_Radial.Picked == i);
        const float wash = animate ? motion.Animate(kWashId + (ImGuiID)i, hot ? 1.0f : 0.0f, 0.0f, Glass::RadialItem) : (hot ? 1.0f : 0.0f);
        if (wash > 0.0f) {
            const float gap = Glass::Px(2.0f) / rOut;
            dl->PathArcTo(c, (rIn + rOut) * 0.5f, ang - step * 0.5f + gap, ang + step * 0.5f - gap, 16);
            dl->PathStroke(White(0.13f * wash), ImDrawFlags_None, rOut - rIn - Glass::Px(4.0f));
            dl->PathArcTo(c, rOut - Glass::Px(2.0f), ang - step * 0.5f + gap * 2.0f, ang + step * 0.5f - gap * 2.0f, 16);
            dl->PathStroke(White(0.55f * wash), ImDrawFlags_None, Glass::Px(1.5f));
        }

        ImVec4 tone = !enabled ? Glass::TextTertiary : Glass::MixOklab(on ? Glass::TextPrimary : Glass::TextSecondary, Glass::AccentBright, wash);
        const float ri = rIcon * (0.55f + 0.45f * p);
        const ImVec2 is = ImGui::CalcTextSize(icon);
        dl->AddText(ImVec2(c.x + u.x * ri - is.x * 0.5f, c.y + u.y * ri - is.y * 0.5f), Tone(tone, pa), icon);
        if (on) {
            const float rd = rIn + Glass::Px(7.0f);
            dl->AddCircleFilled(ImVec2(c.x + u.x * rd, c.y + u.y * rd), Glass::Px(2.0f), Tone(Glass::AccentBright, pa));
        }

        // The label follows the ring (TextOnPath), centred on the wedge.
        const bool lower = u.y > 0.05f;
        // Never wider than the wedge's arc, so neighbours can't collide: cut with a dot.
        const float arcW = 2.0f * IM_PI * rLabel / (float)count - Glass::Px(6.0f);
        char fitted[32];
        std::snprintf(fitted, sizeof(fitted), "%s", label);
        for (size_t n = std::strlen(fitted); n > 2 && ImGui::CalcTextSize(fitted).x > arcW; ) {
            --n;
            fitted[n - 1] = '.';
            fitted[n] = 0;
        }
        label = fitted;
        const float tw = ImGui::CalcTextSize(label).x;
        const float s = lower ? Wrap2Pi(IM_PI - ang) * (lenCCW / (2.0f * IM_PI)) : Wrap2Pi(ang - IM_PI) * (lenCW / (2.0f * IM_PI));
        Glass::ImAnimContext::TextOnPath(lower ? pathCCW : pathCW, label, c, Tone(tone, pa * (enabled ? 1.0f : 0.7f)),
                                         ImMax(0.0f, s - tw * 0.5f), false);
    }

    Glass::LayerTransform xf;
    xf.pivot = c;
    const float sc = 0.82f + 0.18f * t; // the spring's overshoot reads as a bloom
    xf.scale = ImVec2(sc, sc);
    xf.alpha = ImClamp(t, 0.0f, 1.0f);
    Glass::ImAnimContext::EndLayer(dl, layer, xf);
    ImGui::End();
    ImGui::PopStyleVar(2);
}
