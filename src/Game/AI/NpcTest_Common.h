#pragma once
// Helpers the NPC test scenarios share (NpcTest*.cpp).
#include <algorithm>
#include <cmath>
#include <vector>

namespace NpcTestCommon {

// The p-th percentile (0..1) of `v`, nearest rank; 0 for none.
inline float Percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)std::lround(p * (float)(v.size() - 1)))];
}

} // namespace NpcTestCommon
