#include "GlassFeedbackMotion.h"

#include "GlassMotion.h"
#include "GlassMotionHooks.h" // DrawCheckmark
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

} // namespace Glass
