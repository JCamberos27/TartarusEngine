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
    if (!s.Enabled) ImGui::EndDisabled();
    SettingsLabel("##SoundBoard");
    if (SecondaryButton(ICON_FA_VOLUME_HIGH "  Open Sound Board")) m_ShowSoundBoard = true;
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Every editor sound, its files, and a button to hear each one.");
}

void EditorLayer::DrawSoundBoardPanel() {
    if (!m_ShowSoundBoard) return;
    ImGui::SetNextWindowSize(ImVec2(460.0f * m_UIScale, 560.0f * m_UIScale), ImGuiCond_FirstUseEver);
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
    HintText("A row's numbered buttons play each of its variants. Files marked Drive come from the premium set fetched into "
             "assets/Audio/EditorUI; the rest are the built-in set.");

    // The board's own buttons don't click: what they play is the point.
    EditorSoundEvents::ScopedClickSilence quiet;
    for (int c = 0; c < EditorSoundEvents::kCategoryCount; ++c) {
        SectionHeader(EditorSound::CategoryName((EditorSound::Category)c));
        const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg;
        ImGui::PushID(c);
        if (ImGui::BeginTable("##events", 3, flags)) {
            ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Play", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
            for (int i = 0; i < EditorSoundEvents::kEventCount; ++i) {
                const EditorSound::Event e = (EditorSound::Event)i;
                const EditorSound::EventInfo& info = EditorSound::Info(e);
                if ((int)info.Cat != c) continue;
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(info.Name);
                ImGui::TableNextColumn();
                const std::vector<std::string>& files = EditorSound::Files(e);
                if (files.empty()) ImGui::TextDisabled("no file");
                for (int v = 0; v < (int)files.size(); ++v) {
                    if (v > 0) ImGui::SameLine();
                    char label[32];
                    std::snprintf(label, sizeof(label), ICON_FA_PLAY " %d##v%d", v + 1, v);
                    if (SecondaryButton(label)) EditorSound::Audition(e, v);
                    if (ImGui::IsItemHovered())
                        EditorUI::SetTooltip("%s", std::filesystem::u8path(files[v]).filename().u8string().c_str());
                }
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (files.empty()) ImGui::TextUnformatted("");
                else if (EditorSound::IsOverride(e)) ImGui::TextUnformatted("Drive");
                else ImGui::TextDisabled("Built-in");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
    ImGui::End();
}
