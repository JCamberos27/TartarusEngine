#include "AudioEngineInternal.h"
#include "Log.h"
#include "Core/Env.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace AudioInternal;

AudioState AudioInternal::g_Audio;

namespace {
ma_node* AsNode(ma_node_base* n) { return (ma_node*)n; }

bool InitImpl(bool offline, int rate) {
    if (g_Audio.Initialized) return true;
    ma_engine_config cfg = ma_engine_config_init();
    std::string capture;
    if (Env::IsSet(Env::Var::AudioCapture)) capture = Env::Str(Env::Var::AudioCapture);
    if (offline) {
        cfg.noDevice = MA_TRUE;
        cfg.channels = 2;
        cfg.sampleRate = (ma_uint32)rate;
    } else if (!capture.empty()) {
        cfg.sampleRate = 48000; // a capture is a 48 kHz file
    } else {
        // A short period (5.3 ms at 48 kHz, the convolver's head block) instead of miniaudio's 10 ms default: a UI tick reaches
        // the speaker that much sooner. The device may grant something else; the line below says what.
        cfg.periodSizeInFrames = 256;
    }
    if (ma_engine_init(&cfg, &g_Audio.Engine) != MA_SUCCESS) {
        Log::Error("Audio: failed to initialize the audio engine.");
        return false;
    }
    g_Audio.Initialized = true;
    g_Audio.Offline = offline;
    if (const ma_device* dev = offline ? nullptr : ma_engine_get_device(&g_Audio.Engine)) {
        const ma_uint32 period = dev->playback.internalPeriodSizeInFrames, hz = dev->playback.internalSampleRate;
        ma_uint32 buffer = period * dev->playback.internalPeriods;
#ifdef MA_SUPPORT_WASAPI
        // WASAPI's real buffer (the period alone undersells it): the queue a sound waits behind before it reaches the mixer.
        if (dev->pContext->backend == ma_backend_wasapi && dev->wasapi.actualBufferSizeInFramesPlayback)
            buffer = dev->wasapi.actualBufferSizeInFramesPlayback;
#endif
        char line[200];
        std::snprintf(line, sizeof(line), "Audio: output period %u frames (%.1f ms at %u Hz), buffer %u frames (%.1f ms), %s.", period,
                      hz ? 1000.0 * period / hz : 0.0, hz, buffer, hz ? 1000.0 * buffer / hz : 0.0, ma_get_backend_name(dev->pContext->backend));
        Log::Info(line);
    }
    if (Env::IsSet(Env::Var::Mute)) g_Audio.EnvMuted = Env::Flag(Env::Var::Mute);
    ApplyMasterVolume(g_Audio.Engine);
    g_Audio.BusesReady = true;
    for (int b = 0; b < AudioEngine::kBusCount; ++b) {
        if (ma_sound_group_init(&g_Audio.Engine, 0, nullptr, &g_Audio.Buses[b]) != MA_SUCCESS) {
            Log::Error("Audio: failed to create mixer bus " + std::to_string(b) + "; playing without buses.");
            for (int k = 0; k < b; ++k) ma_sound_group_uninit(&g_Audio.Buses[k]);
            g_Audio.BusesReady = false;
            break;
        }
        ma_sound_group_set_volume(&g_Audio.Buses[b], g_Audio.BusVolume[b]);
    }
    ApplyMasterVolume(g_Audio.Engine);
    const int sr = (int)ma_engine_get_sample_rate(&g_Audio.Engine);
    // The output graph: buses -> meters -> limiter -> endpoint, reverb returns -> limiter. Without it (a mono device) every group keeps
    // its default attachment to the endpoint and voices play without a send.
    if (ma_engine_get_channels(&g_Audio.Engine) == 2) {
        auto* lim = new LimiterNode();
        lim->Lim = new MasterLimiter(sr);
        lim->Master = new LoudnessMeter(sr);
        if (InitNode(&LimiterVtable, &lim->Base)) {
            ma_node_attach_output_bus(&lim->Base, 0, ma_engine_get_endpoint(&g_Audio.Engine), 0);
            g_Audio.Limiter = lim;
            g_Audio.LimiterDsp = lim->Lim;
            g_Audio.MasterMeter = lim->Master;
        } else {
            Log::Warn("Audio: could not create the master limiter; playing without it.");
            delete lim->Lim;
            delete lim->Master;
            delete lim;
        }
        if (g_Audio.Limiter && g_Audio.BusesReady) {
            for (int b = 0; b < AudioEngine::kBusCount; ++b) {
                auto* m = new MeterNode();
                m->Meter = new LoudnessMeter(sr);
                if (InitNode(&MeterVtable, &m->Base)) {
                    ma_node_attach_output_bus(AsNode(&m->Base), 0, &g_Audio.Limiter->Base, 0);
                    ma_node_attach_output_bus((ma_node*)&g_Audio.Buses[b], 0, &m->Base, 0);
                    g_Audio.BusMeters[b] = m;
                    g_Audio.BusLoudness[b] = m->Meter;
                } else {
                    delete m->Meter;
                    delete m;
                }
            }
        }
        // The reverb buses. If they can't be built, voices simply start without a send.
        for (int bus = 0; bus < 2; ++bus) {
            auto* node = new ReverbNode();
            node->Verb = new ConvolutionReverb(sr, !offline);
            node->Meter = new LoudnessMeter(sr);
            node->Verb->SetTarget(ReverbRtTarget{});
            node->Enabled.store(bus == 0); // the remote room's reverb runs only while a portal voice feeds it
            if (InitNode(&ReverbVtable, &node->Base)) {
                ma_node_attach_output_bus(&node->Base, 0, g_Audio.Limiter ? &g_Audio.Limiter->Base : ma_engine_get_endpoint(&g_Audio.Engine), 0);
                g_Audio.Reverbs[bus] = node;
                g_Audio.ReverbInit = true;
            } else {
                Log::Warn("Audio: could not create the reverb bus; voices play without a send.");
                delete node->Verb;
                delete node->Meter;
                delete node;
            }
        }
        UpdateReturnGains();
    }
    // The editor's UI sounds: a group of their own (no parent group) -> the clip guard -> the endpoint, beside the limiter. Its volume
    // glides over 256 frames, so a Volume slider dragged while ticks play doesn't zipper; no pitch (the group's resampler would run
    // at 1.0 for nothing).
    ma_sound_group_config gc = ma_sound_group_config_init_2(&g_Audio.Engine);
    gc.flags = MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION;
    gc.volumeSmoothTimeInPCMFrames = 256;
    g_Audio.EditorGroupReady = ma_sound_group_init_ex(&g_Audio.Engine, &gc, &g_Audio.EditorGroup) == MA_SUCCESS;
    ResetEditorSlots();
    if (g_Audio.EditorGroupReady) {
        ma_sound_group_set_volume(&g_Audio.EditorGroup, g_Audio.EditorVolume); // (the first gain set is immediate, not a glide)
        if (ma_engine_get_channels(&g_Audio.Engine) == 2) { // (a mono device: the group stays on the endpoint, unguarded)
            auto* guard = new SoftClipNode();
            if (InitNode(&SoftClipVtable, &guard->Base)) {
                ma_node_attach_output_bus(&guard->Base, 0, ma_engine_get_endpoint(&g_Audio.Engine), 0);
                ma_node_attach_output_bus((ma_node*)&g_Audio.EditorGroup, 0, &guard->Base, 0);
                g_Audio.EditorClipGuard = guard;
            } else {
                delete guard;
            }
        }
    } else {
        Log::Warn("Audio: could not create the editor sound group; editor UI sounds are off.");
    }
    ApplyMasterVolume(g_Audio.Engine); // again, now that the limiter it applies to exists
    if (!capture.empty()) AudioEngine::StartCapture(capture);
    return true;
}

} // namespace

