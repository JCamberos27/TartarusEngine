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
#include "EditorModuleAPI.h" // EditorFolderVisual, kFolderTree* (API v40)
#include "EditorPanels.h"   // vFavorites: the Asset Browser window it overlays
#include "EditorTestProbe.h" // --editor-tests widget tags
#include "World.h"
#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "ComponentRegistry.h"
#include "ProjectPaths.h"

#include "Enhancers/EnhancerCore.h"
#include "Enhancers/EnhancerUserState.h"
#include "Enhancers/FolderStyles.h"
#include "Enhancers/Palette.h"
#include "Enhancers/StyleWidgets.h"
#include "Enhancers/TabState.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <set>

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
                         "D default parent. Over the Inspector: Shift+E isolate a component, Ctrl+Shift+E collapse / "
                         "expand all, A toggle its Enabled field, X remove it. Rebind them under Shortcuts (the "
                         "\"(hover)\" groups).");

    if (SettingsCheckbox("Bookmark chips", &prefs.BookmarkChips)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Show bookmarks as chips on the navigation bars (and a bar under the Asset Browser toolbar). "
                         "Off: each panel's bookmark button opens them as a list, keeping one header row per panel.");

    ImGui::Spacing();
    EditorUIPrimitives::SectionHeader("Hierarchy");
    if (SettingsCheckbox("Row styles", &prefs.HierarchyRowStyles)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Custom row icons, colours and separator rows (right-click a row > Row Style). Off hides them; "
                         "nothing is deleted.");
    if (SettingsCheckbox("Tree lines", &prefs.HierarchyTreeLines)) EditorSettings::Save();
    if (SettingsCheckbox("Zebra striping", &prefs.HierarchyZebra)) EditorSettings::Save();
    if (SettingsCheckbox("Minimal mode", &prefs.HierarchyMinimal)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Hide each row's kind icon unless you gave the row one.");
    if (SettingsCheckbox("Component minimap", &prefs.HierarchyMinimap)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Each row shows its components' icons. Click one to jump to it in the Inspector; "
                         "Alt+click to open it in its own window.");
    if (prefs.HierarchyMinimap) {
        SettingsLabel("Minimap icons");
        if (ImGui::SliderInt("##minimapMax", &prefs.HierarchyMinimapMax, 1, 12)) EditorSettings::Save();
    }
    if (SettingsCheckbox("Navigation bar", &prefs.HierarchyNavBar)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Scene selector, selection Back/Forward and bookmarks above the Hierarchy tree. "
                         "Drop rows on the bar to bookmark them.");

    ImGui::Spacing();
    EditorUIPrimitives::SectionHeader("Asset Browser folders");
    if (SettingsCheckbox("Folder styles", &prefs.FolderStyles)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Folder icons and colours (right-click a folder), folder rules below, and automatic icons. "
                         "Off hides them; nothing is deleted.");
    if (SettingsCheckbox("Colour the whole row", &prefs.FolderRowWash)) EditorSettings::Save();
    if (SettingsCheckbox("Tree lines", &prefs.FolderTreeLines)) EditorSettings::Save();
    if (SettingsCheckbox("Zebra striping", &prefs.FolderZebra)) EditorSettings::Save();
    if (SettingsCheckbox("Minimal mode", &prefs.FolderMinimal)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Hide the folder glyph unless the folder has its own icon.");
    if (SettingsCheckbox("Content minimap", &prefs.FolderMinimap)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Icons of what each folder holds (models, textures, sounds...), most numerous first.");
    if (SettingsCheckbox("Folder bookmarks", &prefs.FolderNavBar)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Bookmarked folders, behind the bookmark button beside the tabs (or as chips with Bookmark chips "
                         "on). Bookmark from a folder's right-click menu, or drop a folder on the button.");

    // Project data from here down: project/editor_folders.json, shared with the team.
    auto& fs = Enhancers::FolderStyles::Get();
    {
        bool autoIcons = fs.AutoIcons;
        if (SettingsCheckbox("Automatic icons", &autoIcons)) { fs.AutoIcons = autoIcons; fs.MarkDirty(); }
        ImGui::SameLine();
        EditorUI::HelpMarker("A folder mostly (60%+) of one kind of asset shows that kind's icon - a cube for models, "
                             "an image for textures. Saved with the project.");
    }

    ImGui::Spacing();
    EditorUIPrimitives::SectionHeader("Folder rules (this project)");
    HintText("Style folders by name: the pattern is matched against the folder's name and its full path. "
             "* matches anything, ? one character. The first matching rule wins; a folder's own style beats any rule.");
    {
        int removeAt = -1, moveFrom = -1, moveTo = -1;
        const float sw = ImGui::GetFrameHeight();
        if (ImGui::BeginTable("##folderRules", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Pattern", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Icon", ImGuiTableColumnFlags_WidthFixed, sw * 1.2f);
            ImGui::TableSetupColumn("Colour", ImGuiTableColumnFlags_WidthFixed, sw * 1.2f);
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, sw * 3.2f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)fs.Rules.size(); ++i) {
                Enhancers::FolderRule& r = fs.Rules[(size_t)i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s", r.Pattern.c_str());
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText("##pattern", buf, sizeof(buf)) && buf[0]) { r.Pattern = buf; fs.MarkDirty(); }
                ImGui::TableSetColumnIndex(1);
                const char* g = r.Style.Icon.empty() ? nullptr : Enhancers::FAIconGlyph(r.Style.Icon.c_str());
                if (EditorUIPrimitives::ActionButton(g ? g : ICON_FA_FOLDER, r.Style.Icon.empty() ? "Choose an icon" : r.Style.Icon.c_str(),
                                                     &Tip, false, ImVec2(sw, sw)))
                    ImGui::OpenPopup("##ruleIcon");
                if (ImGui::BeginPopup("##ruleIcon")) {
                    std::string icon = r.Style.Icon;
                    if (Enhancers::IconPickerGrid("##ruleIconGrid", icon, m_FolderStyleIconSearch, sizeof(m_FolderStyleIconSearch), EditorTheme::Px(280.0f))) {
                        r.Style.Icon = icon;
                        fs.MarkDirty();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(2);
                {
                    ImVec4 c = ImGui::ColorConvertU32ToFloat4(r.Style.Color ? r.Style.Color : IM_COL32(0x8A, 0x8F, 0x98, 255));
                    if (ImGui::ColorEdit3("##ruleColor", &c.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                        r.Style.Color = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 1.0f));
                        fs.MarkDirty();
                    }
                    if (r.Style.Color == 0 && ImGui::IsItemHovered()) Tip("No colour set - pick one, or leave it to keep the default");
                }
                ImGui::TableSetColumnIndex(3);
                ImGui::BeginDisabled(i == 0);
                if (EditorUIPrimitives::ActionButton(ICON_FA_ARROW_UP, "Move up (rules are tried top to bottom)", &Tip, false, ImVec2(sw, sw))) { moveFrom = i; moveTo = i - 1; }
                ImGui::EndDisabled();
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::BeginDisabled(i + 1 == (int)fs.Rules.size());
                if (EditorUIPrimitives::ActionButton(ICON_FA_ARROW_DOWN, "Move down", &Tip, false, ImVec2(sw, sw))) { moveFrom = i; moveTo = i + 1; }
                ImGui::EndDisabled();
                ImGui::SameLine(0.0f, 0.0f);
                if (EditorUIPrimitives::DangerIconButton(ICON_FA_TRASH, "Remove this rule", &Tip, ImVec2(sw, sw))) removeAt = i;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (moveFrom >= 0 && moveTo >= 0 && moveTo < (int)fs.Rules.size()) {
            std::swap(fs.Rules[(size_t)moveFrom], fs.Rules[(size_t)moveTo]);
            fs.MarkDirty();
        }
        if (removeAt >= 0) {
            fs.Rules.erase(fs.Rules.begin() + removeAt);
            fs.MarkDirty();
        }
        if (EditorUIPrimitives::SecondaryButton(ICON_FA_PLUS "  Add rule")) {
            fs.Rules.push_back({"Materials", Enhancers::FolderStyle{"droplet", 0}});
            fs.MarkDirty();
        }
    }

    ImGui::Spacing();
    EditorUIPrimitives::SectionHeader("Inspector");
    if (SettingsCheckbox("Navigation bar##insp", &prefs.InspectorNavBar)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Selection Back/Forward and bookmarked objects and assets above the Inspector. "
                         "Drop Hierarchy rows or assets on the bar to bookmark them.");
    if (SettingsCheckbox("Animations##insp", &prefs.InspectorAnimations)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Component sections slide open and closed, and fade out when removed.");
    if (SettingsCheckbox("Minimal mode##insp", &prefs.InspectorMinimal)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("A component's actions button appears only while its header is hovered.");

    ImGui::Spacing();
    EditorUIPrimitives::SectionHeader("Favorites");
    if (SettingsCheckbox("Favorites overlay", &prefs.Favorites)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Pages of favorite folders, assets and objects, shown over the Asset Browser. Ctrl+Alt+F keeps it "
                         "open, Ctrl+Alt+B adds the selection. Inside: 1-9 / arrows / wheel switch pages, Up/Down + Enter "
                         "open, Esc closes.");
    if (prefs.Favorites) {
        if (SettingsCheckbox("Show while Alt is held", &prefs.FavoritesHoldAlt)) EditorSettings::Save();
        ImGui::SameLine();
        EditorUI::HelpMarker("Hold Alt with the mouse over the Asset Browser to show the overlay; release to hide it. "
                             "Off: only Ctrl+Alt+F (or the pin) shows it.");
    }

    ImGui::Spacing();
    EditorUIPrimitives::SectionHeader("Ruler");
    if (SettingsCheckbox("Ruler (hold Shift+R)", &prefs.Ruler)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Hold Shift+R over the Scene view: the distance from the surface under the cursor to the next "
                         "surface along its normal, and the object's thickness there. The wheel steps to objects behind; "
                         "a click shows the object's size.");
    {
        int units = prefs.MeasureFeet ? 1 : 0;
        SettingsLabel("Length units");
        if (ImGui::Combo("##lengthUnits", &units, "Metric (mm / cm / m / km)\0Feet and inches\0")) {
            prefs.MeasureFeet = units == 1;
            EditorSettings::Save();
        }
        ImGui::SameLine();
        EditorUI::HelpMarker("Used by the ruler and the Measure tool.");
    }

    ImGui::Spacing();
    EditorUIPrimitives::SectionHeader("Tabs");
    if (SettingsCheckbox("Inspector tabs", &prefs.InspectorTabs)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Pin objects, components and assets as tabs at the top of the Inspector: drop them on the "
                         "strip, or press Ctrl+T over the Inspector. A tab keeps showing its target while you select "
                         "other things; the Selection tab follows the selection.");
    if (SettingsCheckbox("Asset Browser tabs", &prefs.AssetTabs)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Folder tabs under the Asset Browser toolbar; the active tab follows where you navigate. "
                         "Ctrl+T new tab, Ctrl+W close, Ctrl+Shift+T reopen (mouse over the panel). Shift+wheel over a "
                         "strip switches tabs, Ctrl+Shift+wheel moves the tab, middle-click closes.");

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

// ============================================================================================
// vHierarchy
// ============================================================================================

std::string EditorLayer::CurrentSceneKey() const {
    if (m_CurrentScenePath.empty()) return std::string();
    const AssetGuid g = AssetDatabase::GuidForPath(m_CurrentScenePath);
    return g.IsValid() ? g.ToString() : std::filesystem::path(m_CurrentScenePath).lexically_normal().generic_string();
}

entt::entity EditorLayer::FindEntityByOrder(const World& world, int order) const {
    if (order < 0) return entt::null;
    for (auto [e, o] : world.Registry.view<const OrderComponent>().each())
        if (o.Value == order) return e;
    return entt::null;
}

entt::entity EditorLayer::ResolveDefaultParent(const World& world) const {
    const auto& dp = Enhancers::EnhancerUserState::Get().DefaultParents;
    const auto it = dp.find(CurrentSceneKey());
    if (it == dp.end()) return entt::null;
    // Kept (not erased) when the entity is missing: an undo can bring it back.
    return FindEntityByOrder(world, it->second);
}

void EditorLayer::ApplyDefaultParent(World& world, entt::entity created) {
    if (created == entt::null || !world.Registry.valid(created)) return;
    const entt::entity dp = ResolveDefaultParent(world);
    if (dp == entt::null || dp == created || !world.Registry.valid(dp)) return;
    world.SetParent(created, dp); // keeps the world pose, so it still appears where it was spawned
}

void EditorLayer::ToggleDefaultParent(World& world, entt::entity entity) {
    if (entity == entt::null || !world.Registry.valid(entity)) return;
    const auto* o = world.Registry.try_get<OrderComponent>(entity);
    if (!o) return;
    auto& us = Enhancers::EnhancerUserState::Get();
    const std::string key = CurrentSceneKey();
    const auto it = us.DefaultParents.find(key);
    const auto* nm = world.Registry.try_get<NameComponent>(entity);
    const std::string name = nm && !nm->Name.empty() ? nm->Name : std::string("object");
    if (it != us.DefaultParents.end() && it->second == o->Value) {
        us.DefaultParents.erase(it);
        Log::Info("Default parent cleared - new objects go to the scene root.");
    } else {
        us.DefaultParents[key] = o->Value;
        Log::Info("Default parent: new objects in this scene are created under '" + name + "'.");
    }
    us.MarkDirty();
}

namespace {
std::string SceneDisplayName(const std::string& path) {
    return std::filesystem::path(path).stem().string();
}
std::string NormalizedPathKey(const std::string& path) {
    std::string k = std::filesystem::path(path).lexically_normal().generic_string();
    for (char& c : k) c = (char)std::tolower((unsigned char)c);
    return k;
}
Enhancers::EditorRef SceneRefFor(const std::string& path) {
    const AssetGuid g = AssetDatabase::GuidForPath(path);
    return Enhancers::EditorRef::MakeScene(g.IsValid() ? g.ToString() : std::string(), path, SceneDisplayName(path));
}
// A bookmarked scene's current path: its GUID wins (it follows renames), the stored path is the
// fallback for a scene that had no .meta when it was starred.
std::string ResolveScenePath(const Enhancers::EditorRef& r) {
    if (!r.Scene.empty()) {
        const std::string p = AssetDatabase::PathForGuid(AssetGuid::FromString(r.Scene));
        if (!p.empty()) return p;
    }
    return r.Path;
}

// One flat, pill-shaped clickable with an optional icon and a label (clipped to maxW). Returns its
// width; click / hover come back through the out bools.
float NavChip(const char* id, const char* icon, const char* label, bool dim, float maxW, bool& clicked, bool& hovered) {
    EditorTheme::PushSmall();
    const float padX = EditorTheme::Px(7.0f);
    const float gap = icon ? EditorTheme::Px(5.0f) : 0.0f;
    const ImVec2 is = icon ? ImGui::CalcTextSize(icon) : ImVec2(0, 0);
    const ImVec2 ls = ImGui::CalcTextSize(label);
    const float h = ImGui::GetFrameHeight() - EditorTheme::Px(4.0f);
    float w = padX * 2.0f + is.x + gap + ls.x;
    if (w > maxW) w = maxW;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 mn(p.x, p.y + EditorTheme::Px(2.0f));
    ImGui::SetCursorScreenPos(mn);
    clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mx(mn.x + w, mn.y + h);
    dl->AddRectFilled(mn, mx, EditorTheme::U32(hovered ? EditorTheme::Hover : EditorTheme::Raised), h * 0.5f);
    const ImU32 tc = EditorTheme::U32(dim ? EditorTheme::Dim : (hovered ? EditorTheme::Text : EditorTheme::Secondary));
    float x = mn.x + padX;
    const float cy = (mn.y + mx.y) * 0.5f;
    if (icon) { dl->AddText(ImVec2(x, cy - is.y * 0.5f), tc, icon); x += is.x + gap; }
    dl->PushClipRect(mn, ImVec2(mx.x - padX * 0.5f, mx.y), true);
    dl->AddText(ImVec2(x, cy - ls.y * 0.5f), tc, label);
    dl->PopClipRect();
    EditorTheme::PopFont();
    return w;
}

// --- The bookmarks popover ----------------------------------------------------------------------
// One button per panel header instead of a row of chips: the list opens under it. Every panel
// (Hierarchy, Inspector, Asset Browser) feeds it rows and acts on what comes back.
struct BookmarkRow {
    std::string Icon;
    std::string Label;
    std::string Tooltip;
    unsigned Color = 0;   // a packed user colour (row style / folder colour), or 0
    bool Missing = false; // dimmed: not in the open scene, or deleted
    int Index = -1;       // into the caller's bookmark list
};
struct BookmarkAction {
    int Activate = -1;
    bool Additive = false; // Ctrl+click
    int Remove = -1;
    int MoveFrom = -1, MoveTo = -1;
    bool Toggle = false;   // the "Bookmark X" / "Remove X" row
};

// The header button: the bookmark glyph, lit while the current item is bookmarked or a compatible
// drag is over it (the tooltip gives the count). Click opens the list.
// `toggleLabel` names the current item ("Alpha", "Bob.cs"); nullptr when there is nothing to add.
void BookmarkButton(const char* id, float h, const char* toggleLabel, bool currentStarred,
                    const std::vector<BookmarkRow>& rows, bool dropHot, BookmarkAction& out, bool forceOpen = false) {
    ImGui::PushID(id);
    const ImGuiID popupId = ImGui::GetID("##bmPop");
    const bool popupOpen = ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None);
    char tip[160];
    std::snprintf(tip, sizeof(tip), "Bookmarks (%d)%s", (int)rows.size(), dropHot ? " - drop to bookmark" : "");
    if (EditorUIPrimitives::ActionButton(ICON_FA_BOOKMARK, tip, &Tip, currentStarred || popupOpen || dropHot, ImVec2(h, h)) || forceOpen)
        ImGui::OpenPopup("##bmPop");
    const ImVec2 bmx = ImGui::GetItemRectMax();

    ImGui::SetNextWindowPos(ImVec2(bmx.x, bmx.y + EditorTheme::Px(4.0f)), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(EditorTheme::Px(240.0f), 0.0f), ImVec2(EditorTheme::Px(340.0f), EditorTheme::Px(420.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(EditorTheme::Px(6.0f), EditorTheme::Px(6.0f)));
    if (ImGui::BeginPopup("##bmPop")) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(EditorTheme::Px(6.0f), EditorTheme::Px(1.0f)));
        const float rowH = ImGui::GetFrameHeight();
        const float w = std::max(ImGui::GetContentRegionAvail().x, EditorTheme::Px(228.0f));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiIO& io = ImGui::GetIO();

        // One list row: icon + label, hover wash, an x on hover. Returns true when the body was clicked.
        auto drawRow = [&](const char* rid, const char* icon, ImVec4 iconCol, const char* label, bool dim,
                           bool closeable, bool& closeClicked) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton(rid, ImVec2(w, rowH));
            const bool hov = ImGui::IsItemHovered();
            const float ht = EditorTheme::AnimT(ImGui::GetItemID(), hov);
            const ImVec2 mx(p.x + w, p.y + rowH);
            if (ht > 0.0f) dl->AddRectFilled(p, mx, EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Hover, ht)), EditorTheme::Px(3.0f));
            const float cy = p.y + rowH * 0.5f;
            float x = p.x + EditorTheme::Px(8.0f);
            if (icon && *icon) {
                const ImVec2 is = ImGui::CalcTextSize(icon);
                dl->AddText(ImVec2(x + (EditorTheme::Px(16.0f) - is.x) * 0.5f, cy - is.y * 0.5f),
                            EditorTheme::U32(dim ? EditorTheme::Dim : iconCol), icon);
            }
            x += EditorTheme::Px(24.0f);
            const float closeW = closeable ? rowH : 0.0f;
            const ImVec2 ls = ImGui::CalcTextSize(label);
            EditorUIPrimitives::TextEllipsis(dl, ImVec2(x, cy - ls.y * 0.5f), mx.x - closeW - EditorTheme::Px(4.0f),
                                             EditorTheme::U32(dim ? EditorTheme::Dim : EditorTheme::Mix(EditorTheme::Secondary, EditorTheme::Text, ht)),
                                             label);
            closeClicked = false;
            if (closeable && hov) {
                const ImVec2 cmn(mx.x - closeW, p.y), cmx = mx;
                const bool overClose = ImGui::IsMouseHoveringRect(cmn, cmx);
                if (overClose) dl->AddCircleFilled(ImVec2((cmn.x + cmx.x) * 0.5f, cy), rowH * 0.32f, EditorTheme::U32(EditorTheme::Pressed), 16);
                EditorTheme::PushSmall();
                const ImVec2 xs = ImGui::CalcTextSize(ICON_FA_XMARK);
                dl->AddText(ImVec2((cmn.x + cmx.x - xs.x) * 0.5f, cy - xs.y * 0.5f),
                            EditorTheme::U32(overClose ? EditorTheme::Text : EditorTheme::Dim), ICON_FA_XMARK);
                EditorTheme::PopFont();
                if (clicked && overClose) closeClicked = true;
            }
            return clicked && !closeClicked;
        };

        EditorUIPrimitives::SectionHeader("Bookmarks");
        if (toggleLabel) {
            char line[200];
            std::snprintf(line, sizeof(line), currentStarred ? "Remove \"%s\"" : "Bookmark \"%s\"", toggleLabel);
            bool unused = false;
            if (drawRow("##bmToggle", currentStarred ? ICON_FA_BOOKMARK : ICON_FA_PLUS,
                        currentStarred ? EditorTheme::Accent : EditorTheme::Secondary, line, false, false, unused))
                out.Toggle = true;
            EditorTestTag("bm:toggle");
            if (!rows.empty()) {
                ImGui::Dummy(ImVec2(0.0f, EditorTheme::Px(2.0f)));
                const float y = std::floor(ImGui::GetCursorScreenPos().y) - 0.5f;
                dl->AddLine(ImVec2(ImGui::GetCursorScreenPos().x, y), ImVec2(ImGui::GetCursorScreenPos().x + w, y),
                            EditorTheme::U32(EditorTheme::Hairline));
                ImGui::Dummy(ImVec2(0.0f, EditorTheme::Px(2.0f)));
            }
        }
        if (rows.empty()) {
            EditorTheme::PushSmall();
            ImGui::Dummy(ImVec2(0.0f, EditorTheme::Px(4.0f)));
            ImGui::Indent(EditorTheme::Px(8.0f));
            ImGui::TextColored(EditorTheme::Dim, "No bookmarks yet.");
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - EditorTheme::Px(16.0f));
            ImGui::TextColored(EditorTheme::Dim, "Drop items on the bookmark button to add them.");
            ImGui::PopTextWrapPos();
            ImGui::Unindent(EditorTheme::Px(8.0f));
            ImGui::Dummy(ImVec2(0.0f, EditorTheme::Px(4.0f)));
            EditorTheme::PopFont();
        }
        for (const BookmarkRow& r : rows) {
            ImGui::PushID(r.Index);
            const ImVec4 ic = r.Color ? EditorTheme::UserGlyphColor(r.Color) : EditorTheme::Secondary;
            bool removeClicked = false;
            const bool hit = drawRow("##bm", r.Icon.c_str(), ic, r.Label.c_str(), r.Missing, true, removeClicked);
            EditorTestTag(("bm:" + r.Label).c_str());
            if (hit && !r.Missing) { out.Activate = r.Index; out.Additive = io.KeyCtrl; }
            if (removeClicked) out.Remove = r.Index;
            if (ImGui::IsItemHovered() && !r.Tooltip.empty() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                EditorUI::SetTooltip("%s", r.Tooltip.c_str());
            // Drag a row onto another to reorder.
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                ImGui::SetDragDropPayload("BOOKMARK_ROW", &r.Index, sizeof(int));
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("BOOKMARK_ROW", ImGuiDragDropFlags_AcceptNoDrawDefaultRect |
                                                                                         ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                    const int from = *(const int*)p->Data;
                    const float y = from < r.Index ? mx.y : mn.y;
                    dl->AddLine(ImVec2(mn.x + EditorTheme::Px(4.0f), y), ImVec2(mx.x - EditorTheme::Px(4.0f), y),
                                EditorTheme::U32(EditorTheme::Accent), EditorTheme::Px(2.0f));
                    if (p->IsDelivery() && from != r.Index) { out.MoveFrom = from; out.MoveTo = r.Index; }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        if ((out.Activate >= 0 && !out.Additive) || out.Toggle) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
}

// Applies a popover's remove / reorder to `list` (rows carry indices into it). True when changed.
bool ApplyBookmarkEdits(std::vector<Enhancers::EditorRef>& list, const BookmarkAction& a) {
    if (a.Remove >= 0 && a.Remove < (int)list.size()) { list.erase(list.begin() + a.Remove); return true; }
    if (a.MoveFrom >= 0 && a.MoveTo >= 0 && a.MoveFrom < (int)list.size() && a.MoveTo < (int)list.size()) {
        Enhancers::EditorRef moved = list[(size_t)a.MoveFrom];
        list.erase(list.begin() + a.MoveFrom);
        list.insert(list.begin() + a.MoveTo, std::move(moved));
        return true;
    }
    return false;
}

// True while something the bar accepts is being dragged over `r` (lights the bookmark button).
bool DragOverRect(const ImRect& r, std::initializer_list<const char*> types) {
    const ImGuiPayload* p = ImGui::GetDragDropPayload();
    if (!p || !ImGui::IsMouseHoveringRect(r.Min, r.Max, false)) return false;
    for (const char* t : types) if (p->IsDataType(t)) return true;
    return false;
}
} // namespace

// vHierarchy's nav bar: [scene selector] [<] [>] [bookmark] [chips ...]. Drop Hierarchy rows
// anywhere on the bar to bookmark them. Host-drawn (API v39): the chips resolve entities by the
// scene's GUID + OrderComponent, which never cross the module boundary.
void EditorLayer::DrawHierarchyNavBar(World& world, AssetLibrary& assets) {
    if (!EditorSettings::Get().HierarchyNavBar) return;
    auto& us = Enhancers::EnhancerUserState::Get();
    const std::string sceneKey = CurrentSceneKey();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 barMin = ImGui::GetCursorScreenPos();
    const float barW = ImGui::GetContentRegionAvail().x;
    ImGui::PushID("##hierNav");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(EditorTheme::Px(4.0f), ImGui::GetStyle().ItemSpacing.y));

    // --- Scene selector ----------------------------------------------------------------------
    const std::string curName = m_CurrentScenePath.empty() ? std::string("Untitled") : SceneDisplayName(m_CurrentScenePath);
    const std::string comboLabel = std::string(ICON_FA_MAP "  ") + curName;
    const bool chips = EditorSettings::Get().BookmarkChips;
    const float gapX = EditorTheme::Px(4.0f);
    ImGui::SetNextItemWidth(chips ? std::min(barW * 0.42f, EditorTheme::Px(180.0f))
                                  : std::max(EditorTheme::Px(80.0f), barW - 3.0f * h - 3.0f * gapX - EditorTheme::Px(6.0f)));
    if (ImGui::BeginCombo("##scene", comboLabel.c_str(), ImGuiComboFlags_HeightLarge)) {
        RefreshScenesListingIfNeeded();
        std::set<std::string> listed;
        const std::string curKey = m_CurrentScenePath.empty() ? std::string() : NormalizedPathKey(m_CurrentScenePath);
        std::string toggleBookmark; // applied after the loops - they iterate the bookmark list
        auto entry = [&](const std::string& path) {
            if (path.empty()) return;
            const std::string key = NormalizedPathKey(path);
            if (!listed.insert(key).second) return;
            std::error_code ec;
            const bool exists = std::filesystem::exists(path, ec);
            const Enhancers::EditorRef ref = SceneRefFor(path);
            const bool starred = Enhancers::FindRef(us.SceneBookmarks, ref) >= 0;
            ImGui::PushID(key.c_str());
            if (EditorUIPrimitives::ActionButton(ICON_FA_STAR, starred ? "Remove the bookmark" : "Bookmark this scene",
                                                 &Tip, starred, ImVec2(h, h)))
                toggleBookmark = path;
            ImGui::SameLine();
            ImGui::BeginDisabled(!exists);
            if (ImGui::Selectable(SceneDisplayName(path).c_str(), key == curKey, 0, ImVec2(0, h)) && key != curKey)
                RequestOpenScene(world, assets, path); // keeps the unsaved-changes prompt
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                EditorUI::SetTooltip("%s%s", ProjectPaths::Relativize(path).c_str(), exists ? "" : "\n(file not found)");
            ImGui::PopID();
        };
        if (!us.SceneBookmarks.empty()) {
            EditorUIPrimitives::SectionHeader("BOOKMARKED");
            for (const auto& r : us.SceneBookmarks) entry(ResolveScenePath(r));
        }
        const auto& recent = EditorSettings::Get().RecentScenes;
        bool recentHeader = false;
        for (size_t i = 0; i < recent.size() && i < 6; ++i) {
            if (listed.count(NormalizedPathKey(recent[i]))) continue;
            if (!recentHeader) { EditorUIPrimitives::SectionHeader("RECENT"); recentHeader = true; }
            entry(recent[i]);
        }
        bool allHeader = false;
        for (const auto& p : m_ScenesListingCache.paths) {
            if (listed.count(NormalizedPathKey(p))) continue;
            if (!allHeader) { EditorUIPrimitives::SectionHeader("ALL SCENES"); allHeader = true; }
            entry(p);
        }
        if (!toggleBookmark.empty()) {
            const Enhancers::EditorRef ref = SceneRefFor(toggleBookmark);
            if (!Enhancers::RemoveRef(us.SceneBookmarks, ref))
                Enhancers::AddUnique(us.SceneBookmarks, ref, Enhancers::EnhancerUserState::kMaxBookmarks);
            us.MarkDirty();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Switch scene - bookmarked, recent and every scene in the project");

    // --- Selection Back / Forward (the existing Ctrl+[ / Ctrl+] history) -----------------------
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanSelectionHistoryBack());
    if (EditorUIPrimitives::ActionButton(ICON_FA_ARROW_LEFT, "Previous selection (Ctrl+[)", &Tip, false, ImVec2(h, h)))
        SelectionHistoryBack(world);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanSelectionHistoryForward());
    if (EditorUIPrimitives::ActionButton(ICON_FA_ARROW_RIGHT, "Next selection (Ctrl+])", &Tip, false, ImVec2(h, h)))
        SelectionHistoryForward(world);
    ImGui::EndDisabled();

    // --- Bookmark the selection ----------------------------------------------------------------
    auto refFor = [&](entt::entity e) {
        const auto* o = world.Registry.try_get<OrderComponent>(e);
        const auto* n = world.Registry.try_get<NameComponent>(e);
        return Enhancers::EditorRef::MakeEntity(sceneKey, o ? o->Value : -1, n ? n->Name : std::string());
    };
    const bool primaryValid = m_Selected != entt::null && world.Registry.valid(m_Selected) &&
                              world.Registry.all_of<OrderComponent>(m_Selected);
    const bool primaryStarred = primaryValid && Enhancers::FindRef(us.EntityBookmarks, refFor(m_Selected)) >= 0;
    auto toggleSelection = [&]() {
        if (primaryStarred) {
            for (entt::entity e : GetSelectedItems()) Enhancers::RemoveRef(us.EntityBookmarks, refFor(e));
        } else {
            for (entt::entity e : GetSelectedItems())
                if (world.Registry.valid(e) && world.Registry.all_of<OrderComponent>(e))
                    Enhancers::AddUnique(us.EntityBookmarks, refFor(e), Enhancers::EnhancerUserState::kMaxBookmarks);
        }
        us.MarkDirty();
    };
    const ImRect barRect(barMin, ImVec2(barMin.x + barW, barMin.y + h));
    auto acceptRowDrop = [&]() {
        if (ImGui::BeginDragDropTargetCustom(barRect, ImGui::GetID("##navDrop"))) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
                const entt::entity dragged = *(const entt::entity*)p->Data;
                if (world.Registry.valid(dragged)) {
                    const std::vector<entt::entity> rows = IsSelected(dragged) ? GetSelectedItems() : std::vector<entt::entity>{dragged};
                    for (entt::entity e : rows)
                        if (world.Registry.valid(e) && world.Registry.all_of<OrderComponent>(e))
                            Enhancers::AddUnique(us.EntityBookmarks, refFor(e), Enhancers::EnhancerUserState::kMaxBookmarks);
                    us.MarkDirty();
                }
            }
            ImGui::EndDragDropTarget();
        }
    };

    if (!chips) {
        // One bookmark button at the bar's right end; the list opens under it.
        std::vector<BookmarkRow> rows;
        for (int i = 0; i < (int)us.EntityBookmarks.size(); ++i) {
            Enhancers::EditorRef& r = us.EntityBookmarks[(size_t)i];
            if (r.Kind != Enhancers::RefKind::Entity || r.Scene != sceneKey) continue;
            BookmarkRow row;
            row.Index = i;
            const entt::entity e = FindEntityByOrder(world, r.Order);
            row.Missing = e == entt::null;
            if (!row.Missing) {
                if (const auto* n = world.Registry.try_get<NameComponent>(e); n && !n->Name.empty()) r.Label = n->Name;
                if (const auto* st = world.Registry.try_get<HierarchyStyleComponent>(e)) {
                    if (!st->Icon.empty()) if (const char* g = Enhancers::FAIconGlyph(st->Icon.c_str())) row.Icon = g;
                    row.Color = st->Color;
                }
            }
            if (row.Icon.empty()) row.Icon = ICON_FA_CUBE;
            row.Label = r.Label.empty() ? std::string("(unnamed)") : r.Label;
            row.Tooltip = row.Missing ? row.Label + "\nNot found in this scene." : row.Label + "\nClick to select (Ctrl+click adds). Drag to reorder.";
            rows.push_back(std::move(row));
        }
        const auto* pn = primaryValid ? world.Registry.try_get<NameComponent>(m_Selected) : nullptr;
        const std::string currentName = primaryValid ? (pn && !pn->Name.empty() ? pn->Name : std::string("(unnamed)")) : std::string();
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(barMin.x + barW - h, barMin.y));
        BookmarkAction a;
        BookmarkButton("##hierBm", h, primaryValid ? currentName.c_str() : nullptr, primaryStarred, rows,
                       DragOverRect(barRect, {"HIERARCHY_ENTITY"}), a);
        if (a.Toggle) toggleSelection();
        if (a.Activate >= 0) {
            const entt::entity e = FindEntityByOrder(world, us.EntityBookmarks[(size_t)a.Activate].Order);
            if (e != entt::null) { SelectItem(e, a.Additive); m_HierarchyScrollToEntity = e; }
        }
        if (ApplyBookmarkEdits(us.EntityBookmarks, a)) us.MarkDirty();
        acceptRowDrop();
        ImGui::PopStyleVar();
        ImGui::PopID();
        return;
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!primaryValid);
    if (EditorUIPrimitives::ActionButton(ICON_FA_BOOKMARK, primaryStarred ? "Remove the selection's bookmark"
                                                                         : "Bookmark the selection (or drop rows on this bar)",
                                         &Tip, primaryStarred, ImVec2(h, h)))
        toggleSelection();
    ImGui::EndDisabled();

    // --- Bookmark chips (this scene's only) -----------------------------------------------------
    int removeAt = -1;
    std::vector<int> overflow;
    const float right = barMin.x + barW;
    for (int i = 0; i < (int)us.EntityBookmarks.size(); ++i) {
        Enhancers::EditorRef& r = us.EntityBookmarks[(size_t)i];
        if (r.Kind != Enhancers::RefKind::Entity || r.Scene != sceneKey) continue;
        const entt::entity e = FindEntityByOrder(world, r.Order);
        const bool found = e != entt::null;
        if (found)
            if (const auto* n = world.Registry.try_get<NameComponent>(e); n && !n->Name.empty())
                r.Label = n->Name; // display refresh only - written with the next real change
        const char* icon = nullptr;
        if (found)
            if (const auto* st = world.Registry.try_get<HierarchyStyleComponent>(e); st && !st->Icon.empty())
                icon = Enhancers::FAIconGlyph(st->Icon.c_str());
        const std::string label = r.Label.empty() ? std::string("(unnamed)") : r.Label;

        ImGui::SameLine();
        const float avail = right - ImGui::GetCursorScreenPos().x - h - EditorTheme::Px(4.0f);
        if (avail < EditorTheme::Px(48.0f) || !overflow.empty()) { overflow.push_back(i); continue; }
        ImGui::PushID(i);
        bool clicked = false, hovered = false;
        NavChip("##chip", icon, label.c_str(), !found, std::min(avail, EditorTheme::Px(140.0f)), clicked, hovered);
        if (clicked && found) {
            SelectItem(e, ImGui::GetIO().KeyCtrl);
            m_HierarchyScrollToEntity = e;
        }
        if (hovered)
            EditorUI::SetTooltip(found ? "%s\nClick to select (Ctrl+click adds), right-click to remove."
                                       : "%s\nNot found in this scene - right-click to remove.", label.c_str());
        if (ImGui::BeginPopupContextItem("##chipCtx")) {
            if (ImGui::MenuItem(ICON_FA_TRASH "  Remove bookmark")) removeAt = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (!overflow.empty()) {
        ImGui::SameLine();
        char more[16];
        std::snprintf(more, sizeof(more), "+%d", (int)overflow.size());
        if (EditorUIPrimitives::ActionButton(more, "More bookmarks", &Tip, false, ImVec2(0, h))) ImGui::OpenPopup("##navMore");
        if (ImGui::BeginPopup("##navMore")) {
            for (int i : overflow) {
                const Enhancers::EditorRef& r = us.EntityBookmarks[(size_t)i];
                const entt::entity e = FindEntityByOrder(world, r.Order);
                ImGui::PushID(i);
                ImGui::BeginDisabled(e == entt::null);
                if (ImGui::Selectable(r.Label.empty() ? "(unnamed)" : r.Label.c_str())) {
                    SelectItem(e, false);
                    m_HierarchyScrollToEntity = e;
                }
                ImGui::EndDisabled();
                if (ImGui::BeginPopupContextItem("##moreCtx")) {
                    if (ImGui::MenuItem(ICON_FA_TRASH "  Remove bookmark")) removeAt = i;
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    }
    if (removeAt >= 0) {
        us.EntityBookmarks.erase(us.EntityBookmarks.begin() + removeAt);
        us.MarkDirty();
    }

    // --- Drop rows anywhere on the bar to bookmark them ----------------------------------------
    acceptRowDrop();

    ImGui::PopStyleVar();
    ImGui::PopID();
}

// ============================================================================================
// vInspector nav bar
// ============================================================================================

namespace {
// Chip icon for an asset bookmark, from the key's extension alone (no disk access - this runs
// every frame per chip). Mirrors the Asset Browser grid's kind glyphs for the common kinds.
const char* AssetChipIcon(const std::string& key) {
    const size_t dot = key.find_last_of('.');
    if (dot == std::string::npos || key.find('/', dot) != std::string::npos) return ICON_FA_FILE;
    char ext[16] = {};
    for (size_t i = dot + 1, n = 0; i < key.size() && n + 1 < sizeof(ext); ++i, ++n)
        ext[n] = (char)std::tolower((unsigned char)key[i]);
    auto is = [&](std::initializer_list<const char*> l) {
        for (const char* e : l) if (std::strcmp(ext, e) == 0) return true;
        return false;
    };
    if (is({"glb", "gltf", "fbx", "obj", "dae", "blend"})) return ICON_FA_CUBE;
    if (is({"png", "jpg", "jpeg", "tga", "bmp", "ktx", "ktx2", "dds", "psd"})) return ICON_FA_IMAGE;
    if (is({"hdr", "exr"})) return ICON_FA_SUN;
    if (is({"mat", "material"})) return ICON_FA_DROPLET;
    if (is({"prefab"})) return ICON_FA_BOX_ARCHIVE;
    if (is({"wav", "ogg", "mp3", "flac"})) return ICON_FA_MUSIC;
    if (is({"cs"})) return ICON_FA_SCROLL;
    if (is({"glsl", "vert", "frag", "comp", "hlsl", "shader"})) return ICON_FA_FILE_CODE;
    if (is({"json"})) return ICON_FA_MAP;
    return ICON_FA_FILE;
}

std::string AssetChipLabel(const std::string& key) {
    const size_t slash = key.find_last_of("/\\");
    return slash == std::string::npos ? key : key.substr(slash + 1);
}
} // namespace

// [<] [>] [bookmark] [chips ... +N] above the Inspector body. Drawn inside DrawInspectorBody
// AFTER the lock swap, so the bookmark toggle names what the Inspector shows (the locked object
// while locked) - but selection changes come back through `act` and are applied by the caller
// once the live selection is restored, or the swap-back would undo them.
void EditorLayer::DrawInspectorNavBar(World& world, InspectorNavAction& act) {
    if (!EditorSettings::Get().InspectorNavBar) return;
    auto& us = Enhancers::EnhancerUserState::Get();
    auto& bm = us.InspectorBookmarks;
    const std::string sceneKey = CurrentSceneKey();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 barMin = ImGui::GetCursorScreenPos();
    const float barW = ImGui::GetContentRegionAvail().x;
    ImGui::PushID("##inspNav");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(EditorTheme::Px(4.0f), ImGui::GetStyle().ItemSpacing.y));

    ImGui::BeginDisabled(!CanSelectionHistoryBack());
    if (EditorUIPrimitives::ActionButton(ICON_FA_ARROW_LEFT, "Previous selection (Ctrl+[)", &Tip, false, ImVec2(h, h)))
        act.Kind = InspectorNavAction::Back;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanSelectionHistoryForward());
    if (EditorUIPrimitives::ActionButton(ICON_FA_ARROW_RIGHT, "Next selection (Ctrl+])", &Tip, false, ImVec2(h, h)))
        act.Kind = InspectorNavAction::Forward;
    ImGui::EndDisabled();

    // --- Bookmark what the Inspector shows ------------------------------------------------------
    auto entityRef = [&](entt::entity e) {
        const auto* o = world.Registry.try_get<OrderComponent>(e);
        const auto* n = world.Registry.try_get<NameComponent>(e);
        return Enhancers::EditorRef::MakeEntity(sceneKey, o ? o->Value : -1, n ? n->Name : std::string());
    };
    Enhancers::EditorRef current;
    bool haveCurrent = false;
    if (m_Selected != entt::null && world.Registry.valid(m_Selected) && world.Registry.all_of<OrderComponent>(m_Selected)) {
        current = entityRef(m_Selected);
        haveCurrent = true;
    } else if (m_Selected == entt::null && !m_SelectedAssetKey.empty() && !m_SelectedAssetIsFolder) {
        current = Enhancers::EditorRef::MakeAsset(m_SelectedAssetKey, AssetChipLabel(m_SelectedAssetKey));
        haveCurrent = true;
    }
    const bool starred = haveCurrent && Enhancers::FindRef(bm, current) >= 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveCurrent);
    if (EditorUIPrimitives::ActionButton(ICON_FA_BOOKMARK, starred ? "Remove the bookmark"
                                                                  : "Bookmark what the Inspector shows (or drop objects / assets on this bar)",
                                         &Tip, starred, ImVec2(h, h))) {
        if (starred) Enhancers::RemoveRef(bm, current);
        else Enhancers::AddUnique(bm, current, Enhancers::EnhancerUserState::kMaxBookmarks);
        us.MarkDirty();
    }
    ImGui::EndDisabled();

    // --- Chips: this scene's objects + every asset ------------------------------------------------
    int removeAt = -1;
    std::vector<int> overflow;
    const float right = barMin.x + barW;
    auto resolve = [&](Enhancers::EditorRef& r, entt::entity& e, const char*& icon) {
        e = entt::null;
        icon = nullptr;
        if (r.Kind == Enhancers::RefKind::Asset) { icon = AssetChipIcon(r.Path); return true; }
        e = FindEntityByOrder(world, r.Order);
        if (e == entt::null) return false;
        if (const auto* n = world.Registry.try_get<NameComponent>(e); n && !n->Name.empty())
            r.Label = n->Name; // display refresh only - written with the next real change
        if (const auto* st = world.Registry.try_get<HierarchyStyleComponent>(e); st && !st->Icon.empty())
            icon = Enhancers::FAIconGlyph(st->Icon.c_str());
        if (!icon) icon = ICON_FA_CUBE;
        return true;
    };
    auto activate = [&](const Enhancers::EditorRef& r, entt::entity e) {
        if (r.Kind == Enhancers::RefKind::Asset) { act.Kind = InspectorNavAction::SelectAsset; act.Asset = r.Path; }
        else if (e != entt::null) { act.Kind = InspectorNavAction::SelectEntity; act.Entity = e; act.Additive = ImGui::GetIO().KeyCtrl; }
    };
    auto shown = [&](const Enhancers::EditorRef& r) {
        return r.Kind == Enhancers::RefKind::Asset || (r.Kind == Enhancers::RefKind::Entity && r.Scene == sceneKey);
    };
    for (int i = 0; i < (int)bm.size(); ++i) {
        Enhancers::EditorRef& r = bm[(size_t)i];
        if (!shown(r)) continue;
        ImGui::SameLine();
        const float avail = right - ImGui::GetCursorScreenPos().x - h - EditorTheme::Px(4.0f);
        if (avail < EditorTheme::Px(48.0f) || !overflow.empty()) { overflow.push_back(i); continue; }
        entt::entity e;
        const char* icon;
        const bool found = resolve(r, e, icon);
        const char* label = r.Label.empty() ? "(unnamed)" : r.Label.c_str();
        ImGui::PushID(i);
        bool clicked = false, hovered = false;
        NavChip("##chip", icon, label, !found, std::min(avail, EditorTheme::Px(140.0f)), clicked, hovered);
        if (clicked) activate(r, e);
        if (hovered) {
            if (r.Kind == Enhancers::RefKind::Asset)
                EditorUI::SetTooltip("%s\nClick to inspect, right-click to remove.", r.Path.c_str());
            else
                EditorUI::SetTooltip(found ? "%s\nClick to select (Ctrl+click adds), right-click to remove."
                                           : "%s\nNot found in this scene - right-click to remove.", label);
        }
        if (ImGui::BeginPopupContextItem("##chipCtx")) {
            if (ImGui::MenuItem(ICON_FA_TRASH "  Remove bookmark")) removeAt = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (!overflow.empty()) {
        ImGui::SameLine();
        char more[16];
        std::snprintf(more, sizeof(more), "+%d", (int)overflow.size());
        if (EditorUIPrimitives::ActionButton(more, "More bookmarks", &Tip, false, ImVec2(0, h))) ImGui::OpenPopup("##navMore");
        if (ImGui::BeginPopup("##navMore")) {
            for (int i : overflow) {
                Enhancers::EditorRef& r = bm[(size_t)i];
                entt::entity e;
                const char* icon;
                const bool found = resolve(r, e, icon);
                char line[256];
                std::snprintf(line, sizeof(line), "%s  %s", icon ? icon : ICON_FA_CUBE, r.Label.empty() ? "(unnamed)" : r.Label.c_str());
                ImGui::PushID(i);
                ImGui::BeginDisabled(!found);
                if (ImGui::Selectable(line)) activate(r, e);
                ImGui::EndDisabled();
                if (ImGui::BeginPopupContextItem("##moreCtx")) {
                    if (ImGui::MenuItem(ICON_FA_TRASH "  Remove bookmark")) removeAt = i;
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    }
    if (removeAt >= 0) {
        bm.erase(bm.begin() + removeAt);
        us.MarkDirty();
    }

    // --- Drop Hierarchy rows or assets anywhere on the bar to bookmark them -----------------------
    const ImRect barRect(barMin, ImVec2(barMin.x + barW, barMin.y + h));
    if (ImGui::BeginDragDropTargetCustom(barRect, ImGui::GetID("##navDrop"))) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
            const entt::entity dragged = *(const entt::entity*)p->Data;
            if (world.Registry.valid(dragged)) {
                const std::vector<entt::entity> rows = IsSelected(dragged) ? GetSelectedItems() : std::vector<entt::entity>{dragged};
                for (entt::entity e : rows)
                    if (world.Registry.valid(e) && world.Registry.all_of<OrderComponent>(e))
                        Enhancers::AddUnique(bm, entityRef(e), Enhancers::EnhancerUserState::kMaxBookmarks);
                us.MarkDirty();
            }
        }
        static const char* const kAssetPayloads[] = {"ASSET_MODEL_PATH", "ASSET_TEXTURE_PATH", "ASSET_MATERIAL_PATH",
                                                     "ASSET_PREFAB_PATH", "ASSET_SOUND_PATH", "ASSET_FILE_PATH"};
        for (const char* type : kAssetPayloads) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type)) {
                const std::string key((const char*)p->Data);
                if (!key.empty()) {
                    Enhancers::AddUnique(bm, Enhancers::EditorRef::MakeAsset(key, AssetChipLabel(key)),
                                         Enhancers::EnhancerUserState::kMaxBookmarks);
                    us.MarkDirty();
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::PopStyleVar();
    ImGui::PopID();
}

void EditorLayer::DrawInspectorHeaderNav(World& world, InspectorNavAction& act, ImVec2 rowMin, bool stripShown) {
    auto& us = Enhancers::EnhancerUserState::Get();
    auto& bm = us.InspectorBookmarks;
    const std::string sceneKey = CurrentSceneKey();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 resume = ImGui::GetCursorScreenPos();
    const float rowW = std::max(ImGui::GetCurrentWindow()->WorkRect.Max.x - rowMin.x, 3.0f * h);
    const float right = rowMin.x + rowW;
    ImGui::PushID("##inspHead");
    ImGui::SetCursorScreenPos(rowMin);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(EditorTheme::Px(2.0f), ImGui::GetStyle().ItemSpacing.y));

    ImGui::BeginDisabled(!CanSelectionHistoryBack());
    if (EditorUIPrimitives::ActionButton(ICON_FA_ARROW_LEFT, "Previous selection (Ctrl+[)", &Tip, false, ImVec2(h, h)))
        act.Kind = InspectorNavAction::Back;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanSelectionHistoryForward());
    if (EditorUIPrimitives::ActionButton(ICON_FA_ARROW_RIGHT, "Next selection (Ctrl+])", &Tip, false, ImVec2(h, h)))
        act.Kind = InspectorNavAction::Forward;
    ImGui::EndDisabled();
    const float navRight = ImGui::GetItemRectMax().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // What the Inspector shows (the locked or tab target by now): the bookmark toggle's subject,
    // and the row's title while there are no tabs.
    auto entityRef = [&](entt::entity e) {
        const auto* o = world.Registry.try_get<OrderComponent>(e);
        const auto* n = world.Registry.try_get<NameComponent>(e);
        return Enhancers::EditorRef::MakeEntity(sceneKey, o ? o->Value : -1, n ? n->Name : std::string());
    };
    Enhancers::EditorRef current;
    bool haveCurrent = false;
    std::string currentName;
    const char* currentIcon = nullptr;
    unsigned currentColor = 0;
    if (m_Selected != entt::null && world.Registry.valid(m_Selected) && world.Registry.all_of<OrderComponent>(m_Selected)) {
        current = entityRef(m_Selected);
        haveCurrent = true;
        currentName = current.Label.empty() ? std::string("(unnamed)") : current.Label;
        currentIcon = ICON_FA_CUBE;
        if (const auto* st = world.Registry.try_get<HierarchyStyleComponent>(m_Selected)) {
            if (!st->Icon.empty()) if (const char* g = Enhancers::FAIconGlyph(st->Icon.c_str())) currentIcon = g;
            currentColor = st->Color;
        }
    } else if (m_Selected == entt::null && !m_SelectedAssetKey.empty() && !m_SelectedAssetIsFolder) {
        current = Enhancers::EditorRef::MakeAsset(m_SelectedAssetKey, AssetChipLabel(m_SelectedAssetKey));
        haveCurrent = true;
        currentName = current.Label;
        currentIcon = AssetChipIcon(m_SelectedAssetKey);
    }
    const bool starred = haveCurrent && Enhancers::FindRef(bm, current) >= 0;

    // No tabs: the inspected item's icon and name, so the row still says something useful.
    if (!stripShown) {
        const float x0 = navRight + EditorTheme::Px(10.0f);
        const float x1 = right - h - EditorTheme::Px(10.0f);
        const float cy = rowMin.y + h * 0.5f;
        if (x1 > x0 + EditorTheme::Px(24.0f)) {
            float x = x0;
            if (haveCurrent && currentIcon) {
                const ImVec2 is = ImGui::CalcTextSize(currentIcon);
                dl->AddText(ImVec2(x, cy - is.y * 0.5f),
                            EditorTheme::U32(currentColor ? EditorTheme::UserGlyphColor(currentColor) : EditorTheme::Secondary), currentIcon);
                x += is.x + EditorTheme::Px(7.0f);
            }
            std::string title = haveCurrent ? currentName : std::string(HasGroupSelection() ? "" : "Nothing selected");
            if (HasGroupSelection()) title = std::to_string(1 + (int)m_ExtraSelection.size()) + " objects selected";
            const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
            EditorUIPrimitives::TextEllipsis(dl, ImVec2(x, cy - ts.y * 0.5f), x1,
                                             EditorTheme::U32(haveCurrent || HasGroupSelection() ? EditorTheme::Text : EditorTheme::Dim),
                                             title.c_str());
        }
        dl->AddLine(ImVec2(rowMin.x, rowMin.y + h - 0.5f), ImVec2(right, rowMin.y + h - 0.5f), EditorTheme::U32(EditorTheme::Hairline));
    }

    // --- Bookmarks ----------------------------------------------------------------------------------
    std::vector<BookmarkRow> rows;
    for (int i = 0; i < (int)bm.size(); ++i) {
        Enhancers::EditorRef& r = bm[(size_t)i];
        BookmarkRow row;
        row.Index = i;
        if (r.Kind == Enhancers::RefKind::Asset) {
            row.Icon = AssetChipIcon(r.Path);
            row.Label = r.Label.empty() ? AssetChipLabel(r.Path) : r.Label;
            row.Tooltip = r.Path + "\nClick to inspect. Drag to reorder.";
        } else if (r.Kind == Enhancers::RefKind::Entity && r.Scene == sceneKey) {
            const entt::entity e = FindEntityByOrder(world, r.Order);
            row.Missing = e == entt::null;
            if (!row.Missing) {
                if (const auto* n = world.Registry.try_get<NameComponent>(e); n && !n->Name.empty()) r.Label = n->Name;
                if (const auto* st = world.Registry.try_get<HierarchyStyleComponent>(e)) {
                    if (!st->Icon.empty()) if (const char* g = Enhancers::FAIconGlyph(st->Icon.c_str())) row.Icon = g;
                    row.Color = st->Color;
                }
            }
            if (row.Icon.empty()) row.Icon = ICON_FA_CUBE;
            row.Label = r.Label.empty() ? std::string("(unnamed)") : r.Label;
            row.Tooltip = row.Missing ? row.Label + "\nNot found in this scene." : row.Label + "\nClick to select (Ctrl+click adds). Drag to reorder.";
        } else {
            continue; // another scene's object
        }
        rows.push_back(std::move(row));
    }
    const ImVec2 btnMin(right - h, rowMin.y);
    const ImRect btnRect(btnMin, ImVec2(right, rowMin.y + h));
    ImGui::SetCursorScreenPos(btnMin);
    BookmarkAction a;
    BookmarkButton("##inspBm", h, haveCurrent ? currentName.c_str() : nullptr, starred, rows,
                   DragOverRect(btnRect, {"HIERARCHY_ENTITY", "ASSET_MODEL_PATH", "ASSET_TEXTURE_PATH", "ASSET_MATERIAL_PATH",
                                          "ASSET_PREFAB_PATH", "ASSET_SOUND_PATH", "ASSET_FILE_PATH"}), a, m_ShotOpenBookmarks);
    m_ShotOpenBookmarks = false;
    if (a.Toggle && haveCurrent) {
        if (starred) Enhancers::RemoveRef(bm, current);
        else Enhancers::AddUnique(bm, current, Enhancers::EnhancerUserState::kMaxBookmarks);
        us.MarkDirty();
    }
    if (a.Activate >= 0) {
        const Enhancers::EditorRef& r = bm[(size_t)a.Activate];
        if (r.Kind == Enhancers::RefKind::Asset) { act.Kind = InspectorNavAction::SelectAsset; act.Asset = r.Path; }
        else if (const entt::entity e = FindEntityByOrder(world, r.Order); e != entt::null) {
            act.Kind = InspectorNavAction::SelectEntity;
            act.Entity = e;
            act.Additive = a.Additive;
        }
    }
    if (ApplyBookmarkEdits(bm, a)) us.MarkDirty();

    // Drop Hierarchy rows or assets on the button (or the Back / Forward end) to bookmark them; the
    // tab strip between takes drops as new tabs.
    if (ImGui::BeginDragDropTargetCustom(btnRect, ImGui::GetID("##inspBmDrop"))) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
            const entt::entity dragged = *(const entt::entity*)p->Data;
            if (world.Registry.valid(dragged)) {
                const std::vector<entt::entity> sel = IsSelected(dragged) ? GetSelectedItems() : std::vector<entt::entity>{dragged};
                for (entt::entity e : sel)
                    if (world.Registry.valid(e) && world.Registry.all_of<OrderComponent>(e))
                        Enhancers::AddUnique(bm, entityRef(e), Enhancers::EnhancerUserState::kMaxBookmarks);
                us.MarkDirty();
            }
        }
        static const char* const kAssetPayloads[] = {"ASSET_MODEL_PATH", "ASSET_TEXTURE_PATH", "ASSET_MATERIAL_PATH",
                                                     "ASSET_PREFAB_PATH", "ASSET_SOUND_PATH", "ASSET_FILE_PATH"};
        for (const char* type : kAssetPayloads)
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type)) {
                const std::string key((const char*)p->Data);
                if (!key.empty()) {
                    Enhancers::AddUnique(bm, Enhancers::EditorRef::MakeAsset(key, AssetChipLabel(key)), Enhancers::EnhancerUserState::kMaxBookmarks);
                    us.MarkDirty();
                }
            }
        ImGui::EndDragDropTarget();
    }

    ImGui::PopStyleVar();
    ImGui::PopID();
    // Continue below the row: under the strip if it drew (it already moved the cursor), else here.
    ImGui::SetCursorScreenPos(stripShown ? resume : ImVec2(rowMin.x, rowMin.y + h + EditorTheme::Px(4.0f)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void EditorLayer::ApplyInspectorNavAction(World& world, const InspectorNavAction& act) {
    // Every nav-bar action is about the selection: show it (the Selection tab).
    if (auto& tabs = Enhancers::TabState::Get().Inspector; act.Kind != InspectorNavAction::None && tabs.Active >= 0) {
        tabs.Active = -1;
        Enhancers::TabState::Get().MarkDirty();
    }
    switch (act.Kind) {
        case InspectorNavAction::Back:    SelectionHistoryBack(world); break;
        case InspectorNavAction::Forward: SelectionHistoryForward(world); break;
        case InspectorNavAction::SelectEntity:
            if (act.Entity != entt::null && world.Registry.valid(act.Entity)) {
                SelectItem(act.Entity, act.Additive);
                m_HierarchyScrollToEntity = act.Entity;
            }
            break;
        case InspectorNavAction::SelectAsset:
            ClearSelection();
            ClearAssetSelection();
            m_SelectedAssetKey = act.Asset;
            m_SelectedAssetIsFolder = false;
            break;
        case InspectorNavAction::None: break;
    }
}

// ============================================================================================
// vTabs
// ============================================================================================

namespace {
// A stable ImGui ID per tab target (so an active drag follows the tab as it moves), with an
// occurrence count for the Asset Browser's duplicate folder tabs.
std::string TabKey(const Enhancers::EditorRef& r) {
    return std::to_string((int)r.Kind) + "|" + r.Scene + "|" + std::to_string(r.Order) + "|" + r.Path + "|" + r.Sub;
}

std::string FolderLeaf(const std::string& path) {
    if (path.empty()) return "Assets";
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool IsStarred(const Enhancers::EditorRef& r) {
    return Enhancers::FindRef(Enhancers::TabState::Get().Starred, r) >= 0;
}

// Top-scoring fuzzy matches of `query` over `count` candidates (index -> text), best first.
template <class TextFn>
std::vector<int> FuzzyTop(const char* query, int count, TextFn text, int limit) {
    std::vector<std::pair<int, int>> scored; // (score, index)
    for (int i = 0; i < count; ++i) {
        const int s = Enhancers::FuzzyScore(query, text(i));
        if (s >= 0) scored.emplace_back(s, i);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<int> out;
    for (size_t i = 0; i < scored.size() && (int)i < limit; ++i) out.push_back(scored[i].second);
    return out;
}
} // namespace

bool EditorLayer::DrawTabStrip(const char* id, Enhancers::TabStrip& strip, TabStripView& view,
                               const std::vector<TabStripItem>& items, const char* liveLabel, const char* liveIcon,
                               const std::function<void()>& plusMenu, const std::function<void()>& acceptDrop,
                               bool showDropHint, float leftInset, float rightInset) {
    auto& ts = Enhancers::TabState::Get();
    ImGuiIO& io = ImGui::GetIO();
    const int before = strip.Active;
    const float h = ImGui::GetFrameHeight();
    // The row is the full width; the strip sits between the insets the caller keeps for its own
    // buttons (the Inspector's Back / Forward and the bookmark button share this row).
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float rowW = ImGui::GetContentRegionAvail().x;
    const ImVec2 barMin(rowMin.x + leftInset, rowMin.y);
    const float barW = std::max(rowW - leftInset - rightInset, h * 2.0f);
    const float plusW = h;
    const ImVec2 stripMin = barMin;
    const ImVec2 stripMax(barMin.x + std::max(barW - plusW - EditorTheme::Px(4.0f), 1.0f), barMin.y + h);
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(rowMin.x, stripMax.y - 0.5f), ImVec2(rowMin.x + rowW, stripMax.y - 0.5f), EditorTheme::U32(EditorTheme::Hairline));

    // Layout: the live tab (index -1) then the strip's tabs.
    EditorTheme::PushSmall();
    const float padX = EditorTheme::Px(8.0f), gap = EditorTheme::Px(5.0f), closeW = EditorTheme::Px(14.0f);
    const int first = liveLabel ? -1 : 0;
    const int n = (int)strip.Tabs.size();
    std::vector<float> xs, ws;
    xs.reserve((size_t)(n + 1)); ws.reserve((size_t)(n + 1));
    float x = 0.0f;
    for (int i = first; i < n; ++i) {
        const char* icon = i < 0 ? liveIcon : (i < (int)items.size() && !items[(size_t)i].Icon.empty() ? items[(size_t)i].Icon.c_str() : nullptr);
        const char* label = i < 0 ? liveLabel : (i < (int)items.size() ? items[(size_t)i].Label.c_str() : "");
        float w = padX * 2.0f + (icon ? ImGui::CalcTextSize(icon).x + gap : 0.0f) + ImGui::CalcTextSize(label).x + (i >= 0 ? closeW : 0.0f);
        w = std::clamp(w, EditorTheme::Px(56.0f), EditorTheme::Px(170.0f));
        xs.push_back(x); ws.push_back(w);
        x += w + EditorTheme::Px(2.0f);
    }
    const float contentW = x;
    const float visibleW = stripMax.x - stripMin.x;
    const float maxScroll = std::max(0.0f, contentW - visibleW);

    // Keep a newly active tab in view.
    if (strip.Active != view.SeenActive) {
        view.SeenActive = strip.Active;
        const size_t k = (size_t)(strip.Active - first);
        if (k < xs.size()) {
            if (xs[k] < view.ScrollTarget) view.ScrollTarget = xs[k];
            else if (xs[k] + ws[k] > view.ScrollTarget + visibleW) view.ScrollTarget = xs[k] + ws[k] - visibleW;
        }
    }

    // Wheel over the strip: scroll; Shift switches tabs; Ctrl+Shift moves the active one.
    const bool overStrip = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(barMin, ImVec2(barMin.x + barW, stripMax.y));
    const ImGuiID wheelOwner = ImGui::GetID("##wheel");
    if (overStrip) {
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, wheelOwner);
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelX, wheelOwner);
        const float wheel = io.MouseWheel != 0.0f ? io.MouseWheel : io.MouseWheelH;
        if (wheel != 0.0f) {
            const int dir = wheel > 0.0f ? -1 : 1;
            if (io.KeyCtrl && io.KeyShift) {
                if (strip.Active >= 0) { strip.Move(strip.Active, strip.Active + dir); ts.MarkDirty(); }
            } else if (io.KeyShift) {
                strip.Active = strip.Step(dir, liveLabel != nullptr);
            } else {
                view.ScrollTarget += dir * EditorTheme::Px(60.0f);
            }
        }
    }
    view.ScrollTarget = std::clamp(view.ScrollTarget, 0.0f, maxScroll);
    view.Scroll += (view.ScrollTarget - view.Scroll) * std::min(1.0f, io.DeltaTime * 16.0f);
    if (std::fabs(view.ScrollTarget - view.Scroll) < 0.5f) view.Scroll = view.ScrollTarget;

    // Tabs.
    ImGui::PushClipRect(stripMin, stripMax, true);
    int closeAt = -1, closeOthers = -2, moveFrom = -1, moveTo = -1;
    bool closeAll = false;
    std::map<std::string, int> seen;
    for (int i = first; i < n; ++i) {
        const size_t k = (size_t)(i - first);
        const ImVec2 mn(stripMin.x + xs[k] - view.Scroll, stripMin.y);
        const ImVec2 mx(mn.x + ws[k], stripMax.y);
        const TabStripItem* item = i >= 0 && i < (int)items.size() ? &items[(size_t)i] : nullptr;
        std::string key = i < 0 ? std::string("##live") : TabKey(strip.Tabs[(size_t)i]);
        key += "#" + std::to_string(seen[key]++);
        ImGui::PushID(key.c_str());
        ImGui::SetCursorScreenPos(mn);
        const bool clicked = ImGui::InvisibleButton("##tab", ImVec2(ws[k], h));
        const bool hov = ImGui::IsItemHovered();
        const bool active = strip.Active == i;
        // Drag to reorder: past a few pixels, the tab trades places with whichever it is over.
        if (i >= 0 && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, EditorTheme::Px(4.0f))) {
            const float mxPos = io.MousePos.x - stripMin.x + view.Scroll;
            for (int j = 0; j < n; ++j) {
                const size_t kj = (size_t)(j - first);
                if (j != i && mxPos >= xs[kj] && mxPos < xs[kj] + ws[kj]) { moveFrom = i; moveTo = j; break; }
            }
        }
        const ImVec2 closeMin(mx.x - closeW - EditorTheme::Px(3.0f), mn.y);
        const bool overClose = i >= 0 && hov && ImGui::IsMouseHoveringRect(closeMin, mx);
        if (clicked) {
            if (overClose) closeAt = i;
            else strip.Active = i;
        }
        if (i >= 0 && hov && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) closeAt = i;
        // Look: the active tab is raised with an accent underline; others are flat.
        const ImU32 bg = EditorTheme::U32(active ? EditorTheme::Raised : (hov ? EditorTheme::Hover : ImVec4(0, 0, 0, 0)));
        dl->AddRectFilled(ImVec2(mn.x, mn.y + EditorTheme::Px(2.0f)), mx, bg, EditorTheme::Px(4.0f), ImDrawFlags_RoundCornersTop);
        if (active) dl->AddLine(ImVec2(mn.x + EditorTheme::Px(3.0f), mx.y - 1.0f), ImVec2(mx.x - EditorTheme::Px(3.0f), mx.y - 1.0f),
                                EditorTheme::U32(EditorTheme::Accent), EditorTheme::Px(2.0f));
        else if (item && item->Color)
            dl->AddLine(ImVec2(mn.x + EditorTheme::Px(6.0f), mx.y - 1.0f), ImVec2(mx.x - EditorTheme::Px(6.0f), mx.y - 1.0f),
                        item->Color | 0xFF000000u, EditorTheme::Px(1.5f));
        const bool dim = item && item->Missing;
        const ImU32 tc = EditorTheme::U32(dim ? EditorTheme::Dim : (active || hov ? EditorTheme::Text : EditorTheme::Secondary));
        const char* icon = i < 0 ? liveIcon : (item && !item->Icon.empty() ? item->Icon.c_str() : nullptr);
        const char* label = i < 0 ? liveLabel : (item ? item->Label.c_str() : "");
        const float cy = (mn.y + mx.y) * 0.5f + EditorTheme::Px(1.0f);
        float tx = mn.x + padX;
        if (icon) {
            const ImVec2 is = ImGui::CalcTextSize(icon);
            dl->AddText(ImVec2(tx, cy - is.y * 0.5f), EditorTheme::U32(active ? EditorTheme::Accent : (dim ? EditorTheme::Dim : EditorTheme::Secondary)), icon);
            tx += is.x + gap;
        }
        const float labelRight = mx.x - (i >= 0 ? closeW + EditorTheme::Px(2.0f) : padX * 0.5f);
        const ImVec2 ls = ImGui::CalcTextSize(label);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tc));
        ImGui::RenderTextEllipsis(dl, ImVec2(tx, cy - ls.y * 0.5f), ImVec2(labelRight, cy + ls.y * 0.5f), labelRight, label, nullptr, &ls);
        ImGui::PopStyleColor();
        if (i >= 0 && (hov || active)) {
            const ImVec2 xs2 = ImGui::CalcTextSize(ICON_FA_XMARK);
            dl->AddText(ImVec2(closeMin.x + (closeW - xs2.x) * 0.5f, cy - xs2.y * 0.5f),
                        EditorTheme::U32(overClose ? EditorTheme::Text : EditorTheme::Dim), ICON_FA_XMARK);
        }
        if (hov && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const std::string& tip = item ? item->Tooltip : std::string();
            EditorUI::SetTooltip("%s%s", i < 0 ? "Follows the selection" : (tip.empty() ? label : tip.c_str()),
                                 i < 0 ? "" : "\nMiddle-click or x closes. Drag to reorder. Shift+wheel switches tabs.");
        }
        if (ImGui::BeginPopupContextItem("##tabCtx")) {
            if (i >= 0) {
                const bool starred = IsStarred(strip.Tabs[(size_t)i]);
                if (ImGui::MenuItem(ICON_FA_STAR "  Starred", nullptr, starred)) {
                    if (starred) Enhancers::RemoveRef(ts.Starred, strip.Tabs[(size_t)i]);
                    else Enhancers::AddUnique(ts.Starred, strip.Tabs[(size_t)i]);
                    ts.MarkDirty();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_XMARK "  Close Tab", "Ctrl+W")) closeAt = i;
            }
            if (ImGui::MenuItem("Close Other Tabs", nullptr, false, n > (i >= 0 ? 1 : 0))) closeOthers = i;
            if (ImGui::MenuItem("Close All Tabs", nullptr, false, n > 0)) closeAll = true;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PopClipRect();
    EditorTheme::PopFont();
    // Overflow fades at the clipped edges.
    if (view.Scroll > 0.5f)
        dl->AddRectFilledMultiColor(stripMin, ImVec2(stripMin.x + EditorTheme::Px(14.0f), stripMax.y),
                                    EditorTheme::U32(EditorTheme::Panel), 0, 0, EditorTheme::U32(EditorTheme::Panel));
    if (view.Scroll < maxScroll - 0.5f)
        dl->AddRectFilledMultiColor(ImVec2(stripMax.x - EditorTheme::Px(14.0f), stripMin.y), stripMax,
                                    0, EditorTheme::U32(EditorTheme::Panel), EditorTheme::U32(EditorTheme::Panel), 0);
    if (showDropHint && n == 0) {
        const char* hint = "Drop here to open a tab";
        const ImVec2 hs = ImGui::CalcTextSize(hint);
        const float hx = stripMin.x + (liveLabel ? xs[0] + ws[0] + EditorTheme::Px(8.0f) : EditorTheme::Px(8.0f));
        if (hx + hs.x < stripMax.x) dl->AddText(ImVec2(hx, (stripMin.y + stripMax.y - hs.y) * 0.5f), EditorTheme::U32(EditorTheme::Dim), hint);
    }

    // "+": the caller's menu (starred tabs and a fuzzy search).
    ImGui::SetCursorScreenPos(ImVec2(barMin.x + barW - plusW, barMin.y));
    if (EditorUIPrimitives::ActionButton(ICON_FA_PLUS, "Open a tab (Ctrl+T)", &Tip, false, ImVec2(plusW, h))) ImGui::OpenPopup("##tabPlus");
    ImGui::SetNextWindowSizeConstraints(ImVec2(EditorTheme::Px(240.0f), 0.0f), ImVec2(EditorTheme::Px(420.0f), EditorTheme::Px(420.0f)));
    if (ImGui::BeginPopup("##tabPlus")) {
        plusMenu();
        ImGui::EndPopup();
    }

    // Drop anything openable anywhere on the bar.
    if (ImGui::BeginDragDropTargetCustom(ImRect(barMin, ImVec2(barMin.x + barW, stripMax.y)), ImGui::GetID("##tabDrop"))) {
        acceptDrop();
        ImGui::EndDragDropTarget();
    }
    ImGui::SetCursorScreenPos(ImVec2(rowMin.x, stripMax.y + EditorTheme::Px(2.0f)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();

    if (moveFrom >= 0) strip.Move(moveFrom, moveTo);
    if (closeAt >= 0) strip.Close(closeAt);
    if (closeOthers != -2) strip.CloseOthers(closeOthers);
    if (closeAll) { while (!strip.Tabs.empty()) strip.Close((int)strip.Tabs.size() - 1); strip.Active = -1; }
    if (!liveLabel && strip.Active < 0 && !strip.Tabs.empty()) strip.Active = 0;
    if (moveFrom >= 0 || closeAt >= 0 || closeOthers != -2 || closeAll || strip.Active != before) ts.MarkDirty();
    return strip.Active != before;
}

// --- Inspector tabs ---------------------------------------------------------------------------

bool EditorLayer::ResolveInspectorTab(const World& world, entt::entity& entity, std::string& asset, bool& missing) const {
    const auto& strip = Enhancers::TabState::Get().Inspector;
    entity = entt::null;
    asset.clear();
    missing = false;
    if (strip.Active < 0 || strip.Active >= (int)strip.Tabs.size()) return false;
    const Enhancers::EditorRef& r = strip.Tabs[(size_t)strip.Active];
    if (r.Kind == Enhancers::RefKind::Asset) { asset = r.Path; return true; }
    if (r.Kind == Enhancers::RefKind::Entity && r.Scene == CurrentSceneKey()) entity = FindEntityByOrder(world, r.Order);
    missing = entity == entt::null;
    return true;
}

void EditorLayer::OpenInspectorTabForCurrent(World& world) {
    auto& ts = Enhancers::TabState::Get();
    entt::entity e = m_Selected;
    std::string assetKey;
    bool missing = false;
    if (ResolveInspectorTab(world, e, assetKey, missing)) return; // a tab is already showing it
    if (m_Selected != entt::null && world.Registry.valid(m_Selected)) {
        const auto* o = world.Registry.try_get<OrderComponent>(m_Selected);
        const auto* nm = world.Registry.try_get<NameComponent>(m_Selected);
        if (!o) return;
        ts.Inspector.Open(Enhancers::EditorRef::MakeEntity(CurrentSceneKey(), o->Value, nm ? nm->Name : std::string()));
    } else if (!m_SelectedAssetKey.empty() && !m_SelectedAssetIsFolder) {
        ts.Inspector.Open(Enhancers::EditorRef::MakeAsset(m_SelectedAssetKey, AssetChipLabel(m_SelectedAssetKey)));
    } else {
        return;
    }
    ts.MarkDirty();
}

bool EditorLayer::DrawInspectorTabStrip(World& world, float leftInset, float rightInset) {
    if (!EditorSettings::Get().InspectorTabs) return false;
    auto& ts = Enhancers::TabState::Get();
    auto& strip = ts.Inspector;
    const std::string sceneKey = CurrentSceneKey();

    // Keyboard (mouse over the Inspector).
    if (Shortcuts::Triggered("inspector.tabs.new")) OpenInspectorTabForCurrent(world);
    if (Shortcuts::Triggered("inspector.tabs.close") && strip.Active >= 0) { strip.Close(strip.Active); ts.MarkDirty(); }
    if (Shortcuts::Triggered("inspector.tabs.reopen") && strip.Reopen() >= 0) ts.MarkDirty();

    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    const bool dragging = drag && (drag->IsDataType("HIERARCHY_ENTITY") || drag->IsDataType("INSPECTOR_COMPONENT") ||
                                   drag->IsDataType("ASSET_MODEL_PATH") || drag->IsDataType("ASSET_TEXTURE_PATH") ||
                                   drag->IsDataType("ASSET_MATERIAL_PATH") || drag->IsDataType("ASSET_PREFAB_PATH") ||
                                   drag->IsDataType("ASSET_SOUND_PATH") || drag->IsDataType("ASSET_FILE_PATH"));
    if (strip.Tabs.empty() && !dragging) return false; // nothing to show: no strip until there is a tab

    std::vector<TabStripItem> items;
    items.reserve(strip.Tabs.size());
    for (Enhancers::EditorRef& r : strip.Tabs) {
        TabStripItem it;
        if (r.Kind == Enhancers::RefKind::Asset) {
            it.Icon = AssetChipIcon(r.Path);
            it.Label = r.Label.empty() ? AssetChipLabel(r.Path) : r.Label;
            it.Tooltip = r.Path;
        } else {
            const entt::entity e = r.Scene == sceneKey ? FindEntityByOrder(world, r.Order) : entt::null;
            if (e != entt::null) {
                if (const auto* nm = world.Registry.try_get<NameComponent>(e); nm && !nm->Name.empty()) r.Label = nm->Name;
                if (const auto* st = world.Registry.try_get<HierarchyStyleComponent>(e); st && !st->Icon.empty())
                    if (const char* g = Enhancers::FAIconGlyph(st->Icon.c_str())) it.Icon = g;
            }
            if (!r.Sub.empty())
                for (const auto& rc : ComponentRegistry::All())
                    if (r.Sub == rc.Meta.Name) { it.Icon = rc.Meta.Icon; break; }
            if (it.Icon.empty()) it.Icon = ICON_FA_CUBE;
            const std::string name = r.Label.empty() ? std::string("(unnamed)") : r.Label;
            it.Label = r.Sub.empty() ? name : r.Sub + " (" + name + ")";
            it.Missing = e == entt::null;
            it.Tooltip = it.Missing ? it.Label + "\nNot in the open scene." : it.Label;
        }
        items.push_back(std::move(it));
    }

    auto entityRef = [&](entt::entity e) {
        const auto* o = world.Registry.try_get<OrderComponent>(e);
        const auto* nm = world.Registry.try_get<NameComponent>(e);
        return Enhancers::EditorRef::MakeEntity(sceneKey, o ? o->Value : -1, nm ? nm->Name : std::string());
    };
    auto plusMenu = [&]() {
        char* q = m_TabSearch[0];
        if (ImGui::IsWindowAppearing()) { q[0] = '\0'; ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##q", ICON_FA_MAGNIFYING_GLASS "  Search objects", q, sizeof(m_TabSearch[0]));
        auto open = [&](const Enhancers::EditorRef& r) { strip.Open(r); ts.MarkDirty(); ImGui::CloseCurrentPopup(); };
        if (m_Selected != entt::null && world.Registry.valid(m_Selected) && world.Registry.all_of<OrderComponent>(m_Selected) &&
            ImGui::Selectable(ICON_FA_PLUS "  Pin the current selection", false))
            open(entityRef(m_Selected));
        bool header = false;
        for (const auto& r : ts.Starred) {
            if (r.Kind == Enhancers::RefKind::Folder || (r.Kind == Enhancers::RefKind::Entity && r.Scene != sceneKey)) continue;
            if (!header) { EditorUIPrimitives::SectionHeader("STARRED"); header = true; }
            const std::string label = r.Kind == Enhancers::RefKind::Asset ? AssetChipLabel(r.Path)
                                    : (r.Sub.empty() ? r.Label : r.Sub + " (" + r.Label + ")");
            ImGui::PushID(&r);
            if (ImGui::Selectable((std::string(ICON_FA_STAR "  ") + label).c_str())) open(r);
            ImGui::PopID();
        }
        std::vector<entt::entity> ents;
        for (auto [e, o, nm] : world.Registry.view<const OrderComponent, const NameComponent>().each()) ents.push_back(e);
        const auto hits = FuzzyTop(q, (int)ents.size(), [&](int i) { return world.Registry.get<NameComponent>(ents[(size_t)i]).Name.c_str(); }, 30);
        EditorUIPrimitives::SectionHeader("OBJECTS");
        if (hits.empty()) ImGui::TextDisabled("No matching objects");
        for (int i : hits) {
            const entt::entity e = ents[(size_t)i];
            const char* icon = ICON_FA_CUBE;
            if (const auto* st = world.Registry.try_get<HierarchyStyleComponent>(e); st && !st->Icon.empty())
                if (const char* g = Enhancers::FAIconGlyph(st->Icon.c_str())) icon = g;
            ImGui::PushID((int)entt::to_integral(e));
            if (ImGui::Selectable((std::string(icon) + "  " + world.Registry.get<NameComponent>(e).Name).c_str())) open(entityRef(e));
            ImGui::PopID();
        }
    };
    auto acceptDrop = [&]() {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
            const entt::entity e = *(const entt::entity*)p->Data;
            if (world.Registry.valid(e) && world.Registry.all_of<OrderComponent>(e)) { strip.Open(entityRef(e)); ts.MarkDirty(); }
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("INSPECTOR_COMPONENT")) {
            const auto* c = (const InspectorComponentPayload*)p->Data;
            const entt::entity e = FindEntityByOrder(world, c->Order);
            const auto* nm = e != entt::null ? world.Registry.try_get<NameComponent>(e) : nullptr;
            strip.Open(Enhancers::EditorRef::MakeComponent(sceneKey, c->Order, c->Component, nm ? nm->Name : std::string()));
            ts.MarkDirty();
        }
        static const char* const kAssetPayloads[] = {"ASSET_MODEL_PATH", "ASSET_TEXTURE_PATH", "ASSET_MATERIAL_PATH",
                                                     "ASSET_PREFAB_PATH", "ASSET_SOUND_PATH", "ASSET_FILE_PATH"};
        for (const char* type : kAssetPayloads)
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type)) {
                const std::string key((const char*)p->Data);
                if (!key.empty()) { strip.Open(Enhancers::EditorRef::MakeAsset(key, AssetChipLabel(key))); ts.MarkDirty(); }
            }
    };
    DrawTabStrip("##inspTabs", strip, m_InspectorTabView, items, "Selection", ICON_FA_ARROW_POINTER, plusMenu, acceptDrop, dragging,
                 leftInset, rightInset);
    return true;
}

// --- Asset Browser tabs -----------------------------------------------------------------------

bool EditorLayer::DrawAssetTabStrip(AssetLibrary& assets, float rightInset, bool alwaysShow) {
    if (!EditorSettings::Get().AssetTabs) return false;
    auto& ts = Enhancers::TabState::Get();
    auto& strip = ts.Assets;
    const auto& folders = assets.Folders();
    auto folderExists = [&](const std::string& p) { return p.empty() || std::find(folders.begin(), folders.end(), p) != folders.end(); };
    auto folderRef = [&](const std::string& p) { return Enhancers::EditorRef::MakeFolder(p, FolderLeaf(p)); };

    // Keyboard (mouse over the Asset Browser). A new tab starts on the current folder.
    if (Shortcuts::Triggered("project.tabs.new")) {
        if (strip.Tabs.empty()) strip.Open(folderRef(m_CurrentAssetFolder)); // first press: the current view becomes tab 1 too
        strip.Open(folderRef(m_CurrentAssetFolder), true, /*allowDuplicate=*/true);
        ts.MarkDirty();
    }
    if (Shortcuts::Triggered("project.tabs.close") && strip.Active >= 0) { strip.Close(strip.Active); ts.MarkDirty(); }
    if (Shortcuts::Triggered("project.tabs.reopen") && strip.Reopen() >= 0) ts.MarkDirty();

    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    const bool dragging = drag && (drag->IsDataType("ASSET_FOLDER_PATH") || drag->IsDataType("ASSET_MODEL_PATH") ||
                                   drag->IsDataType("ASSET_TEXTURE_PATH") || drag->IsDataType("ASSET_MATERIAL_PATH") ||
                                   drag->IsDataType("ASSET_PREFAB_PATH") || drag->IsDataType("ASSET_SOUND_PATH") ||
                                   drag->IsDataType("ASSET_FILE_PATH"));
    if (strip.Tabs.empty() && !dragging && !alwaysShow) return false;

    // The active tab follows navigation: it is the browser's current place.
    if (strip.Active >= 0 && strip.Active < (int)strip.Tabs.size()) {
        Enhancers::EditorRef& cur = strip.Tabs[(size_t)strip.Active];
        if (cur.Path != m_CurrentAssetFolder) { cur.Path = m_CurrentAssetFolder; cur.Label = FolderLeaf(cur.Path); ts.MarkDirty(); }
    }

    std::vector<TabStripItem> items;
    items.reserve(strip.Tabs.size());
    for (const auto& r : strip.Tabs) {
        TabStripItem it;
        EditorFolderVisual vis;
        GetFolderVisual(assets, r.Path, vis);
        it.Icon = vis.Icon[0] ? std::string(vis.Icon) : std::string(r.Path.empty() ? ICON_FA_HOUSE : ICON_FA_FOLDER);
        it.Color = vis.Color;
        it.Label = FolderLeaf(r.Path);
        it.Missing = !folderExists(r.Path);
        it.Tooltip = (r.Path.empty() ? std::string("Assets") : r.Path) + (it.Missing ? "\nThis folder no longer exists." : "");
        items.push_back(std::move(it));
    }

    auto plusMenu = [&]() {
        char* q = m_TabSearch[1];
        if (ImGui::IsWindowAppearing()) { q[0] = '\0'; ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##q", ICON_FA_MAGNIFYING_GLASS "  Search folders", q, sizeof(m_TabSearch[1]));
        auto open = [&](const std::string& p) { strip.Open(folderRef(p), true, /*allowDuplicate=*/true); ts.MarkDirty(); ImGui::CloseCurrentPopup(); };
        if (ImGui::Selectable(ICON_FA_PLUS "  New tab here (Ctrl+T)")) open(m_CurrentAssetFolder);
        bool header = false;
        for (const auto& r : ts.Starred) {
            if (r.Kind != Enhancers::RefKind::Folder) continue;
            if (!header) { EditorUIPrimitives::SectionHeader("STARRED"); header = true; }
            ImGui::PushID(&r);
            if (ImGui::Selectable((std::string(ICON_FA_STAR "  ") + (r.Path.empty() ? "Assets" : r.Path)).c_str())) open(r.Path);
            ImGui::PopID();
        }
        EditorUIPrimitives::SectionHeader("FOLDERS");
        std::vector<std::string> all;
        all.reserve(folders.size() + 1);
        all.emplace_back();
        all.insert(all.end(), folders.begin(), folders.end());
        const auto hits = FuzzyTop(q, (int)all.size(), [&](int i) { return all[(size_t)i].empty() ? "Assets" : all[(size_t)i].c_str(); }, 30);
        if (hits.empty()) ImGui::TextDisabled("No matching folders");
        for (int i : hits) {
            ImGui::PushID(i);
            if (ImGui::Selectable((std::string(ICON_FA_FOLDER "  ") + (all[(size_t)i].empty() ? "Assets" : all[(size_t)i])).c_str())) open(all[(size_t)i]);
            ImGui::PopID();
        }
    };
    auto acceptDrop = [&]() {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_FOLDER_PATH")) {
            strip.Open(folderRef((const char*)p->Data), true, true);
            ts.MarkDirty();
        }
        static const char* const kAssetPayloads[] = {"ASSET_MODEL_PATH", "ASSET_TEXTURE_PATH", "ASSET_MATERIAL_PATH",
                                                     "ASSET_PREFAB_PATH", "ASSET_SOUND_PATH", "ASSET_FILE_PATH"};
        for (const char* type : kAssetPayloads)
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type)) {
                // A file opens its folder in a new tab, with the file selected.
                const std::string key((const char*)p->Data);
                strip.Open(folderRef(assets.AssetFolder(key)), true, true);
                ts.MarkDirty();
                ClearAssetSelection();
                m_SelectedAssetKey = key;
                m_SelectedAssetIsFolder = false;
            }
    };
    // With no tabs yet (the row is up for the bookmark button), the current folder shows as a live
    // tab, like a browser's first tab; Ctrl+T or "+" makes it a real one.
    std::string liveLabel, liveIcon;
    if (strip.Tabs.empty()) {
        EditorFolderVisual vis;
        GetFolderVisual(assets, m_CurrentAssetFolder, vis);
        liveLabel = FolderLeaf(m_CurrentAssetFolder);
        liveIcon = vis.Icon[0] ? std::string(vis.Icon) : std::string(m_CurrentAssetFolder.empty() ? ICON_FA_HOUSE : ICON_FA_FOLDER);
    }
    // Switching tabs navigates; navigation (above) never switches tabs.
    const int before = strip.Active;
    DrawTabStrip("##assetTabs", strip, m_AssetTabView, items, liveLabel.empty() ? nullptr : liveLabel.c_str(),
                 liveIcon.empty() ? nullptr : liveIcon.c_str(), plusMenu, acceptDrop, dragging, 0.0f, rightInset);
    if ((strip.Active != before || (strip.Active >= 0 && strip.Tabs[(size_t)strip.Active].Path != m_CurrentAssetFolder)) &&
        strip.Active >= 0 && strip.Active < (int)strip.Tabs.size()) {
        const std::string& p = strip.Tabs[(size_t)strip.Active].Path;
        if (folderExists(p)) NavigateAssetFolder(p);
    }
    return true;
}

