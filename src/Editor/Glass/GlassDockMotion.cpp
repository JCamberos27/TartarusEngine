#include "GlassDockMotion.h"

#include "GlassCompositor.h" // the startup sequence's per-panel timing
#include "GlassMotion.h"
#include "GlassTheme.h"      // Px
#include "GlassTokens.h"
#include "../EditorPanels.h"

#include <imgui_internal.h>

#include <cstring>

namespace Glass {

namespace {
constexpr ImGuiID kOverlineSalt = 0x0E71A1D3u;
constexpr ImGuiID kRimPulse     = 0x52494D31u; // "RIM1"

// Last frame's overline target per dock node (x relative to its tab bar, width). A handful of
// floats for the panels' own lifetime; the glide itself is a Motion() entry only while it moves.
ImGuiStorage& LastOverline() {
    static ImGuiStorage s_Store;
    return s_Store;
}

bool    s_PlayRimOn    = false; // the play rim is showing (DrawPlayFrame, last frame)
ImGuiID s_FocusedPanel = 0; // the docked panel that had focus last frame
ImGuiID s_RimPanel     = 0; // the one glinting

bool IsViewport(const ImGuiWindow* w) {
    return std::strstr(w->Name, "###Scene") != nullptr || std::strstr(w->Name, "###Game") != nullptr;
}
} // namespace

float BeginDockTabMotion() {
    const float size = ImGui::GetStyle().TabBarOverlineSize;
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, 0.0f);
    return size;
}

void EndDockTabMotion(ImGuiID dockspaceId, float size) {
    ImGui::PopStyleVar();
    if (size <= 0.0f) return;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiStyle& style = g.Style;
    ImGuiStorage& last = LastOverline();
    for (int n = 0; n < g.DockContext.Nodes.Data.Size; ++n) {
        ImGuiDockNode* node = (ImGuiDockNode*)g.DockContext.Nodes.Data[n].val_p;
        if (!node || !node->TabBar || !node->HostWindow || !node->IsVisible) continue;
        if (node->IsHiddenTabBar() || node->IsNoTabBar()) continue;
        if (ImGui::DockNodeGetRootNode(node)->ID != dockspaceId) continue;
        ImGuiTabBar* bar = node->TabBar;
        if (bar->CurrFrameVisible != g.FrameCount || !(bar->Flags & ImGuiTabBarFlags_DrawSelectedOverline)) continue;
        ImGuiTabItem* tab = ImGui::TabBarFindTabByID(bar, bar->VisibleTabId);
        if (!tab) continue;

        // The colour ImGui would have drawn: docked tabs take their window's dock style (the
        // Enhancers' tab tints live there), focused node or not.
        const int slot = node->IsFocused ? ImGuiWindowDockStyleCol_TabSelectedOverline : ImGuiWindowDockStyleCol_TabDimmedSelectedOverline;
        const ImU32 col = tab->Window ? tab->Window->DockStyle.Colors[slot]
                                      : ImGui::GetColorU32(node->IsFocused ? ImGuiCol_TabSelectedOverline : ImGuiCol_TabDimmedSelectedOverline);
        if ((col & IM_COL32_A_MASK) == 0) continue;

        const ImVec2 target(IM_TRUNC(tab->Offset - bar->ScrollingAnim), tab->Width);
        // Glide only when the selected tab changes. A reflow (a resize, a theme switch, a tab
        // added beside it) moves the line with the tab, so it never lags behind a dragged splitter.
        const int lastTab = last.GetInt(node->ID ^ 2u, 0);
        const bool switched = lastTab != 0 && lastTab != (int)bar->VisibleTabId;
        const ImVec2 prev = switched ? ImVec2(last.GetFloat(node->ID, target.x), last.GetFloat(node->ID ^ 1u, target.y)) : target;
        last.SetInt(node->ID ^ 2u, (int)bar->VisibleTabId);
        last.SetFloat(node->ID, target.x);
        last.SetFloat(node->ID ^ 1u, target.y);
        const ImVec2 v = Motion().Follow2(node->ID ^ kOverlineSalt, target, prev, SelectionGlide);

        // ImGui's overline path (imgui_widgets.cpp TabItemEx), at the glided rect.
        ImDrawList* dl = node->HostWindow->DrawList;
        dl->PushClipRect(bar->BarRect.Min, bar->BarRect.Max, false);
        const ImVec2 tl(bar->BarRect.Min.x + v.x, bar->BarRect.Min.y + 1.0f * g.CurrentDpiScale);
        const ImVec2 tr(tl.x + v.y, tl.y);
        if (style.TabRounding > 0.0f) {
            const float r = style.TabRounding;
            dl->PathArcToFast(tl + ImVec2(+r, +r), r, 7, 9);
            dl->PathArcToFast(tr + ImVec2(-r, +r), r, 9, 11);
            dl->PathStroke(col, 0, size);
        } else {
            dl->AddLine(tl - ImVec2(0.5f, 0.5f), tr - ImVec2(0.5f, 0.5f), col, size);
        }
        dl->PopClipRect();
    }
}

