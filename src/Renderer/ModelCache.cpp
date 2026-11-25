#include "ModelCache.h"
#include "Model.h"
#include "Core/Env.h"
#include "Core/Paths.h"
#include "Log.h"
#include "meshoptimizer.h"

#include <assimp/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <type_traits>

#if defined(_WIN32)
#include <process.h> // _getpid
#define MC_GETPID _getpid
#else
#include <unistd.h>
#define MC_GETPID getpid
#endif

// A hash of the source files that decide what an import produces (CMakeLists.txt); a build without it still keys on
// the format version below, which then has to be bumped by hand on every change to the import.
#ifndef TARTARUS_MODEL_COOK_CODE
#define TARTARUS_MODEL_COOK_CODE 0ull
#endif

namespace ModelCache {
namespace {

namespace fs = std::filesystem;

constexpr char kMagic[4] = {'T', 'M', 'D', 'L'};
// Bump on any change to the entry layout below.
constexpr std::uint32_t kFormatVersion = 1;
// A forest's ~860 models cook to about a gigabyte; the cap leaves room for a few scenes' worth.
constexpr std::uint64_t kMaxCacheBytes = 8ull * 1024 * 1024 * 1024;
constexpr auto kUnusedAge = std::chrono::hours(24 * 30);
constexpr auto kAbandonedTempAge = std::chrono::hours(1);

std::mutex s_DirMutex;
std::string s_DirOverride;
std::atomic<bool> s_Bypass{false};
std::mutex s_StatsMutex;
Stats s_Stats;

std::string CacheDir() {
    std::lock_guard<std::mutex> lock(s_DirMutex);
    if (!s_DirOverride.empty()) return s_DirOverride;
    static const std::string dir = ProjectPaths::Resolve("Library/Models");
    return dir;
}

std::string EntryPath(const Key& key) {
    char name[64];
    std::snprintf(name, sizeof name, "%016llx_%016llx.tmdl", (unsigned long long)key.Source, (unsigned long long)key.Settings);
    return (fs::path(CacheDir()) / name).string();
}

// 64-bit multiply-rotate over 8-byte words, then an avalanche (MurmurHash3's fmix64) - fast enough to hash a forest's
// FBX files at disk speed, and any changed byte changes the result.
constexpr std::uint64_t kP1 = 0x9E3779B185EBCA87ull, kP2 = 0xC2B2AE3D27D4EB4Full;
inline std::uint64_t Rotl(std::uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline std::uint64_t Mix(std::uint64_t h, std::uint64_t w) { return Rotl(h ^ (w * kP2), 31) * kP1; }
inline std::uint64_t Avalanche(std::uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}
std::uint64_t HashRaw(const unsigned char* p, std::size_t n, std::uint64_t h) {
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = Mix(h, w);
    }
    std::uint64_t tail = 0;
    for (std::size_t k = 0; i + k < n; ++k) tail |= (std::uint64_t)p[i + k] << (8 * k);
    return Mix(h, tail ^ ((std::uint64_t)(n & 7) << 56));
}

// The file's size and last write time (false when it can't be read).
bool SourceStamp(const std::string& path, std::uint64_t& size, std::int64_t& time) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    const auto bytes = fs::file_size(p, ec);
    if (ec) return false;
    const auto written = fs::last_write_time(p, ec);
    if (ec) return false;
    size = (std::uint64_t)bytes;
    time = (std::int64_t)written.time_since_epoch().count();
    return true;
}

bool IsCookedFormat(const std::string& path) {
    std::string ext = fs::u8path(path).extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".fbx" || ext == ".glb";
}

// --- writer / reader --------------------------------------------------------------------------------------------

struct Writer {
    std::string Out;
    template <typename T> void Pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        Out.append(reinterpret_cast<const char*>(&v), sizeof(T));
    }
    void Str(const std::string& s) {
        Pod((std::uint32_t)s.size());
        Out.append(s);
    }
    template <typename T> void Vec(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        Pod((std::uint64_t)v.size());
        if (!v.empty()) Out.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
    }
};

