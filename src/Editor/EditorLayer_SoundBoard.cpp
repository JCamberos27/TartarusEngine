// The editor's UI sounds in the editor itself: their Preferences rows (on / off, volume, per category) and the Sound Board
// panel (Window > Sound Board), which lists every event with its files and plays any of them.
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorPanels.h"
#include "EditorSettings.h"
#include "EditorSound.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "AudioEngine.h"

#include <imgui.h>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace EditorInternal;

namespace {

// Applies a change live and keeps it (EditorSettings::Save only marks the file dirty; it is flushed later).
void Commit(const EditorSound::Settings& s) {
    EditorSound::Apply(s);
    EditorSound::ToEditorSettings(s);
    EditorSettings::Save();
}

// A 0..1 volume as a percentage slider. True when it changed.
bool PercentSlider(const char* id, float* v) {
    float pct = *v * 100.0f;
    if (!EditorUI::SliderFloat(id, &pct, 0.0f, 100.0f, "%.0f%%")) return false;
    *v = std::clamp(pct / 100.0f, 0.0f, 1.0f);
    return true;
}

} // namespace

void EditorLayer::DrawEditorSoundPreferences() {
    SectionHeader("Editor Sounds");
    EditorSound::Settings s = EditorSound::Current();
    if (SettingsCheckbox("Editor sounds", &s.Enabled)) Commit(s);
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Soft UI sounds for clicks, sliders, menus, drag & drop, Play mode and notifications. Separate from "
                             "the game's audio: View > Mute Audio doesn't silence them, this does.");
    if (!s.Enabled) ImGui::BeginDisabled();
    SettingsLabel("Volume");
    if (PercentSlider("##EditorSoundsVolume", &s.Master)) Commit(s);
    for (int c = 0; c < EditorSoundEvents::kCategoryCount; ++c) {
        ImGui::PushID(c);
        SettingsLabel(EditorSound::CategoryName((EditorSound::Category)c));
        bool on = s.CategoryOn[c];
        if (EditorUIPrimitives::Checkbox("##on", &on)) {
            s.CategoryOn[c] = on;
            Commit(s);
        }
        ImGui::SameLine();
        if (!on) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(ImMax(ImGui::GetFontSize() * 4.0f, ImMin(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 13.0f)));
        if (PercentSlider("##vol", &s.CategoryVolume[c])) Commit(s);
        if (!on) ImGui::EndDisabled();
        ImGui::PopID();
    }
    const int premium = EditorSound::PremiumEventCount();
    if (premium == 0) ImGui::BeginDisabled();
    if (SettingsCheckbox("Premium sounds (Drive)", &s.UsePremium)) Commit(s);
    if (premium == 0) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        EditorUI::SetTooltip(premium ? "Plays the paid Drive set where it has the event (%d of them) and the built-in set for the rest. "
                                       "Compare the two on the Sound Board first."
                                     : "The paid Drive set isn't fetched into this project (assets/Audio/EditorUI): run tools/assets/fetch-assets.ps1.",
                             premium);
    if (!s.Enabled) ImGui::EndDisabled();
    SettingsLabel("##SoundBoard");
    if (SecondaryButton(ICON_FA_VOLUME_HIGH "  Open Sound Board")) m_ShowSoundBoard = true;
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Every editor sound, its files, and a button to hear each one.");
}

void EditorLayer::DrawSoundBoardPanel() {
    if (!m_ShowSoundBoard) return;
    ImGui::SetNextWindowSize(ImVec2(640.0f * m_UIScale, 600.0f * m_UIScale), ImGuiCond_FirstUseEver);
    PushTabChromeText();
    const bool open = ImGui::Begin(EditorPanels::SoundBoard, &m_ShowSoundBoard);
    PopTabChromeText();
    if (!open) { ImGui::End(); return; }

    const EditorSound::Settings& st = EditorSound::Current();
    char line[160];
    std::snprintf(line, sizeof(line), "%s  |  volume %.0f%%  |  %d played  |  %d playing", st.Enabled ? "On" : "Off",
                  st.Master * 100.0f, EditorSound::PlayedCount(), AudioEngine::EditorVoiceCount());
    ImGui::TextDisabled("%s", line);
    if (!st.Enabled) HintText("Editor sounds are off (Preferences > Editor Sounds), so the buttons below are silent.");
    HintText("Each numbered button plays one variant. Built-in is the set in git; Drive is the paid premium set, when it has been "
             "fetched into assets/Audio/EditorUI. The set an event plays now is bright, the other dimmed (Preferences > Sounds > "
             "Premium sounds switches).");

    // The board's own buttons don't click: what they play is the point.
    EditorSoundEvents::ScopedClickSilence quiet;
    for (int c = 0; c < EditorSoundEvents::kCategoryCount; ++c) {
        SectionHeader(EditorSound::CategoryName((EditorSound::Category)c));
        const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg;
        ImGui::PushID(c);
        if (ImGui::BeginTable("##events", 3, flags)) {
            ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Built-in", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Drive", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            if (c == 0) ImGui::TableHeadersRow();
            for (int i = 0; i < EditorSoundEvents::kEventCount; ++i) {
                const EditorSound::Event e = (EditorSound::Event)i;
                const EditorSound::EventInfo& info = EditorSound::Info(e);
                if ((int)info.Cat != c) continue;
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                // The set it plays now is bright; the other is dim.
                ImGui::TextUnformatted(info.Name);
                for (int set = 0; set < 2; ++set) {
                    ImGui::TableNextColumn();
                    const bool premium = set == 1;
                    const std::vector<std::string>& files = premium ? EditorSound::PremiumFiles(e) : EditorSound::BuiltInFiles(e);
                    if (files.empty()) {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled(premium ? "-" : "no file");
                        continue;
                    }
                    const bool active = premium == EditorSound::IsOverride(e);
                    if (!active) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
                    for (int v = 0; v < (int)files.size(); ++v) {
                        if (v > 0) ImGui::SameLine();
                        char label[32];
                        std::snprintf(label, sizeof(label), ICON_FA_PLAY " %d##%c%d", v + 1, premium ? 'p' : 'b', v);
                        if (SecondaryButton(label)) EditorSound::Audition(e, v, premium);
                        if (ImGui::IsItemHovered())
                            EditorUI::SetTooltip("%s%s", std::filesystem::u8path(files[v]).filename().u8string().c_str(),
                                                 active ? "  (playing now)" : "");
                    }
                    if (!active) ImGui::PopStyleVar();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
    ImGui::End();
}
