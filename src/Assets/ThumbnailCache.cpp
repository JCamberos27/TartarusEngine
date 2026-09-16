#include "ThumbnailCache.h"
#include "AssetDatabase.h"
#include "ProjectPaths.h"
#include "Log.h"

#include "stb_image.h"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996) // stb_image_write.h's own sprintf() use, not this file's
#endif
#include <stb_image_write.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <filesystem>

namespace ThumbnailCache {

std::string CacheFilePath(const std::string& path) {
    if (path.empty() || AssetDatabase::IsSynthetic(path)) return {};
    const AssetGuid guid = AssetDatabase::EnsureGuid(path);
    if (!guid.IsValid()) return {};
    return ProjectPaths::Resolve("Library/thumbnails/" + guid.ToString() + ".png");
}

bool Load(const std::string& path, int size, std::vector<unsigned char>& outPixels) {
    const std::string cachePath = CacheFilePath(path);
    if (cachePath.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec) || ec) return false;

    // Stale if the source was modified at or after the cache was written — the source's own
    // mtime may not exist (a deleted/renamed file mid-scan), in which case treat it as a miss
    // rather than trusting a cache that might describe content that's gone.
    const auto cacheTime = std::filesystem::last_write_time(cachePath, ec);
    if (ec) return false;
    const auto srcTime = std::filesystem::last_write_time(path, ec);
    if (ec || cacheTime < srcTime) return false;

    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(cachePath.c_str(), &w, &h, &channels, 4);
    if (!data) return false;
    if (w != size || h != size) { stbi_image_free(data); return false; }

    outPixels.assign(data, data + (size_t)size * size * 4);
    stbi_image_free(data);
    return true;
}

void Save(const std::string& path, int size, const unsigned char* pixels) {
    const std::string cachePath = CacheFilePath(path);
    if (cachePath.empty() || !pixels) return;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(cachePath).parent_path(), ec);

    stbi_write_png_compression_level = 6;
    if (!stbi_write_png(cachePath.c_str(), size, size, 4, pixels, size * 4)) {
        Log::Warn("ThumbnailCache: couldn't write " + ProjectPaths::Relativize(cachePath));
        return;
    }

    const auto srcTime = std::filesystem::last_write_time(path, ec);
    if (!ec) std::filesystem::last_write_time(cachePath, srcTime, ec);
}

} // namespace ThumbnailCache
