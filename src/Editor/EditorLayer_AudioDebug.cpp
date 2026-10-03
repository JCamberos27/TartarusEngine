// Window > Audio: what the audio engine is doing right now (docs/AUDIO.md, "Audio panel"), and in Play the live mix: the ducking, the
// focus, the air, the master's glue and trim and the rooms' wet levels, tuned by ear while playing ("Keep after Play" writes them to
// the scene's Audio Mix / Reverb Bus components when Play stops).
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorPanels.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "AudioEngine.h"
#include "Audio/WeaponAudio.h"
#include "Components.h"
#include "Log.h"
#include "World.h"

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

void EditorLayer::ApplyKeptAudioMix(World& world) {
    if (!m_AudioKeepMix && !m_AudioKeepBus) return;
    entt::entity host = entt::null, mixHost = entt::null;
    for (const entt::entity e : world.Registry.view<ReverbBusComponent>()) { host = e; break; }
    for (const entt::entity e : world.Registry.view<AudioMixComponent>()) { mixHost = e; break; }
    if (mixHost == entt::null) mixHost = host;
    if (host == entt::null && mixHost == entt::null) {
        Log::Warn("Audio: the mix was not kept - the scene has no Reverb Bus or Audio Mix component to hold it.");
    } else {
        PushUndo(world, "Keep Audio Mix");
        if (m_AudioKeepBus && host != entt::null) world.Registry.get<ReverbBusComponent>(host) = *m_AudioKeepBus;
        if (m_AudioKeepMix && mixHost != entt::null) world.Registry.emplace_or_replace<AudioMixComponent>(mixHost, *m_AudioKeepMix);
        Log::Info("Audio: kept the mix tuned in Play (Audio Mix / Reverb Bus); save the scene to keep it.");
    }
    m_AudioKeepMix.reset();
    m_AudioKeepBus.reset();
}

