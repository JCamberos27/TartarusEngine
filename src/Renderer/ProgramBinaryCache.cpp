#include "ProgramBinaryCache.h"
#include "Core/Env.h"
#include "Core/Paths.h"
#include "gl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <process.h> // _getpid
#define PBC_GETPID _getpid
#else
#include <unistd.h>
#define PBC_GETPID getpid
#endif

namespace ProgramBinaryCache {
namespace {

namespace fs = std::filesystem;

constexpr char kMagic[4] = {'T', 'P', 'R', 'G'};
// Bump on any change to the entry layout below.
constexpr std::uint32_t kFormatVersion = 1;
// A full run links a few hundred programs at ~50 KB - 1 MB each; the cap leaves room for several drivers' worth.
constexpr std::uint64_t kMaxCacheBytes = 512ull * 1024 * 1024;
constexpr auto kUnusedAge = std::chrono::hours(24 * 30);
constexpr auto kAbandonedTempAge = std::chrono::hours(1);
constexpr const char* kExtension = ".tprg";

std::mutex s_DirMutex;
std::string s_DirOverride;
std::mutex s_StatsMutex;
Stats s_Stats;

std::string CacheDir() {
    std::lock_guard<std::mutex> lock(s_DirMutex);
    if (!s_DirOverride.empty()) return s_DirOverride;
    static const std::string dir = ProjectPaths::Resolve("Library/ShaderCache");
    return dir;
}

fs::path EntryPath(const Key& key) {
    char name[48];
    std::snprintf(name, sizeof name, "%016llx%s", (unsigned long long)key.Source, kExtension);
    return fs::u8path(CacheDir()) / name;
}

// FNV-1a over the bytes, then an avalanche (MurmurHash3's fmix64) so near-identical sources spread across the key.
std::uint64_t Fnv(std::uint64_t h, const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
std::uint64_t Avalanche(std::uint64_t h) {
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

struct Writer {
    std::string Out;
    template <typename T> void Pod(const T& v) { Out.append(reinterpret_cast<const char*>(&v), sizeof v); }
    void Bytes(std::string_view s) {
        Pod((std::uint32_t)s.size());
        Out.append(s.data(), s.size());
    }
};
struct Reader {
    const char* P;
    const char* End;
    template <typename T> bool Pod(T& v) {
        if ((std::size_t)(End - P) < sizeof v) return false;
        std::memcpy(&v, P, sizeof v);
        P += sizeof v;
        return true;
    }
    bool Bytes(std::string_view& s) {
        std::uint32_t n = 0;
        if (!Pod(n) || (std::size_t)(End - P) < n) return false;
        s = std::string_view(P, n);
        P += n;
        return true;
    }
};

// GL_VENDOR | GL_RENDERER | GL_VERSION of the current context, read once.
const std::string& DriverString() {
    static const std::string s = [] {
        auto str = [](GLenum e) {
            const auto* v = reinterpret_cast<const char*>(glGetString(e));
            return std::string(v ? v : "?");
        };
        return str(GL_VENDOR) + " | " + str(GL_RENDERER) + " | " + str(GL_VERSION);
    }();
    return s;
}

bool ReadFile(const fs::path& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize((std::size_t)size);
    f.read(out.data(), size);
    return (bool)f;
}

bool WriteEntryFile(const Key& key, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(fs::u8path(CacheDir()), ec);
    if (ec) return false;
    // Unique per process and per call: two engines storing the same program never share a temp.
    static std::atomic<std::uint32_t> s_TempCounter{0};
    const fs::path finalPath = EntryPath(key);
    fs::path tempPath = finalPath;
    tempPath += "." + std::to_string(PBC_GETPID()) + "-" + std::to_string(s_TempCounter.fetch_add(1)) + ".tmp";
    {
        std::ofstream f(tempPath, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(bytes.data(), (std::streamsize)bytes.size());
        f.close();
        if (!f) {
            fs::remove(tempPath, ec);
            return false;
        }
    }
    // Atomic replace: a reader sees the old entry or the new one. An entry another engine is reading can't be
    // replaced on Windows; that one is the same program anyway, so the temp just goes.
    fs::rename(tempPath, finalPath, ec);
    if (ec) {
        fs::remove(tempPath, ec);
        return false;
    }
    return true;
}

bool ReadEntryFile(const Key& key, std::string_view driver, std::uint32_t& format, std::string& binary) {
    std::string bytes;
    const fs::path path = EntryPath(key);
    if (!ReadFile(path, bytes)) return false;
    if (!DecodeEntry(bytes, key, driver, format, binary)) return false;
    // Mark it used, so Prune's month-unused rule and the size cap's LRU order see real use. Never on a file with another
    // name (a hard link from another checkout): touching it would change that checkout's copy too.
    std::error_code ec;
    if (fs::hard_link_count(path, ec) == 1 && !ec) fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
    return true;
}

} // namespace

Key MakeKeyFor(std::initializer_list<std::string_view> stages) {
    Key key;
    std::uint64_t h = 1469598103934665603ull;
    std::uint32_t index = 0;
    for (std::string_view s : stages) {
        h = Fnv(h, &index, sizeof index);
        const std::uint64_t len = s.size();
        h = Fnv(h, &len, sizeof len);
        h = Fnv(h, s.data(), s.size());
        key.SourceBytes += len;
        ++index;
    }
    key.Source = Avalanche(h ^ ((std::uint64_t)kFormatVersion << 56));
    key.Valid = true;
    return key;
}

std::string EncodeEntry(const Key& key, std::string_view driver, std::uint32_t format, std::string_view binary) {
    Writer w;
    w.Out.append(kMagic, 4);
    w.Pod(kFormatVersion);
    w.Pod(key.Source);
    w.Pod(key.SourceBytes);
    w.Bytes(driver);
    w.Pod(format);
    w.Pod(Avalanche(Fnv(1469598103934665603ull, binary.data(), binary.size())));
    w.Bytes(binary);
    return std::move(w.Out);
}

bool DecodeEntry(std::string_view bytes, const Key& key, std::string_view driver, std::uint32_t& format, std::string& binary) {
    Reader r{bytes.data(), bytes.data() + bytes.size()};
    if (bytes.size() < 4 || std::memcmp(bytes.data(), kMagic, 4) != 0) return false;
    r.P += 4;
    std::uint32_t version = 0;
    std::uint64_t source = 0, sourceBytes = 0, checksum = 0;
    std::string_view storedDriver, blob;
    if (!r.Pod(version) || version != kFormatVersion) return false;
    if (!r.Pod(source) || !r.Pod(sourceBytes) || source != key.Source || sourceBytes != key.SourceBytes) return false;
    if (!r.Bytes(storedDriver) || storedDriver != driver) return false;
    if (!r.Pod(format) || !r.Pod(checksum) || !r.Bytes(blob) || r.P != r.End || blob.empty()) return false;
    if (Avalanche(Fnv(1469598103934665603ull, blob.data(), blob.size())) != checksum) return false;
    binary.assign(blob.data(), blob.size());
    return true;
}

bool Enabled() {
    static const bool on = [] {
        if (Env::Flag(Env::Var::NoProgramCache)) return false;
        if (!glGetProgramBinary || !glProgramBinary || !glProgramParameteri) return false;
        GLint formats = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
        return formats > 0;
    }();
    return on;
}

Key MakeKey(std::initializer_list<std::string_view> stages) {
    if (!Enabled()) return {};
    return MakeKeyFor(stages);
}

unsigned int TryLoad(const Key& key) {
    if (!key.Valid) return 0;
    const auto start = std::chrono::steady_clock::now();
    std::uint32_t format = 0;
    std::string binary;
    if (!ReadEntryFile(key, DriverString(), format, binary)) return 0; // a miss; Store counts the compile
    const GLuint program = glCreateProgram();
    glProgramBinary(program, (GLenum)format, binary.data(), (GLsizei)binary.size());
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        // The driver no longer takes it (an update that kept the version string): compile instead, and drop the entry
        // so the next run doesn't try again (Store writes the fresh one).
        glDeleteProgram(program);
        std::error_code ec;
        fs::remove(EntryPath(key), ec);
        std::lock_guard<std::mutex> lock(s_StatsMutex);
        ++s_Stats.Refused;
        return 0;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::lock_guard<std::mutex> lock(s_StatsMutex);
    ++s_Stats.Hits;
    s_Stats.HitMs += ms;
    return program;
}

void PrepareForStore(unsigned int program, const Key& key) {
    if (key.Valid && program != 0) glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
}

void Store(const Key& key, unsigned int program, double compileMs) {
    if (!key.Valid || program == 0) return;
    {
        std::lock_guard<std::mutex> lock(s_StatsMutex);
        ++s_Stats.Compiled;
        s_Stats.CompileMs += compileMs;
    }
    GLint length = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
    if (length <= 0) return;
    std::string binary((std::size_t)length, '\0');
    GLsizei written = 0;
    GLenum format = 0;
    glGetProgramBinary(program, length, &written, &format, binary.data());
    if (written <= 0) return;
    binary.resize((std::size_t)written);
    WriteEntryFile(key, EncodeEntry(key, DriverString(), (std::uint32_t)format, binary));
}

Usage DiskUsage() {
    Usage usage;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::u8path(CacheDir()), ec)) {
        std::error_code fileEc;
        if (entry.path().extension() != kExtension || !entry.is_regular_file(fileEc)) continue;
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
        if (entry.path().extension() == kExtension) victims.push_back(entry.path());
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
        if (p.extension() != kExtension) continue;
        // An edited shader is a new key; the old entry is simply never read again and goes after a month unused.
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

bool WriteEntryForTests(const Key& key, std::string_view driver, std::uint32_t format, std::string_view binary) {
    return WriteEntryFile(key, EncodeEntry(key, driver, format, binary));
}

bool ReadEntryForTests(const Key& key, std::string_view driver, std::uint32_t& format, std::string& binary) {
    return ReadEntryFile(key, driver, format, binary);
}

Stats GetStats() {
    std::lock_guard<std::mutex> lock(s_StatsMutex);
    return s_Stats;
}

std::string Summary() {
    const Stats s = GetStats();
    if (s.Hits + s.Compiled + s.Refused == 0) return {};
    std::string out = "Program cache: " + std::to_string(s.Hits) + " programs read (" + std::to_string((int)s.HitMs) + " ms), " +
                      std::to_string(s.Compiled) + " compiled and stored (" + std::to_string((int)s.CompileMs) + " ms)";
    if (s.Refused > 0) out += ", " + std::to_string(s.Refused) + " refused by the driver";
    return out + ".";
}

} // namespace ProgramBinaryCache
