// The editor's top toolbar strip — the dropdown menu bar (File / Add / View / Window /
// Preferences), the custom window min/max/close controls, and the one-click icon row below it
// (document strip, play controls, grid/snap/ground, light gizmos, stats / console / history
// toggles, screenshot) — living inside TartarusEditor.dll so its layout and chrome hot-reload
// while the editor stays open with its scene loaded. Ported in behaviour from
// EditorLayer::DrawTopToolbar / DrawWindowControls (EditorLayer_Toolbar.cpp).
//
// Phase 3 item 3 moved the tool-selection cluster (Hand/Translate/Rotate/.../Universal, Measure,
// Duplicate Array, Local/World, Pivot/Center) and the view-state cluster (draw mode, ortho/persp)
// out of this strip entirely, into the Scene viewport's own left-edge tool palette and top-right
// chips (EditorLayer_ToolPalette.cpp) — the audit's "retire the 25-icon top strip".
//
// What changed in the move: the strip owns the pinned "##Toolbar" window (it pins itself against
// GetToolbarMetrics rather than the caller pre-setting pos/size; it also used to own the
// Windows-XP Luna chrome override, removed along with that theme in Phase 1 item 9); every
// toggle it shows is host state read/written through EditorModuleHostAPI (API v4); and the
// deep menu contents — scene load/save, entity creation, camera framing, the capture options
// popup — are still host code, rendered into these menus through the Draw*Body callbacks (the
// single shared ImGuiContext makes a host-side ImGui call inside a module-begun menu land where
// the module put it). The window controls act on the host's GLFW window through callbacks; the
// module never sees the handle.

#include "EditorModuleAPI.h"
#include "EditorUIPrimitives.h"
#include "EditorIcons.h"

#include <imgui.h>
#include <imgui_internal.h> // ImFloor
#include <IconsFontAwesome6.h>

#include <cstdio>
#include <algorithm>

namespace EditorModuleToolbar {

namespace {

void Tooltip(const EditorModuleHostAPI& host, const char* text) {
    if (host.SetTooltip) host.SetTooltip(text);
}

// #7 — a plain ImGui::BeginPopup is documented to close on Escape by default, but that path
// runs through Dear ImGui's Nav system, and this editor never sets
// ImGuiConfigFlags_NavEnableKeyboard (Defect #33, Phase 2) — so none of this toolbar's popovers
// actually closed on Escape in practice. Call as the first line inside every
// `if (ImGui::BeginPopup(...))` body below, matching the explicit-Escape-check idiom every modal
// dialog elsewhere in the editor already uses for the same reason.
void CloseOnEscape() {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
}

// Forwards to the shared implementation (EditorUIPrimitives.h, Defect #53) — kept as a local
// wrapper so every call site below (which passes `host` first) keeps compiling unchanged.
bool ActionButton(const EditorModuleHostAPI& host, const char* icon, const char* tooltip, bool active = false) {
    return EditorUIPrimitives::ActionButton(icon, tooltip, host.SetTooltip, active);
}

// A slim caret strip beside a full ActionButton, opening a per-cluster options popup (grid/snap,
// gizmos, capture). Used to sit flush against its parent icon with only a 1px SameLine gap, which
// read as one oddly-shaped button rather than two controls; a touch more gap plus a narrower
// FramePadding.x (a slim strip, not a second full-width icon button) makes the two legible as
// separate click targets at a glance.
void CaretDropdownButton(const EditorModuleHostAPI& host, const char* popupId, const char* tooltip) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::PushID(popupId);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * 0.5f, style.FramePadding.y));
    if (ImGui::Button(EDITOR_ICON_CARET_DOWN)) ImGui::OpenPopup(popupId);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) Tooltip(host, tooltip);
    ImGui::PopID();
}

