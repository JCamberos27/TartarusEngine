#pragma once
#include <string>
#include <vector>

// Persistent, on-disk thumbnail cache (Phase 5 item 9). Backs the in-RAM LRUs that already exist
// for model previews and screenshot thumbnails (EditorLayer::m_ModelThumbnails / its screenshot
// equivalent) so a fresh editor launch doesn't have to re-render every thumbnail from scratch —
// only ever a few new/changed ones per frame, same as today's per-frame render budget.
//
// Cache files live at project/Library/thumbnails/{guid}.png, keyed by AssetDatabase's GUID for
// the source path. Staleness is tracked via the cache file's own last-write-time rather than a
// separate metadata file: Save() stamps the cache file's mtime to the source's at write time, so
// Load() can tell a cache is current by checking cache mtime >= source mtime. A re-import or edit
// that bumps the source's mtime makes the existing cache file read as stale (and it's silently
// overwritten the next time Save() runs) without needing any cleanup pass.
namespace ThumbnailCache {

// The cache file path for `path`'s thumbnail (registers a GUID via AssetDatabase::EnsureGuid if
// one doesn't exist yet). Empty for a synthetic path (AssetDatabase::IsSynthetic) or an
// unresolvable one.
std::string CacheFilePath(const std::string& path);

// Loads a cached `size` x `size` RGBA8 thumbnail for `path` into `outPixels` (size*size*4 bytes).
// Returns false — leaving outPixels untouched — on any miss: no cache file, a stale one (source
// modified since it was written), a dimension mismatch, or a decode failure.
bool Load(const std::string& path, int size, std::vector<unsigned char>& outPixels);

// Writes a `size` x `size` RGBA8 thumbnail for `path` to its cache file, stamping the cache
// file's mtime to the source's so a later Load() can detect staleness. Best-effort: I/O failures
// are logged, not fatal — a missing persistent cache just means the in-RAM render happens again
// next launch, not a correctness problem.
void Save(const std::string& path, int size, const unsigned char* pixels);

} // namespace ThumbnailCache
