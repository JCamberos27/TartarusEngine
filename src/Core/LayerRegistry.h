#pragma once
#include <string>

// Human-readable names for the LayerComponent slots (#236 A1 — "layers-lite").
//
// Project-scoped: persisted to project/layers.json alongside the source, NOT editor_prefs.json.
// Layer names are content — a scene authored with "Enemies" meaning slot 3 is meaningless
// without the same slot->name mapping — so they version-control with the project, the way
// Unity's TagManager.asset does. Per-user viewport state (which layers are hidden / pick-locked)
// stays in editor_prefs.json instead; see EditorSettings.
namespace LayerRegistry {

// Slots 0..kCount-1. Deliberately small to start; the on-disk format is a plain string array,
// so raising this later doesn't invalidate any saved scene or layers.json.
constexpr int kCount = 8;

// True for a real slot index. Slot 0 is always "Default" and SetName() ignores it.
bool IsValid(int layer);
bool IsRenamable(int layer);

// The raw authored name for a slot: empty string when the user hasn't named it (slots 1..7
// start empty). Slot 0 always returns "Default".
const std::string& Name(int layer);

// A label that is never empty: the authored name if set, else "Default" for slot 0 or
// "Layer N" for an unnamed slot. Use this for combos / menus.
std::string DisplayName(int layer);

// Trim, strip control characters, clamp length; slot 0 and out-of-range indices are ignored.
// Does not persist — the caller decides when to Save().
void SetName(int layer, const std::string& name);

// project/layers.json. A missing or unparseable file leaves the defaults in place (slot 0
// "Default", the rest empty) and is not treated as an error.
void Load();
void Save();

} // namespace LayerRegistry