// ============================================================================================
// vFavorites
// ============================================================================================

namespace {
// The binding's chord pressed this frame, ignoring contexts: the overlay suppresses every editor
// shortcut while it is up (Alt+1..4 would otherwise also focus panels), so it checks its own.
bool ChordPressedNow(const char* id) {
    const Shortcuts::Shortcut* s = Shortcuts::Find(id);
    if (!s || !s->Current.IsBound() || s->Current.HasPrefix()) return false;
    const ImGuiIO& io = ImGui::GetIO();
    const Shortcuts::Chord& c = s->Current;
    return ImGui::IsKeyPressed(c.Key, false) && io.KeyCtrl == c.Ctrl && io.KeyShift == c.Shift && io.KeyAlt == c.Alt;
}

struct FavoriteDisplay {
    std::string Icon;
    std::string Label;
    std::string Tooltip;
    unsigned Color = 0;
    bool Missing = false;
};

} // namespace

void EditorLayer::AddSelectionToFavorites(const World& world) {
    auto& us = Enhancers::EnhancerUserState::Get();
    const std::string sceneKey = CurrentSceneKey();
    bool changed = false;
    for (entt::entity e : GetSelectedItems()) {
        const auto* o = world.Registry.valid(e) ? world.Registry.try_get<OrderComponent>(e) : nullptr;
        if (!o) continue;
        const auto* nm = world.Registry.try_get<NameComponent>(e);
        changed |= Enhancers::AddFavorite(us.FavoritePages, m_FavPage, Enhancers::EditorRef::MakeEntity(sceneKey, o->Value, nm ? nm->Name : std::string()));
    }
    if (!changed && !m_SelectedAssetKey.empty()) {
        const Enhancers::EditorRef r = m_SelectedAssetIsFolder
            ? Enhancers::EditorRef::MakeFolder(m_SelectedAssetKey, FolderLeaf(m_SelectedAssetKey))
            : Enhancers::EditorRef::MakeAsset(m_SelectedAssetKey, AssetChipLabel(m_SelectedAssetKey));
        changed = Enhancers::AddFavorite(us.FavoritePages, m_FavPage, r);
    }
    if (changed) {
        us.MarkDirty();
        Log::Info("Favorites: added to \"" + us.FavoritePages[(size_t)std::clamp(m_FavPage, 0, (int)us.FavoritePages.size() - 1)].Name + "\".");
    }
}

