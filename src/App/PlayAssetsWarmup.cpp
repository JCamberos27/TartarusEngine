#include "App/PlayAssetsWarmup.h"

#include "Audio/SoundManifest.h"
#include "Components.h"
#include "Core/Paths.h"
#include "Core/Profiler.h"
#include "Log.h"
#include "World.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
constexpr std::chrono::microseconds kPause{1000};
double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
std::string Megabytes(std::size_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return text;
}
} // namespace

PlayAssetsWarmup& PlayAssetsWarmup::Get() {
    static PlayAssetsWarmup* w = new PlayAssetsWarmup(); // leaked: Shutdown() joins it while the engine is still up
    return *w;
}

PlayAssetsWarmup::~PlayAssetsWarmup() { Shutdown(); }

std::vector<std::string> PlayAssetsWarmup::ManifestSoundFiles(const std::string& manifestJson) {
    std::vector<std::string> out;
    SoundManifest manifest;
    if (!SoundManifest::FromJson(manifestJson, manifest)) return out;
    std::unordered_set<std::string> seen;
    for (const std::string& key : manifest.Keys())
        for (const std::string& f : manifest.FilesFor(key)) {
            std::string path = ProjectPaths::Resolve(f); // as WeaponAudio's Preload(f) -> AudioEngine::Load
            if (seen.insert(path).second) out.push_back(std::move(path));
        }
    return out;
}

void PlayAssetsWarmup::Enqueue(Job job) {
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stop) return;
        m_Queue.push_back(std::move(job));
        // Queued mid-frame (a scene open that just started its loads): wait for the next Tick to see it idle.
        m_Idle = false;
        StartWorker();
    }
    m_Cv.notify_all();
}

void PlayAssetsWarmup::SetPriority(bool normal) { // m_Mutex held
#ifdef _WIN32
    for (std::thread& t : m_Threads)
        SetThreadPriority((HANDLE)t.native_handle(), normal ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_LOWEST);
#else
    (void)normal;
#endif
}

void PlayAssetsWarmup::StartWorker() {
    if (!m_Threads.empty()) return;
    // Two threads, so the sounds and the body's database (each ~1.6-2 s of work) are both done by a quick first Play
    // instead of one after the other (one thread: whichever went second made Play wait 0.3-1.5 s for it). Lowest, with a breather after each step (kPause): the editor's (or the load
    // screen's) own frames come first; this only has to be done before Play.
    // A small machine (under 6 hardware threads) keeps one: its editor frames don't share the cores with two decoders,
    // and the body's database (queued at scene open, the bigger Play-entry stall) goes first.
    const int workers = std::thread::hardware_concurrency() >= 6 ? 2 : 1;
    for (int i = 0; i < workers; ++i) m_Threads.emplace_back([this] { Loop(); });
    SetPriority(m_Waiters > 0);
}

void PlayAssetsWarmup::Loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Cv.wait(lock, [&] { return m_Stop || (m_Idle && !m_Queue.empty()); });
            if (m_Stop) return;
            job = std::move(m_Queue.front());
            m_Queue.pop_front();
        }
        job(m_Cancel);
    }
}

void PlayAssetsWarmup::BeginWait() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Waiters == 0) SetPriority(true);
    ++m_Waiters;
    m_Hurry.store(true, std::memory_order_relaxed);
}

void PlayAssetsWarmup::EndWait() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Waiters > 0 && --m_Waiters == 0) {
        m_Hurry.store(false, std::memory_order_relaxed);
        SetPriority(false);
    }
}

void PlayAssetsWarmup::Tick(const World& world, int asyncInFlight, bool playing) {
    StartIfDue(world, asyncInFlight, playing); // (queues its job, which closes the gate until this Tick re-opens it)
    const bool idle = asyncInFlight == 0 && !playing;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (idle != m_Idle) {
        m_Idle = idle;
        if (idle) m_Cv.notify_all();
    }
}

