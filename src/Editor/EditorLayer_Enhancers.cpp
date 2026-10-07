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
#include "World.h"
#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "ComponentRegistry.h"
#include "ProjectPaths.h"

#include "Enhancers/EnhancerCore.h"
#include "Enhancers/EnhancerUserState.h"
#include "Enhancers/Palette.h"
#include "Enhancers/StyleWidgets.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
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
                         "D default parent. Rebind them under Shortcuts (the \"(hover)\" groups).");

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
