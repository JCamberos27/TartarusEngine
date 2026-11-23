#include "ThumbnailCache.h"
#include "AssetDatabase.h"
#include "Core/Paths.h"
#include "Log.h"

#include "stb_image.h"
#include "PngWrite.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

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
        if (std::string g = AssetDatabase::FindMetaGuid(ReadAll(it->path().string())); !g.empty()) out.insert(std::move(g));
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

bool LoadAnyVersion(const std::string& path, int size, std::vector<unsigned char>& outPixels) {
    if (path.empty() || AssetDatabase::IsSynthetic(path)) return false;
    const AssetGuid guid = AssetDatabase::GuidForPath(path);
    if (!guid.IsValid()) return false;
    const std::string cachePath = ProjectPaths::Resolve("Library/thumbnails/" + guid.ToString() + ".png");
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(cachePath.c_str(), &w, &h, &channels, 4);
    if (!data) return false;
    const bool ok = w == size && h == size;
    if (ok) outPixels.assign(data, data + (size_t)size * size * 4);
    stbi_image_free(data);
    return ok;
}

namespace {
// Temp file + rename (replaces the old file in one step), no flush: a lost cache entry only
// means a re-render. The temp name carries the thread so two writers never share one.
bool WriteCacheFile(const std::string& target, const std::string& bytes) {
    namespace fs = std::filesystem;
    const fs::path dst = fs::u8path(target);
    fs::path tmp = dst;
    tmp += ".tmp-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), (std::streamsize)bytes.size());
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, dst, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

void WriteThumbnail(const std::string& cachePath, int size, std::uint64_t key, const unsigned char* pixels) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(cachePath).parent_path(), ec);
    std::string png;
    // PNG first, then the key sidecar: a crash between them leaves the old key, which no longer
    // matches, so the entry is simply re-rendered.
    const bool ok = PngWrite::Encode(png, size, size, 4, pixels, size * 4, 6) &&
                    WriteCacheFile(cachePath, png) && WriteCacheFile(KeyPath(cachePath), KeyToString(key));
    if (!ok) Log::Warn("ThumbnailCache: couldn't write " + ProjectPaths::Relativize(cachePath));
}

// One worker for SaveAsync; jobs are written in order. The destructor (static teardown) drains
// the queue so thumbnails rendered just before exit still land.
class SaveWorker {
public:
    struct Job { std::string CachePath; int Size = 0; std::uint64_t Key = 0; std::vector<unsigned char> Pixels; };
    void Push(Job job) {
        {
            std::lock_guard<std::mutex> lk(m_Mutex);
            m_Jobs.push_back(std::move(job));
            if (!m_Thread.joinable()) m_Thread = std::thread([this] { Run(); });
        }
        m_Wake.notify_one();
    }
    void Flush() {
        std::unique_lock<std::mutex> lk(m_Mutex);
        m_Idle.wait(lk, [this] { return m_Jobs.empty() && !m_Busy; });
    }
    ~SaveWorker() {
        {
            std::lock_guard<std::mutex> lk(m_Mutex);
            m_Stop = true;
        }
        m_Wake.notify_one();
        if (m_Thread.joinable()) m_Thread.join();
    }
private:
    void Run() {
        std::unique_lock<std::mutex> lk(m_Mutex);
        for (;;) {
            m_Wake.wait(lk, [this] { return m_Stop || !m_Jobs.empty(); });
            if (m_Jobs.empty()) return; // stopping, nothing left
            Job job = std::move(m_Jobs.front());
            m_Jobs.pop_front();
            m_Busy = true;
            lk.unlock();
            WriteThumbnail(job.CachePath, job.Size, job.Key, job.Pixels.data());
            lk.lock();
            m_Busy = false;
            if (m_Jobs.empty()) m_Idle.notify_all();
        }
    }
    std::mutex m_Mutex;
    std::condition_variable m_Wake, m_Idle;
    std::deque<Job> m_Jobs;
    std::thread m_Thread;
    bool m_Busy = false, m_Stop = false;
};
SaveWorker& Worker() { static SaveWorker w; return w; }
} // namespace

void Save(const std::string& path, int size, std::uint64_t key, const unsigned char* pixels) {
    const std::string cachePath = CacheFilePath(path);
    if (cachePath.empty() || !pixels) return;
    WriteThumbnail(cachePath, size, key, pixels);
}

void SaveAsync(const std::string& path, int size, std::uint64_t key, std::vector<unsigned char> pixels) {
    if ((int)pixels.size() < size * size * 4) return;
    std::string cachePath = CacheFilePath(path); // AssetDatabase on the calling thread
    if (cachePath.empty()) return;
    Worker().Push({std::move(cachePath), size, key, std::move(pixels)});
}

