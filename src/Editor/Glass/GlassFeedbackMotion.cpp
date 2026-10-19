#include "GlassFeedbackMotion.h"

#include "GlassMotion.h"
#include "GlassMotionHooks.h" // DrawCheckmark
#include "GlassTheme.h"       // Px, DrawSoftShadow
#include "GlassTokens.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace Glass {

namespace {
constexpr ImGuiID kSaveClip     = 0x53415643u; // "SAVC"
constexpr ImGuiID kSaveInstance = 0x53415649u; // "SAVI"
constexpr ImGuiID kChDraw       = 0x44524157u; // "DRAW"
constexpr ImGuiID kChAlpha      = 0x414C5048u; // "ALPH"
constexpr ImGuiID kBusySeed     = 0x42555359u; // "BUSY"
constexpr ImGuiID kSheenId      = 0x5348454Eu; // "SHEN"
constexpr ImGuiID kHeadlineId   = 0x53544844u; // "STHD"
constexpr ImGuiID kSublineId    = 0x53545342u; // "STSB"

void EnsureSaveClip() {
    if (ImAnimContext::ClipDefined(kSaveClip)) return;
    // Draw on over SaveCheckDrawMs, hold, then fade out by SaveCheckTotalMs.
    const ClipKey keys[] = {
        {kChDraw,  0.0f,                  0.0f, EaseId::OutCubic},
        {kChDraw,  SaveCheckDrawMs,       1.0f, EaseId::OutCubic},
        {kChAlpha, 0.0f,                  1.0f, EaseId::OutCubic},
        {kChAlpha, SaveCheckFadeAtMs,     1.0f, EaseId::OutCubic},
        {kChAlpha, SaveCheckTotalMs,      0.0f, EaseId::InCubic},
    };
    ImAnimContext::DefineClip(kSaveClip, keys, (int)(sizeof(keys) / sizeof(keys[0])));
}
} // namespace

void PlaySaveCheck() {
    EnsureSaveClip();
    Motion().PlayClip(kSaveInstance, kSaveClip);
}

bool SaveCheckPlaying() { return Motion().ClipPlaying(kSaveInstance); }

void DrawSaveCheck(ImDrawList* dl, ImVec2 pos, float size, ImVec4 col) {
    ImAnimContext& m = Motion();
    const float draw = m.ClipFloat(kSaveInstance, kChDraw, 1.0f);
    const float alpha = m.ClipFloat(kSaveInstance, kChAlpha, 0.0f);
    if (alpha <= 0.004f || draw <= 0.0f) return;
    col.w *= alpha;
    DrawCheckmark(dl, pos, size, draw, ImGui::ColorConvertFloat4ToU32(col), std::max(1.0f, size * 0.14f));
}

void BusyShimmer(float timeSeconds, float& t, float& segFrac, float& bright) {
    // One sweep per BusySweepSeconds, eased at both ends so the segment accelerates out of the
    // left edge and settles into the right instead of ticking across at constant speed.
    const float u = std::fmod(timeSeconds / BusySweepSeconds, 1.0f);
    t = u * u * (3.0f - 2.0f * u);
    const ImAnimContext& m = Motion();
    segFrac = BusySegment + m.Noise(kBusySeed, BusyNoiseHz, BusySegmentDrift);
    bright = 1.0f - BusyBrightDrift + m.Noise(kBusySeed ^ 0x9E37u, BusyNoiseHz, BusyBrightDrift);
}

ImVec2 StaggeredHeadline(ImGuiID id, const char* text, ImVec2 pos, ImU32 col) {
    ImAnimContext& m = Motion();
    ImGuiStorage* st = ImGui::GetStateStorage();
    // Appearing = not drawn last frame. One int in the window's own storage, not animation state.
    const ImGuiID seenKey = id ^ 0x5EE11u;
    const int frame = ImGui::GetFrameCount();
    const bool appearing = st->GetInt(seenKey, -10) < frame - 1;
    st->SetInt(seenKey, frame);
    const float seconds = ImAnimContext::TextStaggerSeconds(text, HeadlineCharMs, HeadlineCharFadeMs);
    if (appearing) m.Pulse(id, seconds);
    const ImVec2 size = ImGui::CalcTextSize(text);
    if (!m.PulseActive(id)) {
        ImGui::GetWindowDrawList()->AddText(pos, col, text);
        return size;
    }
    m.TextStagger(id, text, pos, col, m.PulseProgress(id), HeadlineCharMs, HeadlineCharFadeMs, HeadlineRisePx);
    return size;
}

