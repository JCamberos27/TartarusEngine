#include "PngWrite.h"

#include <filesystem>
#include <fstream>
#include <mutex>

// stb_image_write's one implementation in the engine.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996) // stb_image_write.h's own sprintf() use, not this file's
#endif
#include <stb_image_write.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace PngWrite {
namespace {

std::mutex& Lock() {
    static std::mutex m;
    return m;
}

void Append(void* context, void* data, int size) { static_cast<std::string*>(context)->append(static_cast<const char*>(data), (size_t)size); }

int ClampLevel(int level) { return level < 0 ? 0 : level > 9 ? 9 : level; }

} // namespace

bool Encode(std::string& out, int width, int height, int comp, const void* pixels, int stride, int level) {
    if (!pixels || width <= 0 || height <= 0 || comp < 1 || comp > 4) return false;
    const size_t before = out.size();
    std::lock_guard<std::mutex> lock(Lock());
    stbi_write_png_compression_level = ClampLevel(level);
    const int ok = stbi_write_png_to_func(Append, &out, width, height, comp, pixels, stride);
    stbi_write_png_compression_level = kDefaultLevel;
    return ok != 0 && out.size() > before;
}

bool ToFile(const std::string& path, int width, int height, int comp, const void* pixels, int stride, int level) {
    std::string png;
    if (!Encode(png, width, height, comp, pixels, stride, level)) return false;
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    return f.write(png.data(), (std::streamsize)png.size()) && f.flush();
}

} // namespace PngWrite