void EditorLayer::ActivateFavorite(World& world, AssetLibrary& assets, const Enhancers::EditorRef& r) {
    switch (r.Kind) {
        case Enhancers::RefKind::Folder:
            NavigateAssetFolder(r.Path);
            break;
        case Enhancers::RefKind::Asset:
            NavigateAssetFolder(assets.AssetFolder(r.Path));
            ClearSelection();
            ClearAssetSelection();
            m_SelectedAssetKey = r.Path;
            m_SelectedAssetIsFolder = false;
            break;
        case Enhancers::RefKind::Entity: {
            if (r.Scene != CurrentSceneKey()) {
                // Another scene: open it (with the usual unsaved-changes prompt), then select.
                const std::string path = ResolveScenePath(Enhancers::EditorRef::MakeScene(r.Scene, ""));
                if (path.empty()) { Log::Warn("Favorites: the scene holding \"" + r.Label + "\" can't be found."); return; }
                m_FavPendingScene = r.Scene;
                m_FavPendingOrder = r.Order;
                RequestOpenScene(world, assets, path);
                break;
            }
            const entt::entity e = FindEntityByOrder(world, r.Order);
            if (e == entt::null) { Log::Warn("Favorites: \"" + r.Label + "\" is no longer in this scene."); return; }
            SelectItem(e, false);
            m_HierarchyScrollToEntity = e;
            break;
        }
        case Enhancers::RefKind::Scene:
            break;
    }
    // Opening something closes the overlay; holding the key again re-opens it.
    m_FavLocked = false;
    m_FavWaitRelease = true;
}

