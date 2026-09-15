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

// EditorUI::VSeparator: a 1px rule in ImGuiCol_Separator spanning the frame height, ItemSpacing.x
// of breathing room either side. `gapScale` widens that gap for a cluster break.
void VSeparator(float gapScale = 1.0f) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float gap = style.ItemSpacing.x * gapScale;
    ImGui::SameLine(0.0f, gap);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x, p.y + h),
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

    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.15f, 0.18f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.12f, 0.15f, 1.0f));
    ImGui::PushID("win_close");
    if (ImGui::Button(EDITOR_ICON_WINDOW_CLOSE, ImVec2(bw, 0.0f)) && host.WindowClose) host.WindowClose();
    if (ImGui::IsItemHovered()) Tooltip(host, "Close");
    ImGui::PopID();
    ImGui::PopStyleColor(2);

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
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

        if (ImGui::MenuItem(ICON_FA_GEAR " Preferences") && host.OpenPreferences) host.OpenPreferences();
        if (ImGui::IsItemHovered()) Tooltip(host, "Per-user editor settings, environment, shortcuts (Ctrl+,)");

        if (ImGui::MenuItem(ICON_FA_GEARS " Project Settings") && host.OpenProjectSettings) host.OpenProjectSettings();
        if (ImGui::IsItemHovered()) Tooltip(host, "Physics, tags and layer names \xE2\x80\x94 saved with the project, not your editor prefs");

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
    if (host.DrawPlayControlsBody) host.DrawPlayControlsBody();

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
    ImGui::SameLine(0.0f, 1.0f);
    ImGui::PushID("##gridSnapOpts");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
    if (ImGui::Button(EDITOR_ICON_CARET_DOWN)) ImGui::OpenPopup("##GridSnapPopup");
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) Tooltip(host, "Grid & snap settings");
    if (ImGui::BeginPopup("##GridSnapPopup")) {
        CloseOnEscape();
        if (host.DrawGridSnapPopupBody) host.DrawGridSnapPopupBody();
        ImGui::EndPopup();
    }
    ImGui::PopID();
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
        ImGui::SameLine(0.0f, 1.0f);
        ImGui::PushID("##gizmosOpts");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        if (ImGui::Button(EDITOR_ICON_CARET_DOWN)) ImGui::OpenPopup("##GizmosPopup");
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) Tooltip(host, "Gizmo visibility");
        if (ImGui::BeginPopup("##GizmosPopup")) {
            CloseOnEscape();
            if (host.DrawGizmosPopupBody) host.DrawGizmosPopupBody();
            ImGui::EndPopup();
        }
        ImGui::PopID();
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
        if (ActionButton(host, EDITOR_ICON_CONSOLE, "Toggle Console", consoleVisible) && cs) cs->Visible = !cs->Visible;
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
        ImGui::SameLine(0.0f, 1.0f);
        ImGui::PushID("##capOpts");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        if (ImGui::Button(EDITOR_ICON_CARET_DOWN)) ImGui::OpenPopup("##CapturePopup");
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) Tooltip(host, "Capture options");
        if (ImGui::BeginPopup("##CapturePopup")) {
            CloseOnEscape();
            if (host.DrawCaptureOptionsPopupBody) host.DrawCaptureOptionsPopupBody();
            ImGui::EndPopup();
        }
        ImGui::PopID();
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