void DockFocusRim(ImGuiID dockspaceId) {
    ImGuiContext& gc = *ImGui::GetCurrentContext();
    ImGuiWindow* focused = gc.NavWindow ? gc.NavWindow->RootWindow : nullptr;
    const bool docked = focused && focused->DockIsActive && focused->DockNode &&
                        ImGui::DockNodeGetRootNode(focused->DockNode)->ID == dockspaceId;
    const ImGuiID id = docked ? focused->ID : 0;
    // Only a focus change the user made (a click, Ctrl+Tab) glints; code moving focus (a theme
    // switch, a panel opening itself) stays quiet.
    const ImGuiIO& io = gc.IO;
    const bool byUser = io.MouseClicked[0] || io.MouseClicked[1] || io.MouseClicked[2] || gc.NavWindowingTarget != nullptr ||
                        gc.NavWindowingTargetAnim != nullptr;
    if (id != 0 && id != s_FocusedPanel && s_FocusedPanel != 0 && byUser) {
        s_RimPanel = id;
        Motion().Pulse(kRimPulse, DockRimGlint.durationMs * 0.001f);
    }
    if (id != 0) s_FocusedPanel = id;

    // The selection rim: a faint white segment slowly travelling the focused panel's edge, so the
    // panel you're working in reads at a glance. Stateless (a function of the motion clock). Not on
    // the Scene and Game views while playing: their own play rim is there then.
    if (docked && !focused->Hidden && !focused->SkipItems && !(s_PlayRimOn && IsViewport(focused))) {
        const ImRect r = focused->Rect();
        if (r.GetWidth() > 24.0f && r.GetHeight() > 24.0f) {
            ImDrawList* dl = focused->DrawList;
            dl->PushClipRect(r.Min, r.Max, false);
            Motion().DrawTravellingRim(dl, r.Min + ImVec2(0.5f, 0.5f), r.Max - ImVec2(0.5f, 0.5f), SelectionRimSeconds,
                                       RimTravelArc, SelectionRimAlpha, 1.0f, focused->ID);
            dl->PopClipRect();
        }
    }

    if (!Motion().PulseActive(kRimPulse)) return;
    const float p = Motion().PulseProgress(kRimPulse);
    ImGuiWindow* w = ImGui::FindWindowByID(s_RimPanel);
    if (!w || !w->WasActive) return;
    const float a = DockRimAlpha * (1.0f - EaseProgress(DockRimGlint, p));
    if (a <= 0.004f) return;
    ImDrawList* dl = w->DrawList;
    dl->PushClipRect(w->Rect().Min, w->Rect().Max, false);
    dl->AddRect(w->Rect().Min + ImVec2(0.5f, 0.5f), w->Rect().Max - ImVec2(0.5f, 0.5f),
                IM_COL32(255, 255, 255, (int)(a * 255.0f)), 0.0f, 0, 1.0f);
    dl->PopClipRect();
}

