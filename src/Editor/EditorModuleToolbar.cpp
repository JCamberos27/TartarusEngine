// The editor's top toolbar strip — the dropdown menu bar (File / Add / View / Window /
// Preferences), the custom window min/max/close controls, and the one-click icon row below it
// (undo/redo, gizmo op + space/pivot, grid/snap/ground, light gizmos, shading, ortho, stats /
// console / history toggles, screenshot) — living inside TartarusEditor.dll so its layout and
// chrome hot-reload while the editor stays open with its scene loaded. Ported in behaviour from
// EditorLayer::DrawTopToolbar / DrawWindowControls (EditorLayer_Toolbar.cpp).
//
// What changed in the move: the strip owns the pinned "##Toolbar" window (it pins itself against
// GetToolbarMetrics rather than the caller pre-setting pos/size) and the Windows-XP Luna chrome;
// every toggle it shows is host state read/written through EditorModuleHostAPI (API v4); and the
// deep menu contents — scene load/save, entity creation, camera framing, the capture options
// popup — are still host code, rendered into these menus through the Draw*Body callbacks (the
// single shared ImGuiContext makes a host-side ImGui call inside a module-begun menu land where
// the module put it). The window controls act on the host's GLFW window through callbacks; the
// module never sees the handle.

#include "EditorModuleAPI.h"

#include <imgui.h>
#include <imgui_internal.h> // ImFloor
#include <IconsFontAwesome6.h>

#include <cstdio>

namespace EditorModuleToolbar {

namespace {

void Tooltip(const EditorModuleHostAPI& host, const char* text) {
    if (host.SetTooltip) host.SetTooltip(text);
}

// EditorInternal::ActionButton, copied module-side (EditorLayerInternal.h pulls in World/EnTT/the
// renderer, none of which belong in this DLL). Flat: no body at rest, faint wash on hover;
// `active` gives an accent body + a 2px bottom keyline for toggles that are "on".
bool ActionButton(const EditorModuleHostAPI& host, const char* icon, const char* tooltip, bool active = false) {
    const ImVec4 keyline(0.55f, 0.60f, 0.72f, 1.0f);
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.400f, 0.435f, 0.520f, 0.32f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.400f, 0.435f, 0.520f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.400f, 0.435f, 0.520f, 0.60f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
    }
    ImGui::PushID(tooltip); // Button() folds its label into its ID — scope to the unique tooltip
    bool clicked = ImGui::Button(icon);
    ImGui::PopID();
    if (active) {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const float y = mx.y - 2.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(mn.x + 3.0f, y), ImVec2(mx.x - 3.0f, mx.y - 1.0f),
                                                  ImGui::ColorConvertFloat4ToU32(keyline), 1.0f);
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) Tooltip(host, tooltip);
    return clicked;
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
    if (ImGui::Button(ICON_FA_MINUS, ImVec2(bw, 0.0f)) && host.WindowMinimize) host.WindowMinimize();
    if (ImGui::IsItemHovered()) Tooltip(host, "Minimize");
    ImGui::PopID();
    ImGui::SameLine();

