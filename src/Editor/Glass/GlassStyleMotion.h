#pragma once
// Category 10 of the motion table: the whole ImGuiStyle in motion.
//
//   - A theme switch (Tartarus <-> Black Glass) eases every colour, size and radius from the old
//     style to the new one (ImAnim's iam_style_register / iam_style_blend_to, OKLAB colours).
//   - Moods layer on top of whichever theme is active, all monochrome:
//       play     panels recede: their black deepens and text drops toward secondary,
//       focus    the same, gentler, while the Scene or Game view has focus,
//       prefab   the chrome lifts (menu bar, tabs, headers a few steps whiter, borders brighter).
//
// At rest nothing runs: the style already holds the result, the mood weights have no motion entry
// and no style is rewritten. The two registered snapshots (from / to) are the one fixed cost, like
// clip definitions, and are only (re)registered when a theme actually changes.
//
// Host-only (EditorLayer.cpp, EditorLayer_Glass.cpp, the unit tests).

#include <imgui.h>

namespace Glass {

// ApplyThemeStyle hands over the style it replaced (`before`) and the one it built (ImGui's
// current style). `animate`: ease from before to after (a theme switch); otherwise just adopt the
// new base (a UI-scale change, startup).
void StyleMotionThemeChanged(const ImGuiStyle& before, bool animate);

// Once per frame, after ImGui::NewFrame. Eases the moods toward these states and rewrites the
// style only while something is moving.
void StyleMotionFrame(bool playing, bool prefabMode, bool viewportFocused, bool glassTheme);

// The mood a style is derived with (tests): 0..1 each.
struct StyleMoods { float play = 0.0f, focus = 0.0f, prefab = 0.0f; };
void ApplyStyleMoods(ImGuiStyle& style, const StyleMoods& moods, bool glassTheme);
StyleMoods CurrentStyleMoods();
bool StyleMotionActive(); // a theme tween or a mood is in flight

} // namespace Glass
