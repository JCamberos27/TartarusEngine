#pragma once

// Shared internals of the audio engine's translation units (AudioEngine, AudioGraph, AudioVoices, AudioReverbBus, EditorAudio).
// Not part of the public API: callers use AudioEngine.h.

#include "AudioEngine.h"
#include "ConvolutionReverb.h"
#include "LoudnessMeter.h"
#include "MasterLimiter.h"
#include "miniaudio.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace AudioInternal {

// Fully-decoded PCM data for a preloaded sound (#202), kept in the engine's own output format so
// playing it needs no runtime resample/convert. Shared (#171): every voice playing the clip reads
// this one buffer through a non-owning ma_audio_buffer and holds a reference, so Unload()ing a
// path can never invalidate a voice already playing from it. Each Play() used to deep-copy the
// whole PCM buffer, allocating megabytes per rapid-fire SFX shot.
struct PreloadedClip {
    std::vector<uint8_t> Data;
    ma_format Format = ma_format_unknown;
    ma_uint32 Channels = 0;
    ma_uint32 SampleRate = 0;
    ma_uint64 FrameCount = 0;
};

// The occlusion low-pass: a miniaudio low-pass filter in a node of our own, so a new cutoff is applied (ma_lpf_reinit, which keeps
// the filter's state) by the audio thread itself between two blocks, never under it. The game thread only states the target.
struct OcclusionNode {
    ma_node_base Base;
    ma_lpf Lpf;
    ma_uint32 Rate = 48000;
    std::atomic<double> Target{20000.0};
    double Applied = 20000.0; // audio thread
};

// One voice slot per live (or recently live) sound. Slots are recycled rather than erased so a
// handle's index stays meaningful; Generation is bumped on every reuse so handles into a
// previous occupant of the slot no longer resolve.
struct Voice {
    std::unique_ptr<ma_sound> Sound;         // null when the slot is free
    std::unique_ptr<OcclusionNode> Lpf;      // occlusion low-pass, between the sound and its splitter / bus
    double LpfHz = 0.0;                      // its cutoff target (SetOcclusion skips an update that changes nothing)
    std::unique_ptr<ma_splitter_node> Split; // output 0: the bus, output 1: the reverb send (its volume is the send level)
    std::unique_ptr<ma_audio_buffer> Buffer; // non-null only for a voice playing preloaded data (#202)
    std::shared_ptr<const PreloadedClip> Clip; // keeps Buffer's PCM alive past Unload() (#171)
    uint16_t Generation = 1;                 // never 0, so a live handle is never InvalidHandle
    bool Looping = false;
    bool Paused = false;                     // stopped by SetPaused(true); resumed, not reaped (#171)
};

// --- the output graph ----------------------------------------------------------------------------------------------
// sound groups (the mixer buses) -> a meter node each -> the MASTER LIMITER node -> the endpoint, and the two reverb returns -> the limiter.
// All three kinds are pass-through / processing nodes written here; miniaudio sums what is attached to an input bus.

// Taps (the --audio-test, AUDIO_CAPTURE): three equal rings of stereo frames. The reverb nodes add their return at the position the
// limiter is about to write; the limiter writes the other two and advances the count (they run in that order within one graph read).
struct TapRing {
    static constexpr unsigned long long kFrames = 1ull << 19; // 10.9 s at 48 kHz
    std::vector<float> Master, Pre, Wet;                      // kFrames x 2 each
    std::atomic<unsigned long long> Written{0};
    unsigned long long Read = 0;
};

struct MeterNode {
    ma_node_base Base;
    LoudnessMeter* Meter = nullptr;
};
struct LimiterNode {
    ma_node_base Base;
    MasterLimiter* Lim = nullptr;
    LoudnessMeter* Master = nullptr;
    std::vector<float> Scratch;
    // Settings from the game thread: written under Lock, picked up by the audio thread when Dirty and the lock is free (it never waits).
    std::mutex Lock;
    LimiterSettings Pending;
    std::atomic<bool> Dirty{false};
    std::atomic<float> LastGrDb{0.0f}, LastGlueGrDb{0.0f};
    std::atomic<float> Gain{1.0f};  // master volume x mute (applied here, ramped per block, not as the node's output bus volume)
    float GainPrev = -1.0f;         // audio thread: the gain at the end of the last block (< 0: none yet)
};