struct Reader {
    const char* P;
    const char* End;
    bool Ok = true;
    bool Take(void* dst, std::size_t n) {
        if (!Ok || (std::size_t)(End - P) < n) return Ok = false;
        if (n) std::memcpy(dst, P, n);
        P += n;
        return true;
    }
    template <typename T> bool Pod(T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        return Take(&v, sizeof(T));
    }
    bool Str(std::string& s) {
        std::uint32_t n = 0;
        if (!Pod(n) || n > (std::uint32_t)(End - P)) return Ok = false;
        s.assign(P, n);
        P += n;
        return true;
    }
    template <typename T> bool Vec(std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::uint64_t n = 0;
        if (!Pod(n) || n > (std::uint64_t)(End - P) / sizeof(T)) return Ok = false;
        v.resize((std::size_t)n);
        return Take(v.data(), (std::size_t)n * sizeof(T));
    }
};

void WriteMaterial(Writer& w, const MaterialSource& m) {
    w.Str(m.Name);
    for (int s = 0; s < MaterialSource::SlotCount; ++s) w.Str(m.Textures[s]);
    w.Pod(m.HasAlphaMode);
    w.Str(m.AlphaMode);
    w.Pod(m.HasAlphaCutoff);
    w.Pod(m.AlphaCutoff);
    w.Pod(m.HasDiffuse);
    w.Pod(m.Diffuse);
    w.Pod(m.HasEmissive);
    w.Pod(m.Emissive);
    w.Pod(m.HasTwoSided);
    w.Pod(m.TwoSided);
    for (const auto& t : m.UvTransforms) {
        w.Pod(t.Has);
        w.Pod(t.Scaling);
        w.Pod(t.Translation);
        w.Pod(t.Rotation);
    }
    w.Pod(m.HasMetallic);
    w.Pod(m.Metallic);
    w.Pod(m.HasRoughness);
    w.Pod(m.Roughness);
}

bool ReadMaterial(Reader& r, MaterialSource& m) {
    r.Str(m.Name);
    for (int s = 0; s < MaterialSource::SlotCount; ++s) r.Str(m.Textures[s]);
    r.Pod(m.HasAlphaMode);
    r.Str(m.AlphaMode);
    r.Pod(m.HasAlphaCutoff);
    r.Pod(m.AlphaCutoff);
    r.Pod(m.HasDiffuse);
    r.Pod(m.Diffuse);
    r.Pod(m.HasEmissive);
    r.Pod(m.Emissive);
    r.Pod(m.HasTwoSided);
    r.Pod(m.TwoSided);
    for (auto& t : m.UvTransforms) {
        r.Pod(t.Has);
        r.Pod(t.Scaling);
        r.Pod(t.Translation);
        r.Pod(t.Rotation);
    }
    r.Pod(m.HasMetallic);
    r.Pod(m.Metallic);
    r.Pod(m.HasRoughness);
    r.Pod(m.Roughness);
    return r.Ok;
}

