#include "TextureCache.h"
#include "AssetDatabase.h"
#include "ProjectPaths.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace TextureCache {
namespace {

constexpr char kMagic[4] = {'T', 'T', 'E', 'X'};
// Bump to invalidate every existing entry after a format or decode-behaviour change.
// v2 (#207): downsample switched from nearest-neighbor to a box filter, so cached pixels baked
// with the old point-sample must be discarded and re-baked.
// v3 (#333 PR3): entry filename changed from FNV(absolutePath) to GUID, so old path-keyed
// entries are unreachable and treated as stale by Prune (version mismatch).
constexpr uint32_t kVersion = 3;

std::string CacheDir() {
    static const std::string dir = ProjectPaths::Resolve("Library/Textures");
    return dir;
}

// Cache entries are keyed by the asset's GUID (16 hex chars) when registered in AssetDatabase,
// falling back to an FNV hash of the absolute path for unregistered sources. The source path is
// also written into the header and compared on load, so even with GUID keying a collision (two
// different GUIDs happening to share a prefix) would produce a miss rather than wrong pixels.
std::string EntryPath(const std::string& sourcePath) {
    AssetGuid guid = AssetDatabase::GuidForPath(sourcePath);
    std::string stem;
    if (guid.IsValid()) {
        stem = guid.ToString();
    } else {
        std::error_code ec;
        std::filesystem::path abs = std::filesystem::absolute(sourcePath, ec);
        std::string key = ec ? sourcePath : abs.lexically_normal().string();
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : key) {
            h ^= (uint64_t)c;
            h *= 1099511628211ull;
        }
        char buf[17];
        snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
        stem = buf;
    }
    return (std::filesystem::path(CacheDir()) / (stem + ".ttex")).string();
}

bool SourceStamp(const std::string& path, uint64_t& outSize, uint64_t& outMtime) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) return false;
    auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    outSize = (uint64_t)size;
    outMtime = (uint64_t)mtime.time_since_epoch().count();
    return true;
}

template <typename T>
void Write(std::ofstream& f, const T& v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <typename T>
bool Read(std::ifstream& f, T& v) {
    return (bool)f.read(reinterpret_cast<char*>(&v), sizeof(T));
}

} // namespace

// Import settings that change the stored PIXELS. Filtering and wrap mode are deliberately
// excluded — they're sampler state applied at upload, so changing them must not throw away a
// perfectly good decode.
uint64_t HashSettings(const TextureImportSettings& s) {
    uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    mix((uint64_t)s.MaxTextureSize);
    mix(s.IsSRGB ? 1u : 0u); // affects the GL internal format chosen for these pixels
    return h;
}

