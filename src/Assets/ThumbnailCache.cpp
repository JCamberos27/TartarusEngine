#include "ThumbnailCache.h"
#include "AssetDatabase.h"
#include "ProjectPaths.h"
#include "EnginePaths.h"
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
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace ThumbnailCache {
namespace {
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime  = 1099511628211ull;

void HashBytes(std::uint64_t& h, const void* data, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= kFnvPrime; }
}
void HashString(std::uint64_t& h, const std::string& s) {
    HashBytes(h, s.data(), s.size());
    HashBytes(h, "\0", 1); // separator, so ("ab","c") != ("a","bc")
}
void HashFileTime(std::uint64_t& h, const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    const long long ticks = ec ? -1ll : (long long)t.time_since_epoch().count();
    HashBytes(h, &ticks, sizeof(ticks));
}

std::string ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string KeyPath(const std::string& cachePath) { return cachePath + ".key"; }

std::string KeyToString(std::uint64_t key) {
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)key);
    return buf;
}

// GUIDs of every .meta under `root` (skipping Library/, which holds caches, not assets).
void CollectMetaGuids(const std::filesystem::path& root, std::unordered_set<std::string>& out) {
    namespace fs = std::filesystem;
    static const std::regex kGuid("\"guid\"\\s*:\\s*\"([0-9a-fA-F]{16})\"");
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_directory(ec) && it.depth() == 0 && it->path().filename() == "Library") {
            it.disable_recursion_pending();
            continue;
        }
        if (it->path().extension() != ".meta") continue;
        std::smatch m;
        const std::string text = ReadAll(it->path().string());
        if (std::regex_search(text, m, kGuid)) {
            std::string g = m[1].str();
            for (char& c : g) c = (char)std::tolower((unsigned char)c);
            out.insert(g);
        }
    }
}
} // namespace

std::string CacheFilePath(const std::string& path) {
    if (path.empty() || AssetDatabase::IsSynthetic(path)) return {};
    const AssetGuid guid = AssetDatabase::EnsureGuid(path);
    if (!guid.IsValid()) return {};
    return ProjectPaths::Resolve("Library/thumbnails/" + guid.ToString() + ".png");
}

std::uint64_t DependencyKey(const std::string& path, const std::vector<std::string>& dependencies) {
    std::uint64_t h = kFnvOffset;
    HashString(h, "thumb-v2");
    HashFileTime(h, path);
    HashString(h, ReadAll(path + ".meta")); // import settings live here
    for (const std::string& dep : dependencies) {
        if (dep.empty()) continue;
        HashString(h, dep);
        HashFileTime(h, dep);
    }
    return h;
}

bool Load(const std::string& path, int size, std::uint64_t key, std::vector<unsigned char>& outPixels) {
    const std::string cachePath = CacheFilePath(path);
    if (cachePath.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec) || ec) return false;

    // No sidecar (a pre-#133 cache entry) or a different key: something the thumbnail depends on
    // changed since it was rendered, so it's a miss and gets re-rendered and overwritten.
    std::string stored = ReadAll(KeyPath(cachePath));
    while (!stored.empty() && std::isspace((unsigned char)stored.back())) stored.pop_back();
    if (stored != KeyToString(key)) return false;

    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(cachePath.c_str(), &w, &h, &channels, 4);
    if (!data) return false;
    if (w != size || h != size) { stbi_image_free(data); return false; }

    outPixels.assign(data, data + (size_t)size * size * 4);
    stbi_image_free(data);
    return true;
}

void Save(const std::string& path, int size, std::uint64_t key, const unsigned char* pixels) {
    const std::string cachePath = CacheFilePath(path);
    if (cachePath.empty() || !pixels) return;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(cachePath).parent_path(), ec);

    stbi_write_png_compression_level = 6;
    if (!stbi_write_png(cachePath.c_str(), size, size, 4, pixels, size * 4)) {
        Log::Warn("ThumbnailCache: couldn't write " + ProjectPaths::Relativize(cachePath));
        return;
    }
    std::ofstream k(KeyPath(cachePath), std::ios::trunc);
    k << KeyToString(key);
}

void PruneOrphans() {
    namespace fs = std::filesystem;
    const fs::path dir = ProjectPaths::Resolve("Library/thumbnails");
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;

    std::unordered_set<std::string> live;
    CollectMetaGuids(ProjectPaths::Root(), live);
    CollectMetaGuids(EnginePaths::Resolve("assets"), live);

    int removed = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        // "{16 hex}.png" and its "{16 hex}.png.key" sidecar are cache entries; anything else
        // (e.g. a .meta an older ScanProject created for a thumbnail) is junk.
        const bool isPng = name.size() == 20 && name.compare(16, 4, ".png") == 0;
        const bool isKey = name.size() == 24 && name.compare(16, 8, ".png.key") == 0;
        std::string guid = (isPng || isKey) ? name.substr(0, 16) : std::string();
        for (char& c : guid) c = (char)std::tolower((unsigned char)c);
        if ((isPng || isKey) && live.count(guid)) continue;
        if (fs::remove(entry.path(), ec)) ++removed;
    }
    if (removed > 0) Log::Info("ThumbnailCache: pruned " + std::to_string(removed) + " orphaned file(s).");
}

} // namespace ThumbnailCache
