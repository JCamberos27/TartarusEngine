// #174 - Project Settings > Build: Unity's Build Settings + Player settings in one page, and the
// Build / Build and Run actions (also Ctrl+Shift+B to open this page, Ctrl+B to build and run).
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "BuildPipeline.h"
#include "ProjectSettings.h"
#include "ProjectPaths.h"
#include "Log.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using namespace EditorInternal;

namespace {

std::string FormatBytes(std::uint64_t b) {
    char buf[32];
    if (b >= 1024ull * 1024ull) std::snprintf(buf, sizeof(buf), "%.1f MB", (double)b / (1024.0 * 1024.0));
    else                        std::snprintf(buf, sizeof(buf), "%.1f KB", (double)b / 1024.0);
    return buf;
}

// std::string-backed InputText without imgui_stdlib.
bool InputString(const char* label, std::string& s, float width) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
    ImGui::SetNextItemWidth(width);
    if (!ImGui::InputText(label, buf, sizeof(buf))) return false;
    s = buf;
    return true;
}

void OpenFolder(const std::string& dir) {
#ifdef _WIN32
    ::ShellExecuteW(nullptr, L"open", std::filesystem::path(dir).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)dir;
#endif
}

} // namespace

void EditorLayer::RunBuild(bool runAfter) {
    // Build what's on disk: an unsaved scene would otherwise ship without its latest edits.
    if (IsDirty()) Log::Warn("Build: the open scene has unsaved changes; the build uses the saved file.");
    m_LastBuildReport = std::make_shared<BuildPipeline::Report>(BuildPipeline::Build(ProjectSettings::Build()));
    if (m_LastBuildReport->Ok && runAfter) BuildPipeline::Run(m_LastBuildReport->ExePath);
}