void WriteBody(Writer& w, const Cooked& c) {
    w.Pod((std::uint32_t)c.Materials.size());
    for (const auto& m : c.Materials) WriteMaterial(w, m);
    w.Pod((std::uint32_t)c.Meshes.size());
    for (const auto& m : c.Meshes) {
        w.Vec(m.Vertices);
        w.Vec(m.Indices);
        w.Vec(m.Posed);
        w.Pod(m.MaterialIndex);
        w.Pod(m.VertexColors);
        w.Pod(m.Skinned);
    }
    w.Pod((std::uint32_t)c.Bones.size());
    for (const auto& b : c.Bones) {
        w.Str(b.Name);
        w.Pod(b.Id);
        w.Pod(b.Offset);
    }
    w.Pod(c.BoneCounter);
    w.Pod(c.SkinVertices);
    w.Pod(c.SkinOverflow);
    w.Pod(c.SkinMaxInfluences);
    w.Pod(c.SkinMaxDropped);
    w.Pod(c.GlobalInverseTransform);
    w.Pod((std::uint32_t)c.Nodes.size());
    for (const auto& n : c.Nodes) {
        w.Str(n.Name);
        w.Pod(n.Parent);
        w.Pod(n.BindLocal);
        w.Pod(n.BindTRS.T);
        w.Pod(n.BindTRS.R);
        w.Pod(n.BindTRS.S);
        w.Pod(n.BoneId);
        w.Pod(n.BoneOffset);
    }
    w.Pod((std::uint32_t)c.Animations.size());
    for (const auto& a : c.Animations) {
        w.Str(a.Name);
        w.Pod(a.DurationTicks);
        w.Pod(a.TicksPerSecond);
        w.Pod(a.StartTicks);
        w.Pod(a.EndTicks);
        w.Pod((std::uint32_t)a.Channels.size());
        for (const auto& ch : a.Channels) {
            w.Str(ch.BoneName);
            w.Vec(ch.Positions);
            w.Vec(ch.Rotations);
            w.Vec(ch.Scales);
        }
        w.Vec(a.NodeChannel);
    }
    w.Vec(c.BindPoseBones);
    w.Pod(c.BoundsMin);
    w.Pod(c.BoundsMax);
    w.Vec(c.BoneBoxMin);
    w.Vec(c.BoneBoxMax);
    w.Pod(c.UnweightedMin);
    w.Pod(c.UnweightedMax);
}

bool ReadBody(Reader& r, Cooked& c) {
    // Counts are bounded by what is left to read, so a corrupt count fails instead of allocating gigabytes.
    auto count = [&r](std::uint32_t& n) { return r.Pod(n) && n <= (std::uint32_t)(r.End - r.P); };
    std::uint32_t n = 0;
    if (!count(n)) return false;
    c.Materials.resize(n);
    for (auto& m : c.Materials)
        if (!ReadMaterial(r, m)) return false;
    if (!count(n)) return false;
    c.Meshes.resize(n);
    for (auto& m : c.Meshes) {
        r.Vec(m.Vertices);
        r.Vec(m.Indices);
        r.Vec(m.Posed);
        r.Pod(m.MaterialIndex);
        r.Pod(m.VertexColors);
        r.Pod(m.Skinned);
        if (!r.Ok) return false;
        for (unsigned int i : m.Indices)
            if (i >= m.Vertices.size()) return false; // corrupt: a draw would read past the vertices
    }
    if (!count(n)) return false;
    c.Bones.resize(n);
    for (auto& b : c.Bones) {
        r.Str(b.Name);
        r.Pod(b.Id);
        r.Pod(b.Offset);
    }
    r.Pod(c.BoneCounter);
    r.Pod(c.SkinVertices);
    r.Pod(c.SkinOverflow);
    r.Pod(c.SkinMaxInfluences);
    r.Pod(c.SkinMaxDropped);
    r.Pod(c.GlobalInverseTransform);
    if (!count(n)) return false;
    c.Nodes.resize(n);
    for (auto& node : c.Nodes) {
        r.Str(node.Name);
        r.Pod(node.Parent);
        r.Pod(node.BindLocal);
        r.Pod(node.BindTRS.T);
        r.Pod(node.BindTRS.R);
        r.Pod(node.BindTRS.S);
        r.Pod(node.BoneId);
        r.Pod(node.BoneOffset);
        if (!r.Ok || node.Parent >= (int)c.Nodes.size()) return false;
    }
    if (!count(n)) return false;
    c.Animations.resize(n);
    for (auto& a : c.Animations) {
        r.Str(a.Name);
        r.Pod(a.DurationTicks);
        r.Pod(a.TicksPerSecond);
        r.Pod(a.StartTicks);
        r.Pod(a.EndTicks);
        std::uint32_t channels = 0;
        if (!count(channels)) return false;
        a.Channels.resize(channels);
        for (auto& ch : a.Channels) {
            r.Str(ch.BoneName);
            r.Vec(ch.Positions);
            r.Vec(ch.Rotations);
            r.Vec(ch.Scales);
            if (!r.Ok) return false;
            ch.Finalize();
        }
        r.Vec(a.NodeChannel);
        if (!r.Ok) return false;
        for (int ch : a.NodeChannel)
            if (ch >= (int)a.Channels.size()) return false;
    }
    r.Vec(c.BindPoseBones);
    r.Pod(c.BoundsMin);
    r.Pod(c.BoundsMax);
    r.Vec(c.BoneBoxMin);
    r.Vec(c.BoneBoxMax);
    r.Pod(c.UnweightedMin);
    r.Pod(c.UnweightedMax);
    return r.Ok && r.P == r.End;
}

