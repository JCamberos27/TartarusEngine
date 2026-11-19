#pragma once
// PNG encoding for the whole engine. stb_image_write keeps its compression level in one global
// (stbi_write_png_compression_level), so two threads encoding at once (a thumbnail worker, a
// graffiti bake, an impostor bake) could each change the other's level mid-write. Every PNG write
// goes through here instead: the level is set and the encode runs under one lock.
#include <string>

namespace PngWrite {

constexpr int kDefaultLevel = 8; // stb's own default

// Encodes `pixels` (`width` x `height`, `comp` 1-4 bytes per pixel, rows `stride` bytes apart) and
// appends the PNG to `out`. False when nothing was written.
bool Encode(std::string& out, int width, int height, int comp, const void* pixels, int stride, int level = kDefaultLevel);

// The same, written to `path` (UTF-8). Not atomic: callers that need that use Encode and AtomicFile.
bool ToFile(const std::string& path, int width, int height, int comp, const void* pixels, int stride, int level = kDefaultLevel);

} // namespace PngWrite
