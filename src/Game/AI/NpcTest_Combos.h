#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

// --npc-test combos' plain helpers (NpcTest_Combos.cpp), for the unit tests.
namespace Combos {
// `id` matches `pattern`: '*' stands for any run of characters (none too); everything else as is.
bool Match(const std::string& id, const std::string& pattern);
// The cells' ids (weapon/action/state), in the order they run: `weapons` is TARTARUS_COMBOS (ak, 870, rev, comma-separated;
// empty: the AK), `cells` TARTARUS_COMBOS_CELLS (comma-separated patterns; empty: every cell). An unknown weapon is skipped.
std::vector<std::string> CellIds(const std::string& weapons, const std::string& cells);
// A view-space point (the view looking down -Z) inside a vertical FOV of `vfovDeg` at `aspect`, with `margin` (a share of the
// half-extent) of slack past each edge. Behind or at the eye: false.
bool OnScreen(const glm::vec3& view, float vfovDeg, float aspect, float margin);
} // namespace Combos
