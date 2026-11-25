#include "TextureCache.h"
#include "AssetDatabase.h"
#include "Core/Paths.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <numeric>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <process.h> // _getpid
#define TT_GETPID _getpid
#else
#include <unistd.h>
#define TT_GETPID getpid
#endif

namespace TextureCache {
namespace {

// #159: entries are raw pixels (a 4K RGBA texture is 64 MB), so the directory needs a ceiling.
// Prune() evicts least-recently-used entries past it. A cap below a project's working set makes
// every open re-decode (and re-encode BCn) the textures evicted last time: the Forest's 4.47 GB
// against the old fixed 4 GB cap made its 2nd and 3rd warm opens 45 and 71 s instead of ~10 s.
// So the cap is at least this, grows with what this session uses, and never evicts that.
constexpr uint64_t kGiB = 1024ull * 1024 * 1024;
constexpr uint64_t kMinCapBytes = 8 * kGiB;
// However large the working set, pruning keeps the disk at least this free (pinned entries excepted).
constexpr uint64_t kKeepFreeDiskBytes = 20 * kGiB;
// An in-flight ".tmp" older than this was abandoned by a crash, not being written right now.
constexpr auto kAbandonedTempAge = std::chrono::hours(1);

// How the source path is written into, and compared against, an entry's header.
std::string NormalizedSourcePath(const std::string& sourcePath) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(sourcePath, ec);
    return ec ? sourcePath : abs.lexically_normal().string();
}

constexpr char kMagic[4] = {'T', 'T', 'E', 'X'};
// Bump to invalidate every existing entry after a format or decode-behaviour change.
// v2 (#207): downsample switched from nearest-neighbor to a box filter, so cached pixels baked
// with the old point-sample must be discarded and re-baked.
// v3 (#333 PR3): entry filename changed from FNV(absolutePath) to GUID, so old path-keyed
// entries are unreachable and treated as stale by Prune (version mismatch).
// v4 (#156): the Max Size downsample now averages sRGB textures in linear space, alpha-weighted.
// v5 (#156): header gains the compressed format + mip level sizes (BCn entries).
// v6: one entry per (texture, settings) - the filename carries the settings hash. A texture loaded two ways (a model's
// own material at the defaults, a material asset at its .meta's) used to share one entry and evict each other every
// session: both re-decoded and, the compressed one, re-encoded on every Play (~7 s for one 4K weapon set).
// v7: a compressed normal map (TextureType NormalMap) is BC5 (x, y; z rebuilt in the shader) with renormalized mips, not BC1.
constexpr uint32_t kVersion = 7;
// A variant whose settings aren't the texture's current .meta ones is still wanted (a model's own material loads it at
// the defaults); it goes only once nothing has used it for this long.
constexpr auto kUnusedVariantAge = std::chrono::hours(24 * 14);

std::string CacheDir() {
    static const std::string dir = ProjectPaths::Resolve("Library/Textures");
    return dir;
}

// Entries this process loaded or stored (file names): its working set, which Prune never evicts. Plus the ones
// requested and not loaded yet (Pin): pinned too, but not expected on disk (SessionEntriesMissing).
// Store runs on the texture loader threads too, so it's locked.
std::mutex s_SessionMutex;
std::unordered_set<std::string> s_SessionEntries;
std::unordered_set<std::string> s_RequestedEntries;

void MarkUsedThisSession(const std::string& entryPath) {
    const std::string name = std::filesystem::path(entryPath).filename().string();
    std::lock_guard<std::mutex> lock(s_SessionMutex);
    s_SessionEntries.insert(name);
}

bool UsedThisSession(const std::filesystem::path& entryPath) {
    const std::string name = entryPath.filename().string();
    std::lock_guard<std::mutex> lock(s_SessionMutex);
    return s_SessionEntries.count(name) != 0 || s_RequestedEntries.count(name) != 0;
}

} // namespace

