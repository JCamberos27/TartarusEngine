#include "AssetThumbnailLoader.h"
#include "ThumbnailCache.h"
#include "GLStateCache.h"
#include "gl.h"

#include "stb_image.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr size_t kMaxThumbs = 512;   // 64 KB each
constexpr size_t kMaxQueued = 256;   // oldest requests (long scrolled past) are dropped beyond this
constexpr int kWorkers = 2;

// `path` decoded and box-filtered into a kSize x kSize RGBA square, centred, aspect kept.
bool MakeImageThumb(const std::string& path, std::vector<unsigned char>& out) {
    const int size = AssetThumbnailLoader::kSize;
    int w = 0, h = 0, n = 0;
    unsigned char* src = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!src) return false;
    const float scale = std::min((float)size / (float)w, (float)size / (float)h);
    const int dw = std::max(1, (int)std::lround(w * scale)), dh = std::max(1, (int)std::lround(h * scale));
    const int ox = (size - dw) / 2, oy = (size - dh) / 2;
    out.assign((size_t)size * size * 4, 0);
    for (int y = 0; y < dh; ++y) {
        const int y0 = (int)((long long)y * h / dh), y1 = std::max(y0 + 1, (int)((long long)(y + 1) * h / dh));
        for (int x = 0; x < dw; ++x) {
            const int x0 = (int)((long long)x * w / dw), x1 = std::max(x0 + 1, (int)((long long)(x + 1) * w / dw));
            unsigned sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                const unsigned char* p = src + ((size_t)sy * w + x0) * 4;
                for (int sx = x0; sx < x1; ++sx, p += 4)
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
            }
            const unsigned count = (unsigned)((y1 - y0) * (x1 - x0));
            unsigned char* d = out.data() + ((size_t)(oy + y) * size + (ox + x)) * 4;
            for (int c = 0; c < 4; ++c) d[c] = (unsigned char)((sum[c] + count / 2) / count);
        }
    }
    stbi_image_free(src);
    return true;
}
} // namespace

AssetThumbnailLoader::AssetThumbnailLoader() {
    for (int i = 0; i < kWorkers; ++i) m_Threads.emplace_back([this] { Worker(); });
}

AssetThumbnailLoader::~AssetThumbnailLoader() {
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Stop = true;
        m_Jobs.clear();
    }
    m_Wake.notify_all();
    for (std::thread& t : m_Threads) t.join();
}

void AssetThumbnailLoader::Worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_Mutex);
            m_Wake.wait(lk, [&] { return m_Stop || !m_Jobs.empty(); });
            if (m_Stop) return;
            job = std::move(m_Jobs.front());
            m_Jobs.pop_front();
        }
        Result r{job.Path, {}, job.From == Source::Cached, job.Ticket};
        if (job.From == Source::Cached) {
            ThumbnailCache::LoadAnyVersion(job.Path, kSize, r.Pixels);
        } else {
            // Keyed by the image's mtime and .meta, so an edited file is decoded again.
            const std::uint64_t key = ThumbnailCache::DependencyKey(job.Path, {});
            if (!ThumbnailCache::Load(job.Path, kSize, key, r.Pixels) && MakeImageThumb(job.Path, r.Pixels))
                ThumbnailCache::Save(job.Path, kSize, key, r.Pixels.data());
        }
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Done.push_back(std::move(r));
    }
}

AssetThumbnailLoader::Thumb AssetThumbnailLoader::Get(const std::string& path, Source source) {
    auto it = m_Entries.find(path);
    if (it != m_Entries.end()) {
        m_Lru.splice(m_Lru.begin(), m_Lru, it->second.Lru);
        return it->second.T;
    }
    Entry e;
    e.Ticket = m_NextTicket++;
    m_Lru.push_front(path);
    e.Lru = m_Lru.begin();
    m_Entries.emplace(path, e);
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Jobs.push_front({path, source, e.Ticket});
        if (m_Jobs.size() > kMaxQueued) {
            // Forget the dropped requests too, so asking again re-queues them.
            const std::string dropped = m_Jobs.back().Path;
            m_Jobs.pop_back();
            if (auto d = m_Entries.find(dropped); d != m_Entries.end() && d->second.Pending) Remove(d);
        }
    }
    m_Wake.notify_one();
    if (m_Entries.size() > kMaxThumbs) Remove(m_Entries.find(m_Lru.back()));
    return {};
}

void AssetThumbnailLoader::Pump(int maxUploads) {
    std::vector<Result> done;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        if (m_Done.empty()) return;
        const size_t take = std::min(m_Done.size(), (size_t)std::max(maxUploads, 0));
        done.assign(std::make_move_iterator(m_Done.begin()), std::make_move_iterator(m_Done.begin() + take));
        m_Done.erase(m_Done.begin(), m_Done.begin() + take);
    }
    bool uploaded = false;
    for (Result& r : done) {
        auto it = m_Entries.find(r.Path);
        if (it == m_Entries.end() || !it->second.Pending || it->second.Ticket != r.Ticket) continue; // evicted / superseded
        Entry& e = it->second;
        e.Pending = false;
        e.T.FlipV = r.FlipV;
        if (r.Pixels.size() != (size_t)kSize * kSize * 4) continue; // no thumbnail: the tile keeps its glyph
        glGenTextures(1, &e.T.Tex);
        glBindTexture(GL_TEXTURE_2D, e.T.Tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, r.Pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        uploaded = true;
    }
    if (uploaded) GLStateCache::Invalidate(); // raw glBindTexture above
}

void AssetThumbnailLoader::Remove(std::unordered_map<std::string, Entry>::iterator it) {
    if (it == m_Entries.end()) return;
    if (it->second.T.Tex) glDeleteTextures(1, &it->second.T.Tex);
    m_Lru.erase(it->second.Lru);
    m_Entries.erase(it);
}

void AssetThumbnailLoader::Invalidate(const std::string& path) {
    Remove(m_Entries.find(path));
}

void AssetThumbnailLoader::Clear() {
    for (auto& [path, e] : m_Entries) { (void)path; if (e.T.Tex) glDeleteTextures(1, &e.T.Tex); }
    m_Entries.clear();
    m_Lru.clear();
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Jobs.clear();
    m_Done.clear();
}