// The importer's identity: what a cooked entry depends on besides the file and its settings.
std::uint64_t ImporterHash() {
    static const std::uint64_t h = [] {
        const std::uint64_t parts[] = {
            kFormatVersion,
            (std::uint64_t)aiGetVersionMajor(), (std::uint64_t)aiGetVersionMinor(), (std::uint64_t)aiGetVersionPatch(),
            (std::uint64_t)aiGetVersionRevision(),
            (std::uint64_t)MESHOPTIMIZER_VERSION,
            (std::uint64_t)sizeof(ModelVertex), (std::uint64_t)MAX_BONE_INFLUENCE, (std::uint64_t)MAX_BONES,
            (std::uint64_t)TARTARUS_MODEL_COOK_CODE,
        };
        return HashBytes(parts, sizeof parts, 0x4D444C43ull);
    }();
    return h;
}

} // namespace

std::uint64_t HashBytes(const void* data, std::size_t size, std::uint64_t seed) {
    return Avalanche(HashRaw(static_cast<const unsigned char*>(data), size, seed ^ kP2) ^ (std::uint64_t)size);
}

bool Enabled() {
    static const bool off = Env::Flag(Env::Var::NoModelCache);
    return !off;
}

std::uint64_t HashSettings(const ModelImportSettings& s) {
    // Every field: a new one must be added here, or two imports that differ in it would share an entry. The size check
    // below catches a field added to the struct without this function (MSVC x64 release layout).
#if defined(_MSC_VER) && defined(_WIN64) && !defined(_DEBUG)
    static_assert(sizeof(ModelImportSettings) == 232, "ModelImportSettings changed: hash the new field in ModelCache::HashSettings, then update this size");
#endif
    Writer w;
    w.Pod(s.GlobalScale);
    w.Pod(s.ImportNormals);
    w.Pod(s.ImportAnimations);
    w.Pod(s.ImportSkeleton);
    w.Pod(s.OptimizeGraph);
    w.Pod(s.SimplifyRatio);
    w.Pod(s.SourceUpAxis);
    w.Pod(s.PivotOffset);
    w.Pod((std::uint32_t)s.ExcludeNodes.size());
    for (const auto& n : s.ExcludeNodes) w.Str(n);
    w.Pod((std::uint32_t)s.MeshNodes.size());
    for (const auto& n : s.MeshNodes) w.Str(n);
    w.Pod(s.RootTransform);
    w.Pod((std::uint32_t)s.UnrotateNodes.size());
    for (const auto& n : s.UnrotateNodes) w.Str(n);
    w.Pod((std::uint32_t)s.MoveNodes.size());
    for (const auto& m : s.MoveNodes) {
        w.Str(m.Node);
        w.Pod(m.By);
    }
    w.Pod(s.MergeByMaterial);
    w.Pod(s.SkinIgnoresMeshNode);
    w.Pod(s.RootNodeInModelSpace);
    w.Pod(s.MaterialImportMode);
    w.Pod((std::uint32_t)s.ClipTrims.size());
    for (const auto& t : s.ClipTrims) {
        w.Str(t.Clip);
        w.Pod(t.StartSeconds);
        w.Pod(t.EndSeconds);
    }
    w.Pod(s.AnimationOnly);
    return HashBytes(w.Out.data(), w.Out.size(), ImporterHash());
}

