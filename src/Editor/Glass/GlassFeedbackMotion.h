#pragma once
// Feedback moments (category 9 in GlassTokens.h). Host-only, every theme.
//
//   * Save: the status bar's unsaved dot gives way to a check that draws on, holds and fades, an
//     authored keyframe clip (ImAnim iam_clip) with two channels, draw and alpha.
//   * Busy: an indeterminate progress bar's segment eases across its track with its width and
//     brightness drifting on smooth noise (stateless; Glass::ImAnimContext::Noise).
//   * Text: a headline whose characters fade and rise in one after another (iam_text_stagger),
//     used only on empty states and the About title.

#include <imgui.h>

namespace Glass {

void PlaySaveCheck();
bool SaveCheckPlaying();
// The check inside the square at `pos` of side `size`, at the clip's current draw / alpha.
void DrawSaveCheck(ImDrawList* dl, ImVec2 pos, float size, ImVec4 col);

// The indeterminate bar at `timeSeconds`: `t` 0..1 position of the segment's travel, `segFrac` its
// width as a fraction of the track, `bright` its alpha.
void BusyShimmer(float timeSeconds, float& t, float& segFrac, float& bright);

// A staggered headline at `pos` (top-left) in the current window's draw list. `id` names the
// headline: it plays when it first appears (not drawn last frame) and is static after that.
// Returns the text's size so the caller can lay out around it.
ImVec2 StaggeredHeadline(ImGuiID id, const char* text, ImVec2 pos, ImU32 col);

} // namespace Glass
