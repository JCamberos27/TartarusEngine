#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
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
    void Adopt(); // main thread, the worker done (joined)

    std::thread m_Thread;
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