// Cache entries are keyed by the asset's GUID (16 hex chars) when registered in AssetDatabase,
// falling back to an FNV hash of the absolute path for unregistered sources. The source path is
// also written into the header and compared on load, so even with GUID keying a collision (two
// different GUIDs happening to share a prefix) would produce a miss rather than wrong pixels.
std::string EntryPath(const std::string& sourcePath, const TextureImportSettings& settings) {
    AssetGuid guid = AssetDatabase::GuidForPath(sourcePath);
    std::string stem;
    if (guid.IsValid()) {
        stem = guid.ToString();
    } else {
        const std::string key = NormalizedSourcePath(sourcePath);
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : key) {
            h ^= (uint64_t)c;
            h *= 1099511628211ull;
        }
        char buf[17];
        snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
        stem = buf;
    }
    char variant[10];
    snprintf(variant, sizeof(variant), "_%08x", (unsigned)(HashSettings(settings) & 0xFFFFFFFFu));
    return (std::filesystem::path(CacheDir()) / (stem + variant + ".ttex")).string();
}

namespace {

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
    mix((uint64_t)s.TextureType); // #156: Normal Map forces linear, which changes the downsample
    // #156: compressed entries hold the encoded blocks of every mip level, so the mode and
    // whether there's a mip chain at all both change what's stored.
    mix((uint64_t)s.CompressionMode + 17);
    if (s.CompressionMode != TextureImportSettings::Compression::None) mix(s.GenerateMipmaps ? 3u : 5u);
    // Mixed only when set, so every existing entry (None) keeps the hash it was stored under.
    if (s.ForceFormat != TextureImportSettings::ForcedFormat::None) mix((uint64_t)s.ForceFormat + 101);
    return h;
}