// --- Motion v2 (shell) -------------------------------------------------------------------------------

void DrawChromeSheen(bool busy) {
    ImAnimContext& m = Motion();
    // Gradients off: no sheen, and no entry for its strength either.
    if (!m.CategoryOn(MotionCategory::Gradients)) return;
    const float strength = m.Animate(kSheenId, busy ? 1.0f : 0.0f, 0.0f, MoodIn, MoodOut);
    if (strength <= 0.004f) return;
    const int frame = ImGui::GetFrameCount();
    for (const char* name : {"##Toolbar", "##StatusBar"}) {
        ImGuiWindow* w = ImGui::FindWindowByName(name);
        // Only a bar drawn this frame: an inactive window's buffers may have been freed.
        if (!w || w->Hidden || w->LastFrameActive < frame || w->DrawList->CmdBuffer.empty()) continue;
        // Each bar its own seed, so the two bands don't sweep in lockstep.
        m.DrawSheen(w->DrawList, w->Rect().Min, w->Rect().Max, w->ID, strength);
    }
}

float ProgressPercentWidth() { return ImAnimContext::OdometerWidth(100.0, 0, "%"); }

float DrawProgressPercent(ImGuiID id, ImDrawList* dl, ImVec2 pos, float fraction, ImU32 col) {
    const float f = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    return Motion().Odometer(id, dl, pos, std::floor(f * 100.0f + 0.5f), 0, col, "%");
}

void DrawStartupHeadline(const char* title, const char* subtitle, ImVec2 centre) {
    ImAnimContext& m = Motion();
    const float t = m.StartupElapsed();
    if (!m.StartupRunning() || t < StartupHeadlineAt) return;
    // Hold, then fade so it has gone before the sequence settles.
    const float out = (t - StartupHeadlineOutAt) / StartupHeadlineOutS;
    const float alpha = out <= 0.0f ? 1.0f : (out >= 1.0f ? 0.0f : 1.0f - EaseProgress(MoodOut, out));
    if (alpha <= 0.0f) return;

    // A click-through overlay over everything: the panels arrive underneath it.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##GlassStartupHeadline", nullptr, flags);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::PushFont(nullptr, Px(StartupHeadlineSize));
    const ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::PopFont();
    const ImVec2 ss = subtitle && *subtitle ? ImGui::CalcTextSize(subtitle) : ImVec2(0.0f, 0.0f);
    const float gap = Px(6.0f);
    const float h = ts.y + (ss.y > 0.0f ? gap + ss.y : 0.0f);
    const float top = centre.y - h * 0.5f;
    // A soft dark pool behind the words so they read over a bright scene, fading with them.
    const float w = ImMax(ts.x, ss.x);
    const ImVec2 pad(Px(28.0f), Px(18.0f));
    DrawSoftShadow(dl, ImVec2(centre.x - w * 0.5f - pad.x, top - pad.y), ImVec2(centre.x + w * 0.5f + pad.x, top + h + pad.y),
                   Px(18.0f), 0.55f * alpha, Px(26.0f));

    const ImU32 titleCol = U32(WithAlpha(TextPrimary, TextPrimary.w * alpha));
    ImGui::PushFont(nullptr, Px(StartupHeadlineSize));
    StaggeredHeadline(kHeadlineId, title, ImVec2(std::floor(centre.x - ts.x * 0.5f), std::floor(top)), titleCol);
    ImGui::PopFont();
    if (ss.y > 0.0f && t >= StartupSublineAt) {
        const ImU32 subCol = U32(WithAlpha(TextTertiary, TextTertiary.w * alpha));
        StaggeredHeadline(kSublineId, subtitle, ImVec2(std::floor(centre.x - ss.x * 0.5f), std::floor(top + ts.y + gap)), subCol);
    }
    ImGui::End();
}

} // namespace Glass
