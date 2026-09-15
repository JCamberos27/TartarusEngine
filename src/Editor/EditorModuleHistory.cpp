// The editor's Undo History panel, living inside TartarusEditor.dll so its chrome hot-reloads
// while the editor stays open with its scene loaded. The "History" heading, then every recorded
// change oldest-to-newest with the current position highlighted.
//
// Phase 3 item 8 (audit #5) — this used to be a compact, transparent HUD pinned to the Scene
// viewport's bottom-right corner (mirroring the old Stats HUD at top-left), forced there every
// frame and height-capped so a long history never climbed into the top-right nav cluster. It's a
// real dockable panel now, same as Hierarchy/Inspector/Console — draggable into any dock node,
// its position persisted in imgui.ini like theirs, freely resizable instead of height-ceilinged
// against the nav gizmo.
//
// Thin slice (issue #229): the row list itself — the undo/redo stacks, the click-to-jump
// (JumpToUndo/RedoEntry), the per-row tooltips — stays host code, drawn into this window through
// host.DrawHistoryListBody(). It used to steer its text between white-on-dark and black-on-light
// with an async GPU luminance readback the host drove; Defect #54 (Phase 1) replaced that with a
// fixed opaque plate for the corner-HUD era. As an ordinary docked panel now, it just follows the
// theme's normal WindowBg/Text like every other panel — no more bespoke plate.

#include "EditorModuleAPI.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

namespace EditorModuleHistory {

void Draw(const EditorModuleHostAPI& host) {
    const bool shown = host.GetShowHistory && host.GetShowHistory();
    if (!shown) return;

    bool open = shown;
    if (ImGui::Begin(ICON_FA_CLOCK_ROTATE_LEFT "  History", &open)) {
        // #156 — a one-line explanation instead of a persistent "(?)" glyph or a title-bar
        // tooltip (the title bar is ImGui's own chrome now, not a Text() item this code draws,
        // so there's nothing of ours to hang a tooltip off there).
        ImGui::TextDisabled("Click any entry to jump straight there.");
        ImGui::Separator();

        if (host.DrawHistoryListBody) host.DrawHistoryListBody();
    }
    ImGui::End();

    if (!open && host.SetShowHistory) host.SetShowHistory(false); // the title-bar X
}

} // namespace EditorModuleHistory
