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

#include <imgui.h>
#include <imgui_internal.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
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
    EditorUI::HelpMarker("Scene selector, selection Back/Forward and bookmarked objects above the Hierarchy tree. "
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
    if (SettingsCheckbox("Bookmark bar", &prefs.FolderNavBar)) EditorSettings::Save();
    ImGui::SameLine();
    EditorUI::HelpMarker("Bookmarked folders as chips under the toolbar. Bookmark from a folder's right-click menu, "
                         "or drop a folder on the bar.");

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
    ImGui::SetNextItemWidth(std::min(barW * 0.42f, EditorTheme::Px(180.0f)));
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
    ImGui::SameLine();
    ImGui::BeginDisabled(!primaryValid);
    if (EditorUIPrimitives::ActionButton(ICON_FA_BOOKMARK, primaryStarred ? "Remove the selection's bookmark"
                                                                         : "Bookmark the selection (or drop rows on this bar)",
                                         &Tip, primaryStarred, ImVec2(h, h))) {
        if (primaryStarred) {
            for (entt::entity e : GetSelectedItems()) Enhancers::RemoveRef(us.EntityBookmarks, refFor(e));
        } else {
            for (entt::entity e : GetSelectedItems())
                if (world.Registry.valid(e) && world.Registry.all_of<OrderComponent>(e))
                    Enhancers::AddUnique(us.EntityBookmarks, refFor(e), Enhancers::EnhancerUserState::kMaxBookmarks);
        }
        us.MarkDirty();
    }
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
    const ImRect barRect(barMin, ImVec2(barMin.x + barW, barMin.y + h));
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

void EditorLayer::ApplyInspectorNavAction(World& world, const InspectorNavAction& act) {
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
void EditorLayer::DrawFolderNavBar(World& world, AssetLibrary& assets) {
    (void)world;
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
