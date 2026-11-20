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
    if (const Entry* found = m_Entries.Find(path)) return found->Pending ? nullptr : &found->Peaks;
    Entry e;
    const unsigned ticket = e.Ticket = m_NextTicket++;
    m_Entries.Insert(path, std::move(e));
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Jobs.push_front({path, ticket});
        if (m_Jobs.size() > kMaxQueued) {
            // Forget the dropped request too, so asking again re-queues it.
            const std::string dropped = m_Jobs.back().Path;
            m_Jobs.pop_back();
            if (const Entry* d = m_Entries.Peek(dropped); d && d->Pending) m_Entries.Erase(dropped);
        }
    }
    m_Wake.notify_one();
    m_Entries.TrimTo(kMaxEntries);
    return nullptr;
}

const std::vector<float>* AssetWaveformCache::Peek(const std::string& path) {
    const Entry* e = m_Entries.Peek(path);
    return e && !e->Pending ? &e->Peaks : nullptr;
}

void AssetWaveformCache::Pump() {
    std::vector<Result> done;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        if (m_Done.empty()) return;
        done.swap(m_Done);
    }
    for (Result& r : done) {
        Entry* e = m_Entries.Peek(r.Path);
        if (!e || !e->Pending || e->Ticket != r.Ticket) continue; // evicted / superseded
        e->Pending = false;
        e->Peaks = std::move(r.Peaks);
    }
}

void AssetWaveformCache::Invalidate(const std::string& path) { m_Entries.Erase(path); }

void AssetWaveformCache::Clear() {
    m_Entries.Clear();
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Jobs.clear();
    m_Done.clear();
}
