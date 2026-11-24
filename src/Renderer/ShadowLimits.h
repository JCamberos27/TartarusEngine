#pragma once

// How many local lights can cast shadows at once: the spot map's and the point map's layer counts. Here on their own so
// what only needs the numbers (LightGather) doesn't pull in the shadow-map classes.
namespace ShadowLimits {
constexpr int kMaxSpots = 16;
constexpr int kMaxPoints = 12; // GI Phase 1: 8 -> 12, for the house's 20 extra shadowed lamps (the budget is per scene)
}
