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

void PlayAssetsWarmup::Tick(const World& world, int asyncInFlight, bool playing) {
    if (m_Started) {
        if (!m_Adopted && m_Done.load(std::memory_order_acquire)) Adopt();
        return;
    }
    // Play would load these itself now (or already did: later Plays find them loaded).
    if (playing || asyncInFlight > 0 || BloodRenderer::Get().Loaded() || KnifeFxLibrary::Get().Loaded()) return;
    const auto controllers = world.Registry.view<FirstPersonControllerComponent>();
    if (controllers.begin() == controllers.end()) return;
    m_Started = true;
    const AudioEngine::PreloadFormat format = AudioEngine::PreloadDecodeFormat(); // the engine's: main thread
    m_Thread = std::thread([this, format] {
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
        m_Sounds = AudioEngine::DecodeForPreload(ManifestSoundFiles(manifest), format, &m_Cancel);
        if (!m_Cancel.load(std::memory_order_relaxed)) m_Blood = BloodRenderer::ReadFiles(&m_Cancel);
        if (!m_Cancel.load(std::memory_order_relaxed)) m_Knife = KnifeFxLibrary::ReadFiles(&m_Cancel);
        m_WorkerMs = MsSince(t0);
        m_Done.store(true, std::memory_order_release);
    });
#ifdef _WIN32
    // Below normal: the editor's (or the load screen's) own frames come first; this only has to be done before Play.
    SetThreadPriority((HANDLE)m_Thread.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

void PlayAssetsWarmup::Adopt() {
    if (m_Thread.joinable()) m_Thread.join();
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
        const auto t0 = std::chrono::steady_clock::now();
        Adopt(); // joins: Play waits only for what the worker has left
        m_Stats.WaitMs = MsSince(t0);
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
    if (m_Thread.joinable()) m_Thread.join();
    m_Sounds.reset();
    m_Blood.reset();
    m_Knife.reset();
}
