// The editor's top toolbar strip — now just the dropdown menu bar (File / Create / View / Window /
// Settings / Help) and the custom window min/max/close controls, living inside TartarusEditor.dll
// so its layout and chrome hot-reload while the editor stays open with its scene loaded. Ported in
// behaviour from EditorLayer::DrawTopToolbar / DrawWindowControls (EditorLayer_Toolbar.cpp).
//
// Phase 3 item 3 moved the tool-selection cluster (Hand/Translate/Rotate/.../Universal, Measure,
// Duplicate Array, Local/World, Pivot/Center) and the view-state cluster (draw mode, ortho/persp)
// out of this strip entirely, into the Scene viewport's own left-edge tool palette and top-right
// chips (EditorLayer_ToolPalette.cpp) — the audit's "retire the 25-icon top strip". A later pass
// finished the job: the one-click icon row that used to live below this menu bar (document strip,
// Undo/Redo/Save, Play/Stop/Pause/Step, grid/snap/ground, gizmos, Stats/Console/History toggles,
// Capture, the notification bell) moved out too, split between the left tool palette (viewport
// display options: grid/snap/gizmos) and a new floating action bar centered over the Scene/Game
// viewport (session actions: Undo/Redo/Save, Play controls, panel toggles, Capture, notifications
// — see EditorLayer::DrawViewportActionBar in EditorLayer_Toolbar.cpp). This strip is just the menu
// bar row now, which freed that vertical space back to the viewport (host-owned kToolbarHeight
// shrank to match).
//
// What changed in the move: the strip owns the pinned "##Toolbar" window (it pins itself against
// GetToolbarMetrics rather than the caller pre-setting pos/size; it also used to own the
// Windows-XP Luna chrome override, removed along with that theme in Phase 1 item 9); every
// menu's contents — scene load/save, entity creation, camera framing — are still host code,
// rendered into these menus through the Draw*Body callbacks (the single shared ImGuiContext makes
// a host-side ImGui call inside a module-begun menu land where the module put it). The window
// controls act on the host's GLFW window through callbacks; the module never sees the handle.

#include "EditorModuleAPI.h"
#include "EditorUIPrimitives.h"
#include "EditorIcons.h"

#include <imgui.h>
#include <imgui_internal.h> // ImFloor
#include <IconsFontAwesome6.h>

namespace EditorModuleToolbar {

namespace {

void Tooltip(const EditorModuleHostAPI& host, const char* text) {
    if (host.SetTooltip) host.SetTooltip(text);
}

// Minimize / maximize-restore / close, right-aligned in the toolbar's menu-bar row. The OS title
// bar is removed (Win32 custom frame in Window.cpp); these act on the host's GLFW window through
// callbacks.
void DrawWindowControls(const EditorModuleHostAPI& host) {
    const float h = ImGui::GetFrameHeight();
    const float bw = ImFloor(h * 1.6f);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float startX = ImGui::GetWindowWidth() - bw * 3.0f - st.WindowPadding.x;
    ImGui::SameLine(startX > 0.0f ? startX : 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, st.ItemSpacing.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f); // flat window controls, no hairline box
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.09f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.16f));

    ImGui::PushID("win_min");
    if (ImGui::Button(EDITOR_ICON_WINDOW_MINIMIZE, ImVec2(bw, 0.0f)) && host.WindowMinimize) host.WindowMinimize();
    if (ImGui::IsItemHovered()) Tooltip(host, "Minimize");
    ImGui::PopID();
    ImGui::SameLine();

    const bool maxed = host.WindowIsMaximized && host.WindowIsMaximized();
    ImGui::PushID("win_max");
    if (ImGui::Button(maxed ? EDITOR_ICON_WINDOW_RESTORE : EDITOR_ICON_WINDOW_MAXIMIZE, ImVec2(bw, 0.0f)) && host.WindowToggleMaximize)
        host.WindowToggleMaximize();
    if (ImGui::IsItemHovered()) Tooltip(host, maxed ? "Restore" : "Maximize");
    ImGui::PopID();
    ImGui::SameLine();

    {
        ImVec4 danger = EditorUIPrimitives::DangerColor(); // Unity "Error Text" #D32222
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(danger.x, danger.y, danger.z, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(danger.x * 0.9f, danger.y * 0.9f, danger.z * 0.9f, 1.0f));
    }
    ImGui::PushID("win_close");
    if (ImGui::Button(EDITOR_ICON_WINDOW_CLOSE, ImVec2(bw, 0.0f)) && host.WindowClose) host.WindowClose();
    if (ImGui::IsItemHovered()) Tooltip(host, "Close");
    ImGui::PopID();
    ImGui::PopStyleColor(2);

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}

} // namespace