void PlayAssetsWarmup::StartIfDue(const World& world, int asyncInFlight, bool playing) {
    if (m_Started) {
        if (!m_Adopted && m_Done.load(std::memory_order_acquire)) Adopt();
        return;
    }
    // Play would load these itself now (or already did: later Plays find them loaded).
    // (Tried, not Loaded: a checkout without the effect data never loads them, and the worker would read it all again
    // after every Stop.)
    if (playing || asyncInFlight > 0 || BloodRenderer::Get().LoadTried() || KnifeFxLibrary::Get().LoadTried()) return;
    const auto controllers = world.Registry.view<FirstPersonControllerComponent>();
    if (controllers.begin() == controllers.end()) return;
    m_Started = true;
    const AudioEngine::PreloadFormat format = AudioEngine::PreloadDecodeFormat(); // the engine's: main thread
    Enqueue([this, format](const std::atomic<bool>&) {
        if (m_Claimed.exchange(true)) return; // FinishForPlay got to it first
        const auto t0 = std::chrono::steady_clock::now();
        std::string manifest;
        {
            std::ifstream in(std::filesystem::u8path(ProjectPaths::Resolve("assets/Audio/audio_manifest.json")), std::ios::binary);
            if (in) {
                std::stringstream ss;
                ss << in.rdbuf();
                manifest = ss.str();
            }
        }
        m_Sounds = AudioEngine::DecodeForPreload(ManifestSoundFiles(manifest), format, &m_Cancel, kPause, &m_Hurry);
        if (!m_Cancel.load(std::memory_order_relaxed)) m_Blood = BloodRenderer::ReadFiles(&m_Cancel);
        if (!m_Cancel.load(std::memory_order_relaxed)) m_Knife = KnifeFxLibrary::ReadFiles(&m_Cancel);
        m_WorkerMs = MsSince(t0);
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Done.store(true, std::memory_order_release);
        }
        m_DoneCv.notify_all();
    });
}

void PlayAssetsWarmup::Adopt() {
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_DoneCv.wait(lock, [&] { return m_Done.load(std::memory_order_acquire); });
    }
    m_Adopted = true;
    if (m_Cancel.load(std::memory_order_relaxed)) { // part-read: Play reads everything itself, as before
        m_Sounds.reset();
        m_Blood.reset();
        m_Knife.reset();
        return;
    }
    m_Stats.Ran = true;
    m_Stats.WorkerMs = m_WorkerMs;
    if (m_Sounds) {
        m_Stats.Sounds = AudioEngine::DecodedCount(*m_Sounds);
        m_Stats.SoundBytes = AudioEngine::DecodedBytes(*m_Sounds);
        m_Stats.SoundsAdopted = AudioEngine::AdoptPreloaded(*m_Sounds);
        m_Sounds.reset();
    }
    m_Stats.BloodBytes = m_Blood ? BloodRenderer::Bytes(*m_Blood) : 0;
    m_Stats.KnifeBytes = m_Knife ? KnifeFxLibrary::Bytes(*m_Knife) : 0;
    Log::Info("Play assets warm-up: " + std::to_string(m_Stats.SoundsAdopted) + " of " + std::to_string(m_Stats.Sounds) +
              " sounds decoded (" + Megabytes(m_Stats.SoundBytes) + "), blood " + Megabytes(m_Stats.BloodBytes) +
              " + Knife " + Megabytes(m_Stats.KnifeBytes) + " read for Play, in " + std::to_string((int)m_Stats.WorkerMs) +
              " ms (worker)");
}

void PlayAssetsWarmup::FinishForPlay() {
    PROFILE_SCOPE("Play Assets Warm-up");
    if (m_Started && !m_Adopted) {
        if (!m_Claimed.exchange(true)) {
            m_Adopted = true; // still queued (behind another job, or loads kept it waiting): Play reads it all itself
        } else {
            const auto t0 = std::chrono::steady_clock::now();
            BeginWait();
            Adopt(); // Play waits only for what the worker has left
            EndWait();
            m_Stats.WaitMs = MsSince(t0);
        }
    }
    // From the worker's files when it read them (only the upload is left), else from disk as before. Their file data
    // goes once uploaded: the GPU copy is what Play uses.
    BloodRenderer::Get().Load(m_Blood.get());
    KnifeFxLibrary::Get().Load(m_Knife.get());
    m_Blood.reset();
    m_Knife.reset();
}

void PlayAssetsWarmup::Shutdown() {
    m_Cancel.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stop = true;
        m_Queue.clear(); // jobs not started never run
    }
    m_Cv.notify_all();
    for (std::thread& t : m_Threads)
        if (t.joinable()) t.join();
    m_Threads.clear();
    m_Sounds.reset();
    m_Blood.reset();
    m_Knife.reset();
}