bool Load(const std::string& sourcePath, const TextureImportSettings& settings, Image& out) {
    uint64_t srcSize = 0, srcMtime = 0;
    if (!SourceStamp(sourcePath, srcSize, srcMtime)) return false; // source gone: nothing to validate against

    std::ifstream f(EntryPath(sourcePath), std::ios::binary);
    if (!f) return false;

    char magic[4];
    uint32_t version = 0;
    if (!f.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return false;
    if (!Read(f, version) || version != kVersion) return false;

    uint64_t size = 0, mtime = 0, hash = 0;
    if (!Read(f, size) || !Read(f, mtime) || !Read(f, hash)) return false;
    if (size != srcSize || mtime != srcMtime || hash != HashSettings(settings)) return false;

    // The source path is stored too, so a (vanishingly unlikely) filename-hash collision is
    // caught here and treated as a miss.
    uint32_t pathLen = 0;
    if (!Read(f, pathLen) || pathLen > 4096) return false;
    std::string storedPath(pathLen, '\0');
    if (pathLen && !f.read(storedPath.data(), pathLen)) return false;

    int32_t sw = 0, sh = 0, w = 0, h = 0, ch = 0;
    if (!Read(f, sw) || !Read(f, sh) || !Read(f, w) || !Read(f, h) || !Read(f, ch)) return false;
    if (w <= 0 || h <= 0 || ch < 1 || ch > 4) return false;

    const size_t bytes = (size_t)w * (size_t)h * (size_t)ch;
    std::vector<unsigned char> pixels(bytes);
    if (!f.read(reinterpret_cast<char*>(pixels.data()), (std::streamsize)bytes)) return false;

    out.SourceWidth = sw;
    out.SourceHeight = sh;
    out.Width = w;
    out.Height = h;
    out.Channels = ch;
    out.Pixels = std::move(pixels);
    return true;
}

void Store(const std::string& sourcePath, const TextureImportSettings& settings, const Image& img) {
    if (img.Pixels.empty() || img.Width <= 0 || img.Height <= 0) return;

    uint64_t srcSize = 0, srcMtime = 0;
    if (!SourceStamp(sourcePath, srcSize, srcMtime)) return;

    std::error_code ec;
    std::filesystem::create_directories(CacheDir(), ec);
    if (ec) return;

    // Write to a temporary then rename, so a crash or a full disk mid-write can't leave a
    // truncated entry that would later be read back as a valid one.
    const std::string finalPath = EntryPath(sourcePath);
    const std::string tempPath = finalPath + ".tmp";
    {
        std::ofstream f(tempPath, std::ios::binary | std::ios::trunc);
        if (!f) return;

        f.write(kMagic, 4);
        Write(f, kVersion);
        Write(f, srcSize);
        Write(f, srcMtime);
        Write(f, HashSettings(settings));

        std::string abs = std::filesystem::absolute(sourcePath, ec).lexically_normal().string();
        if (ec) abs = sourcePath;
        Write(f, (uint32_t)abs.size());
        f.write(abs.data(), (std::streamsize)abs.size());

        Write(f, (int32_t)img.SourceWidth);
        Write(f, (int32_t)img.SourceHeight);
        Write(f, (int32_t)img.Width);
        Write(f, (int32_t)img.Height);
        Write(f, (int32_t)img.Channels);
        f.write(reinterpret_cast<const char*>(img.Pixels.data()), (std::streamsize)img.Pixels.size());
        if (!f) {
            f.close();
            std::filesystem::remove(tempPath, ec);
            return;
        }
    }
    std::filesystem::rename(tempPath, finalPath, ec);
    if (ec) std::filesystem::remove(tempPath, ec);
}

void Prune(const std::function<std::optional<uint64_t>(const std::string& sourcePath)>& currentSettingsHash) {
    std::error_code ec;
    std::filesystem::directory_iterator dir(CacheDir(), ec);
    if (ec) return; // no cache directory yet (or can't be listed): nothing to prune

    for (const auto& entry : std::filesystem::directory_iterator(CacheDir(), ec)) {
        if (ec) break;
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || fileEc) continue;

        const std::filesystem::path path = entry.path();
        if (path.extension() != ".ttex") continue; // ignore ".tmp" in-flight writes and stray files

        // Read the header ONLY — magic, version, source stamp, settings hash, source path — and
        // stop before the pixel payload that follows. Never decodes or even reads the pixels.
        std::ifstream f(path, std::ios::binary);
        bool stale = !f; // unreadable file: already useless, safe to remove

        std::string storedPath;
        uint64_t storedHash = 0;
        if (!stale) {
            char magic[4];
            uint32_t version = 0;
            uint64_t size = 0, mtime = 0;
            uint32_t pathLen = 0;
            if (!f.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) {
                stale = true; // not one of our files (or corrupt): safe to remove
            } else if (!Read(f, version) || version != kVersion) {
                stale = true; // predates the current format; Load() would never accept it either
            } else if (!Read(f, size) || !Read(f, mtime) || !Read(f, storedHash)) {
                stale = true; // truncated header
            } else if (!Read(f, pathLen) || pathLen > 4096) {
                stale = true; // truncated/corrupt header
            } else {
                storedPath.resize(pathLen);
                if (pathLen && !f.read(storedPath.data(), pathLen)) stale = true; // truncated header
            }
        }
        f.close(); // done with the header either way; the pixel payload is never touched

        if (!stale) {
            std::error_code existsEc;
            if (!std::filesystem::exists(storedPath, existsEc) || existsEc) {
                stale = true; // source texture was deleted from the project
            } else if (currentSettingsHash) {
                std::optional<uint64_t> current = currentSettingsHash(storedPath);
                if (current.has_value() && *current != storedHash) {
                    stale = true; // settings changed since this entry was baked
                }
            }
        }

        if (stale) std::filesystem::remove(path, ec);
    }
}

} // namespace TextureCache
