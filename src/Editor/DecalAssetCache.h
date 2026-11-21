#pragma once

// .decal files as the editor reads them, re-read when the file changes on disk. The Asset Browser
// asks for every visible decal tile each frame, so a lookup must be cheap and the disk check rare:
//   - the caller's own spelling of the path is remembered, so a repeat ask is one hash lookup (no
//     path resolve, no lower-cased key, no allocation);
//   - the resolved albedo path is kept with the entry (the tile's thumbnail source);
//   - each entry's write time is looked at again once it is kMaxAge old, but at most kStatsPerFrame
//     such checks run in one frame: tiles that all loaded together don't all stat in the same frame
//     a second later (the rest wait a frame or two, showing what they already have).
// Editor UI thread only.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

struct DecalAsset;

namespace DecalAssetCache {

using Clock = std::chrono::steady_clock;
inline constexpr Clock::duration kMaxAge = std::chrono::seconds(1);
inline constexpr int kStatsPerFrame = 4;

struct Hit {
    std::shared_ptr<DecalAsset> Asset;      // null only for an empty path
    const std::string* AlbedoAbs = nullptr; // absolute albedo path, "" when none / missing; never null with an Asset
};

// `frame` is any number that changes once a frame (the stat budget resets when it changes). `force`
// re-reads the file now (after the editor wrote it) and is not budgeted.
Hit Get(const std::string& path, std::uint64_t frame, Clock::time_point now = Clock::now(), bool force = false);

void Clear();
std::uint64_t DiskStats(); // write-time checks made so far (tests, profiling)
std::uint64_t Loads();     // .decal reads made so far

} // namespace DecalAssetCache
