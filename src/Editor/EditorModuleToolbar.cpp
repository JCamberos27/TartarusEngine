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
#include "EditorTheme.h"

#include <imgui.h>
#include <imgui_internal.h> // ImFloor
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

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
    const float startX = ImGui::GetWindowWidth() - bw * 3.0f;
    ImGui::SameLine(startX > 0.0f ? startX : 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, st.ItemSpacing.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f); // flat window controls, no hairline box
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorUIPrimitives::FlatHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorUIPrimitives::FlatPressed());
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Secondary);

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
        // Close turns the system red on hover, like every Windows title bar.
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Rgb(0xC4, 0x2B, 0x1C));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Rgb(0xA3, 0x24, 0x18));
    }
    ImGui::PushID("win_close");
    if (ImGui::Button(EDITOR_ICON_WINDOW_CLOSE, ImVec2(bw, 0.0f)) && host.WindowClose) host.WindowClose();
    if (ImGui::IsItemHovered()) Tooltip(host, "Close");
    ImGui::PopID();
    ImGui::PopStyleColor(2);

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
}

} // namespace

void Draw(const EditorModuleHostAPI& host) {
    float winW = 0.0f, toolbarH = 0.0f, uiScale = 1.0f;
    if (host.GetToolbarMetrics) host.GetToolbarMetrics(&winW, &toolbarH, &uiScale);
    if (winW < 1.0f || toolbarH < 1.0f) return;

    // Always pinned regardless of Lock Layout â€” pos/size forced every frame. Fixed size +
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
    // The menu bar fills the strip: no window padding above or below it, and the row's height
    // comes from the frame padding so the menus centre vertically in the strip.
    const float fontH = ImGui::GetFontSize();
    const float padY = std::max(0.0f, (toolbarH - fontH) * 0.5f);
    const ImVec2 normalFramePadding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, padY));
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, EditorTheme::Base);

    if (!ImGui::Begin("##Toolbar", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        return;
    }

    if (ImGui::BeginMenuBar()) {
        // The engine's monogram, then the menus: plain words, like every desktop application.
        ImGui::Dummy(ImVec2(EditorTheme::Px(6.0f), 0.0f));
        if (const unsigned int mark = host.GetEngineMarkTexture ? host.GetEngineMarkTexture() : 0u) {
            const float s = std::floor(toolbarH * 0.62f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)mark, ImVec2(p.x, std::floor((toolbarH - s) * 0.5f)),
                                                 ImVec2(p.x + s, std::floor((toolbarH - s) * 0.5f) + s), ImVec2(0, 0), ImVec2(1, 1),
                                                 EditorTheme::U32(EditorTheme::Accent));
            ImGui::Dummy(ImVec2(s + EditorTheme::Px(8.0f), 0.0f));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Secondary);
        // Inside an open menu: the ordinary text colour and frame padding again.
        auto menu = [&](const char* label) {
            const bool open = ImGui::BeginMenu(label);
            if (open) {
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Text);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, normalFramePadding);
            }
            return open;
        };
        auto endMenu = []() { ImGui::PopStyleVar(); ImGui::PopStyleColor(); ImGui::EndMenu(); };
        if (menu("File")) {
            if (host.DrawFileMenuBody) host.DrawFileMenuBody();
            endMenu();
        }
        if (menu("Edit")) {
            if (host.DrawEditMenuBody) host.DrawEditMenuBody();
            endMenu();
        }
        if (menu("Create")) {
            if (host.DrawAddEntityMenuItems) host.DrawAddEntityMenuItems();
            endMenu();
        }
        if (menu("View")) {
            if (host.DrawViewMenuBody) host.DrawViewMenuBody();
            endMenu();
        }
        if (menu("Window")) {
            if (host.DrawWindowMenuBody) host.DrawWindowMenuBody();
            endMenu();
        }
        // Phase 6 item 11 / #184: the shortcuts reference, docs, logs, a bug-report path, About.
        if (menu("Help")) {
            if (ImGui::MenuItem(ICON_FA_KEYBOARD "  Shortcuts") && host.OpenShortcutsReference)
                host.OpenShortcutsReference();
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
            endMenu();
        }
        ImGui::PopStyleColor();

        // The open scene, centred in the title bar like a document title: its name, an accent dot
        // while it has unsaved changes, and a PLAYING tag in Play.
        if (host.GetSceneTitle) {
            char title[128] = {};
            bool dirty = false, playing = false;
            host.GetSceneTitle(title, (int)sizeof(title), &dirty, &playing);
            const float tw = ImGui::CalcTextSize(title).x;
            const float dot = dirty ? EditorTheme::Px(14.0f) : 0.0f;
            EditorTheme::PushHeading();
            const char* tag = "P L A Y I N G";
            const float tagW = playing ? ImGui::CalcTextSize(tag).x + EditorTheme::Px(22.0f) : 0.0f;
            EditorTheme::PopFont();
            const float total = tw + dot + tagW;
            const float x = std::floor((winW - total) * 0.5f);
            const float menusEnd = ImGui::GetCursorScreenPos().x + EditorTheme::Px(24.0f);
            const float controlsStart = winW - std::floor(ImGui::GetFrameHeight() * 1.6f) * 3.0f - EditorTheme::Px(24.0f);
            if (x > menusEnd && x + total < controlsStart) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float y = std::floor((toolbarH - fontH) * 0.5f);
                dl->AddText(ImVec2(x, y), EditorTheme::U32(playing ? EditorTheme::Accent : EditorTheme::Secondary), title);
                float cx = x + tw;
                if (dirty) {
                    dl->AddCircleFilled(ImVec2(cx + EditorTheme::Px(8.0f), y + fontH * 0.55f), EditorTheme::Px(3.0f),
                                        EditorTheme::U32(EditorTheme::Accent));
                    cx += dot;
                }
                if (playing) {
                    EditorTheme::PushHeading();
                    const float th = ImGui::GetFontSize();
                    const ImVec2 mn(cx + EditorTheme::Px(10.0f), std::floor((toolbarH - th) * 0.5f) - EditorTheme::Px(3.0f));
                    const ImVec2 mx(mn.x + ImGui::CalcTextSize(tag).x + EditorTheme::Px(12.0f), mn.y + th + EditorTheme::Px(6.0f));
                    dl->AddRectFilled(mn, mx, EditorTheme::U32(EditorTheme::AccentWash), EditorTheme::Px(3.0f));
                    dl->AddText(ImVec2(mn.x + EditorTheme::Px(6.0f), mn.y + EditorTheme::Px(3.0f)), EditorTheme::U32(EditorTheme::Accent), tag);
                    EditorTheme::PopFont();
                }
            }
        }

        // Custom window controls, right-aligned â€” the OS title bar is gone (Win32 custom frame,
        // Window.cpp), so minimize / maximize-restore / close live here instead.
        DrawWindowControls(host);

        ImGui::EndMenuBar();
    }
    // A hairline under the title bar, separating it from the dock space.
    ImGui::GetWindowDrawList()->AddLine(ImVec2(0.0f, toolbarH - 0.5f), ImVec2(winW, toolbarH - 0.5f),
                                        EditorTheme::U32(EditorTheme::Hairline));

    // The toolbar's empty space is the window drag handle (the OS caption is gone). True only
    // when the cursor is over this strip and not over any widget / open menu / active drag â€”
    // Window.cpp's WM_NCHITTEST reads this (via the host) to return HTCAPTION.
    const bool dragHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        !ImGui::IsAnyItemHovered() &&
        !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (host.SetTitleBarDragHovered) host.SetTitleBarDragHovered(dragHovered);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

} // namespace EditorModuleToolbar

