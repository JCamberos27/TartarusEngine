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
#include "RadialLayout.h"
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
#include <string>

namespace {

constexpr Glass::MotionCategory kViewport = Glass::MotionCategory::Viewport;

ImU32 White(float a) { return IM_COL32(255, 255, 255, (int)(ImClamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)); }
ImU32 Tone(ImVec4 c, float a) { c.w *= ImClamp(a, 0.0f, 1.0f); return ImGui::ColorConvertFloat4ToU32(c); }

// The radial menu's items, clockwise from the top: the tools across the top, the toggles down the
// right, the view down the left.
enum RadialItem : int { RMove, RRotate, RScale, RRect, RSnap, RSpace, RFrame, RProjection, RShading, RHand, RCount };

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
    const float rBlock = R * 0.78f;              // the icon + label block centre, mid-ring
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
            // With Spray in Scene view on, the Graffiti Designer's tools, colours and sizes.
            m_Radial.GraffitiRing = GraffitiViewportSprayActive() && (m_Radial.Count = GraffitiRadialBuild()) >= 2;
            m_Radial.Decals = !m_Radial.GraffitiRing && m_DecalTool == DecalToolMode::Brush && (m_Radial.Count = DecalRadialBuild()) >= 2;
            if (!m_Radial.Decals && !m_Radial.GraffitiRing) m_Radial.Count = RCount;
            // Centred on the pointer, nudged in so the ring fits the view when it can.
            const float fit = rOut + Glass::Px(Glass::Space1);
            ImVec2 c = io.MousePos;
            if (m_ViewportSize.x > fit * 2.0f) c.x = ImClamp(c.x, m_ViewportPos.x + fit, m_ViewportPos.x + m_ViewportSize.x - fit);
            if (m_ViewportSize.y > fit * 2.0f) c.y = ImClamp(c.y, m_ViewportPos.y + fit, m_ViewportPos.y + m_ViewportSize.y - fit);
            m_Radial.Center = RadialLayout::Snap(c); // whole pixels, so every glyph lands on the grid
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
    const bool graffitiRing = m_Radial.GraffitiRing;
    const bool decalRing = m_Radial.Decals || graffitiRing; // a ring of dynamic slots, not the tools
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
                if (graffitiRing) RunGraffitiRadialItem(pick);
                else if (decalRing) RunDecalRadialItem(pick);
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

    const float staggerElapsed = animate ? motion.PulseElapsed(kStaggerId, 1.0e3f) : 1.0e3f;
    const int gizmoOp = (int)m_GizmoOp;
    const bool showLabels = RadialLayout::ShowLabels(count);
    // Arcs get a segment per few pixels of their length, so a big radius or UI scale stays round.
    auto arcSegs = [&](float r) { return ImMax(12, (int)std::ceil(r * step / Glass::Px(5.0f))); };

