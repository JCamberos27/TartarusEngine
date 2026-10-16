#pragma once
// Motion for the editor's dock space (category 6 in GlassTokens.h). Host-only, every theme.
//
//   * The selected tab's overline glides and resizes from tab to tab instead of jumping. ImGui's
//     own overline is switched off for the dock space's tab bars only (BeginDockTabMotion pushes
//     TabBarOverlineSize 0 around DockSpace()), and EndDockTabMotion redraws it at the glided
//     position, with the exact colour and shape ImGui would have used. Tab bars inside panels
//     (Script IDE, Inspector field tabs) keep ImGui's line.
//   * A docked panel that takes focus catches the light: its edge glints and cools (RimGlint).
//
// Both are Glass::Motion() entries that exist only while moving.

#include <imgui.h>

namespace Glass {

// Wrap the editor's ImGui::DockSpace call.
float BeginDockTabMotion();
void  EndDockTabMotion(ImGuiID dockspaceId, float overlineSize);

// Once a frame, after every panel has been submitted.
void DockFocusRim(ImGuiID dockspaceId);

// The play-mode frame (category 7): a 1 px white inner border on the Scene and Game views that
// fades in on Play, breathes slightly while playing and dims while paused. Once a frame, after
// every panel has been submitted.
void DrawPlayFrame(bool playing, bool paused);

} // namespace Glass
