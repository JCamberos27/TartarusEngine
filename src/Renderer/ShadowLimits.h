#pragma once

// How many local lights can cast shadows at once: the spot map's and the point map's layer counts. Here on their own so
// what only needs the numbers (LightGather) doesn't pull in the shadow-map classes.
namespace ShadowLimits {
constexpr int kMaxSpots = 16;
constexpr int kMaxPoints = 8;
}
