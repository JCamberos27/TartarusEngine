#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Persistent, on-disk thumbnail cache (Phase 5 item 9). Backs the in-RAM LRUs that already exist
// for model previews and screenshot thumbnails (EditorLayer::m_ModelThumbnails / its screenshot
// equivalent) so a fresh editor launch doesn't have to re-render every thumbnail from scratch —
// only ever a few new/changed ones per frame, same as today's per-frame render budget.
//
// Cache files live at project/Library/thumbnails/{guid}.png, keyed by AssetDatabase's GUID for
// the source path. #133 — staleness used to be "cache mtime >= source mtime", which missed a
// thumbnail's other inputs (import settings in the .meta, the textures its materials use). Now
// each PNG has a {guid}.png.key sidecar holding a DependencyKey() hash; a cache entry is only
// used when the caller's current key matches.
namespace ThumbnailCache {

// The cache file path for `path`'s thumbnail (registers a GUID via AssetDatabase::EnsureGuid if
// one doesn't exist yet). Empty for a synthetic path (AssetDatabase::IsSynthetic) or an
// unresolvable one.
std::string CacheFilePath(const std::string& path);

// Hash of everything a thumbnail of `path` depends on: the source's mtime, its .meta content
// (import settings), and each of `dependencies`' path + mtime (e.g. a model's texture files).
// A missing file contributes a sentinel, so creating or deleting one also changes the key.
std::uint64_t DependencyKey(const std::string& path, const std::vector<std::string>& dependencies);

// Loads a cached `size` x `size` RGBA8 thumbnail for `path` into `outPixels` (size*size*4 bytes).
// Returns false — leaving outPixels untouched — on any miss: no cache file, a key mismatch
// (something it depends on changed), a dimension mismatch, or a decode failure.
bool Load(const std::string& path, int size, std::uint64_t key, std::vector<unsigned char>& outPixels);

// Writes a `size` x `size` RGBA8 thumbnail for `path` plus its key sidecar. Best-effort: I/O
// failures are logged, not fatal — a missing persistent cache just means the in-RAM render
// happens again next launch, not a correctness problem.
void Save(const std::string& path, int size, std::uint64_t key, const unsigned char* pixels);

// Deletes cached thumbnails whose GUID no longer belongs to any asset (no .meta under the
// project or the engine's shipped assets carries it) and any stray non-thumbnail files in the
// cache folder. Call once at startup, after AssetDatabase::ScanProject().
void PruneOrphans();

} // namespace ThumbnailCache