void DrawPlayFrame(bool playing, bool paused) {
    static bool s_WasPlaying = false, s_WasPaused = false;
    ImAnimContext& m = Motion();
    const float env = m.AnimateToggle(0x504C4659u /* "PLFY" */, playing, s_WasPlaying, PlayFrameIn, PlayFrameOut);
    const float pausedT = m.AnimateToggle(0x504C4650u /* "PLFP" */, playing && paused, s_WasPaused, PlayFrameIn, PlayFrameIn);
    s_WasPlaying = playing;
    s_WasPaused = playing && paused;
    s_PlayRimOn = env > 0.004f;
    if (!s_PlayRimOn) return;
    // Gradients on: a white gradient segment travelling round each view, dimmer while paused, over
    // a faint steady frame so the edge never goes dark between passes. Off: the flat frame that
    // breathes (the static look).
    const bool travel = m.CategoryOn(MotionCategory::Gradients);
    const float breathing = PlayFrameAlpha + PlayFrameBreath * m.Oscillate(PlayFrameHz);
    const float flat = env * (travel ? PlayFramePaused * (1.0f - 0.5f * pausedT)
                                     : breathing + (PlayFramePaused - breathing) * pausedT);
    const float travelA = env * RimTravelAlpha * (1.0f + (RimTravelPausedScale - 1.0f) * pausedT);
    const ImU32 col = IM_COL32(255, 255, 255, (int)(flat * 255.0f));
    const float thick = ImMax(1.0f, Px(1.5f));
    for (const char* name : {EditorPanels::Scene, EditorPanels::Game}) {
        ImGuiWindow* w = ImGui::FindWindowByName(name);
        if (!w || !w->WasActive || w->Hidden || w->SkipItems) continue;
        const ImRect r = w->InnerRect;
        if (r.GetWidth() < 8.0f || r.GetHeight() < 8.0f) continue;
        w->DrawList->PushClipRect(r.Min, r.Max, false);
        w->DrawList->AddRect(r.Min + ImVec2(0.5f, 0.5f), r.Max - ImVec2(0.5f, 0.5f), col, 0.0f, 0, 1.0f);
        // Inset by half the stroke so the whole line sits inside the view. The two views share one
        // phase seed, so with both on screen the light runs round them in step.
        if (travel)
            m.DrawTravellingRim(w->DrawList, r.Min + ImVec2(thick * 0.5f, thick * 0.5f), r.Max - ImVec2(thick * 0.5f, thick * 0.5f),
                                RimTravelSeconds, RimTravelArc, travelA, thick, 0x504C5249u /* "PLRI" */);
        w->DrawList->PopClipRect();
    }
}

void DrawStartupRims() {
    if (!StartupPosing()) return;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 dMin = vp->Pos, dMax(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    const float elapsed = Motion().StartupElapsed();
    for (ImGuiWindow* w : g.Windows) {
        // Docked panels only: a floating window's glass edge glints in the compositor.
        if (!w->Active || w->Hidden || w->SkipItems || w != w->RootWindow || !w->DockIsActive || w->Viewport != vp) continue;
        const ImRect r = w->Rect();
        if (r.GetWidth() < 8.0f || r.GetHeight() < 8.0f) continue;
        // The glint starts partway through the panel's own arrival, so they catch the light one
        // after another as the wave spreads out.
        const float since = elapsed - StartupDelay(r, dMin, dMax) - StartupPanelMs * 0.001f * StartupRimAtShare;
        if (since < 0.0f) continue;
        const float a = StartupRimBoost * (1.0f - EaseProgress(RimGlint, since / (RimGlint.durationMs * 0.001f)));
        if (a <= 0.004f) continue;
        w->DrawList->PushClipRect(r.Min, r.Max, false);
        w->DrawList->AddRect(r.Min + ImVec2(0.5f, 0.5f), r.Max - ImVec2(0.5f, 0.5f), IM_COL32(255, 255, 255, (int)(a * 255.0f)),
                             0.0f, 0, 1.0f);
        w->DrawList->PopClipRect();
    }
}

} // namespace Glass