Key MakeKey(const std::string& path, const ModelImportSettings& settings) {
    Key key;
    if (!Enabled() || s_Bypass.load() || !IsCookedFormat(path)) return key;
    // The skin audit imports with Assimp's influence cap off (ImportFromFile): never cooked, never read.
    static const bool s_SkinAudit = Env::Flag(Env::Var::SkinAudit);
    if (s_SkinAudit) return key;
    if (!SourceStamp(path, key.StampSize, key.StampTime)) return key; // before the read: Store compares against it
    key.Path = path;
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return key;
    std::vector<unsigned char> buf(1 << 20);
    std::uint64_t h = kP1, total = 0;
    // Each chunk is a whole number of 8-byte words except the last, so the result is HashRaw over the whole file.
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)buf.size());
        const std::size_t got = (std::size_t)f.gcount();
        if (got == 0) break;
        h = HashRaw(buf.data(), got, h);
        total += got;
        if (got < buf.size()) break;
    }
    if (f.bad()) return key;
    key.Source = Avalanche(h ^ total);
    key.SourceBytes = total;
    key.Settings = HashSettings(settings);
    key.Valid = true;
    return key;
}

bool Load(const Key& key, Cooked& out) {
    if (!key.Valid) return false;
    const std::string path = EntryPath(key);
    std::ifstream f(fs::u8path(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size <= 0) return false;
    std::string data((std::size_t)size, '\0');
    f.seekg(0);
    if (!f.read(data.data(), size)) return false;
    f.close();

    Reader r{data.data(), data.data() + data.size()};
    char magic[4];
    std::uint32_t version = 0;
    std::uint64_t source = 0, settings = 0, bytes = 0, bodyHash = 0;
    if (!r.Take(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return false;
    if (!r.Pod(version) || version != kFormatVersion) return false;
    if (!r.Pod(source) || !r.Pod(settings) || !r.Pod(bytes) || !r.Pod(bodyHash)) return false;
    if (source != key.Source || settings != key.Settings || bytes != key.SourceBytes) return false;
    // The body's own hash: a torn or bit-flipped entry is a miss, never wrong geometry.
    if (HashBytes(r.P, (std::size_t)(r.End - r.P)) != bodyHash) return false;
    Cooked c;
    if (!ReadBody(r, c)) return false;
    out = std::move(c);

    // Recently used: Prune's size cap evicts the oldest-stamped entries first.
    std::error_code ec;
    fs::last_write_time(fs::u8path(path), fs::file_time_type::clock::now(), ec);
    return true;
}

void Store(const Key& key, const Cooked& cooked) {
    if (!key.Valid) return;
    for (const auto& m : cooked.Materials)
        if (m.AnyEmbedded()) return; // embedded textures live only in the source file
    // The file changed after it was hashed: what was imported may be the new file, which this key doesn't describe.
    // Nothing is filed; the next import hashes the file as it is then.
    std::uint64_t size = 0;
    std::int64_t time = 0;
    if (!SourceStamp(key.Path, size, time) || size != key.StampSize || time != key.StampTime) {
        Log::Warn("Model cache: '" + key.Path + "' changed while it was being imported; its import was not cached.");
        return;
    }
    Writer body;
    WriteBody(body, cooked);
    Writer head;
    head.Out.append(kMagic, 4);
    head.Pod(kFormatVersion);
    head.Pod(key.Source);
    head.Pod(key.Settings);
    head.Pod(key.SourceBytes);
    head.Pod(HashBytes(body.Out.data(), body.Out.size()));

    std::error_code ec;
    fs::create_directories(fs::u8path(CacheDir()), ec);
    if (ec) return;
    // Unique per process and per call: two engines, or two loader threads, cooking the same file never share a temp.
    static std::atomic<std::uint32_t> s_TempCounter{0};
    const std::string finalPath = EntryPath(key);
    const std::string tempPath = finalPath + "." + std::to_string(MC_GETPID()) + "-" + std::to_string(s_TempCounter.fetch_add(1)) + ".tmp";
    {
        std::ofstream f(fs::u8path(tempPath), std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(head.Out.data(), (std::streamsize)head.Out.size());
        f.write(body.Out.data(), (std::streamsize)body.Out.size());
        f.close();
        if (!f) {
            fs::remove(fs::u8path(tempPath), ec);
            return;
        }
    }
    // Atomic replace: a reader sees the old entry or the new one. An entry another engine is reading can't be replaced
    // on Windows; that one is the same data anyway, so the temp just goes.
    fs::rename(fs::u8path(tempPath), fs::u8path(finalPath), ec);
    if (ec) fs::remove(fs::u8path(tempPath), ec);
}

Usage DiskUsage() {
    Usage usage;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::u8path(CacheDir()), ec)) {
        std::error_code fileEc;
        if (entry.path().extension() != ".tmdl" || !entry.is_regular_file(fileEc)) continue;
        const auto bytes = entry.file_size(fileEc);
        if (fileEc) continue;
        usage.Bytes += (std::uint64_t)bytes;
        ++usage.Entries;
    }
    return usage;
}

int Clear() {
    std::vector<fs::path> victims;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::u8path(CacheDir()), ec))
        if (entry.path().extension() == ".tmdl") victims.push_back(entry.path());
    int removed = 0;
    for (const auto& p : victims) {
        std::error_code rmEc;
        if (fs::remove(p, rmEc)) ++removed;
    }
    return removed;
}

