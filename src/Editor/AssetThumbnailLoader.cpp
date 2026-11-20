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

// Mask thumbnails are cached in the entries and queues under the file path plus this tag.
constexpr char kMaskTag = '';
std::string MaskKey(const std::string& path, bool inverted) { return path + kMaskTag + (inverted ? 'i' : 'm'); }

// The thumbnail's pixels turned into white paint whose opacity is the luminance (1 - it, inverted).
void LumaToAlpha(std::vector<unsigned char>& px, bool inverted) {
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        const float l = (0.2126f * px[i] + 0.7152f * px[i + 1] + 0.0722f * px[i + 2]) * (px[i + 3] / 255.0f) / 255.0f;
        px[i] = px[i + 1] = px[i + 2] = 255;
        px[i + 3] = (unsigned char)std::lround((inverted ? px[i + 3] / 255.0f - l : l) * 255.0f);
    }
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
            // A mask thumbnail's key is the file plus a tag; it is made from the file's image thumbnail.
            const bool mask = job.From == Source::Mask || job.From == Source::MaskInverted;
            const std::string file = mask ? job.Path.substr(0, job.Path.find(kMaskTag)) : job.Path;
            // Keyed by the image's mtime and .meta, so an edited file is decoded again.
            const std::uint64_t key = ThumbnailCache::DependencyKey(file, {});
            if (!ThumbnailCache::Load(file, kSize, key, r.Pixels) && MakeImageThumb(file, r.Pixels))
                ThumbnailCache::Save(file, kSize, key, r.Pixels.data());
            if (mask && !r.Pixels.empty()) LumaToAlpha(r.Pixels, job.From == Source::MaskInverted);
        }
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Done.push_back(std::move(r));
    }
}

AssetThumbnailLoader::Thumb AssetThumbnailLoader::Get(const std::string& pathIn, Source source) {
    const bool mask = source == Source::Mask || source == Source::MaskInverted;
    const std::string path = mask ? MaskKey(pathIn, source == Source::MaskInverted) : pathIn;
    if (const Entry* found = m_Entries.Find(path)) return found->T;
    Entry e;
    e.Ticket = m_NextTicket++;
    m_Entries.Insert(path, e);
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Jobs.push_front({path, source, e.Ticket});
        if (m_Jobs.size() > kMaxQueued) {
            // Forget the dropped requests too, so asking again re-queues them.
            const std::string dropped = m_Jobs.back().Path;
            m_Jobs.pop_back();
            if (const Entry* d = m_Entries.Peek(dropped); d && d->Pending) Remove(dropped);
        }
    }
    m_Wake.notify_one();
    m_Entries.TrimTo(kMaxThumbs, [](Entry& old) { if (old.T.Tex) glDeleteTextures(1, &old.T.Tex); });
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
        Entry* found = m_Entries.Peek(r.Path);
        if (!found || !found->Pending || found->Ticket != r.Ticket) continue; // evicted / superseded
        Entry& e = *found;
        e.Pending = false;
        e.T.FlipV = r.FlipV;
        if (r.Pixels.size() != (size_t)kSize * kSize * 4) continue; // no thumbnail: the tile keeps its glyph
        e.T.Tex = MakeTexture(kSize, r.Pixels.data());
        uploaded = true;
    }
    if (uploaded) GLStateCache::Invalidate(); // raw glBindTexture above
}

unsigned int AssetThumbnailLoader::MakeTexture(int size, const unsigned char* pixels) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

void AssetThumbnailLoader::Remove(const std::string& key) {
    m_Entries.Erase(key, [](Entry& e) { if (e.T.Tex) glDeleteTextures(1, &e.T.Tex); });
}

void AssetThumbnailLoader::Invalidate(const std::string& path) {
    Remove(path);
    Remove(MaskKey(path, false));
    Remove(MaskKey(path, true));
}

void AssetThumbnailLoader::Clear() {
    m_Entries.ForEach([](Entry& e) { if (e.T.Tex) glDeleteTextures(1, &e.T.Tex); });
    m_Entries.Clear();
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Jobs.clear();
    m_Done.clear();
}