void AudioEngine::Init() { InitImpl(false, 0); }

void AudioEngine::InitOffline(int sampleRate) { InitImpl(true, sampleRate); }

bool AudioEngine::IsOffline() { return g_Audio.Offline; }

int AudioEngine::SampleRate() { return g_Audio.Initialized ? (int)ma_engine_get_sample_rate(&g_Audio.Engine) : 48000; }

void AudioEngine::RenderOffline(float* out, int frames) {
    if (!g_Audio.Initialized || frames <= 0) return;
    static std::vector<float> scratch;
    if (!out) {
        if (scratch.size() < 2 * (size_t)frames) scratch.assign(2 * (size_t)frames, 0.0f);
        out = scratch.data();
    }
    ma_uint64 read = 0;
    ma_engine_read_pcm_frames(&g_Audio.Engine, out, (ma_uint64)frames, &read);
}

void AudioEngine::Shutdown() {
    if (!g_Audio.Initialized) return;
    StopCapture();
    StopPreview();
    StopAll();
    g_Audio.Voices.clear();
    g_Audio.FreeSlots.clear();
    for (int i = 0; i < g_Audio.EditorActiveCount; ++i) FreeEditorVoice(g_Audio.EditorVoices[g_Audio.EditorActive[i]]);
    ResetEditorSlots();
    for (EditorLoop& l : g_Audio.EditorLoops)
        if (l.Clip) FreeEditorLoop(l);
    if (g_Audio.EditorGroupReady) ma_sound_group_uninit(&g_Audio.EditorGroup);
    g_Audio.EditorGroupReady = false;
    if (g_Audio.EditorClipGuard) { // after the group: nothing feeds it any more
        ma_node_uninit(&g_Audio.EditorClipGuard->Base, nullptr);
        delete g_Audio.EditorClipGuard;
        g_Audio.EditorClipGuard = nullptr;
    }
    // (The editor clips stay: plain PCM at their own rate, still playable by a later Init, and their ids stay meaningful.)
    g_Audio.Preloaded.clear();
    g_Audio.LoadFailed.clear();
    g_Audio.Paused = false;
    if (g_Audio.BusesReady) for (int b = 0; b < kBusCount; ++b) ma_sound_group_uninit(&g_Audio.Buses[b]);
    g_Audio.BusesReady = false;
    for (ReverbNode*& r : g_Audio.Reverbs)
        if (r) {
            ma_node_uninit(&r->Base, nullptr);
            delete r->Verb;
            delete r->Meter;
            delete r;
            r = nullptr;
        }
    for (int b = 0; b < kBusCount; ++b)
        if (g_Audio.BusMeters[b]) {
            ma_node_uninit(&g_Audio.BusMeters[b]->Base, nullptr);
            delete g_Audio.BusMeters[b]->Meter;
            delete g_Audio.BusMeters[b];
            g_Audio.BusMeters[b] = nullptr;
            g_Audio.BusLoudness[b] = nullptr;
        }
    if (g_Audio.Limiter) {
        ma_node_uninit(&g_Audio.Limiter->Base, nullptr);
        delete g_Audio.Limiter->Lim;
        delete g_Audio.Limiter->Master;
        delete g_Audio.Limiter;
        g_Audio.Limiter = nullptr;
        g_Audio.LimiterDsp = nullptr;
        g_Audio.MasterMeter = nullptr;
    }
    g_Audio.ReverbInit = false;
    g_Audio.IrCache.clear();
    ma_engine_uninit(&g_Audio.Engine);
    g_Audio.Taps.store(nullptr);
    g_Audio.TapStorage.reset();
    g_Audio.Initialized = false;
    g_Audio.Offline = false;
}