void Prune() {
    std::error_code ec;
    const fs::path dir = fs::u8path(CacheDir());
    if (!fs::is_directory(dir, ec)) return;
    struct Survivor {
        fs::file_time_type LastUsed;
        std::uint64_t Bytes;
        fs::path Path;
    };
    std::vector<Survivor> survivors;
    const auto now = fs::file_time_type::clock::now();
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc)) continue;
        const fs::path p = entry.path();
        const auto used = entry.last_write_time(fileEc);
        if (fileEc) continue;
        if (p.extension() == ".tmp") {
            // A Store cut off by a crash. A recent one may be another engine's write in progress.
            if (now - used > kAbandonedTempAge) fs::remove(p, fileEc);
            continue;
        }
        if (p.extension() != ".tmdl") continue;
        // Keys are content hashes, so an entry for a changed or deleted file, or an older importer, is simply never
        // read again: it goes once nothing has used it for a month.
        if (now - used > kUnusedAge) {
            fs::remove(p, fileEc);
            continue;
        }
        const auto bytes = entry.file_size(fileEc);
        if (!fileEc) survivors.push_back({used, (std::uint64_t)bytes, p});
    }
    std::uint64_t total = 0;
    for (const auto& s : survivors) total += s.Bytes;
    if (total <= kMaxCacheBytes) return;
    std::sort(survivors.begin(), survivors.end(), [](const Survivor& a, const Survivor& b) { return a.LastUsed < b.LastUsed; });
    for (const auto& s : survivors) {
        if (total <= kMaxCacheBytes) break;
        std::error_code rmEc;
        if (fs::remove(s.Path, rmEc)) total -= s.Bytes;
    }
}

void SetDirectoryForTests(const std::string& dir) {
    std::lock_guard<std::mutex> lock(s_DirMutex);
    s_DirOverride = dir;
}

void SetBypass(bool bypass) { s_Bypass = bypass; }

Stats GetStats() {
    std::lock_guard<std::mutex> lock(s_StatsMutex);
    return s_Stats;
}
void RecordHit(double ms) {
    std::lock_guard<std::mutex> lock(s_StatsMutex);
    ++s_Stats.Hits;
    s_Stats.HitMs += ms;
}
void RecordCook(double ms) {
    std::lock_guard<std::mutex> lock(s_StatsMutex);
    ++s_Stats.Cooked;
    s_Stats.CookMs += ms;
}

} // namespace ModelCache
