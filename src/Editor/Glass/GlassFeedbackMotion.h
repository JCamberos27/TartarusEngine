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

// --- Motion v2 (shell) ---
// The busy sheen (category 11): a soft white band sweeping the menu bar ("##Toolbar") and the
// status bar ("##StatusBar") while something works. `busy` eases a strength in and out; at rest
// (idle and faded) nothing is drawn and nothing is held. Once a frame, after both bars are drawn.
void DrawChromeSheen(bool busy);

// A progress percentage as rolling digits (Odometer) at `pos`, top-left. Returns its width.
// ProgressPercentWidth is the room it takes at most ("100%").
float DrawProgressPercent(ImGuiID id, ImDrawList* dl, ImVec2 pos, float fraction, ImU32 col);
float ProgressPercentWidth();

// The startup sequence's headline: `title` large, `subtitle` below it, centred on `centre`. Each
// staggers in (StaggeredHeadline), holds, and fades before the sequence ends; drawn in its own
// click-through overlay window over the panels. Call once a frame while the sequence runs.
void DrawStartupHeadline(const char* title, const char* subtitle, ImVec2 centre);

} // namespace Glass