void EditorLayer::DrawBuildSettingsBody() {
    ProjectSettings::BuildSettings& b = ProjectSettings::MutableBuild();
    const float kw = 260.0f * m_UIScale;
    bool changed = false;

    ImGui::SeparatorText("Scenes In Build");
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("The first scene is the one the game starts in.");
        ImGui::PopStyleColor();

        int removeAt = -1, moveUp = -1;
        for (int i = 0; i < (int)b.Scenes.size(); ++i) {
            ImGui::PushID(i);
            if (ActionButton(ICON_FA_XMARK, "Remove from the build", false, ImVec2(ImGui::GetFrameHeight(), 0.0f)))
                removeAt = i;
            ImGui::SameLine();
            ImGui::BeginDisabled(i == 0);
            if (ActionButton(ICON_FA_ARROW_UP, "Move up", false, ImVec2(ImGui::GetFrameHeight(), 0.0f)))
                moveUp = i;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d  %s", i, b.Scenes[i].c_str());
            ImGui::PopID();
        }
        if (removeAt >= 0) { b.Scenes.erase(b.Scenes.begin() + removeAt); changed = true; }
        if (moveUp > 0) { std::swap(b.Scenes[moveUp], b.Scenes[moveUp - 1]); changed = true; }
        if (b.Scenes.empty()) ImGui::TextDisabled("(none - add a scene below)");

        // Everything under project/scenes that isn't in the list yet.
        std::vector<std::string> candidates;
        for (const std::string& s : BuildPipeline::FindProjectScenes())
            if (std::find(b.Scenes.begin(), b.Scenes.end(), s) == b.Scenes.end()) candidates.push_back(s);
        ImGui::BeginDisabled(candidates.empty());
        ImGui::SetNextItemWidth(kw);
        if (ImGui::BeginCombo("##addscene", candidates.empty() ? "All scenes added" : "Add scene...")) {
            for (const std::string& s : candidates)
                if (ImGui::Selectable(s.c_str())) { b.Scenes.push_back(s); changed = true; }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const std::string open = CurrentScenePath();
        std::string openRel;
        if (!open.empty()) {
            openRel = ProjectPaths::Relativize(open); // unchanged (absolute) when outside the project
            if (std::filesystem::path(openRel).is_absolute()) openRel.clear();
        }
        const bool openListed = openRel.empty() || std::find(b.Scenes.begin(), b.Scenes.end(), openRel) != b.Scenes.end();
        ImGui::BeginDisabled(openListed);
        if (ActionButton(ICON_FA_PLUS " Add Open Scene", "Add the scene you're editing to the build")) {
            b.Scenes.push_back(openRel);
            changed = true;
        }
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Player");
    changed |= InputString("Product Name", b.ProductName, kw);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("The game's window title, exe name and default output folder name.");
    changed |= InputString("Company Name", b.CompanyName, kw);
    changed |= InputString("Version", b.Version, kw);
    changed |= EditorUIPrimitives::Checkbox("Fullscreen", &b.Fullscreen);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Start borderless fullscreen. F11 toggles while playing.");
    ImGui::BeginDisabled(b.Fullscreen);
    ImGui::SetNextItemWidth(kw * 0.5f);
    int wh[2] = {b.Width, b.Height};
    if (ImGui::InputInt2("Window Size", wh)) {
        b.Width = std::clamp(wh[0], 320, 16384);
        b.Height = std::clamp(wh[1], 200, 16384);
        changed = true;
    }
    ImGui::EndDisabled();
    changed |= EditorUIPrimitives::Checkbox("V-Sync", &b.VSync);
    changed |= EditorUIPrimitives::Checkbox("Development Build", &b.DevelopmentBuild);
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Keeps the statistics overlay and the physics debug keys (F5 / F6) in the game.");

    // #174 - product branding. Project-relative paths, copied into the build by BuildPipeline;
    // empty leaves the game with the engine's own icon and no splash.
    ImGui::SeparatorText("Branding");
    changed |= InputString("Icon", b.IconPath, kw * 1.5f);
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("PNG used for the game's window and taskbar icon.\n"
                             "Project-relative, e.g. textures/game_icon.png. Empty keeps the engine's icon.\n"
                             "Does not change the icon Explorer shows on the .exe itself.");
    changed |= InputString("Splash Image", b.SplashPath, kw * 1.5f);
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("PNG shown while the game loads, at its own pixel size.\n"
                             "Project-relative. Empty means no splash, as before.");

    ImGui::SeparatorText("Output");
    const std::string resolved = BuildPipeline::ResolveOutputDir(b);
    changed |= InputString("Output Folder", b.OutputDir, kw * 1.5f);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Leave empty for Builds/<Product Name> next to the project folder.");
    ImGui::TextDisabled("Builds to: %s", resolved.c_str());

    if (changed) ProjectSettings::Save();

    ImGui::Spacing();
    if (PrimaryButton(ICON_FA_HAMMER "  Build")) RunBuild(false);
    ImGui::SameLine();
    if (PrimaryButton(ICON_FA_PLAY "  Build and Run")) RunBuild(true);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Ctrl+B");
    ImGui::SameLine();
    {
        std::error_code ec;
        ImGui::BeginDisabled(!std::filesystem::is_directory(resolved, ec));
        if (ActionButton(ICON_FA_FOLDER_OPEN " Open Folder", "Show the build in Explorer")) OpenFolder(resolved);
        ImGui::EndDisabled();
    }

    if (m_LastBuildReport) {
        const BuildPipeline::Report& r = *m_LastBuildReport;
        ImGui::SeparatorText("Last Build");
        if (r.Ok) ImGui::TextColored(EditorUIPrimitives::SuccessColor(), "%s", r.Message.c_str());
        else      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", r.Message.c_str());
        if (r.Ok && ImGui::BeginTable("##buildreport", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            for (const auto& [group, bytes] : r.BytesByGroup) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(group.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatBytes(bytes).c_str());
            }
            ImGui::EndTable();
        }
        if (r.Ok && ImGui::TreeNode("Largest files")) {
            for (const auto& [path, bytes] : r.LargestFiles)
                ImGui::Text("%-10s  %s", FormatBytes(bytes).c_str(), path.c_str());
            ImGui::TreePop();
        }
    }
}