// --- the reverb bus ------------------------------------------------------------------------------------------------
// One node, input bus 0 = every send (miniaudio sums what is attached to an input bus), output bus 0 -> the limiter.
// The game thread states where the reverb is heading (ConvolutionReverb::SetTarget); the audio callback glides there.
struct ReverbNode {
    ma_node_base Base;
    ConvolutionReverb* Verb = nullptr;
    LoudnessMeter* Meter = nullptr;
    std::atomic<float> Return{1.0f};    // linear gain of the wet signal (the reverb return level x the SFX bus volume)
    std::atomic<bool> Enabled{true};
    bool WasEnabled = true;             // audio thread: Enabled at the last callback (the switch-off resets the reverb)
    float PrevGain = -1.0f;             // audio thread: Return at the end of the last block (< 0: none yet)
    std::atomic<std::uint64_t> Callbacks{0};
    std::atomic<double> TotalMicros{0.0}, MaxMicros{0.0};
    std::atomic<int> LastFrames{0};
    std::map<std::string, int> IrIndex; // game thread only: path -> the convolver's index
    std::vector<std::string> IrPaths;   // game thread only: by index
    std::set<std::string> IrFullWarned; // game thread only: paths already warned about for want of a free convolver slot
};

// The editor's UI sounds: their own group, through a clip guard to the endpoint, beside the limiter rather than through it.
// Clips are a store of their own (apart from Load()'s game clips): decoded once at the file's own channel count, miniaudio's
// buffer kept as is. Shared: a voice holds a reference, so UnloadClip never pulls PCM out from under a voice still playing it.
struct EditorClip {
    float* Pcm = nullptr; // from ma_decode_file; ma_free'd with the clip
    ma_uint32 Channels = 0;
    ma_uint32 SampleRate = 0;
    ma_uint64 FrameCount = 0;
    std::string Path;
    EditorClip() = default;
    EditorClip(const EditorClip&) = delete;
    EditorClip& operator=(const EditorClip&) = delete;
    ~EditorClip() { ma_free(Pcm, nullptr); }
};

// Fixed slots, the ma_sound and its buffer in place: a play allocates no voice. Twice the voice cap, so a voice fading out
// after being stolen (it keeps its slot until the fade ends) never blocks the play that stole it.
struct EditorVoice {
    ma_sound Sound;
    ma_audio_buffer Buffer;
    std::shared_ptr<const EditorClip> Clip; // null when the slot is free
    uint64_t Order = 0;                     // when it started: the oldest is stolen first
    bool Stolen = false;                    // fading out to make room: freed by the reap once it has stopped
};
constexpr int kEditorSlots = 2 * AudioEngine::kEditorVoices;
constexpr unsigned kEditorStealFadeMs = 8; // a voice cut mid-sound fades over this instead of stopping on a click

// Held loops: a handful of slots of their own on the same group. A loop's level rides the sound's fader (a short glide per
// change), so a control moved every frame never zippers; the sound's own volume stays 1.
struct EditorLoop {
    ma_sound Sound;
    ma_audio_buffer Buffer;
    std::shared_ptr<const EditorClip> Clip; // null when the slot is free
    uint32_t Generation = 0;                // bumped per start: a stale LoopId no longer matches
    bool Stopping = false;                  // fading out: freed by the reap once it has stopped
};
constexpr unsigned kEditorLoopGlideMs = 25;
constexpr int kEditorLoopSlotBits = 8;

// The editor output's clip guard: unity below -3 dBFS, above it a tanh knee (slope 1 where it starts, so no corner) that
// approaches -0.3 dBFS and never passes it. Per sample, no state, no lookahead: it adds no latency, and a lone tick never
// reaches it - only a pile-up of loud voices does.
struct SoftClipNode {
    ma_node_base Base;
};