    const bool maxed = host.WindowIsMaximized && host.WindowIsMaximized();
    ImGui::PushID("win_max");
    if (ImGui::Button(maxed ? ICON_FA_COMPRESS : ICON_FA_EXPAND, ImVec2(bw, 0.0f)) && host.WindowToggleMaximize)
        host.WindowToggleMaximize();
    if (ImGui::IsItemHovered()) Tooltip(host, maxed ? "Restore" : "Maximize");
    ImGui::PopID();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.15f, 0.18f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.12f, 0.15f, 1.0f));
    ImGui::PushID("win_close");
    if (ImGui::Button(ICON_FA_XMARK, ImVec2(bw, 0.0f)) && host.WindowClose) host.WindowClose();
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 3.0f));

    // Windows XP theme (2): the toolbar strip is the Luna taskbar blue with white menu/icon text
    // — the one spot the theme shows blue chrome (panels stay beige). ImGui paints one global
    // text colour, so the white is pushed for the whole strip and then locally reverted to the
    // body near-black inside each menu-dropdown / options popup (those render on the beige
    // PopupBg). See xpMenuTextPush/Pop below.
    const bool xpBar = host.GetEditorTheme && host.GetEditorTheme() == 2;
    const ImVec4 xpBodyText(0.09f, 0.09f, 0.09f, 1.0f);
    const ImVec4 xpBodyTextDim(0.50f, 0.50f, 0.50f, 1.0f);
    auto xpMenuTextPush = [&]() {
        if (xpBar) { ImGui::PushStyleColor(ImGuiCol_Text, xpBodyText);
                     ImGui::PushStyleColor(ImGuiCol_TextDisabled, xpBodyTextDim); }
    };
    auto xpMenuTextPop = [&]() { if (xpBar) ImGui::PopStyleColor(2); };
    const int xpBarCols = xpBar ? 4 : 0;
    if (xpBar) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg,     ImVec4(0.161f, 0.396f, 0.878f, 1.0f)); // #295EE0 Luna blue
        ImGui::PushStyleColor(ImGuiCol_MenuBarBg,    ImVec4(0.133f, 0.337f, 0.804f, 1.0f)); // #2256CD
        ImGui::PushStyleColor(ImGuiCol_Text,         ImVec4(0.97f, 0.98f, 1.0f, 1.0f));     // white chrome text
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.74f, 0.81f, 0.95f, 1.0f));
    }

    if (!ImGui::Begin("##Toolbar", nullptr, flags)) {
        ImGui::End();
        if (xpBarCols) ImGui::PopStyleColor(xpBarCols);
        ImGui::PopStyleVar();
        return;
    }

    // Real dropdown menus for the stuff you reach for occasionally (import, add primitive, scene
    // save/load) — keeps the always-visible row below reserved for one-click toggles. The menu
    // bodies are host code (deep scene / entity / camera logic); rendered here through callbacks.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(ICON_FA_FOLDER_OPEN " File")) {
            xpMenuTextPush(); // beige dropdown -> revert the strip's white text to body near-black
            if (host.DrawFileMenuBody) host.DrawFileMenuBody();
            xpMenuTextPop();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_CUBES " Create")) {
            xpMenuTextPush();
            if (host.DrawAddEntityMenuItems) host.DrawAddEntityMenuItems();
            xpMenuTextPop();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_CAMERA " View")) {
            xpMenuTextPush();
            if (host.DrawViewMenuBody) host.DrawViewMenuBody();
            xpMenuTextPop();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_TABLE_COLUMNS " Window")) {
            xpMenuTextPush();
            if (host.DrawWindowMenuBody) host.DrawWindowMenuBody();
            xpMenuTextPop();
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem(ICON_FA_GEAR " Preferences") && host.OpenPreferences) host.OpenPreferences();
        if (ImGui::IsItemHovered()) Tooltip(host, "Editor settings, environment, shortcuts (Ctrl+,)");

        // Custom window controls, right-aligned — the OS title bar is gone (Win32 custom frame,
        // Window.cpp), so minimize / maximize-restore / close live here instead.
        DrawWindowControls(host);

        ImGui::EndMenuBar();
    }

    // A spaced group separator so the toolbar reads as distinct clusters (history · tools ·
    // grid/snap · view · panels · capture) instead of one dense left-jammed run of identical
    // squares (audit #66 / #147).
    auto divider = []() { VSeparator(1.5f); };

    if (ActionButton(host, ICON_FA_ROTATE_LEFT, "Undo (Ctrl+Z)") && host.ToolbarUndo) host.ToolbarUndo();
    ImGui::SameLine();
    if (ActionButton(host, ICON_FA_ROTATE_RIGHT, "Redo (Ctrl+Y)") && host.ToolbarRedo) host.ToolbarRedo();

    const int gizmoOp = host.GetGizmoOp ? host.GetGizmoOp() : 0; // 0 Translate 1 Rotate 2 Scale 3 Rect 4 Universal
    const bool handTool = host.GetHandTool && host.GetHandTool();
    divider();
    if (ActionButton(host, ICON_FA_HAND, "Hand — drag to pan the view (Q)", handTool) && host.SetHandTool)
        host.SetHandTool(!handTool);
    ImGui::SameLine();
    if (ActionButton(host, ICON_FA_UP_DOWN_LEFT_RIGHT, "Translate (W)", !handTool && gizmoOp == 0) && host.SetGizmoOp) host.SetGizmoOp(0);
    ImGui::SameLine();
    if (ActionButton(host, ICON_FA_ARROWS_SPIN, "Rotate (E)", !handTool && gizmoOp == 1) && host.SetGizmoOp) host.SetGizmoOp(1);
    ImGui::SameLine();
    if (ActionButton(host, ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Scale (R)", !handTool && gizmoOp == 2) && host.SetGizmoOp) host.SetGizmoOp(2);
    ImGui::SameLine();
    if (ActionButton(host, ICON_FA_VECTOR_SQUARE, "Rect — move + non-uniform scale via corner/edge handles (T)",
            !handTool && gizmoOp == 3) && host.SetGizmoOp) host.SetGizmoOp(3);
    ImGui::SameLine();
    if (ActionButton(host, ICON_FA_ARROWS_TO_CIRCLE, "Transform — move + rotate + scale in one gizmo (Y)",
            !handTool && gizmoOp == 4) && host.SetGizmoOp) host.SetGizmoOp(4);
    ImGui::SameLine();
    const bool measureTool = host.GetMeasureTool && host.GetMeasureTool();
    if (ActionButton(host, ICON_FA_RULER, "Measure — click two points in the viewport to measure the distance",
            measureTool) && host.SetMeasureTool) host.SetMeasureTool(!measureTool);

    divider(); // transform tools | gizmo-space modifiers
    const bool localSpace = host.GetGizmoLocalSpace && host.GetGizmoLocalSpace();
    if (ActionButton(host, localSpace ? ICON_FA_ARROWS_TO_DOT : ICON_FA_GLOBE,
            localSpace ? "Local space (click for World)" : "World space (click for Local)") && host.SetGizmoLocalSpace) {
        host.SetGizmoLocalSpace(!localSpace);
    }
    ImGui::SameLine();
    const bool pivotCenter = host.GetGizmoPivotCenter && host.GetGizmoPivotCenter();
    if (ActionButton(host, pivotCenter ? ICON_FA_CIRCLE_DOT : ICON_FA_CROSSHAIRS,
            pivotCenter
                ? "Center - gizmo sits on the bounding-box center (click for Pivot)"
                : "Pivot - gizmo sits on the object's own origin (click for Center)") && host.SetGizmoPivotCenter) {
        host.SetGizmoPivotCenter(!pivotCenter);
    }

    divider();
    const bool showGrid = host.GetShowGrid && host.GetShowGrid();
    if (ActionButton(host, ICON_FA_TABLE_CELLS, "Toggle Grid", showGrid) && host.SetShowGrid) host.SetShowGrid(!showGrid);
    ImGui::SameLine();
    const bool gridSnap = host.GetGridSnapEnabled && host.GetGridSnapEnabled();
    if (ActionButton(host, ICON_FA_MAGNET, "Toggle Snap to Grid (hold Ctrl to invert while dragging)", gridSnap)
            && host.SetGridSnapEnabled) {
        host.SetGridSnapEnabled(!gridSnap);
    }
    ImGui::SameLine(0.0f, 1.0f);
    ImGui::PushID("##gridSnapOpts");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
    if (ImGui::Button(ICON_FA_CARET_DOWN)) ImGui::OpenPopup("##GridSnapPopup");
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) Tooltip(host, "Grid & snap settings");
    if (ImGui::BeginPopup("##GridSnapPopup")) {
        xpMenuTextPush();
        if (host.DrawGridSnapPopupBody) host.DrawGridSnapPopupBody();
        xpMenuTextPop();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::SameLine();
    {
        const bool canSnap = host.CanSnapSelectionToGround && host.CanSnapSelectionToGround();
        ImGui::BeginDisabled(!canSnap);
        if (ActionButton(host, ICON_FA_DOWN_LONG, "Snap selection to ground") && host.SnapSelectionToGround)
            host.SnapSelectionToGround();
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    {
        const bool gizmosOn = host.GetGizmosMasterVisible && host.GetGizmosMasterVisible();
        if (ActionButton(host, ICON_FA_UP_DOWN_LEFT_RIGHT,
                "Toggle Gizmos (transform gizmo, entity icons, light gizmos)\nCaret: per-type visibility",
                gizmosOn) && host.SetGizmosMasterVisible) {
            host.SetGizmosMasterVisible(!gizmosOn);
        }
        ImGui::SameLine(0.0f, 1.0f);
        ImGui::PushID("##gizmosOpts");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        if (ImGui::Button(ICON_FA_CARET_DOWN)) ImGui::OpenPopup("##GizmosPopup");
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) Tooltip(host, "Gizmo visibility");
        if (ImGui::BeginPopup("##GizmosPopup")) {
            xpMenuTextPush();
            if (host.DrawGizmosPopupBody) host.DrawGizmosPopupBody();
            xpMenuTextPop();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    ImGui::SameLine();
    // Scene-view shading, cycling Shaded -> Wireframe -> Unlit like a draw-mode dropdown — part of
    // the same "what the viewport shows" cluster as grid / snap / light gizmos.
    {
        const int shading = host.GetShadingMode ? host.GetShadingMode() : 0; // 0 Shaded 1 Wireframe 2 Unlit
        const char* shadingIcon = ICON_FA_CIRCLE_HALF_STROKE;
        const char* shadingTip = "Shaded (click for Wireframe)";
        if (shading == 1) {
            shadingIcon = ICON_FA_BORDER_NONE;
            shadingTip = "Wireframe (click for Unlit)";
        } else if (shading == 2) {
            shadingIcon = ICON_FA_SUN;
            shadingTip = "Unlit (click for Shaded)";
        }
        if (ActionButton(host, shadingIcon, shadingTip, shading != 0) && host.SetShadingMode) {
            host.SetShadingMode(shading == 0 ? 1 : (shading == 1 ? 2 : 0));
        }
    }

    ImGui::SameLine();
    // Orthographic / perspective toggle (shortcut 5) — it has a distinct on/off state so it
    // belongs on the strip beside the shading mode (#148).
    {
        const bool ortho = host.IsOrthographic && host.IsOrthographic();
        if (ActionButton(host, ortho ? ICON_FA_VECTOR_SQUARE : ICON_FA_EYE,
                ortho ? "Orthographic (click for Perspective) — 5"
                      : "Perspective (click for Orthographic) — 5",
                ortho) && host.ToggleOrthographic) {
            host.ToggleOrthographic();
        }
    }

    divider();
    {
        const bool showStats = host.GetShowStats && host.GetShowStats();
        if (ActionButton(host, ICON_FA_CHART_SIMPLE, "Toggle Statistics", showStats) && host.SetShowStats)
            host.SetShowStats(!showStats);
    }
    ImGui::SameLine();
    {
        // Console visibility lives host-side (not in the module) so this toggle keeps working
        // across a TartarusEditor.dll reload, and so a hidden Console stays hidden through one.
        EditorConsoleState* cs = host.ConsoleState ? host.ConsoleState() : nullptr;
        const bool consoleVisible = cs && cs->Visible;
        if (ActionButton(host, ICON_FA_TERMINAL, "Toggle Console", consoleVisible) && cs) cs->Visible = !cs->Visible;
    }
    ImGui::SameLine();
    {
        const bool showHistory = host.GetShowHistory && host.GetShowHistory();
        if (ActionButton(host, ICON_FA_CLOCK_ROTATE_LEFT, "Toggle History", showHistory) && host.SetShowHistory)
            host.SetShowHistory(!showHistory);
    }

    divider();
    // Capture: click = shoot with the current settings; the caret opens the options popup (host
    // body). The button tooltip is built host-side from EditorSettings.
    {
        char tip[128] = "Capture screenshot (Print Screen)";
        if (host.GetCaptureButtonTooltip) host.GetCaptureButtonTooltip(tip, (int)sizeof(tip));
        if (ActionButton(host, ICON_FA_CAMERA_RETRO, tip) && host.RequestCapture) host.RequestCapture();
        ImGui::SameLine(0.0f, 1.0f);
        ImGui::PushID("##capOpts");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        if (ImGui::Button(ICON_FA_CARET_DOWN)) ImGui::OpenPopup("##CapturePopup");
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) Tooltip(host, "Capture options");
        if (ImGui::BeginPopup("##CapturePopup")) {
            xpMenuTextPush();
            if (host.DrawCaptureOptionsPopupBody) host.DrawCaptureOptionsPopupBody();
            xpMenuTextPop();
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
    if (xpBarCols) ImGui::PopStyleColor(xpBarCols); // XP toolbar blue + white chrome text
    ImGui::PopStyleVar(); // WindowPadding
}

} // namespace EditorModuleToolbar