void EditorLayer::DrawFavoritesOverlay(World& world, AssetLibrary& assets) {
    const EditorSettings& es = EditorSettings::Get();
    auto& us = Enhancers::EnhancerUserState::Get();
    ImGuiIO& io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    const bool wasVisible = m_FavVisible;
    m_FavVisible = false;

    // A favorite in another scene: select it once that scene has opened.
    if (!m_FavPendingScene.empty() && m_FavPendingScene == CurrentSceneKey()) {
        const entt::entity e = FindEntityByOrder(world, m_FavPendingOrder);
        if (e != entt::null) { SelectItem(e, false); m_HierarchyScrollToEntity = e; }
        m_FavPendingScene.clear();
    }
    if (!es.Favorites) { m_FavLocked = false; m_FavAlpha = 0.0f; return; }

    // Lock / add-selection shortcuts (Triggered while hidden; checked directly while shown,
    // since every editor shortcut is suppressed then).
    if (wasVisible ? ChordPressedNow("favorites.toggle") : Shortcuts::Triggered("favorites.toggle")) { m_FavLocked = !m_FavLocked; m_FavWaitRelease = false; }
    if (!wasVisible && Shortcuts::Triggered("favorites.addSelection")) AddSelectionToFavorites(world);

    // Where: over the Asset Browser's content (centred in the main viewport if it is closed).
    ImGuiWindow* panel = ImGui::FindWindowByName(EditorPanels::Assets);
    const bool panelShown = panel && panel->WasActive && !panel->Hidden && panel->InnerRect.GetWidth() > 80.0f;
    ImRect area;
    if (panelShown) {
        area = panel->InnerRect;
    } else {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 size(std::min(vp->WorkSize.x * 0.6f, EditorTheme::Px(720.0f)), std::min(vp->WorkSize.y * 0.6f, EditorTheme::Px(460.0f)));
        const ImVec2 c(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f);
        area = ImRect(ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), ImVec2(c.x + size.x * 0.5f, c.y + size.y * 0.5f));
    }

    // When: hold the key (Alt, nothing else) over the panel for a moment, or locked open.
    const bool holdDown = es.FavoritesHoldAlt && io.KeyAlt && !io.KeyCtrl && !io.KeyShift;
    if (!holdDown) { m_FavHoldStart = -1.0; m_FavWaitRelease = false; }
    ImGuiWindow* hoveredRoot = GImGui->HoveredWindow ? GImGui->HoveredWindow->RootWindow : nullptr;
    const bool overPanel = panelShown && ImGui::IsMouseHoveringRect(area.Min, area.Max, false) &&
                           (hoveredRoot == panel->RootWindow || (hoveredRoot && std::strcmp(hoveredRoot->Name, "##vFavorites") == 0));
    if (holdDown && !m_FavWaitRelease && m_FavHoldStart < 0.0 && overPanel) m_FavHoldStart = now;
    const bool want = m_FavLocked || (holdDown && !m_FavWaitRelease && m_FavHoldStart >= 0.0 && now - m_FavHoldStart >= 0.12 && (overPanel || wasVisible));
    m_FavAlpha = std::clamp(m_FavAlpha + (want ? 1.0f : -1.0f) * io.DeltaTime * 9.0f, 0.0f, 1.0f);
    if (!want) m_FavRenaming = -1; // a rename left open must not grab the keys when it reopens
    if (m_FavAlpha <= 0.01f) return;
    m_FavVisible = true;

    Enhancers::EnsureFavoritePage(us.FavoritePages);
    const int pageCount = (int)us.FavoritePages.size();
    m_FavPage = std::clamp(m_FavPage, 0, pageCount - 1);
    auto switchPage = [&](int p) {
        p = ((p % pageCount) + pageCount) % pageCount;
        if (p == m_FavPage) return;
        m_FavAnimDir = p > m_FavPage ? 1 : -1;
        m_FavAnimStart = now;
        m_FavPage = p;
        m_FavHighlight = -1;
        m_FavRenaming = -1;
    };

    const float pad = EditorTheme::Px(6.0f);
    ImGui::SetNextWindowPos(ImVec2(area.Min.x + pad, area.Min.y + pad));
    ImGui::SetNextWindowSize(ImVec2(area.GetWidth() - pad * 2.0f, area.GetHeight() - pad * 2.0f));
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_FavAlpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, EditorTheme::Px(6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::Raised);
    ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::Strong);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##vFavorites", nullptr, flags)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const bool typing = ImGui::GetIO().WantTextInput;
        auto& pages = us.FavoritePages;

        // --- Keyboard: 1-9 pick a page, Left / Right step, Up / Down + Enter pick an item, Esc closes.
        if (!typing) {
            for (int k = 0; k < 9 && k < pageCount; ++k)
                if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + k), false) || ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_Keypad1 + k), false)) switchPage(k);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) switchPage(m_FavPage + 1);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  switchPage(m_FavPage - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { m_FavLocked = false; m_FavWaitRelease = true; }
        }
        // Wheel over the overlay: one notch, one page.
        if (hovered && !typing) {
            ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, ImGui::GetID("##favWheel"));
            m_FavWheel += io.MouseWheel;
            if (m_FavWheel <= -1.0f) { switchPage(m_FavPage + 1); m_FavWheel = 0.0f; }
            else if (m_FavWheel >= 1.0f) { switchPage(m_FavPage - 1); m_FavWheel = 0.0f; }
        } else {
            m_FavWheel = 0.0f;
        }

        // --- Header: page chips (double-click renames), "+" page, pin.
        const float h = ImGui::GetFrameHeight();
        int deletePage = -1, movePageFrom = -1, movePageTo = -1;
        EditorTheme::PushSmall();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(EditorTheme::Accent, ICON_FA_STAR);
        EditorTheme::PopFont();
        for (int p = 0; p < pageCount; ++p) {
            ImGui::SameLine();
            ImGui::PushID(p);
            if (m_FavRenaming == p) {
                ImGui::SetNextItemWidth(EditorTheme::Px(120.0f));
                if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
                const bool done = ImGui::InputText("##rename", m_FavRenameBuf, sizeof(m_FavRenameBuf),
                                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (done || ImGui::IsItemDeactivated()) {
                    if (m_FavRenameBuf[0] && pages[(size_t)p].Name != m_FavRenameBuf) { pages[(size_t)p].Name = m_FavRenameBuf; us.MarkDirty(); }
                    m_FavRenaming = -1;
                }
            } else {
                char label[96];
                std::snprintf(label, sizeof(label), "%s%s", p < 9 ? std::to_string(p + 1).append("  ").c_str() : "", pages[(size_t)p].Name.c_str());
                if (EditorUIPrimitives::ActionButton(label, "Click to show, double-click to rename. Drop favorites here to move them.",
                                                     &Tip, p == m_FavPage, ImVec2(0.0f, h)))
                    switchPage(p);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    m_FavRenaming = p;
                    std::snprintf(m_FavRenameBuf, sizeof(m_FavRenameBuf), "%s", pages[(size_t)p].Name.c_str());
                }
                // Drop an item on a page chip to move it there.
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("FAVORITE_ITEM")) {
                        const int from = *(const int*)pl->Data;
                        auto& src = pages[(size_t)m_FavPage].Items;
                        if (p != m_FavPage && from >= 0 && from < (int)src.size()) {
                            const Enhancers::EditorRef moved = src[(size_t)from];
                            src.erase(src.begin() + from);
                            Enhancers::AddFavorite(pages, p, moved);
                            us.MarkDirty();
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::BeginPopupContextItem("##pageCtx")) {
                    if (ImGui::MenuItem(ICON_FA_PEN "  Rename")) {
                        m_FavRenaming = p;
                        std::snprintf(m_FavRenameBuf, sizeof(m_FavRenameBuf), "%s", pages[(size_t)p].Name.c_str());
                    }
                    if (ImGui::MenuItem(ICON_FA_ARROW_LEFT "  Move Left", nullptr, false, p > 0)) { movePageFrom = p; movePageTo = p - 1; }
                    if (ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Move Right", nullptr, false, p + 1 < pageCount)) { movePageFrom = p; movePageTo = p + 1; }
                    ImGui::Separator();
                    if (ImGui::MenuItem(ICON_FA_TRASH "  Delete Page", nullptr, false, pageCount > 1)) deletePage = p;
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }
        ImGui::SameLine();
        if (EditorUIPrimitives::ActionButton(ICON_FA_PLUS, "New page", &Tip, false, ImVec2(h, h))) {
            pages.push_back(Enhancers::FavoritePage{"Page " + std::to_string(pageCount + 1), {}});
            us.MarkDirty();
            // Straight to the new page (switchPage wraps by the page count from before the add).
            m_FavAnimDir = 1;
            m_FavAnimStart = now;
            m_FavPage = pageCount;
            m_FavHighlight = -1;
            m_FavRenaming = pageCount;
            std::snprintf(m_FavRenameBuf, sizeof(m_FavRenameBuf), "%s", pages.back().Name.c_str());
        }
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - h);
        if (EditorUIPrimitives::ActionButton(m_FavLocked ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN,
                                             m_FavLocked ? "Unpin (Esc)" : "Keep open (Ctrl+Alt+F)", &Tip, m_FavLocked, ImVec2(h, h)))
            m_FavLocked = !m_FavLocked;
        if (deletePage >= 0) {
            pages.erase(pages.begin() + deletePage);
            us.MarkDirty();
            m_FavPage = std::clamp(m_FavPage > deletePage ? m_FavPage - 1 : m_FavPage, 0, (int)pages.size() - 1);
        }
        if (movePageFrom >= 0) {
            Enhancers::MoveFavoritePage(pages, movePageFrom, movePageTo);
            if (m_FavPage == movePageFrom) m_FavPage = std::clamp(movePageTo, 0, (int)pages.size() - 1);
            us.MarkDirty();
        }
        ImGui::Separator();

        // --- Items of the shown page, sliding in on a page switch.
        const float t = m_FavAnimStart < 0.0 ? 1.0f : EditorTheme::Ease::OutCubic((float)((now - m_FavAnimStart) / 0.16));
        if (t >= 1.0f) m_FavAnimStart = -1.0;
        const float slide = (1.0f - t) * m_FavAnimDir * EditorTheme::Px(48.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (0.35f + 0.65f * t));
        ImGui::BeginChild("##favItems", ImVec2(0.0f, -ImGui::GetTextLineHeightWithSpacing()), ImGuiChildFlags_None);
        auto& items = pages[(size_t)std::clamp(m_FavPage, 0, (int)pages.size() - 1)].Items;
        const std::string sceneKey = CurrentSceneKey();
        const auto& folders = assets.Folders();

        std::vector<FavoriteDisplay> shown(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            Enhancers::EditorRef& r = items[i];
            FavoriteDisplay& d = shown[i];
            if (r.Kind == Enhancers::RefKind::Folder) {
                EditorFolderVisual vis;
                GetFolderVisual(assets, r.Path, vis);
                d.Icon = vis.Icon[0] ? std::string(vis.Icon) : std::string(r.Path.empty() ? ICON_FA_HOUSE : ICON_FA_FOLDER);
                d.Color = vis.Color;
                d.Label = FolderLeaf(r.Path);
                d.Missing = !r.Path.empty() && std::find(folders.begin(), folders.end(), r.Path) == folders.end();
                d.Tooltip = (r.Path.empty() ? std::string("Assets") : r.Path) + " (folder)";
            } else if (r.Kind == Enhancers::RefKind::Asset) {
                d.Icon = AssetChipIcon(r.Path);
                d.Label = r.Label.empty() ? AssetChipLabel(r.Path) : r.Label;
                d.Tooltip = r.Path;
            } else {
                const bool here = r.Scene == sceneKey;
                const entt::entity e = here ? FindEntityByOrder(world, r.Order) : entt::null;
                d.Icon = ICON_FA_CUBE;
                if (e != entt::null) {
                    if (const auto* nm = world.Registry.try_get<NameComponent>(e); nm && !nm->Name.empty()) r.Label = nm->Name;
                    if (const auto* st = world.Registry.try_get<HierarchyStyleComponent>(e); st && !st->Icon.empty())
                        if (const char* g = Enhancers::FAIconGlyph(st->Icon.c_str())) d.Icon = g;
                    if (const auto* st = world.Registry.try_get<HierarchyStyleComponent>(e); st && st->Color) d.Color = st->Color;
                }
                d.Label = r.Label.empty() ? std::string("(unnamed)") : r.Label;
                d.Missing = here && e == entt::null;
                d.Tooltip = d.Label + (here ? (d.Missing ? "\nNo longer in this scene." : "") : "\nIn another scene - click to open it.");
            }
        }

        // Keyboard highlight.
        if (!typing && !items.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_FavHighlight = std::min(m_FavHighlight + 1, (int)items.size() - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   m_FavHighlight = std::max(m_FavHighlight - 1, 0);
        }
        int activate = -1, removeAt = -1, moveFrom = -1, moveTo = -1, moveToPage = -1;
        if (!typing && m_FavHighlight >= 0 && m_FavHighlight < (int)items.size() &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
            activate = m_FavHighlight;

        // Tiles flow left to right.
        const float tileW = EditorTheme::Px(170.0f), tileH = ImGui::GetFrameHeight() + EditorTheme::Px(8.0f);
        const float gap = EditorTheme::Px(6.0f);
        const float availW = ImGui::GetContentRegionAvail().x;
        const int cols = std::max(1, (int)((availW + gap) / (tileW + gap)));
        const float realW = (availW - gap * (cols - 1)) / cols;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < (int)items.size(); ++i) {
            const FavoriteDisplay& d = shown[(size_t)i];
            const ImVec2 mn(origin.x + (i % cols) * (realW + gap) + slide, origin.y + (i / cols) * (tileH + gap));
            const ImVec2 mx(mn.x + realW, mn.y + tileH);
            ImGui::PushID(i);
            ImGui::SetCursorScreenPos(mn);
            if (ImGui::InvisibleButton("##fav", ImVec2(realW, tileH))) activate = i;
            const bool hov = ImGui::IsItemHovered();
            if (hov) m_FavHighlight = i;
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("FAVORITE_ITEM", &i, sizeof(int));
                ImGui::Text("%s  %s", d.Icon.c_str(), d.Label.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("FAVORITE_ITEM")) { moveFrom = *(const int*)pl->Data; moveTo = i; }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("##favCtx")) {
                if (ImGui::MenuItem(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE "  Open")) activate = i;
                if (pageCount > 1 && ImGui::BeginMenu(ICON_FA_ARROW_RIGHT "  Move to Page")) {
                    for (int p = 0; p < pageCount; ++p)
                        if (p != m_FavPage && ImGui::MenuItem(pages[(size_t)p].Name.c_str())) { moveFrom = i; moveToPage = p; }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_TRASH "  Remove from Favorites")) removeAt = i;
                ImGui::EndPopup();
            }
            const bool lit = hov || m_FavHighlight == i;
            dl->AddRectFilled(mn, mx, EditorTheme::U32(lit ? EditorTheme::Hover : EditorTheme::Card), EditorTheme::Px(4.0f));
            if (d.Color) dl->AddRectFilled(mn, ImVec2(mn.x + EditorTheme::Px(3.0f), mx.y), d.Color | 0xFF000000u, EditorTheme::Px(4.0f), ImDrawFlags_RoundCornersLeft);
            const float cy = (mn.y + mx.y) * 0.5f;
            const ImU32 tc = EditorTheme::U32(d.Missing ? EditorTheme::Dim : EditorTheme::Text);
            const ImVec2 is = ImGui::CalcTextSize(d.Icon.c_str());
            float tx = mn.x + EditorTheme::Px(10.0f);
            dl->AddText(ImVec2(tx, cy - is.y * 0.5f), EditorTheme::U32(d.Missing ? EditorTheme::Dim : EditorTheme::Secondary), d.Icon.c_str());
            tx += std::max(is.x, ImGui::GetFontSize()) + EditorTheme::Px(8.0f);
            // The number key hint for the first nine items of a page isn't used (digits pick pages).
            const ImVec2 ls = ImGui::CalcTextSize(d.Label.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tc));
            ImGui::RenderTextEllipsis(dl, ImVec2(tx, cy - ls.y * 0.5f), ImVec2(mx.x - EditorTheme::Px(6.0f), cy + ls.y * 0.5f),
                                      mx.x - EditorTheme::Px(6.0f), d.Label.c_str(), nullptr, &ls);
            ImGui::PopStyleColor();
            if (hov && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) EditorUI::SetTooltip("%s", d.Tooltip.c_str());
            ImGui::PopID();
        }
        const int rows = ((int)items.size() + cols - 1) / cols;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (tileH + gap)));
        if (items.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Drop folders, assets or Hierarchy objects here, star assets in the Asset Browser,");
            ImGui::TextDisabled("or press Ctrl+Alt+B to add the selection.");
        }
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // Drop anything favoritable anywhere on the overlay: the shown page gets it.
        if (ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->InnerRect, ImGui::GetID("##favDrop"))) {
            bool added = false;
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
                const entt::entity dragged = *(const entt::entity*)p->Data;
                const std::vector<entt::entity> dropped = IsSelected(dragged) ? GetSelectedItems() : std::vector<entt::entity>{dragged};
                for (entt::entity e : dropped) {
                    const auto* o = world.Registry.valid(e) ? world.Registry.try_get<OrderComponent>(e) : nullptr;
                    const auto* nm = o ? world.Registry.try_get<NameComponent>(e) : nullptr;
                    if (o) added |= Enhancers::AddFavorite(pages, m_FavPage, Enhancers::EditorRef::MakeEntity(sceneKey, o->Value, nm ? nm->Name : std::string()));
                }
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_FOLDER_PATH")) {
                const std::string path((const char*)p->Data);
                added |= Enhancers::AddFavorite(pages, m_FavPage, Enhancers::EditorRef::MakeFolder(path, FolderLeaf(path)));
            }
            static const char* const kAssetPayloads[] = {"ASSET_MODEL_PATH", "ASSET_TEXTURE_PATH", "ASSET_MATERIAL_PATH",
                                                         "ASSET_PREFAB_PATH", "ASSET_SOUND_PATH", "ASSET_FILE_PATH"};
            for (const char* type : kAssetPayloads)
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type)) {
                    const std::string key((const char*)p->Data);
                    added |= Enhancers::AddFavorite(pages, m_FavPage, Enhancers::EditorRef::MakeAsset(key, AssetChipLabel(key)));
                }
            if (added) us.MarkDirty();
            ImGui::EndDragDropTarget();
        }

        EditorTheme::PushSmall();
        ImGui::TextDisabled("1-9 / arrows / wheel: pages    Up/Down + Enter: open    double-click a page to rename    Esc: close");
        EditorTheme::PopFont();

        // Apply list edits after drawing.
        if (moveFrom >= 0 && moveToPage >= 0 && moveFrom < (int)items.size()) {
            const Enhancers::EditorRef moved = items[(size_t)moveFrom];
            items.erase(items.begin() + moveFrom);
            Enhancers::AddFavorite(pages, moveToPage, moved);
            us.MarkDirty();
        } else if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo) {
            Enhancers::MoveRef(items, moveFrom, moveTo);
            us.MarkDirty();
        }
        if (removeAt >= 0 && removeAt < (int)items.size()) {
            items.erase(items.begin() + removeAt);
            us.MarkDirty();
        }
        if (activate >= 0 && activate < (int)items.size()) {
            const Enhancers::EditorRef r = items[(size_t)activate];
            ActivateFavorite(world, assets, r);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

// ============================================================================================
// Pinned component windows (vHierarchy minimap Alt+click; vInspector "Open in Window")
// ============================================================================================

void EditorLayer::OpenPinnedComponent(World& world, entt::entity entity, const char* componentName) {
    if (!componentName || entity == entt::null || !world.Registry.valid(entity)) return;
    const auto* o = world.Registry.try_get<OrderComponent>(entity);
    if (!o) return;
    const std::string key = CurrentSceneKey();
    for (auto& p : m_Pins)
        if (p.Order == o->Value && p.Component == componentName && p.SceneKey == key) {
            p.Placed = false; // already open: bring it back under the cursor
            p.SpawnPos = ImGui::GetIO().MousePos;
            return;
        }
    PinnedComponent p;
    p.Order = o->Value;
    p.Component = componentName;
    p.SceneKey = key;
    const ImVec2 m = ImGui::GetIO().MousePos;
    p.SpawnPos = ImVec2(m.x + EditorTheme::Px(16.0f), m.y - EditorTheme::Px(12.0f));
    m_Pins.push_back(std::move(p));
}

// One small floating editor per pin, showing the same field body the Inspector draws (so undo,
// prefab override tints and the C# managed inspector all behave identically). A pin whose scene
// isn't open is kept but not drawn; one whose object or component is gone says so until closed.
void EditorLayer::DrawPinnedComponentWindows(World& world, AssetLibrary& assets) {
    if (m_Pins.empty()) return;
    const std::string key = CurrentSceneKey();
    for (size_t i = 0; i < m_Pins.size();) {
        PinnedComponent& p = m_Pins[i];
        if (p.SceneKey != key) { ++i; continue; }
        const entt::entity e = FindEntityByOrder(world, p.Order);
        const RegisteredComponent* rc = nullptr;
        for (const auto& c : ComponentRegistry::All())
            if (p.Component == c.Meta.Name) { rc = &c; break; }
        std::string objName = "(missing)";
        if (e != entt::null)
            if (const auto* n = world.Registry.try_get<NameComponent>(e)) objName = n->Name.empty() ? std::string("(unnamed)") : n->Name;
        const std::string title = std::string(rc && rc->Meta.Icon ? rc->Meta.Icon : ICON_FA_PUZZLE_PIECE) + "  " + p.Component +
                                  "  -  " + objName + "###pin" + std::to_string(p.Order) + p.Component;
        if (!p.Placed) {
            ImGui::SetNextWindowPos(p.SpawnPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(EditorTheme::Px(340.0f), EditorTheme::Px(280.0f)), ImGuiCond_Appearing);
            ImGui::SetNextWindowFocus();
            p.Placed = true;
        }
        bool open = true;
        if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
            if (e == entt::null) {
                ImGui::TextDisabled("The object is no longer in this scene.");
            } else if (!rc || !rc->Has(world.Registry, e)) {
                ImGui::TextDisabled("This object no longer has a %s.", p.Component.c_str());
            } else {
                if (EditorUIPrimitives::ActionButton(ICON_FA_ARROW_POINTER, "Select this object", &Tip))
                    SelectItem(e, false);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", objName.c_str());
                ImGui::Separator();
                ImGui::PushID((int)entt::to_integral(e));
                DrawReflectedComponentFields(world, assets, *rc, e);
                ImGui::PopID();
            }
        }
        ImGui::End();
        if (!open) m_Pins.erase(m_Pins.begin() + (std::ptrdiff_t)i);
        else ++i;
    }
}