bool Load(const std::string& sourcePath, const TextureImportSettings& settings, Image& out) {
    uint64_t srcSize = 0, srcMtime = 0;
    if (!SourceStamp(sourcePath, srcSize, srcMtime)) return false; // source gone: nothing to validate against

    const std::string entryPath = EntryPath(sourcePath, settings);
    std::ifstream f(entryPath, std::ios::binary);
    if (!f) return false;

    char magic[4];
    uint32_t version = 0;
    if (!f.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return false;
    if (!Read(f, version) || version != kVersion) return false;

    uint64_t size = 0, mtime = 0, hash = 0;
    if (!Read(f, size) || !Read(f, mtime) || !Read(f, hash)) return false;
    if (size != srcSize || mtime != srcMtime || hash != HashSettings(settings)) return false;

    // The source path is stored too, so a (vanishingly unlikely) filename-hash collision is
    // caught here and treated as a miss (#159: this used to read the path but never compare it).
    // A GUID-keyed texture that was moved also misses once, and Store() re-bakes it in place.
    uint32_t pathLen = 0;
    if (!Read(f, pathLen) || pathLen > 4096) return false;
    std::string storedPath(pathLen, '\0');
    if (pathLen && !f.read(storedPath.data(), pathLen)) return false;
    if (storedPath != NormalizedSourcePath(sourcePath)) return false;

    int32_t sw = 0, sh = 0, w = 0, h = 0, ch = 0;
    if (!Read(f, sw) || !Read(f, sh) || !Read(f, w) || !Read(f, h) || !Read(f, ch)) return false;
    if (w <= 0 || h <= 0 || ch < 1 || ch > 4) return false;

    uint32_t glFormat = 0, levelCount = 0;
    if (!Read(f, glFormat) || !Read(f, levelCount) || levelCount > 32) return false;
    std::vector<uint32_t> levelSizes(levelCount);
    size_t bytes = 0;
    for (uint32_t& s : levelSizes) {
        if (!Read(f, s)) return false;
        bytes += s;
    }
    if (glFormat == 0) bytes = (size_t)w * (size_t)h * (size_t)ch;
    else if (levelCount == 0 || bytes > (size_t)w * h * 4 * 2) return false; // corrupt header
    std::vector<unsigned char> pixels(bytes);
    if (!f.read(reinterpret_cast<char*>(pixels.data()), (std::streamsize)bytes)) return false;
    f.close();

    // Mark the entry as recently used; Prune()'s size cap evicts the oldest-stamped entries first.
    std::error_code touchEc;
    std::filesystem::last_write_time(entryPath, std::filesystem::file_time_type::clock::now(), touchEc);
    MarkUsedThisSession(entryPath);

    out.SourceWidth = sw;
    out.SourceHeight = sh;
    out.Width = w;
    out.Height = h;
    out.Channels = ch;
    out.Pixels = std::move(pixels);
    out.GLFormat = glFormat;
    out.LevelSizes = std::move(levelSizes);
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
    // The temp name is unique per process and per call (#159): two editor instances, or two
    // threads, baking the same entry must never share, and so truncate, one temp file.
    static std::atomic<uint32_t> s_TempCounter{0};
    const std::string finalPath = EntryPath(sourcePath, settings);
    const std::string tempPath = finalPath + "." + std::to_string(TT_GETPID()) + "-" +
                                 std::to_string(s_TempCounter.fetch_add(1)) + ".tmp";
    {
        std::ofstream f(tempPath, std::ios::binary | std::ios::trunc);
        if (!f) return;

        f.write(kMagic, 4);
        Write(f, kVersion);
        Write(f, srcSize);
        Write(f, srcMtime);
        Write(f, HashSettings(settings));

        const std::string abs = NormalizedSourcePath(sourcePath);
        Write(f, (uint32_t)abs.size());
        f.write(abs.data(), (std::streamsize)abs.size());

        Write(f, (int32_t)img.SourceWidth);
        Write(f, (int32_t)img.SourceHeight);
        Write(f, (int32_t)img.Width);
        Write(f, (int32_t)img.Height);
        Write(f, (int32_t)img.Channels);
        Write(f, (uint32_t)img.GLFormat);
        Write(f, (uint32_t)img.LevelSizes.size());
        for (uint32_t s : img.LevelSizes) Write(f, s);
        f.write(reinterpret_cast<const char*>(img.Pixels.data()), (std::streamsize)img.Pixels.size());
        if (!f) {
            f.close();
            std::filesystem::remove(tempPath, ec);
            return;
        }
    }
    std::filesystem::rename(tempPath, finalPath, ec);
    if (ec) std::filesystem::remove(tempPath, ec);
    else MarkUsedThisSession(finalPath);
}

uint64_t CapBytes(uint64_t pinnedBytes, uint64_t minCapBytes) {
    return std::max(minCapBytes ? minCapBytes : kMinCapBytes, pinnedBytes + pinnedBytes / 4);
}

std::vector<size_t> ChooseEvictions(const std::vector<EntryInfo>& entries, uint64_t freeDiskBytes, uint64_t minCapBytes,
                                    uint64_t* capOut) {
    uint64_t total = 0, pinned = 0;
    for (const EntryInfo& e : entries) {
        total += e.Bytes;
        if (e.Pinned) pinned += e.Bytes;
    }
    uint64_t cap = CapBytes(pinned, minCapBytes);
    if (freeDiskBytes != std::numeric_limits<uint64_t>::max()) {
        // Deleting entries frees their bytes: the directory may hold at most (total + free) - the reserve.
        const uint64_t room = total + freeDiskBytes;
        cap = std::min(cap, room > kKeepFreeDiskBytes ? room - kKeepFreeDiskBytes : 0);
    }
    if (capOut) *capOut = cap;
    std::vector<size_t> victims;
    if (total <= cap) return victims;
    std::vector<size_t> order(entries.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&entries](size_t a, size_t b) { return entries[a].LastUsed < entries[b].LastUsed; });
    for (size_t i : order) {
        if (total <= cap) break;
        if (entries[i].Pinned) continue; // this session's working set stays, whatever the cap
        victims.push_back(i);
        total -= entries[i].Bytes;
    }
    return victims;
}

