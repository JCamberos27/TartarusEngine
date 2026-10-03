// Window > Audio: what the audio engine is doing right now (docs/AUDIO.md, "Audio panel"). Read-only: the levels live on the
// Reverb Bus / Reverb Zone components and in Project Settings > Audio.
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorPanels.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "AudioEngine.h"
#include "Audio/WeaponAudio.h"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>

using namespace EditorInternal;

namespace {

// One meter row: a bar from -60 to 0 dB (the peak), the peak and the momentary / short-term loudness as text.
void MeterRow(const char* label, const AudioEngine::MeterReading& m, float barWidth) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(90.0f * ImGui::GetFontSize() / 13.0f);
    const float frac = std::clamp((m.PeakDb + 60.0f) / 60.0f, 0.0f, 1.0f);
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), m.PeakDb <= -119.0f ? "-inf" : "%.1f dB", m.PeakDb);
    ImGui::ProgressBar(frac, ImVec2(barWidth, 0.0f), overlay);
    ImGui::SameLine();
    if (m.MomentaryLufs <= -119.0f) ImGui::TextDisabled("M -inf  S -inf");
    else ImGui::Text("M %.1f  S %.1f", m.MomentaryLufs, m.ShortTermLufs);
}

std::string ShortName(const std::string& path) {
    return path.empty() ? std::string("(none)") : std::filesystem::path(path).filename().string();
}

} // namespace

void EditorLayer::DrawAudioDebugPanel() {
    if (!m_ShowAudioDebug) return;
    ImGui::SetNextWindowSize(ImVec2(380.0f * m_UIScale, 460.0f * m_UIScale), ImGuiCond_FirstUseEver);
    PushTabChromeText();
    const bool open = ImGui::Begin(EditorPanels::Audio, &m_ShowAudioDebug);
    PopTabChromeText();
    if (!open) { ImGui::End(); return; }
    if (!AudioEngine::IsInitialized()) {
        ImGui::TextDisabled("The audio engine is not running.");
        ImGui::End();
        return;
    }
    const float bar = 130.0f * m_UIScale;

    EditorUIPrimitives::SectionHeader("Meters");
    ImGui::TextDisabled("Peak (bar, dBFS); momentary / short-term loudness (LUFS)");
    MeterRow("Master", AudioEngine::GetMeter(AudioEngine::MeterMaster), bar);
    static const char* kBus[] = {"SFX", "Music", "Ambient", "UI", "Voice"};
    for (int b = 0; b < AudioEngine::kBusCount; ++b) MeterRow(kBus[b], AudioEngine::GetMeter(AudioEngine::MeterBusFirst + b), bar);
    MeterRow("Reverb", AudioEngine::GetMeter(AudioEngine::MeterWet), bar);
    // The limiter reports its deepest reduction since the last read: held here and released at 20 dB/s so a short catch stays visible.
    m_AudioGrHold = std::max(AudioEngine::TakeLimiterGainReductionDb(), m_AudioGrHold - 20.0f * ImGui::GetIO().DeltaTime);
    m_AudioGrHold = std::max(m_AudioGrHold, 0.0f);
    const LimiterSettings lim = AudioEngine::GetMasterLimiter();
    if (lim.Enabled) ImGui::Text("Limiter: %.1f dB reduction (ceiling %.1f dBFS)", m_AudioGrHold, lim.CeilingDb);
    else ImGui::TextDisabled("Limiter: off");
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("The master peak limiter (Reverb Bus > Master Limiter). Reduction above a dB or two on ordinary play means\n"
                             "the mix is hitting the ceiling: lower the loud sets rather than relying on the limiter.");
    ImGui::Text("Voices: %d", AudioEngine::VoiceCount());

    EditorUIPrimitives::SectionHeader("Reverb");
    for (int bus = 0; bus < 2; ++bus) {
        const AudioEngine::ReverbInfo info = AudioEngine::GetReverbInfo(bus);
        const bool on = AudioEngine::ReverbEnabled(bus);
        ImGui::Text("%s: %s", bus == 0 ? "Listener's room" : "Remote room (portals)", on ? "on" : "off");
        int running = 0;
        for (const AudioEngine::ReverbInfo::Entry& e : info.Entries) {
            if (!e.Running) continue;
            ++running;
            ImGui::BulletText("%s  %.0f%%", ShortName(e.Ir).c_str(), 100.0f * e.Weight);
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", e.Ir.c_str());
        }
        if (on && running == 0) ImGui::TextDisabled("  (no impulse response running)");
        const AudioEngine::ReverbStats st = AudioEngine::GetReverbStats(bus);
        if (on && st.Callbacks > 0)
            ImGui::TextDisabled("  %d IR(s) loaded; %.0f us mean / %.0f us max per callback; %llu late tail block(s)", info.Instances,
                                st.TotalMicros / (double)st.Callbacks, st.MaxMicros, (unsigned long long)st.LateBlocks);
    }

    WeaponAudio& wa = WeaponAudio::Get();
    EditorUIPrimitives::SectionHeader("Listener's space");
    if (!wa.Active()) {
        ImGui::TextDisabled("Enter Play to see the zones, the reverb's target and the ambience.");
    } else {
        const ReverbZoneMix& mix = wa.ListenerMix();
        const std::vector<ReverbZoneVolume>& zones = wa.Zones().Zones();
        for (int i = 0; i < mix.ClaimCount; ++i) {
            const int zi = mix.Claims[i].Zone;
            if (zi < 0 || zi >= (int)zones.size()) continue;
            ImGui::BulletText("Zone %d (%s)  %.0f%%", zi, SpaceClassName(zones[(size_t)zi].Class), 100.0f * mix.Claims[i].Weight);
        }
        if (mix.ProbeShare > 1e-3f) ImGui::BulletText("Probe (no zone)  %.0f%%", 100.0f * mix.ProbeShare);
        const AudioEngine::ReverbSpec& spec = wa.CurrentReverb();
        ImGui::TextUnformatted("Reverb target:");
        if (spec.Count == 0) ImGui::TextDisabled("  none (no impulse response for this space)");
        for (int l = 0; l < spec.Count; ++l) {
            const AudioEngine::ReverbLayerSpec& L = spec.Layers[l];
            ImGui::BulletText("%s  %.0f%%  wet %.1f dB  pre-delay %.0f ms", ShortName(L.Ir).c_str(), 100.0f * L.Weight, L.WetDb, L.PreDelayMs);
        }
        ImGui::TextUnformatted("Ambience:");
        if (wa.Ambience().empty()) ImGui::TextDisabled("  none");
        for (const WeaponAudio::AmbienceDebug& a : wa.Ambience())
            ImGui::BulletText("%s  %.2f -> %.2f%s", a.Key.c_str(), a.Level, a.Target, a.Playing ? "" : "  (not playing)");
    }
    ImGui::End();
}