// ============================================================================================
// vFolders
// ============================================================================================

namespace {
Enhancers::FolderKind FolderKindOf(AssetGridCell::Kind k) {
    using K = AssetGridCell::Kind;
    using F = Enhancers::FolderKind;
    switch (k) {
        case K::Model:      return F::Model;
        case K::Texture:
        case K::Hdri:
        case K::Screenshot: return F::Texture;
        case K::Material:   return F::Material;
        case K::Sound:      return F::Sound;
        case K::Prefab:     return F::Prefab;
        case K::Scene:      return F::Scene;
        case K::Script:     return F::Script;
        case K::Animator:   return F::Animation;
        case K::Shader:     return F::Shader;
        default:            return F::Other;
    }
}
void CopyGlyph(char (&dst)[8], const char* glyph) {
    std::snprintf(dst, sizeof(dst), "%s", glyph ? glyph : "");
}
} // namespace

// Content counts per folder, from what the Asset Browser grid would list there: library assets
// by their AssetFolder, plus the project index's not-yet-loaded files by theirs. One pass over
// every asset - only when something changed, or once a second while the browser is open.
void EditorLayer::RebuildFolderSummariesIfNeeded(AssetLibrary& assets) {
    const size_t sig = m_ListedAssetSignature ^ (m_ProjectAssetIndex.files.size() * 0x9E3779B97F4A7C15ull) ^
                       (assets.AssetFolders().size() << 20) ^ (assets.Folders().size() << 40);
    m_FolderSummaryAge += ImGui::GetIO().DeltaTime;
    if (sig == m_FolderSummarySig && m_FolderSummaryAge < 1.0f) return;
    m_FolderSummarySig = sig;
    m_FolderSummaryAge = 0.0f;
    m_FolderSummaries.clear();
    using F = Enhancers::FolderKind;
    for (const auto& m : assets.Models())    m_FolderSummaries[assets.AssetFolder(m->Path())].Add(F::Model);
    for (const auto& t : assets.Textures())  m_FolderSummaries[assets.AssetFolder(t->Path())].Add(F::Texture);
    for (const auto& m : assets.Materials()) m_FolderSummaries[assets.AssetFolder(m->Path)].Add(F::Material);
    for (const auto& s : assets.Sounds())    m_FolderSummaries[assets.AssetFolder(s)].Add(F::Sound);
    for (const auto& p : assets.Prefabs())   m_FolderSummaries[assets.AssetFolder(p)].Add(F::Prefab);
    for (const auto& f : m_ProjectAssetIndex.files) {
        if (m_ListedAssetKeys.count(f.Key)) continue;
        const std::string moved = assets.AssetFolder(f.Path);
        m_FolderSummaries[moved.empty() ? f.Folder : moved].Add(FolderKindOf(f.Kind));
    }
}

