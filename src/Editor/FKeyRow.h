#pragma once
#include <imgui.h>
#include <string>

class EditorLayer;

// The F-row: F1-F4 play controls (Shortcuts' play.*), F5-F6 view states that cycle, F7-F8 the player's look and
// clothes (PlayerLook), F9-F12 Play-only spawns (DevSpawner). Every key is one action or one state you flip through - no
// menus. The view states live in EditorSettings (FRowOverlay / FRowLightDebug, plus the editor's own shading mode for
// F5), so they survive Play and restarts. Each press says what it did in a glass pill over the view (Show / Draw).
namespace FKeyRow {

enum Overlay : int { OverlayOff = 0, OverlayPhysics, OverlayAi, OverlayNav, OverlayHitboxes, OverlayCount };
enum LightView : int { LightOff = 0, LightLighting, LightAlbedo, LightRoughMetal, LightComplexity, LightCount };

// F5-F6 (and Shift+F5-F6 back), from the main loop through Shortcuts::TriggeredGlfw, in any mode. Outside Play F6
// skips AI and Hitboxes (they need living NPCs).
void PollViews(EditorLayer& editor, bool playing);

int  CurrentOverlay();
bool PostFxOff();
// The model shader's uDebugView for a view drawn with `shadingIndex` (EditorLayer::ShadingMode): F5's light views (the
// end of its cycle) override Normals / Cascades / Mip.
int DebugViewFor(int shadingIndex);

// The pill: "F10" | "Spider" | "Hunter (Passive)"; `step` of `count` draws the cycle's dots (count 0: none).
// A new press replaces the one showing and pops again.
void Show(const char* key, const std::string& title, const std::string& value, int step = 0, int count = 0);
// A pill that stays up while something loads ("Dressing... 3/7"): `value` changes in place, no pop. Release ends it
// with `value` ("Ready"), and it fades as usual.
void Hold(const char* key, const std::string& title, const std::string& value);
void Release(const std::string& value);
// Draws the pill at the bottom centre of [min, max] (screen px), clear of the toolbars. Inside an ImGui frame.
void Draw(ImVec2 min, ImVec2 max);

} // namespace FKeyRow