// Everything the engine used to keep in file-level statics. One instance (g_Audio, defined in AudioEngine.cpp).
struct AudioState {
    ma_engine Engine;
    bool Initialized = false;

    std::unordered_map<std::string, std::shared_ptr<const PreloadedClip>> Preloaded;
    // Paths whose decode failed (Load / LoadClip): remembered so the failure is logged once, not on every play. Reload(path) clears one.
    // (A Drive file that is not fetched yet is never remembered: it may arrive.)
    std::set<std::string> LoadFailed, ClipFailed;

    std::vector<Voice> Voices;

    std::atomic<TapRing*> Taps{nullptr};
    std::unique_ptr<TapRing> TapStorage;
    bool TapsExternal = false;

    LimiterNode* Limiter = nullptr;
    MeterNode* BusMeters[AudioEngine::kBusCount] = {};

    ReverbNode* Reverbs[2] = {nullptr, nullptr}; // 0: the listener's room, 1: the remote room (a portal voice's own)
    bool ReverbInit = false;
    float ReverbReturn = 1.0f;
    float ReverbGlide = 0.35f;
    bool Offline = false;
    std::map<std::string, std::shared_ptr<const IrData>> IrCache; // game thread

    LoudnessMeter* WetMeter = nullptr;
    LoudnessMeter* BusLoudness[AudioEngine::kBusCount] = {};
    MasterLimiter* LimiterDsp = nullptr;
    LoudnessMeter* MasterMeter = nullptr;
    ma_encoder Encoder;
    bool EncoderOpen = false;

    // #171 - mixer buses: every voice plays through one sound group, whose volume is the bus
    // volume. Master is the engine volume (times the editor's Mute).
    ma_sound_group Buses[AudioEngine::kBusCount];
    bool BusesReady = false;
    float BusVolume[AudioEngine::kBusCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float MasterVolume = 1.0f;
    bool MutedFlag = false;
    bool EnvMuted = false; // TARTARUS_MUTE=1: a test run on a desktop someone is using - everything mixes as ever, silently
    std::vector<uint32_t> FreeSlots;
    bool Paused = false;

    std::unique_ptr<ma_sound> PreviewSound;
    std::string PreviewPath;

    std::vector<std::shared_ptr<const EditorClip>> EditorClips;          // by ClipId; null once unloaded (ids are never reused)
    std::unordered_map<std::string, AudioEngine::ClipId> EditorClipIds; // path -> id, for LoadClip only (no lookup per play)
    ma_sound_group EditorGroup;
    bool EditorGroupReady = false;
    float EditorVolume = 1.0f;
    EditorVoice EditorVoices[kEditorSlots];
    int EditorFree[kEditorSlots];   // free slot indices (a stack)
    int EditorFreeCount = 0;
    int EditorActive[kEditorSlots]; // slots in use, in no order (swap-and-pop)
    int EditorActiveCount = 0;
    int EditorLive = 0;             // in use and not stolen: what EditorVoiceCount reports
    uint64_t EditorOrder = 0;
    EditorLoop EditorLoops[AudioEngine::kEditorLoops];
    SoftClipNode* EditorClipGuard = nullptr;
};

extern AudioState g_Audio;

// The node vtables (defined in AudioGraph.cpp).
extern ma_node_vtable OcclusionVtable, MeterVtable, LimiterVtable, ReverbVtable, SoftClipVtable;

// AudioGraph.cpp
void OcclusionUninit(OcclusionNode& n);
void ApplyMasterVolume(ma_engine& engine);
void UpdateReturnGains();
bool InitNode(ma_node_vtable* vt, ma_node_base* node);

// AudioVoices.cpp: uninits the voice in a slot and recycles the slot.
void FreeSlot(uint32_t index);

// EditorAudio.cpp
void ResetEditorSlots();
void FreeEditorVoice(EditorVoice& v);
void ReapEditorVoices();
void FreeEditorLoop(EditorLoop& l);
void ReapEditorLoops();

} // namespace AudioInternal