bool EditorLayer::GetFolderVisual(AssetLibrary& assets, const std::string& path, EditorFolderVisual& out) {
    out = EditorFolderVisual{};
    RebuildFolderSummariesIfNeeded(assets);
    const auto it = m_FolderSummaries.find(path);
    const Enhancers::FolderSummary* summary = it != m_FolderSummaries.end() ? &it->second : nullptr;
    if (EditorSettings::Get().FolderStyles) {
        const Enhancers::ResolvedFolderStyle r = Enhancers::FolderStyles::Get().Resolve(path, summary);
        CopyGlyph(out.Icon, r.Icon.empty() ? nullptr : Enhancers::FAIconGlyph(r.Icon.c_str()));
        out.Color = r.Color;
    }
    if (summary) {
        out.Total = summary->Total;
        Enhancers::FolderKind kinds[4];
        const int n = Enhancers::TopKinds(*summary, kinds, 4);
        for (int i = 0; i < n; ++i) {
            const char* name = Enhancers::FolderKindIconName(kinds[i]);
            if (!name) continue;
            CopyGlyph(out.Mini[out.MiniCount++], Enhancers::FAIconGlyph(name));
        }
    }
    return true;
}

unsigned EditorLayer::FolderTreeFlags() const {
    const EditorSettings& s = EditorSettings::Get();
    unsigned f = 0;
    if (s.FolderTreeLines) f |= kFolderTreeLines;
    if (s.FolderMinimap)   f |= kFolderTreeMinimap;
    if (s.FolderStyles)    f |= kFolderTreeStyles;
    if (s.FolderRowWash)   f |= kFolderTreeWash;
    if (s.FolderZebra)     f |= kFolderTreeZebra;
    if (s.FolderMinimal)   f |= kFolderTreeMinimal;
    return f;
}