void AudioEngine::Update() {
    if (!g_Audio.Initialized) return;

    // Reap one-shots that have run to their end (#200: nothing used to free these, so every
    // Play leaked a ma_sound plus its open streaming file handle for the life of the process).
    // Looping voices never end on their own — they only leave via Stop()/StopAll().
    for (uint32_t i = 0; i < g_Audio.Voices.size(); ++i) {
        Voice& voice = g_Audio.Voices[i];
        // A paused voice is stopped too, but it's waiting to resume, not finished.
        if (!voice.Sound || voice.Looping || voice.Paused) continue;
        if (ma_sound_at_end(voice.Sound.get()) || !ma_sound_is_playing(voice.Sound.get())) {
            FreeSlot(i);
        }
    }

    // Same treatment for the Asset Browser preview: a preview that played to its end used to sit
    // on its file handle until the next preview or shutdown.
    if (g_Audio.PreviewSound && (ma_sound_at_end(g_Audio.PreviewSound.get()) ||
                           !ma_sound_is_playing(g_Audio.PreviewSound.get()))) {
        StopPreview();
    }
    ReapEditorVoices();
    ReapEditorLoops();
    if (g_Audio.EncoderOpen && !g_Audio.TapsExternal) { // AUDIO_CAPTURE: keep the file fed
        std::vector<float> a, b, c;
        DrainTaps(a, b, c);
    }
}

void AudioEngine::SetPaused(bool paused) {
    if (paused == g_Audio.Paused || !g_Audio.Initialized) return;
    g_Audio.Paused = paused;
    for (Voice& voice : g_Audio.Voices) {
        if (!voice.Sound) continue;
        if (paused) {
            // Only voices audible right now; ma_sound_stop keeps the cursor, so resuming
            // continues mid-clip. A one-shot that already ended is left for Update() to reap.
            if (ma_sound_is_playing(voice.Sound.get()) && !ma_sound_at_end(voice.Sound.get())) {
                ma_sound_stop(voice.Sound.get());
                voice.Paused = true;
            }
        } else if (voice.Paused) {
            ma_sound_start(voice.Sound.get());
            voice.Paused = false;
        }
    }
}

void AudioEngine::SetMuted(bool muted) {
    g_Audio.MutedFlag = muted;
    if (g_Audio.Initialized) ApplyMasterVolume(g_Audio.Engine);
}

void AudioEngine::SetMasterVolume(float volume) {
    g_Audio.MasterVolume = std::clamp(volume, 0.0f, 1.0f);
    if (g_Audio.Initialized) ApplyMasterVolume(g_Audio.Engine);
}

bool AudioEngine::IsInitialized() { return g_Audio.Initialized; }