PruneStats Prune(const std::function<std::optional<uint64_t>(const std::string& sourcePath)>& currentSettingsHash,
                 uint64_t minCapBytes) {
    PruneStats stats;
    std::error_code ec;
    if (!std::filesystem::is_directory(CacheDir(), ec)) return stats; // no cache directory yet: nothing to prune

    std::vector<EntryInfo> survivors; // valid entries, candidates for the size cap below
    const auto now = std::filesystem::file_time_type::clock::now();

    for (const auto& entry : std::filesystem::directory_iterator(CacheDir(), ec)) {
        if (ec) break;
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || fileEc) continue;

        const std::filesystem::path path = entry.path();
        if (path.extension() == ".tmp") {
            // A Store() interrupted by a crash leaves its temp behind forever. Recent ones may
            // belong to another running editor instance mid-write, so only old ones go (#159).
            std::error_code timeEc;
            auto written = entry.last_write_time(timeEc);
            if (!timeEc && now - written > kAbandonedTempAge) std::filesystem::remove(path, timeEc);
            continue;
        }
        if (path.extension() != ".ttex") continue; // stray files aren't ours to judge

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
                // Not the .meta's settings: another loader's variant (kept while it's used), or the settings
                // changed since it was baked (unused from then on, so it ages out).
                std::optional<uint64_t> current = currentSettingsHash(storedPath);
                std::error_code timeEc;
                const auto used = entry.last_write_time(timeEc);
                if (current.has_value() && *current != storedHash && !timeEc && now - used > kUnusedVariantAge) stale = true;
            }
        }

        if (stale) {
            std::error_code removeEc; // a file in use elsewhere: skip it, keep pruning
            std::filesystem::remove(path, removeEc);
        } else {
            std::error_code statEc;
            auto lastUsed = entry.last_write_time(statEc);
            auto bytes = entry.file_size(statEc);
            if (!statEc)
                survivors.push_back({path.string(), (uint64_t)bytes, (int64_t)lastUsed.time_since_epoch().count(),
                                     UsedThisSession(path)});
        }
    }

    // Size cap (#159, P-53): evict least-recently-used entries until the directory fits, never one this
    // session loaded or stored. Load() and Store() both refresh an entry's timestamp.
    std::error_code spaceEc;
    const auto space = std::filesystem::space(CacheDir(), spaceEc);
    const uint64_t freeBytes = spaceEc ? std::numeric_limits<uint64_t>::max() : (uint64_t)space.available;
    for (const EntryInfo& e : survivors) {
        ++stats.Entries;
        stats.Bytes += e.Bytes;
        if (e.Pinned) { ++stats.Pinned; stats.PinnedBytes += e.Bytes; }
    }
    for (size_t i : ChooseEvictions(survivors, freeBytes, minCapBytes, &stats.CapBytes)) {
        std::error_code rmEc;
        if (!std::filesystem::remove(survivors[i].Path, rmEc)) continue;
        ++stats.Evicted;
        stats.EvictedBytes += survivors[i].Bytes;
    }
    return stats;
}

void Pin(const std::string& sourcePath, const TextureImportSettings& settings) {
    const std::string name = std::filesystem::path(EntryPath(sourcePath, settings)).filename().string();
    std::lock_guard<std::mutex> lock(s_SessionMutex);
    s_RequestedEntries.insert(name);
}

bool Pinned(const std::string& sourcePath, const TextureImportSettings& settings) {
    return UsedThisSession(EntryPath(sourcePath, settings));
}

int SessionEntriesMissing() {
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(s_SessionMutex);
        names.assign(s_SessionEntries.begin(), s_SessionEntries.end());
    }
    int missing = 0;
    for (const std::string& name : names) {
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(CacheDir()) / name, ec)) ++missing;
    }
    return missing;
}

Usage DiskUsage() {
    Usage usage;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(CacheDir(), ec)) {
        if (ec) break;
        std::error_code fileEc;
        if (entry.path().extension() != ".ttex" || !entry.is_regular_file(fileEc)) continue;
        const auto bytes = entry.file_size(fileEc);
        if (fileEc) continue;
        usage.Bytes += (uint64_t)bytes;
        ++usage.Entries;
    }
    return usage;
}

int Clear() {
    std::vector<std::filesystem::path> victims;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(CacheDir(), ec)) {
        if (ec) break;
        if (entry.path().extension() == ".ttex") victims.push_back(entry.path());
    }
    int removed = 0;
    for (const auto& path : victims) {
        std::error_code rmEc;
        if (std::filesystem::remove(path, rmEc)) ++removed;
    }
    return removed;
}

} // namespace TextureCache
