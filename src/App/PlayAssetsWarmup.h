#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "Audio/AudioEngine.h"
#include "Renderer/BloodRenderer.h"
#include "Renderer/KnifeFxLibrary.h"

class World;

// P-56: the first Play of a process decoded every weapon sound (~1.7 s in the Forest) and read the blood and Knife
// effect files (~0.35 s) on the main thread, inside PlaySession::Start. Once a scene that Play arms is open (it has a
// First Person Controller) and its loads have landed, one below-normal-priority worker does that file work instead:
// the sounds join the preload table as soon as they are decoded (Tick), and the effect files go to their Load at Play
// (FinishForPlay), which waits only for what is left. Once per process, as those loads are: later Plays found them
// done anyway. A sound or file the worker didn't cover still loads at Play, as before.
class PlayAssetsWarmup {
public:
    static PlayAssetsWarmup& Get();

    // Main thread, once a frame (editor, player and the test harnesses alike): starts the worker when `world` has a
    // First Person Controller, nothing is loading (`asyncInFlight` 0) and Play isn't running; adopts the decoded sounds
    // once it is done.
    void Tick(const World& world, int asyncInFlight, bool playing);
    // Main thread, at Play, before the effects and the weapon audio start: waits for a running worker, adopts its sounds
    // and loads the blood + Knife effects from what it read (from disk, as before, when it never ran).
    void FinishForPlay();
    // Cancels and joins the worker (process exit, before the audio engine goes).
    void Shutdown();

    // The worker is a queue served by two lowest-priority threads (one under 6 hardware threads), shared by every Play warm-up (the sounds and effect files
    // here, the motion database's prefetch: SharedMotionDatabase::Prefetch). A job runs only while Tick last saw nothing
    // loading and Play not running, so it never competes with a scene open's loads; first in, first out. `cancel` is set
    // at Shutdown: a long job checks it between its steps. Main thread; starts the worker on the first job.
    using Job = std::function<void(const std::atomic<bool>& cancel)>;
    void Enqueue(Job job);
    // Any thread, around a wait on the worker's work: it runs at normal priority while anything waits on it (the main
    // thread mustn't sit behind a lowest-priority thread), back to lowest at the last EndWait.
    void BeginWait();
    void EndWait();
    ~PlayAssetsWarmup();

    struct Stats {
        bool Ran = false;           // the worker ran (and finished)
        int Sounds = 0;             // sounds it decoded (failures included)
        int SoundsAdopted = 0;      // joined the preload table (the rest were there already, or failed)
        std::size_t SoundBytes = 0; // decoded PCM
        std::size_t BloodBytes = 0, KnifeBytes = 0; // file data held from the worker until Play uploads it
        double WorkerMs = 0.0;      // the worker's own time
        double WaitMs = 0.0;        // what FinishForPlay waited for it
    };
    const Stats& LastStats() const { return m_Stats; }

    // Every sound file the audio manifest's keys name, resolved as WeaponAudio preloads them (ProjectPaths::Resolve),
    // each once, in the manifest's order. Pure: no files read.
    static std::vector<std::string> ManifestSoundFiles(const std::string& manifestJson);

private:
    PlayAssetsWarmup() = default;
    void Adopt(); // main thread, the warm-up job done (m_Done)
    void StartIfDue(const World& world, int asyncInFlight, bool playing); // Tick: adopts, or queues the warm-up job
    void StartWorker(); // main thread, m_Mutex held
    void Loop();        // the worker

    void SetPriority(bool normal); // every worker thread; m_Mutex held
    std::vector<std::thread> m_Threads;
    std::mutex m_Mutex; // m_Queue, m_Idle, m_Stop, m_Waiters; m_DoneCv
    std::condition_variable m_Cv, m_DoneCv;
    std::deque<Job> m_Queue;
    bool m_Idle = false; // Tick: nothing loading, not playing
    bool m_Stop = false;
    int m_Waiters = 0;
    // The warm-up job is claimed once: by the worker (it runs) or by FinishForPlay while still queued (Play reads the
    // files itself, as before).
    std::atomic<bool> m_Claimed{false};
    std::atomic<bool> m_Hurry{false}; // something waits on the worker (BeginWait): its jobs skip their breathers
    std::atomic<bool> m_Cancel{false};
    std::atomic<bool> m_Done{false};
    bool m_Started = false;
    bool m_Adopted = false;
    // Written by the worker until m_Done; read by the main thread after it.
    std::shared_ptr<AudioEngine::DecodedSounds> m_Sounds;
    std::shared_ptr<BloodRenderer::FileData> m_Blood;
    std::shared_ptr<KnifeFxLibrary::FileData> m_Knife;
    double m_WorkerMs = 0.0;
    Stats m_Stats;
};
