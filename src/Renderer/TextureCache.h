#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "Texture.h" // TextureImportSettings

// Decoded-pixel cache for imported textures.
//
// The problem it solves: loading a scene decoded every texture in the asset library from PNG on
// the main thread, at startup. For a modest library (22 textures, several 4K) that is ~180
// megapixels of zlib inflate + unfilter — measured at ~4.6s of a ~5.6s cold boot, with the
// window frozen for the duration.
//
// A cache entry stores the pixels exactly as they would be handed to glTexImage2D — already
// decoded, already downsampled to MaxTextureSize — so a warm load is a header check plus one
// sequential read instead of a PNG decode. Mip levels are deliberately NOT stored: regenerating
// them on the GPU via glGenerateMipmap is far cheaper than reading another 33% off disk.
//
// Entries self-invalidate. The header records the source file's size and modification time plus
// a hash of the import settings, so editing the source, changing import settings, or bumping the
// format version all miss the cache and re-bake transparently. Nothing here is authoritative:
// the whole cache directory can be deleted at any time and will simply be rebuilt.
namespace TextureCache {

struct Image {
    int SourceWidth = 0, SourceHeight = 0;  // dimensions as authored, before any downsample
    int Width = 0, Height = 0;              // dimensions actually uploaded
    int Channels = 0;                       // 1, 3 or 4
    std::vector<unsigned char> Pixels;      // tightly packed, Width * Height * Channels
    // #156 - block-compressed entries: GLFormat is the compressed internal format and Pixels
    // holds every mip level's blocks back to back, LevelSizes[i] bytes each (level 0 first).
    // GLFormat 0 = the plain uncompressed layout above.
    uint32_t GLFormat = 0;
    std::vector<uint32_t> LevelSizes;
};

// Returns false (leaving `out` untouched) on a miss — no entry, a stale one, or an unreadable
// file. A miss is normal and never an error.
bool Load(const std::string& sourcePath, const TextureImportSettings& settings, Image& out);

// Writes `img` as the cache entry for this source + settings pair. Failures are silent by
// design: a cache that can't be written costs speed, never correctness.
void Store(const std::string& sourcePath, const TextureImportSettings& settings, const Image& img);

// Hashes import settings exactly the way Store/Load do internally. Exposed so a caller (e.g. a
// startup prune pass) can ask "what would this asset's current settings hash to" without
// duplicating the mix logic.
uint64_t HashSettings(const TextureImportSettings& settings);

// The file Store writes (and Load reads) for this source + settings pair, under Library/Textures.
std::string EntryPath(const std::string& sourcePath, const TextureImportSettings& settings);

// Startup maintenance pass (#226): the cache never evicts on its own, so a texture deleted from
// the project, or reimported under different settings, leaves its old entry on disk forever.
// This walks the cache directory once and deletes any entry that's provably stale:
//   - its source file no longer exists on disk, or
//   - its format version predates the running build's kVersion (already permanently unreadable
//     via Load, e.g. after a decode-behaviour change bumps the version), or
//   - `currentSettingsHash` is supplied, returns a value for that entry's source path, and that
//     value disagrees with the hash stored in the entry's header.
// `currentSettingsHash` returning std::nullopt for a path means "unknown — don't judge this entry
// by settings alone"; pass nullptr to skip the settings check entirely and only prune entries
// whose source is gone or whose version is stale. Reads only each entry's header, never the pixel
// payload, so this is cheap regardless of cache size. Safe to call every launch, and safe to call
// with no cache directory yet (a no-op).
// It then enforces a size cap (#159, CapBytes below): if the surviving entries exceed it, the least
// recently used (Load/Store refresh an entry's timestamp) are deleted until they fit, except the
// entries this process loaded or stored (P-53: the open scene's working set is never evicted). Temp files left by
// an interrupted Store are removed once they're over an hour old.
// `minCapBytes` (0 = the 8 GB default) lowers the cap's floor: tests and the smoke harness's pin check.
struct PruneStats {
    int Entries = 0, Pinned = 0, Evicted = 0; // after the stale sweep
    uint64_t Bytes = 0, PinnedBytes = 0, EvictedBytes = 0;
    uint64_t CapBytes = 0; // the cap actually applied (after the free-disk clamp)
};
PruneStats Prune(const std::function<std::optional<uint64_t>(const std::string& sourcePath)>& currentSettingsHash = nullptr,
                 uint64_t minCapBytes = 0);

// How many entries this process loaded or stored (the pinned set) are no longer on disk: 0 after any Prune, or
// the pinning failed. The smoke harness's pin check.
int SessionEntriesMissing();

// P-53 - the size cap Prune enforces: at least 8 GB, and 1.25x the bytes this session loaded or stored
// (its working set) when that's larger. Those entries are pinned: never evicted, whatever the cap.
uint64_t CapBytes(uint64_t pinnedBytes, uint64_t minCapBytes = 0);

// One valid entry as Prune's size cap sees it.
struct EntryInfo {
    std::string Path;
    uint64_t Bytes = 0;
    int64_t LastUsed = 0; // file time ticks; older goes first
    bool Pinned = false;  // loaded or stored by this process
};
// Which entries (indices) the size cap evicts: least recently used first, never a pinned one, until the
// rest fits CapBytes(pinned bytes) and leaves at least 20 GB of `freeDiskBytes` (UINT64_MAX = unknown).
// `capOut`: the cap it applied, after that clamp.
std::vector<size_t> ChooseEvictions(const std::vector<EntryInfo>& entries, uint64_t freeDiskBytes, uint64_t minCapBytes = 0,
                                    uint64_t* capOut = nullptr);

// Total bytes and entry count currently on disk (#159) - a directory walk, so callers showing
// it in the UI should cache the result rather than calling it every frame.
struct Usage {
    uint64_t Bytes = 0;
    int Entries = 0;
};
Usage DiskUsage();

// Deletes every cache entry (Preferences' "Clear texture cache", #159). Textures already on the
// GPU are unaffected; each is re-baked from its source the next time it loads. In-flight temp
// files from another running editor instance are left alone. Returns the number of entries removed.
int Clear();

} // namespace TextureCache