// EditorUI::VSeparator: a 1px rule in ImGuiCol_Separator spanning the frame height, ItemSpacing.x
// of breathing room either side. `gapScale` widens that gap for a cluster break.
void VSeparator(float gapScale = 1.0f) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float gap = style.ItemSpacing.x * gapScale;
    ImGui::SameLine(0.0f, gap);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    // Inset top/bottom rather than spanning the full frame height — a full-height rule reads as
    // hard as the icon glyphs themselves, so every cluster break looked identical to every other
    // one and the whole strip read as one dense row rather than distinct groups (history · tools ·
    // grid/snap · view · panels · capture). A shorter, inset tick is the same visual weight
    // Unity's own toolbar dividers use and lets the wider `divider()` gap do most of the grouping.
    const float inset = h * 0.22f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + inset), ImVec2(p.x, p.y + h - inset),
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(1.0f, h));
    ImGui::SameLine(0.0f, gap);
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
    // Tight vertical window padding so the icon row hugs the menu bar and the bottom edge — the
    // strip is only as tall as its two rows (host-owned kToolbarHeight).
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
    // save/load) — keeps the always-visible row below reserved for one-click toggles. The menu
    // bodies are host code (deep scene / entity / camera logic); rendered here through callbacks.
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
            ImGui::EndMenu();
        }

        // Custom window controls, right-aligned — the OS title bar is gone (Win32 custom frame,
        // Window.cpp), so minimize / maximize-restore / close live here instead.
        DrawWindowControls(host);

        ImGui::EndMenuBar();
    }

    // A spaced group separator so the toolbar reads as distinct clusters (history · tools ·
    // grid/snap · view · panels · capture) instead of one dense left-jammed run of identical
    // squares (audit #66 / #147).
    auto divider = []() { VSeparator(1.5f); };

    // --- Document strip (Zone A, Phase 3 item 1) --------------------------------------------
    // The open scene's name + an unsaved-changes dot, plus Undo/Redo/Save — until now there was
    // no visible Save control anywhere in the editor (audit #5), only File > Save and Ctrl+S.
    {
        char sceneName[128] = "Untitled";
        if (host.GetSceneDisplayName) host.GetSceneDisplayName(sceneName, (int)sizeof(sceneName));
        const bool dirty = host.GetSceneDirty && host.GetSceneDirty();

        ImGui::AlignTextToFramePadding();
        if (dirty) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), ICON_FA_CIRCLE);
            if (ImGui::IsItemHovered()) Tooltip(host, "Unsaved changes");
            ImGui::SameLine(0.0f, 6.0f);
        }
        ImGui::TextUnformatted(sceneName);
        ImGui::SameLine();
    }
    divider();

    if (ActionButton(host, EDITOR_ICON_UNDO, "Undo (Ctrl+Z)") && host.ToolbarUndo) host.ToolbarUndo();
    ImGui::SameLine();
    if (ActionButton(host, EDITOR_ICON_REDO, "Redo (Ctrl+Y)") && host.ToolbarRedo) host.ToolbarRedo();
    ImGui::SameLine();
    if (ActionButton(host, EDITOR_ICON_SAVE, "Save (Ctrl+S)") && host.DoSaveScene) host.DoSaveScene();

    // --- Play controls (Zone B, Phase 3 item 2) ---------------------------------------------
    // Replaces the old floating `##PlayStopButton` overlay for windowed play — that overlay sat
    // over the viewport at ~60% opacity and all but disappeared on a pale scene (audit #5's
    // "single most important control ... least visible thing on screen", ~2:1 contrast). Living
    // in the toolbar means it's always on a fully opaque background by construction. The floating
    // version still exists for maximized play only, which hides this whole strip.
    divider();
    {
        // Play/Stop/Pause/Step/Fullscreen center themselves in the toolbar row, matching Unity's
        // own layout — the play cluster anchors to the middle of the strip regardless of what
        // sits either side of it, rather than immediately following Undo/Redo/Save. The cluster's
        // width isn't known until after it's drawn, so this centers off last frame's width (a
        // one-frame lag on a resize or Play/Stop state change is imperceptible) — the standard
        // immediate-mode centering trick.
        static float s_playClusterW = 0.0f;
        const float startX = ImGui::GetCursorPosX();
        const float centerX = (winW - s_playClusterW) * 0.5f;
        if (centerX > startX) ImGui::SetCursorPosX(centerX);
        ImGui::BeginGroup();
        if (host.DrawPlayControlsBody) host.DrawPlayControlsBody();
        ImGui::EndGroup();
        s_playClusterW = ImGui::GetItemRectSize().x;
    }

    // Phase 3 item 3 — Hand/Translate/Rotate/Scale/Rect/Universal, Measure, Duplicate Array, and
    // the Local/World + Pivot/Center modifiers moved out of the strip into the Scene viewport's
    // own left-edge tool palette (EditorLayer_ToolPalette.cpp, audit #5 Q7's T-panel model).

    divider();
    const bool showGrid = host.GetShowGrid && host.GetShowGrid();
    if (ActionButton(host, EDITOR_ICON_GRID, "Toggle Grid", showGrid) && host.SetShowGrid) host.SetShowGrid(!showGrid);
    ImGui::SameLine();
    const bool gridSnap = host.GetGridSnapEnabled && host.GetGridSnapEnabled();
    if (ActionButton(host, EDITOR_ICON_SNAP_TO_GRID, "Toggle Snap to Grid (hold Ctrl to invert while dragging)", gridSnap)
            && host.SetGridSnapEnabled) {
        host.SetGridSnapEnabled(!gridSnap);
    }
    CaretDropdownButton(host, "##GridSnapPopup", "Grid & snap settings");
    if (ImGui::BeginPopup("##GridSnapPopup")) {
        CloseOnEscape();
        if (host.DrawGridSnapPopupBody) host.DrawGridSnapPopupBody();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    {
        const bool canSnap = host.CanSnapSelectionToGround && host.CanSnapSelectionToGround();
        ImGui::BeginDisabled(!canSnap);
        if (ActionButton(host, EDITOR_ICON_SNAP_TO_GROUND, "Snap selection to ground") && host.SnapSelectionToGround)
            host.SnapSelectionToGround();
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    {
        const bool gizmosOn = host.GetGizmosMasterVisible && host.GetGizmosMasterVisible();
        if (ActionButton(host, EDITOR_ICON_TOGGLE_GIZMOS,
                "Toggle Gizmos (transform gizmo, entity icons, light gizmos)\nCaret: per-type visibility",
                gizmosOn) && host.SetGizmosMasterVisible) {
            host.SetGizmosMasterVisible(!gizmosOn);
        }
        CaretDropdownButton(host, "##GizmosPopup", "Gizmo visibility");
        if (ImGui::BeginPopup("##GizmosPopup")) {
            CloseOnEscape();
            if (host.DrawGizmosPopupBody) host.DrawGizmosPopupBody();
            ImGui::EndPopup();
        }
    }

    // Phase 3 item 3 — the draw-mode dropdown and orthographic/perspective toggle moved into the
    // Scene viewport's own top-right view-state chips (EditorLayer_ToolPalette.cpp).

    divider();
    {
        const bool showStats = host.GetShowStats && host.GetShowStats();
        if (ActionButton(host, EDITOR_ICON_STATISTICS, "Toggle Statistics", showStats) && host.SetShowStats)
            host.SetShowStats(!showStats);
    }
    ImGui::SameLine();
    {
        // Console visibility lives host-side (not in the module) so this toggle keeps working
        // across a TartarusEditor.dll reload, and so a hidden Console stays hidden through one.
        EditorConsoleState* cs = host.ConsoleState ? host.ConsoleState() : nullptr;
        const bool consoleVisible = cs && cs->Visible;
        if (ActionButton(host, EDITOR_ICON_CONSOLE, "Toggle Console", consoleVisible) && cs) {
            // Defect #11 — this used to just flip Visible off whenever Console was already open,
            // even when it was sitting open-but-buried behind a sibling dock tab (Asset Browser):
            // the click closed it instead of surfacing it, so bringing it forward took a second
            // click on this same button (open again, only THEN visible). If it's currently a
            // hidden (non-selected) tab, focus it instead of closing it — only actually close when
            // it's already the front/selected tab.
            if (!cs->Visible) {
                cs->Visible = true;
            } else {
                ImGuiWindow* consoleWin = ImGui::FindWindowByName(ICON_FA_TERMINAL "  Console");
                if (consoleWin && consoleWin->Hidden) ImGui::FocusWindow(consoleWin);
                else cs->Visible = false;
            }
        }
    }
    ImGui::SameLine();
    {
        const bool showHistory = host.GetShowHistory && host.GetShowHistory();
        if (ActionButton(host, EDITOR_ICON_HISTORY, "Toggle History", showHistory) && host.SetShowHistory)
            host.SetShowHistory(!showHistory);
    }

    divider();
    // Capture: click = shoot with the current settings; the caret opens the options popup (host
    // body). The button tooltip is built host-side from EditorSettings.
    {
        char tip[128] = "Capture screenshot (Print Screen)";
        if (host.GetCaptureButtonTooltip) host.GetCaptureButtonTooltip(tip, (int)sizeof(tip));
        if (ActionButton(host, EDITOR_ICON_CAPTURE, tip) && host.RequestCapture) host.RequestCapture();
        CaretDropdownButton(host, "##CapturePopup", "Capture options");
        if (ImGui::BeginPopup("##CapturePopup")) {
            CloseOnEscape();
            if (host.DrawCaptureOptionsPopupBody) host.DrawCaptureOptionsPopupBody();
            ImGui::EndPopup();
        }
    }

    // --- Notification bell (Phase 6 item 14) -------------------------------------------------
    // Unread count is Warning/Error only (see EditorNotification::Level) — captures and other
    // info/success entries never badge the bell, only ever show up as their own floating card.
    divider();
    {
        const int unread = host.GetNotificationUnreadCount ? host.GetNotificationUnreadCount() : 0;
        char tip[32];
        std::snprintf(tip, sizeof(tip), "Notifications%s", unread > 0 ? " (unread)" : "");
        if (ActionButton(host, ICON_FA_BELL, tip)) {
            ImGui::OpenPopup("##NotificationsPopup");
            if (host.MarkNotificationsRead) host.MarkNotificationsRead();
        }
        if (unread > 0) {
            const ImVec2 btnMin = ImGui::GetItemRectMin();
            const ImVec2 btnMax = ImGui::GetItemRectMax();
            char countStr[8];
            std::snprintf(countStr, sizeof(countStr), "%d", unread > 99 ? 99 : unread);
            const ImVec2 textSize = ImGui::CalcTextSize(countStr);
            const float badgeR = std::max(7.0f * uiScale, textSize.x * 0.5f + 2.0f * uiScale);
            const ImVec2 center(btnMax.x - badgeR * 0.7f, btnMin.y + badgeR * 0.7f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(center, badgeR, IM_COL32(219, 60, 60, 255));
            dl->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
                        IM_COL32(255, 255, 255, 255), countStr);
        }
        if (ImGui::BeginPopup("##NotificationsPopup")) {
            CloseOnEscape();
            ImGui::TextDisabled("Notifications");
            ImGui::Separator();
            if (host.DrawNotificationsPopupBody) host.DrawNotificationsPopupBody();
            ImGui::EndPopup();
        }
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