void Draw(const EditorModuleHostAPI& host) {
    float winW = 0.0f, toolbarH = 0.0f, uiScale = 1.0f;
    if (host.GetToolbarMetrics) host.GetToolbarMetrics(&winW, &toolbarH, &uiScale);
    if (winW < 1.0f || toolbarH < 1.0f) return;

    // Always pinned regardless of Lock Layout — pos/size forced every frame. Fixed size +
    // NoResize + size constraints keep it from ever stretching down over the dock panels' tab
    // bars; NoMove/NoDocking keep it from being dragged off or docked; NoSavedSettings stops a
    // stale imgui.ini from repositioning it.
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(winW, toolbarH), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(winW, toolbarH), ImVec2(winW, toolbarH));
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
    // Tight vertical window padding so the menu bar hugs the top and bottom edge — the strip is
    // only as tall as that one row now (host-owned kToolbarHeight).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f * uiScale, 3.0f * uiScale)); // #37

    // Phase 1 item 9 — the Windows XP theme (and its Luna-blue toolbar-strip override, the one
    // consumer host.GetEditorTheme ever had) is gone; the strip now just follows the active
    // theme's ordinary MenuBarBg/Text colours like every other panel.
    if (!ImGui::Begin("##Toolbar", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // Real dropdown menus for the stuff you reach for occasionally (import, add primitive, scene
    // save/load). The menu bodies are host code (deep scene / entity / camera logic); rendered
    // here through callbacks.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(ICON_FA_FOLDER_OPEN " File")) {
            if (host.DrawFileMenuBody) host.DrawFileMenuBody();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_CUBES " Create")) {
            if (host.DrawAddEntityMenuItems) host.DrawAddEntityMenuItems();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_CAMERA " View")) {
            if (host.DrawViewMenuBody) host.DrawViewMenuBody();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_TABLE_COLUMNS " Window")) {
            if (host.DrawWindowMenuBody) host.DrawWindowMenuBody();
            ImGui::EndMenu();
        }

        // Preferences and Project Settings are one and the same searchable window (a "THIS
        // MACHINE" / "THIS PROJECT" sidebar inside DrawSettingsWindow) — two separate menu items
        // both opening it just made it look like two different destinations. One entry now;
        // Ctrl+, still lands on the editor side, Ctrl+Shift+P still lands on the project side
        // (editor.preferences / project.settings shortcuts, unchanged), and either way both
        // groups are one click away in the sidebar once it's open.
        if (ImGui::MenuItem(ICON_FA_GEAR " Settings") && host.OpenPreferences) host.OpenPreferences();
        if (ImGui::IsItemHovered()) Tooltip(host, "Editor preferences and project settings (Ctrl+,)");

        // Phase 6 item 11 — the shortcut coverage pass's own "add a Help ▸ Shortcuts reference"
        // ask. Jumps straight to the existing press-to-bind editor rather than duplicating it.
        if (ImGui::BeginMenu(ICON_FA_CIRCLE_QUESTION " Help")) {
            if (ImGui::MenuItem(ICON_FA_KEYBOARD "  Shortcuts") && host.OpenShortcutsReference)
                host.OpenShortcutsReference();
            // #184 — the rest of a Help menu: docs, logs, a bug-report path, About.
            if (ImGui::MenuItem(ICON_FA_BOOK "  Documentation") && host.OpenDocumentation)
                host.OpenDocumentation();
            if (ImGui::IsItemHovered()) Tooltip(host, "Open the project's documentation in your browser");
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open Log Folder") && host.OpenLogFolder)
                host.OpenLogFolder();
            if (ImGui::IsItemHovered()) Tooltip(host, "Show Editor.log (this session) and Editor-prev.log in Explorer");
            if (ImGui::MenuItem(ICON_FA_BUG "  Report a Bug...") && host.ReportBug)
                host.ReportBug();
            if (ImGui::IsItemHovered())
                Tooltip(host, "Copies the system report to the clipboard, shows Editor.log, and opens\n"
                              "the issue tracker - paste the report and attach the log there.");
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_CIRCLE_INFO "  About Tartarus Engine") && host.OpenAbout)
                host.OpenAbout();
            ImGui::EndMenu();
        }

        // Custom window controls, right-aligned — the OS title bar is gone (Win32 custom frame,
        // Window.cpp), so minimize / maximize-restore / close live here instead.
        DrawWindowControls(host);

        ImGui::EndMenuBar();
    }

    // The toolbar's empty space is the window drag handle (the OS caption is gone). True only
    // when the cursor is over this strip and not over any widget / open menu / active drag —
    // Window.cpp's WM_NCHITTEST reads this (via the host) to return HTCAPTION.
    const bool dragHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        !ImGui::IsAnyItemHovered() &&
        !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (host.SetTitleBarDragHovered) host.SetTitleBarDragHovered(dragHovered);

    ImGui::End();
    ImGui::PopStyleVar(); // WindowPadding
}

} // namespace EditorModuleToolbar