// AssetLibrary's rename / delete hook: folder styles (project file) and folder bookmarks
// (per-user) follow the folder. Installed every frame the browser draws - cheap, and it keeps
// the capture pointing at this EditorLayer whatever order things were constructed in.
void EditorLayer::InstallFolderPathHook(AssetLibrary& assets) {
    if (assets.OnFolderPathChanged) return;
    assets.OnFolderPathChanged = [this](const std::string& oldPath, const std::string& newPath) {
        Enhancers::FolderStyles::Get().OnFolderPathChanged(oldPath, newPath);
        auto& us = Enhancers::EnhancerUserState::Get();
        bool changed = false;
        for (auto it = us.FolderBookmarks.begin(); it != us.FolderBookmarks.end();) {
            if (it->Kind == Enhancers::RefKind::Folder && Enhancers::RemapFolderPath(it->Path, oldPath, newPath)) {
                changed = true;
                if (newPath.empty()) { it = us.FolderBookmarks.erase(it); continue; }
                it->Label = it->Path.substr(it->Path.find_last_of('/') == std::string::npos ? 0 : it->Path.find_last_of('/') + 1);
            }
            ++it;
        }
        if (changed) us.MarkDirty();
        if (Enhancers::RemapFolderPath(m_HoverAssetFolder, oldPath, newPath) && newPath.empty()) m_HoverAssetFolderSet = false;
    };
}

