#pragma once
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
};

// Returns false (leaving `out` untouched) on a miss — no entry, a stale one, or an unreadable
// file. A miss is normal and never an error.
bool Load(const std::string& sourcePath, const TextureImportSettings& settings, Image& out);

// Writes `img` as the cache entry for this source + settings pair. Failures are silent by
// design: a cache that can't be written costs speed, never correctness.
void Store(const std::string& sourcePath, const TextureImportSettings& settings, const Image& img);

} // namespace TextureCache
