#include "FKeyRow.h"
#include "EditorLayer.h"
#include "EditorSettings.h"
#include "EditorSound.h"
#include "Shortcuts.h"
#include "PhysicsWorld.h"
#include "Glass/GlassMotion.h"
#include "Glass/GlassTokens.h"
#include <imgui_internal.h>
#include <algorithm>

namespace FKeyRow {
namespace {

const char* kDrawNames[] = {"Shaded", "Wireframe", "Unlit", "Normals", "Shadow Cascades", "Mip / Texel"};
const char* kOverlayNames[OverlayCount] = {"Off", "Physics", "AI", "Nav", "Hitboxes"};
const char* kLightNames[LightCount] = {"Off", "Lighting Only", "Albedo", "Roughness / Metal", "Light Complexity"};
constexpr int kDrawCount = (int)(sizeof(kDrawNames) / sizeof(kDrawNames[0]));

// The pill showing (one at a time: a new press replaces it).
struct Pill {
    std::string Key, Title, Value;
    int Step = 0, Count = 0;
    unsigned Serial = 0; // bumped per Show: the stagger and pop restart
};
Pill g_Pill;
constexpr ImGuiID kPulse = 0x46524F57u; // 'FROW': the pill's life
constexpr float kLife = 1.6f;           // s on screen, fade included

int Wrap(int i, int n) { return ((i % n) + n) % n; }

// A step of a cycle: the pitch climbs with the step so you can hear where you are, back on the default is FKeyOff.
void CycleSound(int now, bool back) {
    using EditorSound::Event;
    if (now == 0) EditorSound::Play(Event::FKeyOff);
    else EditorSound::Play(back ? Event::FKeyCycleBack : Event::FKeyCycle, -1.0f, 1.0f + 0.06f * (float)now);
}

// Steps a cycle `n` long on F-row key `fwd` / `back`: -1 when neither fired, else the new index.
int Step(const char* fwd, const char* back, int cur, int n, bool& wentBack) {
    wentBack = false;
    if (Shortcuts::TriggeredGlfw(fwd)) return Wrap(cur + 1, n);
    if (back && Shortcuts::TriggeredGlfw(back)) { wentBack = true; return Wrap(cur - 1, n); }
    return -1;
}

void ApplyOverlay(EditorSettings& es, int overlay) {
    const bool physics = overlay == OverlayPhysics;
    if (physics && !es.PlayDebugOverlay) {
        // The physics channel's first look, as the old F5 did: colliders plus a sensible set of debug channels.
        es.ShowColliders = true;
        if (es.PhysicsDebugDrawFlags == 0u)
            es.PhysicsDebugDrawFlags = PhysicsWorld::PDD_Contacts | PhysicsWorld::PDD_Raycasts | PhysicsWorld::PDD_Velocity;
    }
    es.PlayDebugOverlay = physics;
}

} // namespace

void PollViews(EditorLayer& editor) {
    EditorSettings& es = EditorSettings::Get();
    bool back = false;
    // F5: the draw modes, then the light views (Shaded under them), in one cycle.
    const int light = std::clamp(es.FRowLightDebug, 0, LightCount - 1);
    const int cur = light != LightOff ? kDrawCount - 1 + light : editor.ShadingModeIndex();
    int i = Step("fkey.drawMode", "fkey.drawModeBack", cur, kDrawCount + LightCount - 1, back);
    if (i >= 0) {
        const bool lightView = i >= kDrawCount;
        editor.SetShadingModeIndex(lightView ? 0 : i);
        es.FRowLightDebug = lightView ? i - kDrawCount + 1 : LightOff;
        EditorSettings::Save();
        CycleSound(i, back);
        Show("F5", "View", lightView ? kLightNames[es.FRowLightDebug] : kDrawNames[i], i, kDrawCount + LightCount - 1);
    }
    i = Step("fkey.overlay", "fkey.overlayBack", std::clamp(es.FRowOverlay, 0, OverlayCount - 1), OverlayCount, back);
    if (i >= 0) {
        es.FRowOverlay = i;
        ApplyOverlay(es, i);
        EditorSettings::Save();
        CycleSound(i, back);
        Show("F6", "Overlay", kOverlayNames[i], i, OverlayCount);
    }
    // Post FX on / off: no default key any more (fkey.postFx, rebindable).
    if (Shortcuts::TriggeredGlfw("fkey.postFx")) {
        es.PostFxOff = !es.PostFxOff;
        EditorSettings::Save();
        CycleSound(es.PostFxOff ? 1 : 0, false);
        Show("", "Post FX", es.PostFxOff ? "Off" : "On", es.PostFxOff ? 1 : 0, 2);
    }
}

int CurrentOverlay() { return std::clamp(EditorSettings::Get().FRowOverlay, 0, OverlayCount - 1); }
bool PostFxOff() { return EditorSettings::Get().PostFxOff; }

int DebugViewFor(int shadingIndex) {
    const int light = std::clamp(EditorSettings::Get().FRowLightDebug, 0, LightCount - 1);
    if (light != LightOff) return 3 + light; // 4 Lighting, 5 Albedo, 6 Roughness / Metal, 7 Light complexity
    // EditorLayer::ShadingMode: 3 Normals, 4 Cascades, 5 Mip -> uDebugView 1, 2, 3.
    return shadingIndex >= 3 && shadingIndex <= 5 ? shadingIndex - 2 : 0;
}

void Show(const char* key, const std::string& title, const std::string& value, int step, int count) {
    g_Pill.Key = key ? key : "";
    g_Pill.Title = title;
    g_Pill.Value = value;
    g_Pill.Step = step;
    g_Pill.Count = count;
    ++g_Pill.Serial;
    Glass::Motion().Pulse(kPulse, kLife);
}

void Draw(ImVec2 min, ImVec2 max) {
    Glass::ImAnimContext& m = Glass::Motion();
    if (g_Pill.Serial == 0 || !m.PulseActive(kPulse)) return;
    const float age = m.PulseElapsed(kPulse, kLife);
    const float t = age / kLife;
    if (t >= 1.0f) return;

    // In: the toast's spring pop (scale and rise). Out: a fade over the last quarter.
    const float pop = Glass::EaseProgress(Glass::ToastPop, age / (Glass::ToastPop.durationMs * 0.001f));
    const float scale = Glass::ToastPopFromScale + (1.0f - Glass::ToastPopFromScale) * pop;
    const float alpha = std::min(std::min(age / 0.08f, 1.0f), std::clamp((1.0f - t) / 0.25f, 0.0f, 1.0f));

    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const std::string head = g_Pill.Key + "  " + g_Pill.Title;
    const ImVec2 headSz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, head.c_str());
    const ImVec2 valSz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, g_Pill.Value.c_str());
    const float padX = fs * 0.9f, padY = fs * 0.45f, gap = fs * 0.8f, dot = fs * 0.22f;
    const bool dots = g_Pill.Count > 1;
    const float dotsW = dots ? (float)g_Pill.Count * dot * 2.6f : 0.0f;
    const float w = (padX * 2.0f + headSz.x + gap + valSz.x + (dots ? gap + dotsW : 0.0f)) * scale;
    const float h = (padY * 2.0f + fs) * scale;
    const float rise = Glass::ToastPopRisePx * (1.0f - pop);
    const ImVec2 p0((min.x + max.x) * 0.5f - w * 0.5f, min.y + fs * 2.2f + rise);
    const ImVec2 p1(p0.x + w, p0.y + h);

    // A borderless, input-less window the size of the pill: the text stagger draws into the current window.
    ImGui::SetNextWindowPos(p0);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##FKeyRowPill", nullptr, flags)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const auto col = [&](float r, float g, float b, float a) { return ImGui::GetColorU32(ImVec4(r, g, b, a * alpha)); };
        // Monochrome glass: a dark smoked fill, a faint top sheen and a hairline rim.
        dl->AddRectFilled(p0, p1, col(0.06f, 0.06f, 0.07f, 0.78f), h * 0.5f);
        dl->AddRectFilledMultiColor(ImVec2(p0.x + h * 0.4f, p0.y + 1.0f), ImVec2(p1.x - h * 0.4f, p0.y + h * 0.45f),
                                    col(1, 1, 1, 0.07f), col(1, 1, 1, 0.07f), col(1, 1, 1, 0.0f), col(1, 1, 1, 0.0f));
        dl->AddRect(p0, p1, col(1, 1, 1, 0.16f), h * 0.5f, 0, 1.0f);

        const float fsS = fs * scale;
        float x = p0.x + padX * scale;
        const float y = p0.y + (h - fsS) * 0.5f;
        dl->AddText(font, fsS, ImVec2(x, y), col(1, 1, 1, 0.45f), head.c_str());
        x += (headSz.x + gap) * scale;
        // The value types itself in (ImAnim's text stagger), fresh per press.
        const float stagger = Glass::ImAnimContext::TextStaggerSeconds(g_Pill.Value.c_str(), 14.0f, 120.0f);
        const float prog = stagger > 0.0f ? std::clamp(age / stagger, 0.0f, 1.0f) : 1.0f;
        ImGui::SetWindowFontScale(scale);
        m.TextStagger(ImHashStr("FKeyRowValue") ^ g_Pill.Serial, g_Pill.Value.c_str(), ImVec2(x, y), col(1, 1, 1, 0.95f), prog,
                      14.0f, 120.0f, 4.0f);
        ImGui::SetWindowFontScale(1.0f);
        if (dots) {
            x += (valSz.x + gap) * scale;
            const float cy = p0.y + h * 0.5f;
            for (int k = 0; k < g_Pill.Count; ++k) {
                const ImVec2 c(x + (dot * 1.3f + (float)k * dot * 2.6f) * scale, cy);
                if (k == g_Pill.Step) dl->AddCircleFilled(c, dot * scale, col(1, 1, 1, 0.95f));
                else dl->AddCircle(c, dot * scale * 0.8f, col(1, 1, 1, 0.35f), 0, 1.0f);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace FKeyRow
