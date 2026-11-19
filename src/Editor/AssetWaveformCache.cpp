#include "AssetWaveformCache.h"

#include "AudioEngine.h"

AssetWaveformCache::AssetWaveformCache() : m_Thread([this] { Worker(); }) {}

AssetWaveformCache::~AssetWaveformCache() {
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Stop = true;
        m_Jobs.clear();
    }
    m_Wake.notify_all();
    m_Thread.join();
}

void AssetWaveformCache::Worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_Mutex);
            m_Wake.wait(lk, [&] { return m_Stop || !m_Jobs.empty(); });
            if (m_Stop) return;
            job = std::move(m_Jobs.front());
            m_Jobs.pop_front();
        }
        Result r{std::move(job.Path), {}, job.Ticket};
        AudioEngine::WaveformPeaks(r.Path, kBuckets, r.Peaks); // own decoder: safe off the main thread
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Done.push_back(std::move(r));
    }
}

const std::vector<float>* AssetWaveformCache::Get(const std::string& path) {
    auto it = m_Entries.find(path);
    if (it != m_Entries.end()) {
        m_Lru.splice(m_Lru.begin(), m_Lru, it->second.Lru);
        return it->second.Pending ? nullptr : &it->second.Peaks;
    }
    Entry e;
    const unsigned ticket = e.Ticket = m_NextTicket++;
    m_Lru.push_front(path);
    e.Lru = m_Lru.begin();
    m_Entries.emplace(path, std::move(e));
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Jobs.push_front({path, ticket});
        if (m_Jobs.size() > kMaxQueued) {
            // Forget the dropped request too, so asking again re-queues it.
            const std::string dropped = m_Jobs.back().Path;
            m_Jobs.pop_back();
            if (auto d = m_Entries.find(dropped); d != m_Entries.end() && d->second.Pending) Remove(d);
        }
    }
    m_Wake.notify_one();
    if (m_Entries.size() > kMaxEntries) Remove(m_Entries.find(m_Lru.back()));
    return nullptr;
}

void AssetWaveformCache::Pump() {
    std::vector<Result> done;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        if (m_Done.empty()) return;
        done.swap(m_Done);
    }
    for (Result& r : done) {
        auto it = m_Entries.find(r.Path);
        if (it == m_Entries.end() || !it->second.Pending || it->second.Ticket != r.Ticket) continue; // evicted / superseded
        it->second.Pending = false;
        it->second.Peaks = std::move(r.Peaks);
    }
}

void AssetWaveformCache::Remove(std::unordered_map<std::string, Entry>::iterator it) {
    if (it == m_Entries.end()) return;
    m_Lru.erase(it->second.Lru);
    m_Entries.erase(it);
}

void AssetWaveformCache::Invalidate(const std::string& path) { Remove(m_Entries.find(path)); }

void AssetWaveformCache::Clear() {
    m_Entries.clear();
    m_Lru.clear();
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Jobs.clear();
    m_Done.clear();
}
