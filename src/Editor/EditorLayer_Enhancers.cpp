// Editor Enhancers (docs/EDITOR_ENHANCERS.md) - the host-side glue for the vHierarchy /
// vFolders / vInspector / vTabs / vFavorites / vRuler-style workflow features. The pure logic
// (refs, bookmark lists, history, matching, units, the icon table) lives in Enhancers/ so the
// unit tests link it directly; this file holds the EditorLayer members that turn it into UI.
// Its own translation unit per #179's "topic-sized EditorLayer files" precedent.

#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorSettings.h"
#include "EditorTheme.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "FileDialog.h"
#include "Log.h"
#include "Shortcuts.h"

#include "Enhancers/EnhancerCore.h"
#include "Enhancers/Palette.h"
#include "Enhancers/StyleWidgets.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <IconsFontAwesome6.h>

#include <algorithm>

using namespace EditorInternal;

namespace {
void Tip(const char* s) { EditorUI::SetTooltip("%s", s); }
} // namespace

// Preferences > Editor Enhancers. The palette is the menu every Style picker offers (row colours,
// folder colours, quick-pick icons); editing it here never touches styles already applied - those
// store the colour value / icon name itself, not a palette index.
void EditorLayer::DrawEnhancerPreferences() {
    EditorSettings& prefs = EditorSettings::Get();
    Enhancers::Palette& pal = Enhancers::Palette::Get();

    EditorUIPrimitives::SectionHeader("Input");
    if (SettingsCheckbox("Hover keys", &prefs.EnhancerHoverKeys)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Single-letter keys act on the row, folder or component under the mouse, whichever panel has "
                         "focus: E expand, Shift+E isolate, Ctrl+Shift+E collapse all, A toggle active, X delete, F frame, "
                         "D default parent. Rebind them under Shortcuts (the \"(hover)\" groups).");

    ImGui::Spacing();
    EditorUIPrimitives::SectionHeader("Palette colours");
    HintText("Offered by every Style menu (Hierarchy rows, folders). Click a swatch to edit it.");
    {
        const float sw = EditorTheme::Px(22.0f);
        int removeAt = -1;
        for (int i = 0; i < (int)pal.Colors.size(); ++i) {
            ImGui::PushID(i);
            if (i > 0) ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < sw) ImGui::NewLine();
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(pal.Colors[(std::size_t)i]);
            if (ImGui::ColorEdit3("##c", &c.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                pal.Colors[(std::size_t)i] = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 1.0f));
                Enhancers::Palette::MarkDirty();
            }
            if (ImGui::BeginPopupContextItem("##ctx")) {
                if (ImGui::MenuItem(ICON_FA_TRASH "  Remove") && pal.Colors.size() > 1) removeAt = i;
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered()) Tip("Click to edit, right-click to remove");
            ImGui::PopID();
        }
        if (removeAt >= 0) {
            pal.Colors.erase(pal.Colors.begin() + removeAt);
            Enhancers::Palette::MarkDirty();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(pal.Colors.size() >= Enhancers::Palette::kMaxColors);
        if (EditorUIPrimitives::ActionButton(ICON_FA_PLUS, "Add a colour", &Tip)) {
            pal.Colors.push_back(Enhancers::PackRGBA(0x8A, 0x8F, 0x98));
            Enhancers::Palette::MarkDirty();
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    EditorUIPrimitives::SectionHeader("Palette icons");
    HintText("Quick picks listed first in every icon picker. Right-click one to remove it.");
    {
        int removeAt = -1;
        const float cell = EditorTheme::Px(28.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < (int)pal.Icons.size(); ++i) {
            const char* glyph = Enhancers::FAIconGlyph(pal.Icons[(std::size_t)i].c_str());
            if (!glyph) continue;
            ImGui::PushID(i);
            if (i > 0) ImGui::SameLine(0.0f, EditorTheme::Px(2.0f));
            if (ImGui::GetContentRegionAvail().x < cell) ImGui::NewLine();
            ImGui::InvisibleButton("##ic", ImVec2(cell, cell));
            const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            const bool hov = ImGui::IsItemHovered();
            if (hov) dl->AddRectFilled(mn, mx, EditorTheme::U32(EditorUIPrimitives::FlatHover()), EditorTheme::Px(3.0f));
            const ImVec2 gs = ImGui::CalcTextSize(glyph);
            dl->AddText(ImVec2((mn.x + mx.x - gs.x) * 0.5f, (mn.y + mx.y - gs.y) * 0.5f),
                        EditorTheme::U32(hov ? EditorTheme::Text : EditorTheme::Secondary), glyph);
            if (hov) Tip(pal.Icons[(std::size_t)i].c_str());
            if (ImGui::BeginPopupContextItem("##ctx")) {
                if (ImGui::MenuItem(ICON_FA_TRASH "  Remove") && pal.Icons.size() > 1) removeAt = i;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (removeAt >= 0) {
            pal.Icons.erase(pal.Icons.begin() + removeAt);
            Enhancers::Palette::MarkDirty();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(pal.Icons.size() >= Enhancers::Palette::kMaxIcons);
        if (EditorUIPrimitives::ActionButton(ICON_FA_PLUS, "Add an icon", &Tip)) ImGui::OpenPopup("##addPaletteIcon");
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("##addPaletteIcon")) {
            static char s_Search[64] = {};
            std::string picked;
            ImGui::SetNextItemWidth(EditorTheme::Px(320.0f));
            if (Enhancers::IconPickerGrid("##paletteIcons", picked, s_Search, sizeof(s_Search), EditorTheme::Px(300.0f)) && !picked.empty()) {
                if (std::find(pal.Icons.begin(), pal.Icons.end(), picked) == pal.Icons.end()) {
                    pal.Icons.push_back(picked);
                    Enhancers::Palette::MarkDirty();
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    ImGui::Spacing();
    if (EditorUIPrimitives::SecondaryButton(ICON_FA_FILE_EXPORT "  Export...")) {
        const std::string path = FileDialog::SaveFile("Palette (*.json)\0*.json\0", "json", m_Window);
        if (!path.empty()) {
            if (pal.ExportTo(path)) Log::Info("Editor Enhancers: palette exported to " + path);
            else Log::Warn("Editor Enhancers: could not write " + path);
        }
    }
    ImGui::SameLine();
    if (EditorUIPrimitives::SecondaryButton(ICON_FA_FILE_IMPORT "  Import...")) {
        const std::string path = FileDialog::OpenFile("Palette (*.json)\0*.json\0All Files\0*.*\0", m_Window);
        if (!path.empty()) {
            if (pal.ImportFrom(path)) { Enhancers::Palette::MarkDirty(); Log::Info("Editor Enhancers: palette imported from " + path); }
            else Log::Warn("Editor Enhancers: " + path + " is not a palette file.");
        }
    }
    ImGui::SameLine();
    if (EditorUIPrimitives::SecondaryButton(ICON_FA_ROTATE_LEFT "  Reset to defaults")) {
        pal = Enhancers::Palette::Defaults();
        Enhancers::Palette::MarkDirty();
    }
}