void FlushAsync() { Worker().Flush(); }

struct AsyncProbe::Impl {
    struct Job { std::string Path; unsigned Ticket = 0; int Size = 0; std::vector<std::string> Deps; };
    std::mutex Mutex;
    std::condition_variable Wake, Idle;
    std::deque<Job> Jobs; // front = newest
    std::vector<Result> Done;
    std::thread Thread;
    bool Busy = false, Stop = false;

    void Run() {
        std::unique_lock<std::mutex> lk(Mutex);
        for (;;) {
            Wake.wait(lk, [this] { return Stop || !Jobs.empty(); });
            if (Stop) return;
            Job job = std::move(Jobs.front());
            Jobs.pop_front();
            Busy = true;
            lk.unlock();
            Result r{job.Path, job.Ticket, DependencyKey(job.Path, job.Deps), {}};
            Load(job.Path, job.Size, r.Key, r.Pixels);
            lk.lock();
            Done.push_back(std::move(r));
            Busy = false;
            if (Jobs.empty()) Idle.notify_all();
        }
    }
};

AsyncProbe::AsyncProbe() : m_Impl(std::make_unique<Impl>()) {}

AsyncProbe::~AsyncProbe() {
    {
        std::lock_guard<std::mutex> lk(m_Impl->Mutex);
        m_Impl->Stop = true;
        m_Impl->Jobs.clear();
    }
    m_Impl->Wake.notify_one();
    if (m_Impl->Thread.joinable()) m_Impl->Thread.join();
}

void AsyncProbe::Push(std::string path, unsigned ticket, int size, std::vector<std::string> deps) {
    {
        std::lock_guard<std::mutex> lk(m_Impl->Mutex);
        m_Impl->Jobs.push_front({std::move(path), ticket, size, std::move(deps)});
        if (!m_Impl->Thread.joinable()) m_Impl->Thread = std::thread([impl = m_Impl.get()] { impl->Run(); });
    }
    m_Impl->Wake.notify_one();
}

void AsyncProbe::Take(std::vector<Result>& out, size_t max) {
    std::lock_guard<std::mutex> lk(m_Impl->Mutex);
    const size_t n = std::min(max, m_Impl->Done.size());
    for (size_t i = 0; i < n; ++i) out.push_back(std::move(m_Impl->Done[i]));
    m_Impl->Done.erase(m_Impl->Done.begin(), m_Impl->Done.begin() + (std::ptrdiff_t)n);
}

void AsyncProbe::WaitIdle() {
    std::unique_lock<std::mutex> lk(m_Impl->Mutex);
    m_Impl->Idle.wait(lk, [this] { return m_Impl->Jobs.empty() && !m_Impl->Busy; });
}

void PruneOrphans() {
    namespace fs = std::filesystem;
    const fs::path dir = ProjectPaths::Resolve("Library/thumbnails");
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;

    // The project's GUIDs are the AssetDatabase's (its scan, which this waits for, read every .meta once already); the
    // engine's own assets get theirs on first use, so their small folder is read here. A thumbnail written after this
    // started is left alone: its asset may have been registered after the check below looked.
    std::unordered_set<std::string> engineLive;
    CollectMetaGuids(EnginePaths::Resolve("assets"), engineLive);
    const auto started = fs::file_time_type::clock::now();
    auto live = [&](const std::string& guid) {
        if (engineLive.count(guid)) return true;
        if (guid.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
        return !AssetDatabase::PathForGuid(AssetGuid::FromString(guid)).empty();
    };

    int removed = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::error_code tec;
        if (entry.last_write_time(tec) >= started && !tec) continue;
        const std::string name = entry.path().filename().string();
        // "{16 hex}.png" and its "{16 hex}.png.key" sidecar are cache entries; anything else
        // (e.g. a .meta an older ScanProject created for a thumbnail) is junk.
        const bool isPng = name.size() == 20 && name.compare(16, 4, ".png") == 0;
        const bool isKey = name.size() == 24 && name.compare(16, 8, ".png.key") == 0;
        std::string guid = (isPng || isKey) ? name.substr(0, 16) : std::string();
        for (char& c : guid) c = (char)std::tolower((unsigned char)c);
        if ((isPng || isKey) && live(guid)) continue;
        if (fs::remove(entry.path(), ec)) ++removed;
    }
    if (removed > 0) Log::Info("ThumbnailCache: pruned " + std::to_string(removed) + " orphaned file(s).");
}

} // namespace ThumbnailCache