// The folder tree's right-click menu (and the grid's folder-tile menu): style, bookmark, rule.
// Edits go to project/editor_folders.json, which the global file undo journals like any project file.
void EditorLayer::DrawFolderContextMenuBody(World& world, AssetLibrary& assets, const std::string& path) {
    (void)world;
    auto& fs = Enhancers::FolderStyles::Get();
    const bool isRoot = path.empty();
    const std::string leaf = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
    if (!isRoot) {
        ImGui::TextDisabled("%s", leaf.c_str());
        EditorUIPrimitives::SectionHeader("Folder colour");
        const auto it = fs.Folders.find(path);
        const Enhancers::FolderStyle own = it != fs.Folders.end() ? it->second : Enhancers::FolderStyle{};
        auto setStyle = [&](const Enhancers::FolderStyle& s) {
            if (s.IsEmpty()) fs.Folders.erase(path);
            else fs.Folders[path] = s;
            fs.MarkDirty();
        };
        std::uint32_t color = own.Color;
        if (Enhancers::PaletteColorRow("##folderColor", color)) {
            Enhancers::FolderStyle s = own;
            s.Color = color;
            setStyle(s);
        }
        EditorUIPrimitives::SectionHeader("Folder icon");
        if (ImGui::BeginMenu(own.Icon.empty() ? ICON_FA_ICONS "  Choose icon..." : ICON_FA_ICONS "  Change icon...")) {
            std::string icon = own.Icon;
            if (Enhancers::IconPickerGrid("##folderIcon", icon, m_FolderStyleIconSearch, sizeof(m_FolderStyleIconSearch), EditorTheme::Px(280.0f))) {
                Enhancers::FolderStyle s = own;
                s.Icon = icon;
                setStyle(s);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_ERASER "  Clear Folder Style", nullptr, false, !own.IsEmpty())) setStyle({});
        // vFolders' rules: one click to give every folder with this name the same look.
        const bool canRule = !own.IsEmpty() && !leaf.empty();
        const std::string ruleLabel = std::string(ICON_FA_WAND_MAGIC_SPARKLES "  Style every folder named \"") + leaf + "\"";
        if (ImGui::MenuItem(ruleLabel.c_str(), nullptr, false, canRule)) {
            fs.Rules.push_back({leaf, own});
            fs.MarkDirty();
            Log::Info("Folder rule added: every folder named '" + leaf + "' gets this style (Settings > Editor Enhancers > Folder rules).");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            EditorUI::SetTooltip(canRule ? "Adds a folder rule for this name - edit or remove it in Settings > Editor Enhancers."
                                         : "Give this folder a colour or icon first.");
        ImGui::Separator();
    }

    auto& us = Enhancers::EnhancerUserState::Get();
    const Enhancers::EditorRef ref = Enhancers::EditorRef::MakeFolder(path, isRoot ? std::string("Assets") : leaf);
    const bool marked = Enhancers::FindRef(us.FolderBookmarks, ref) >= 0;
    if (ImGui::MenuItem(ICON_FA_BOOKMARK "  Bookmark Folder", nullptr, marked)) {
        if (marked) Enhancers::RemoveRef(us.FolderBookmarks, ref);
        else Enhancers::AddUnique(us.FolderBookmarks, ref, Enhancers::EnhancerUserState::kMaxBookmarks);
        us.MarkDirty();
    }
    {
        const int favPage = Enhancers::FindFavorite(us.FavoritePages, ref);
        if (ImGui::MenuItem(ICON_FA_STAR "  Favorite", nullptr, favPage >= 0)) {
            if (favPage >= 0) Enhancers::RemoveFavorite(us.FavoritePages, ref);
            else Enhancers::AddFavorite(us.FavoritePages, m_FavPage, ref);
            us.MarkDirty();
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Hold Alt over the Asset Browser to see your favorites.");
    }
    if (!isRoot) {
        if (ImGui::MenuItem(ICON_FA_ANGLES_DOWN "  Expand All Inside"))  SetFolderExpandedRecursive(assets, path, true, true);
        if (ImGui::MenuItem(ICON_FA_ANGLES_UP "  Collapse All Inside")) SetFolderExpandedRecursive(assets, path, false, true);
    }
}

// vFolders hover keys, applied after the module drew the tree (it reported the hovered folder
// through SetHoveredAssetFolder; grid folder tiles report theirs from DrawAssetCell).
void EditorLayer::HandleFolderHoverKeys(AssetLibrary& assets) {
    const bool hovered = m_HoverAssetFolderSet;
    const std::string folder = m_HoverAssetFolder;
    m_HoverAssetFolderSet = false;
    m_HoverAssetFolder.clear();
    if (!EditorSettings::Get().EnhancerHoverKeys) return;
    if (Shortcuts::Triggered("project.hover.collapseAll")) {
        m_ExpandedAssetFolders.clear();
        return;
    }
    if (!hovered || folder.empty()) return;
    if (Shortcuts::Triggered("project.hover.expand")) {
        if (m_ExpandedAssetFolders.count(folder)) m_ExpandedAssetFolders.erase(folder);
        else m_ExpandedAssetFolders.insert(folder);
    } else if (Shortcuts::Triggered("project.hover.isolate")) {
        m_ExpandedAssetFolders.clear();
        for (size_t s = 0;;) { // every ancestor, then the folder itself
            const size_t slash = folder.find('/', s);
            m_ExpandedAssetFolders.insert(folder.substr(0, slash));
            if (slash == std::string::npos) break;
            s = slash + 1;
        }
        (void)assets;
    }
}

// Folder bookmark chips under the Asset Browser toolbar: click to go there, right-click to
// remove, drop a folder (tree row or grid tile) on the row to add one. Nothing is drawn when
// there are no bookmarks, except while a folder is being dragged, so the drop target appears
// exactly when it can be used.
// Everything the Asset Browser module draws between its toolbar and the content (API v40): one
// row - vTabs' strip, with the folder bookmarks behind a button at its right end. With Bookmark
// chips on (or the tab strip off), the strip and the chip bar are separate rows as before.
void EditorLayer::DrawFolderNavBar(World& world, AssetLibrary& assets) {
    (void)world;
    const EditorSettings& es = EditorSettings::Get();
    if (es.BookmarkChips || !es.AssetTabs) {
        DrawAssetTabStrip(assets);
        DrawFolderBookmarkBar(assets);
        return;
    }
    const bool bmOn = es.FolderNavBar;
    bool anyBm = false;
    for (const auto& r : Enhancers::EnhancerUserState::Get().FolderBookmarks)
        if (r.Kind == Enhancers::RefKind::Folder) { anyBm = true; break; }
    const float h = ImGui::GetFrameHeight();
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float rowW = ImGui::GetContentRegionAvail().x;
    const float inset = bmOn ? h + EditorTheme::Px(6.0f) : 0.0f;
    if (!DrawAssetTabStrip(assets, inset, /*alwaysShow=*/bmOn && anyBm) || !bmOn) return;
    const ImVec2 resume = ImGui::GetCursorScreenPos();
    const ImVec2 btnMin(rowMin.x + rowW - h, rowMin.y);
    DrawFolderBookmarkButton(assets, btnMin, btnMin, ImVec2(btnMin.x + h, btnMin.y + h));
    ImGui::SetCursorScreenPos(resume);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void EditorLayer::DrawFolderBookmarkButton(AssetLibrary& assets, ImVec2 at, ImVec2 dropMin, ImVec2 dropMax) {
    const ImRect dropRect(dropMin, dropMax);
    auto& us = Enhancers::EnhancerUserState::Get();
    const auto& folders = assets.Folders();
    const float h = ImGui::GetFrameHeight();
    std::vector<BookmarkRow> rows;
    for (int i = 0; i < (int)us.FolderBookmarks.size(); ++i) {
        const Enhancers::EditorRef& r = us.FolderBookmarks[(size_t)i];
        if (r.Kind != Enhancers::RefKind::Folder) continue;
        BookmarkRow row;
        row.Index = i;
        EditorFolderVisual vis;
        GetFolderVisual(assets, r.Path, vis);
        row.Icon = vis.Icon[0] ? std::string(vis.Icon) : std::string(r.Path.empty() ? ICON_FA_HOUSE : ICON_FA_FOLDER);
        row.Color = vis.Color;
        row.Label = r.Path.empty() ? std::string("Assets") : (r.Label.empty() ? r.Path : r.Label);
        row.Missing = !(r.Path.empty() || std::find(folders.begin(), folders.end(), r.Path) != folders.end());
        row.Tooltip = (r.Path.empty() ? std::string("Assets") : r.Path) +
                      (row.Missing ? "\nThis folder no longer exists." : "\nClick to open. Drag to reorder.");
        rows.push_back(std::move(row));
    }
    const Enhancers::EditorRef current = Enhancers::EditorRef::MakeFolder(m_CurrentAssetFolder, FolderLeaf(m_CurrentAssetFolder));
    const bool starred = Enhancers::FindRef(us.FolderBookmarks, current) >= 0;
    const std::string currentName = FolderLeaf(m_CurrentAssetFolder);
    ImGui::SetCursorScreenPos(at);
    BookmarkAction a;
    BookmarkButton("##folderBm", h, currentName.c_str(), starred, rows, DragOverRect(dropRect, {"ASSET_FOLDER_PATH"}), a);
    if (a.Toggle) {
        if (starred) Enhancers::RemoveRef(us.FolderBookmarks, current);
        else Enhancers::AddUnique(us.FolderBookmarks, current, Enhancers::EnhancerUserState::kMaxBookmarks);
        us.MarkDirty();
    }
    if (a.Activate >= 0) NavigateAssetFolder(us.FolderBookmarks[(size_t)a.Activate].Path);
    if (ApplyBookmarkEdits(us.FolderBookmarks, a)) us.MarkDirty();
    if (ImGui::BeginDragDropTargetCustom(dropRect, ImGui::GetID("##folderBmDrop"))) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_FOLDER_PATH")) {
            const std::string path((const char*)p->Data);
            Enhancers::AddUnique(us.FolderBookmarks, Enhancers::EditorRef::MakeFolder(path, FolderLeaf(path)),
                                 Enhancers::EnhancerUserState::kMaxBookmarks);
            us.MarkDirty();
        }
        ImGui::EndDragDropTarget();
    }
}

void EditorLayer::DrawFolderBookmarkBar(AssetLibrary& assets) {
    if (!EditorSettings::Get().FolderNavBar) return;
    auto& us = Enhancers::EnhancerUserState::Get();
    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    const bool draggingFolder = drag && drag->IsDataType("ASSET_FOLDER_PATH");
    bool any = false;
    for (const auto& r : us.FolderBookmarks) if (r.Kind == Enhancers::RefKind::Folder) { any = true; break; }
    if (!any && !draggingFolder) return;

    const float h = ImGui::GetFrameHeight();
    const ImVec2 barMin = ImGui::GetCursorScreenPos();
    const float barW = ImGui::GetContentRegionAvail().x;
    ImGui::PushID("##folderNav");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(EditorTheme::Px(4.0f), ImGui::GetStyle().ItemSpacing.y));
    EditorTheme::PushSmall();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(EditorTheme::Dim, ICON_FA_BOOKMARK);
    EditorTheme::PopFont();
    if (!any) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Drop a folder here to bookmark it");
    }
    const auto& folders = assets.Folders();
    int removeAt = -1;
    for (int i = 0; i < (int)us.FolderBookmarks.size(); ++i) {
        const Enhancers::EditorRef& r = us.FolderBookmarks[(size_t)i];
        if (r.Kind != Enhancers::RefKind::Folder) continue;
        const bool exists = r.Path.empty() || std::find(folders.begin(), folders.end(), r.Path) != folders.end();
        EditorFolderVisual vis;
        GetFolderVisual(assets, r.Path, vis);
        const std::string label = r.Path.empty() ? std::string("Assets") : (r.Label.empty() ? r.Path : r.Label);
        ImGui::SameLine();
        const float avail = barMin.x + barW - ImGui::GetCursorScreenPos().x;
        if (avail < EditorTheme::Px(48.0f)) break;
        ImGui::PushID(i);
        bool clicked = false, hov = false;
        NavChip("##chip", vis.Icon[0] ? vis.Icon : ICON_FA_FOLDER, label.c_str(), !exists, std::min(avail, EditorTheme::Px(150.0f)), clicked, hov);
        if (vis.Color) { // the folder's colour as a thin underline on its chip
            const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x + EditorTheme::Px(6.0f), mx.y - 1.0f), ImVec2(mx.x - EditorTheme::Px(6.0f), mx.y - 1.0f),
                                                vis.Color | 0xFF000000u, EditorTheme::Px(2.0f));
        }
        if (clicked && exists) NavigateAssetFolder(r.Path);
        if (hov) EditorUI::SetTooltip(exists ? "%s\nClick to open, right-click to remove." : "%s\nThis folder no longer exists - right-click to remove.",
                                      r.Path.empty() ? "Assets" : r.Path.c_str());
        if (ImGui::BeginPopupContextItem("##chipCtx")) {
            if (ImGui::MenuItem(ICON_FA_TRASH "  Remove bookmark")) removeAt = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        us.FolderBookmarks.erase(us.FolderBookmarks.begin() + removeAt);
        us.MarkDirty();
    }
    const ImRect barRect(barMin, ImVec2(barMin.x + barW, barMin.y + h));
    if (ImGui::BeginDragDropTargetCustom(barRect, ImGui::GetID("##folderNavDrop"))) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_FOLDER_PATH")) {
            const std::string path((const char*)p->Data);
            const std::string leaf = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
            Enhancers::AddUnique(us.FolderBookmarks, Enhancers::EditorRef::MakeFolder(path, leaf), Enhancers::EnhancerUserState::kMaxBookmarks);
            us.MarkDirty();
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
}
