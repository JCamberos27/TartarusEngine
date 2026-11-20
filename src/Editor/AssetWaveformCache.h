#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "LruMap.h"

// Waveform envelopes for the Asset Browser's sound tiles. Getting one means decoding the whole
// file (AudioEngine::WaveformPeaks), tens of ms for a long clip, so it runs on a worker thread:
// Get() queues on first ask and returns null until the peaks land (Pump, once a frame); the tile
// keeps its type glyph meanwhile. The most recently asked-for are decoded first, so the tiles on
// screen fill in before ones scrolled past. Kept in an LRU (kMaxEntries); a changed file is
// dropped with Invalidate and decoded again on its next Get.
class AssetWaveformCache {
public:
    static constexpr int kBuckets = 48;

    AssetWaveformCache();
    ~AssetWaveformCache(); // joins the worker

    // Null while pending; an empty vector when the file can't be decoded (kept, so it isn't re-probed).
    const std::vector<float>* Get(const std::string& path);
    // The decoded peaks if they are in, without asking for them or counting as a use (tests).
    const std::vector<float>* Peek(const std::string& path);
    // Main thread, once a frame: moves finished decodes in.
    void Pump();
    // The file changed: drop its peaks so the next Get() decodes it again.
    void Invalidate(const std::string& path);
    void Clear();

private:
    static constexpr size_t kMaxEntries = 1024, kMaxQueued = 256;
    struct Job { std::string Path; unsigned Ticket; };
    struct Result { std::string Path; std::vector<float> Peaks; unsigned Ticket; };
    struct Entry { std::vector<float> Peaks; bool Pending = true; unsigned Ticket = 0; };

    void Worker();

    LruMap<Entry> m_Entries;   // main thread only
    unsigned m_NextTicket = 1; // a result is used only by the request that asked for it

    std::mutex m_Mutex; // guards everything below
    std::condition_variable m_Wake;
    std::deque<Job> m_Jobs; // front = newest request
    std::vector<Result> m_Done;
    bool m_Stop = false;
    std::thread m_Thread;
};
