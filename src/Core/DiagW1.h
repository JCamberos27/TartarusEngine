#pragma once
// TEMPORARY sizing counters (diag branch only, never merged).
#include <cstdint>
namespace DiagW1 {
inline bool InFpIk = false;
inline std::uint64_t FpFrames = 0, Resolves = 0, ResolveNodes = 0, Applies = 0, RotateChains = 0, RotatePieces = 0,
                     ShoulderPairs = 0, FpModels = 0;
inline double ResolveMs = 0.0;
inline std::uint64_t SunCalls = 0, Casting = 0, PadHit[3] = {0, 0, 0}, PadHitXY[3] = {0, 0, 0}, PadCasting[3] = {0, 0, 0};
} // namespace DiagW1