void EditorLayer::DrawAudioDebugPanel(World& world) {
    (void)world;
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

    WeaponAudio& wa = WeaponAudio::Get();
    EditorUIPrimitives::SectionHeader("Mix");
    if (!wa.Active()) {
        ImGui::TextDisabled("Enter Play to see the mix move and tune it live.");
    } else {
        const MixDucker& duck = wa.Ducker();
        m_AudioGlueHold = std::max(AudioEngine::TakeGlueGainReductionDb(), m_AudioGlueHold - 20.0f * ImGui::GetIO().DeltaTime);
        m_AudioGlueHold = std::max(m_AudioGlueHold, 0.0f);
        char overlay[48];
        std::snprintf(overlay, sizeof(overlay), "duck %.0f%%", 100.0f * duck.Amount());
        ImGui::ProgressBar(duck.Amount(), ImVec2(bar, 0.0f), overlay);
        ImGui::SameLine();
        ImGui::Text("focus %.0f%%  glue %.1f dB", 100.0f * duck.Focus(), m_AudioGlueHold);
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Duck: how far the groups under a loud event are down (a shot, a round cracking past).\n"
                                 "Focus: aiming down sights. Glue: the master compressor's gain reduction (a few dB in a fight is right).");
        int voices[(int)MixGroup::Count];
        wa.Player().GroupVoices(voices);
        for (int g = 1; g < (int)MixGroup::Count; ++g)
            ImGui::BulletText("%-10s %2d voice(s)  %+.1f dB", MixGroupName((MixGroup)g), voices[g], duck.GainDb((MixGroup)g));
        AudioMixComponent& m = wa.Mix();
        bool changed = false;
        ImGui::PushItemWidth(150.0f * m_UIScale);
        if (ImGui::TreeNode("Ducking")) {
            changed |= ImGui::Checkbox("Enabled", &m.DuckEnabled);
            changed |= ImGui::SliderFloat("Beds dB", &m.DuckBedDb, 0.0f, 24.0f, "%.1f");
            changed |= ImGui::SliderFloat("Own foley dB", &m.DuckOwnFoleyDb, 0.0f, 24.0f, "%.1f");
            changed |= ImGui::SliderFloat("Casings dB", &m.DuckDebrisDb, 0.0f, 24.0f, "%.1f");
            changed |= ImGui::SliderFloat("Impacts dB", &m.DuckWorldDb, 0.0f, 24.0f, "%.1f");
            changed |= ImGui::SliderFloat("Threshold dB", &m.DuckThresholdDb, -40.0f, 0.0f, "%.1f");
            changed |= ImGui::SliderFloat("Full at dB", &m.DuckFullDb, -30.0f, 6.0f, "%.1f");
            changed |= ImGui::SliderFloat("Hold s", &m.DuckHold, 0.0f, 2.0f, "%.2f");
            changed |= ImGui::SliderFloat("Release s", &m.DuckRelease, 0.05f, 5.0f, "%.2f");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Focus (aiming)")) {
            changed |= ImGui::Checkbox("Enabled", &m.FocusEnabled);
            changed |= ImGui::SliderFloat("Beds dB", &m.FocusBedDb, 0.0f, 24.0f, "%.1f");
            changed |= ImGui::SliderFloat("Own foley dB", &m.FocusOwnFoleyDb, 0.0f, 24.0f, "%.1f");
            changed |= ImGui::SliderFloat("Casings dB", &m.FocusDebrisDb, 0.0f, 24.0f, "%.1f");
            changed |= ImGui::SliderFloat("Time s", &m.FocusTime, 0.01f, 2.0f, "%.2f");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Air (distance)")) {
            changed |= ImGui::Checkbox("Enabled", &m.AirEnabled);
            changed |= ImGui::SliderFloat("Start m", &m.AirStartDistance, 1.0f, 200.0f, "%.0f");
            changed |= ImGui::SliderFloat("Exponent", &m.AirExponent, 0.1f, 2.0f, "%.2f");
            changed |= ImGui::SliderFloat("Min Hz", &m.AirMinHz, 500.0f, 20000.0f, "%.0f");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Master")) {
            changed |= ImGui::SliderFloat("Trim dB", &m.MasterTrimDb, -24.0f, 12.0f, "%.1f");
            changed |= ImGui::Checkbox("Glue", &m.GlueEnabled);
            changed |= ImGui::SliderFloat("Glue threshold dB", &m.GlueThresholdDb, -40.0f, 0.0f, "%.1f");
            changed |= ImGui::SliderFloat("Glue ratio", &m.GlueRatio, 1.0f, 10.0f, "%.1f");
            changed |= ImGui::SliderFloat("Glue attack ms", &m.GlueAttackMs, 0.1f, 200.0f, "%.1f");
            changed |= ImGui::SliderFloat("Glue release ms", &m.GlueReleaseMs, 10.0f, 2000.0f, "%.0f");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Rooms (reverb wet)")) {
            ReverbBusComponent& b = wa.Bus();
            ImGui::SliderFloat("Small room dB", &b.WetIndoorSmallDb, -40.0f, 6.0f, "%.1f");
            ImGui::SliderFloat("Hall dB", &b.WetIndoorLargeDb, -40.0f, 6.0f, "%.1f");
            ImGui::SliderFloat("Urban dB", &b.WetOutdoorUrbanDb, -40.0f, 6.0f, "%.1f");
            ImGui::SliderFloat("Open dB", &b.WetOutdoorOpenDb, -40.0f, 6.0f, "%.1f");
            ImGui::SliderFloat("Trim dB", &b.WetTrimDb, -12.0f, 6.0f, "%.1f");
            ImGui::TreePop();
        }
        ImGui::PopItemWidth();
        if (changed) wa.ApplyMix();
        if (ImGui::Button("Keep after Play")) {
            m_AudioKeepMix = std::make_shared<AudioMixComponent>(wa.Mix());
            m_AudioKeepBus = std::make_shared<ReverbBusComponent>(wa.Bus());
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Writes these values to the scene's Audio Mix and Reverb Bus components when Play stops (Play otherwise\n"
                                 "restores the scene as it was). Save the scene after.");
        ImGui::SameLine();
        if (ImGui::Button("Defaults")) {
            m = AudioMixComponent{};
            wa.ApplyMix();
        }
        if (m_AudioKeepMix) {
            ImGui::SameLine();
            ImGui::TextDisabled("kept: written on Stop");
        }
    }

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