    // One slot's icon, label, lit state and swatch.
    struct Slot { const char* icon = ""; const char* label = ""; bool on = false; ImU32 dot = 0; };
    auto slotAt = [&](int i) {
        Slot sl;
        if (graffitiRing) sl.on = GraffitiRadialItem(i, sl.icon, sl.label, sl.dot);
        else if (decalRing) sl.on = DecalRadialItem(i, sl.icon, sl.label);
        else switch (i) {
            case RMove:   sl.icon = EDITOR_ICON_TRANSLATE; sl.label = "Move";   sl.on = !m_HandTool && gizmoOp == 0; break;
            case RRotate: sl.icon = EDITOR_ICON_ROTATE;    sl.label = "Rotate"; sl.on = !m_HandTool && gizmoOp == 1; break;
            case RScale:  sl.icon = EDITOR_ICON_SCALE;     sl.label = "Scale";  sl.on = !m_HandTool && gizmoOp == 2; break;
            case RRect:   sl.icon = EDITOR_ICON_RECT_TOOL; sl.label = "Rect";   sl.on = !m_HandTool && gizmoOp == 3; break;
            case RSnap:   sl.icon = EDITOR_ICON_SNAP_TO_GRID; sl.label = "Snap"; sl.on = m_GridSnapEnabled; break;
            case RSpace:  sl.icon = m_GizmoLocalSpace ? EDITOR_ICON_LOCAL_SPACE : EDITOR_ICON_WORLD_SPACE;
                          sl.label = m_GizmoLocalSpace ? "Local" : "World"; break;
            case RFrame:  sl.icon = ICON_FA_CROSSHAIRS; sl.label = "Frame"; break;
            case RProjection: sl.icon = editorCamera.Orthographic ? EDITOR_ICON_ORTHOGRAPHIC : EDITOR_ICON_PERSPECTIVE;
                              sl.label = editorCamera.Orthographic ? "Ortho" : "Persp"; sl.on = editorCamera.Orthographic; break;
            case RShading: sl.icon = EDITOR_ICON_WIREFRAME_MODE; sl.label = "Wire"; sl.on = ShadingModeIndex() == 1; break;
            case RHand:   sl.icon = EDITOR_ICON_HAND_TOOL; sl.label = "Hand"; sl.on = m_HandTool; break;
            default: break;
        }
        return sl;
    };
    // Items bloom out one after another (a radial slide + fade).
    auto itemProgress = [&](int i) {
        float p = 1.0f;
        if (staggerElapsed < 1.0e2f) {
            const float delay = Glass::StaggerDelay(i, Glass::RadialStaggerMs, count);
            p = Glass::EaseProgress(Glass::RadialOpen, (staggerElapsed - delay) / (Glass::RadialOpen.durationMs * 0.001f));
        }
        return p;
    };
    // The hovered wedge: a white wash easing in (RadialItem); the picked one holds it as the menu fades.
    auto itemWash = [&](int i) {
        const bool hot = (open && m_Radial.Hovered == i && itemEnabled(i)) || (!open && m_Radial.Picked == i);
        return animate ? motion.Animate(kWashId + (ImGuiID)i, hot ? 1.0f : 0.0f, 0.0f, Glass::RadialItem) : (hot ? 1.0f : 0.0f);
    };
    // A glyph's ink box (what is actually drawn, not its advance box), with its offset from the pen.
    auto inkOf = [&](const char* text, ImVec2& off) {
        off = ImVec2(0.0f, 0.0f);
        ImVec2 size = ImGui::CalcTextSize(text);
        unsigned int ch = 0;
        ImTextCharFromUtf8(&ch, text, nullptr);
        if (const ImFontGlyph* g = ch ? ImGui::GetFontBaked()->FindGlyph((ImWchar)ch) : nullptr) {
            off = ImVec2(g->X0, g->Y0);
            size = ImVec2(g->X1 - g->X0, g->Y1 - g->Y0);
        }
        return size;
    };
    // `text` cut with an ellipsis to fit `maxW` at the font's native size.
    auto fitText = [&](const char* text, float maxW) {
        std::string s = text;
        if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
        while (!s.empty()) {
            do s.pop_back(); while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80);
            if (ImGui::CalcTextSize((s + "\xE2\x80\xA6").c_str()).x <= maxW) break;
        }
        return s + "\xE2\x80\xA6";
    };
    const float gap = Glass::Px(3.0f);
    const float lineH = ImGui::GetFontSize();
    const float swatchPx = Glass::Px(16.0f);
    const float blockH = swatchPx + (showLabels ? gap + lineH : 0.0f);
    const float maxLabelW = RadialLayout::MaxLabelWidth(rBlock, blockH, count, Glass::Px(6.0f));

    // Pass 1, the geometry: it blooms (scale + alpha) on the spring. The text is drawn after, unscaled.
    const int geoLayer = Glass::ImAnimContext::BeginLayer(dl);

    // The plate: glass in the HUDs' opaque fallback (the frosted blur belongs to the Glass HUD pass),
    // a soft shadow, the 15% white rim and a hairline round the cancel hub.
    const int plateSegs = ImMax(64, arcSegs(rOut) * count);
    for (int i = 3; i >= 1; --i)
        dl->AddCircleFilled(ImVec2(c.x, c.y + Glass::Px(2.0f)), rOut + Glass::Px(3.0f) * (float)i,
                            Tone(Glass::Shadow, 0.10f * (float)(4 - i)), plateSegs);
    dl->AddCircleFilled(c, rOut, Tone(Glass::GlassFallback, 0.94f), plateSegs);
    dl->AddCircle(c, rOut, Tone(Glass::GlassEdge, 1.0f), plateSegs, 1.0f);
    dl->AddCircle(c, rIn, Tone(Glass::Separator, 1.0f), ImMax(48, plateSegs * 3 / 4), 1.0f);
    for (int i = 0; i < count; ++i) { // wedge dividers
        const float a = -IM_PI * 0.5f + ((float)i + 0.5f) * step;
        const ImVec2 u(std::cos(a), std::sin(a));
        dl->AddLine(ImVec2(c.x + u.x * (rIn + Glass::Px(4.0f)), c.y + u.y * (rIn + Glass::Px(4.0f))),
                    ImVec2(c.x + u.x * (rOut - Glass::Px(4.0f)), c.y + u.y * (rOut - Glass::Px(4.0f))), White(0.05f), 1.0f);
    }

    for (int i = 0; i < count; ++i) {
        const Slot sl = slotAt(i);
        const bool enabled = itemEnabled(i);
        const float ang = RadialLayout::WedgeAngle(i, count);
        const ImVec2 u(std::cos(ang), std::sin(ang));
        const float pa = ImClamp(itemProgress(i), 0.0f, 1.0f);
        const float wash = itemWash(i);
        if (wash > 0.0f) {
            const float g = Glass::Px(2.0f) / rOut;
            dl->PathArcTo(c, (rIn + rOut) * 0.5f, ang - step * 0.5f + g, ang + step * 0.5f - g, arcSegs((rIn + rOut) * 0.5f));
            dl->PathStroke(White(0.13f * wash), ImDrawFlags_None, rOut - rIn - Glass::Px(4.0f));
            dl->PathArcTo(c, rOut - Glass::Px(2.0f), ang - step * 0.5f + g * 2.0f, ang + step * 0.5f - g * 2.0f, arcSegs(rOut));
            dl->PathStroke(White(0.55f * wash), ImDrawFlags_None, Glass::Px(1.5f));
        }
        const float rb = rBlock * (0.55f + 0.45f * pa);
        ImVec2 off;
        const ImVec2 ink = sl.dot ? ImVec2(swatchPx, swatchPx) : inkOf(sl.icon, off);
        const RadialLayout::Block b = RadialLayout::Place(c, ang, rb, ink, ImVec2(0.0f, lineH), gap, showLabels && sl.label[0]);
        const ImVec2 ic(b.iconPos.x + ink.x * 0.5f, b.iconPos.y + ink.y * 0.5f);
        const ImVec4 tone = !enabled ? Glass::TextTertiary : Glass::MixOklab(sl.on ? Glass::TextPrimary : Glass::TextSecondary, Glass::AccentBright, wash);
        if (sl.dot) { // a colour slot: its swatch where the icon would be
            dl->AddCircleFilled(ic, Glass::Px(8.0f), (sl.dot & 0x00FFFFFFu) | ((ImU32)(255.0f * pa) << 24), 20);
            dl->AddCircle(ic, Glass::Px(8.0f), Tone(tone, pa), 20, 1.5f);
        } else if (wash > 0.0f) { // a soft ring round the lit icon
            dl->AddCircle(ic, ImMax(ink.x, ink.y) * 0.5f + Glass::Px(7.0f), White(0.32f * wash * pa), 24, 1.0f);
        }
        if (sl.on) {
            const float rd = rIn + Glass::Px(7.0f);
            dl->AddCircleFilled(ImVec2(c.x + u.x * rd, c.y + u.y * rd), Glass::Px(2.0f), Tone(Glass::AccentBright, pa));
        }
    }

    Glass::LayerTransform xf;
    xf.pivot = c;
    const float sc = 0.82f + 0.18f * t; // the spring's overshoot reads as a bloom
    xf.scale = ImVec2(sc, sc);
    xf.alpha = ImClamp(t, 0.0f, 1.0f);
    Glass::ImAnimContext::EndLayer(dl, geoLayer, xf);

    // Pass 2, the text: it only fades (never scaled, so glyphs stay on whole pixels), each block
    // sliding out from the hub in whole-pixel steps.
    const int textLayer = Glass::ImAnimContext::BeginLayer(dl);
    for (int i = 0; i < count; ++i) {
        const Slot sl = slotAt(i);
        const bool enabled = itemEnabled(i);
        const float ang = RadialLayout::WedgeAngle(i, count);
        const float pa = ImClamp(itemProgress(i), 0.0f, 1.0f);
        const float wash = itemWash(i);
        const ImVec4 tone = !enabled ? Glass::TextTertiary : Glass::MixOklab(sl.on ? Glass::TextPrimary : Glass::TextSecondary, Glass::AccentBright, wash);
        const float rb = rBlock * (0.88f + 0.12f * pa);
        const bool hasLabel = showLabels && sl.label[0];
        const std::string label = hasLabel ? fitText(sl.label, maxLabelW) : std::string();
        ImVec2 off;
        const ImVec2 ink = sl.dot ? ImVec2(swatchPx, swatchPx) : inkOf(sl.icon, off);
        const ImVec2 ls = hasLabel ? ImGui::CalcTextSize(label.c_str()) : ImVec2(0.0f, 0.0f);
        const RadialLayout::Block b = RadialLayout::Place(c, ang, rb, ink, ls, gap, hasLabel);
        if (!sl.dot)
            dl->AddText(RadialLayout::Snap(ImVec2(b.iconPos.x - off.x, b.iconPos.y - off.y)), Tone(tone, pa), sl.icon);
        if (hasLabel) { // hovered: the label comes up to full white
            const ImVec4 lt = enabled ? Glass::MixOklab(sl.on ? Glass::TextPrimary : Glass::TextSecondary, Glass::TextPrimary, wash) : Glass::TextTertiary;
            dl->AddText(b.labelPos, Tone(lt, pa * (enabled ? 1.0f : 0.7f)), label.c_str());
        }
    }
    // The hub: an X, lit while the pointer rests there (letting go there cancels). Past 8 wedges
    // there is no room for labels, so the hovered slot's whole name is shown here instead.
    {
        const bool hubHot = open && m_Radial.Hovered < 0;
        const int hv = m_Radial.Hovered;
        const char* full = "";
        if (open && hv >= 0) {
            if (decalRing) full = graffitiRing ? GraffitiRadialFullName(hv) : DecalRadialFullName(hv);
            else if (!showLabels) full = slotAt(hv).label;
        }
        if (full[0]) {
            const std::string name = fitText(full, rIn * 2.0f - Glass::Px(8.0f));
            const ImVec2 ts = ImGui::CalcTextSize(name.c_str());
            dl->AddText(RadialLayout::Snap(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f)), Tone(Glass::TextPrimary, 1.0f), name.c_str());
        } else {
            ImVec2 off;
            const ImVec2 ink = inkOf(ICON_FA_XMARK, off);
            dl->AddText(RadialLayout::Snap(ImVec2(c.x - ink.x * 0.5f - off.x, c.y - ink.y * 0.5f - off.y)),
                        Tone(hubHot ? Glass::TextPrimary : Glass::TextTertiary, 1.0f), ICON_FA_XMARK);
        }
    }
    Glass::LayerTransform tx;
    tx.alpha = ImClamp(t, 0.0f, 1.0f);
    Glass::ImAnimContext::EndLayer(dl, textLayer, tx);
    ImGui::End();
    ImGui::PopStyleVar(2);
}
