#include "AudioEngineInternal.h"
#include "Log.h"
#include "Dsp.h"
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace AudioInternal;

namespace {
void OcclusionProcess(ma_node* pNode, const float** ppIn, ma_uint32* pFrameCountIn, float** ppOut, ma_uint32* pFrameCountOut) {
    OcclusionNode* n = reinterpret_cast<OcclusionNode*>(pNode);
    const double want = n->Target.load(std::memory_order_relaxed);
    if (want != n->Applied) {
        n->Applied = want;
        const ma_lpf_config lc = ma_lpf_config_init(ma_format_f32, 2, n->Rate, want, 2);
        ma_lpf_reinit(&lc, &n->Lpf);
    }
    ma_lpf_process_pcm_frames(&n->Lpf, ppOut[0], ppIn[0], *pFrameCountOut);
    (void)pFrameCountIn;
}

void MeterProcess(ma_node* pNode, const float** ppIn, ma_uint32* pFrameCountIn, float** ppOut, ma_uint32* pFrameCountOut) {
    DenormalGuard denormals;
    MeterNode* n = reinterpret_cast<MeterNode*>(pNode);
    const ma_uint32 frames = *pFrameCountOut;
    float* out = ppOut[0];
    if (ppIn && ppIn[0]) std::copy(ppIn[0], ppIn[0] + 2 * (size_t)frames, out);
    else std::fill(out, out + 2 * (size_t)frames, 0.0f);
    if (n->Meter) n->Meter->Process(out, (int)frames);
    (void)pFrameCountIn;
}

void LimiterProcess(ma_node* pNode, const float** ppIn, ma_uint32* pFrameCountIn, float** ppOut, ma_uint32* pFrameCountOut) {
    DenormalGuard denormals;
    LimiterNode* n = reinterpret_cast<LimiterNode*>(pNode);
    const ma_uint32 frames = *pFrameCountOut;
    float* out = ppOut[0];
    const float* in = ppIn && ppIn[0] ? ppIn[0] : nullptr;
    if (!in) {
        if (n->Scratch.size() < 2 * (size_t)frames) n->Scratch.assign(2 * (size_t)frames, 0.0f); // (grows on the first callbacks only)
        std::fill(n->Scratch.begin(), n->Scratch.begin() + 2 * (size_t)frames, 0.0f);
        in = n->Scratch.data();
    }
    if (n->Dirty.load(std::memory_order_acquire) && n->Lock.try_lock()) {
        const LimiterSettings want = n->Pending;
        n->Dirty.store(false, std::memory_order_relaxed);
        n->Lock.unlock();
        n->Lim->Configure(want);
    }
    TapRing* taps = g_Audio.Taps.load(std::memory_order_acquire);
    unsigned long long w = 0;
    if (taps) {
        w = taps->Written.load(std::memory_order_relaxed);
        for (ma_uint32 i = 0; i < frames; ++i) {
            const size_t at = (size_t)((w + i) & (TapRing::kFrames - 1)) * 2;
            taps->Pre[at] = in[2 * (size_t)i];
            taps->Pre[at + 1] = in[2 * (size_t)i + 1];
        }
    }
    n->Lim->Process(in, out, (int)frames);
    n->Master->Process(out, (int)frames);
    const float gr = n->Lim->TakeGainReductionDb();
    if (gr > 0.0f) n->LastGrDb.store(std::max(gr, n->LastGrDb.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    const float glue = n->Lim->TakeGlueReductionDb();
    if (glue > 0.0f) n->LastGlueGrDb.store(std::max(glue, n->LastGlueGrDb.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    if (taps) {
        for (ma_uint32 i = 0; i < frames; ++i) {
            const size_t at = (size_t)((w + i) & (TapRing::kFrames - 1)) * 2;
            taps->Master[at] = out[2 * (size_t)i];
            taps->Master[at + 1] = out[2 * (size_t)i + 1];
        }
        taps->Written.store(w + frames, std::memory_order_release);
    }
    // The master level (volume x mute), after the meter and taps: ramped across the block so a change never steps.
    const float g1 = n->Gain.load(std::memory_order_relaxed);
    const float g0 = n->GainPrev < 0.0f ? g1 : n->GainPrev;
    if (g0 != 1.0f || g1 != 1.0f) {
        const float step = (g1 - g0) / (float)std::max<ma_uint32>(frames, 1);
        float g = g0;
        for (ma_uint32 i = 0; i < frames; ++i) {
            g += step;
            out[2 * (size_t)i] *= g;
            out[2 * (size_t)i + 1] *= g;
        }
    }
    n->GainPrev = g1;
    (void)pFrameCountIn;
}

void ReverbProcess(ma_node* pNode, const float** ppIn, ma_uint32* pFrameCountIn, float** ppOut, ma_uint32* pFrameCountOut) {
    ReverbNode* n = reinterpret_cast<ReverbNode*>(pNode);
    const ma_uint32 frames = *pFrameCountOut;
    float* out = ppOut[0];
    (void)pFrameCountIn;
    if (!n->Verb || !n->Enabled.load(std::memory_order_relaxed)) {
        if (n->Verb && n->WasEnabled) n->Verb->Silence(); // switched off: the tails stop here, so a re-enable never replays a stale one
        n->WasEnabled = false;
        n->PrevGain = -1.0f;
        std::fill(out, out + 2 * (size_t)frames, 0.0f);
        if (n->Meter) n->Meter->Process(out, (int)frames);
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    static thread_local std::vector<float> silence;
    const float* in = ppIn && ppIn[0] ? ppIn[0] : nullptr;
    if (!in) {
        if (silence.size() < 2 * (size_t)frames) silence.assign(2 * (size_t)frames, 0.0f); // (grows once, on the first callback)
        in = silence.data();
    }
    n->WasEnabled = true;
    n->Verb->Process(in, out, (int)frames);
    const float gain = n->Return.load(std::memory_order_relaxed);
    const float g0 = n->PrevGain < 0.0f ? gain : n->PrevGain; // (the return level ramps across the block: no step)
    if (g0 == gain) {
        for (size_t i = 0; i < 2 * (size_t)frames; ++i) out[i] *= gain;
    } else {
        const float step = (gain - g0) / (float)std::max<ma_uint32>(frames, 1);
        float g = g0;
        for (ma_uint32 i = 0; i < frames; ++i) {
            g += step;
            out[2 * (size_t)i] *= g;
            out[2 * (size_t)i + 1] *= g;
        }
    }
    n->PrevGain = gain;
    if (n->Meter) n->Meter->Process(out, (int)frames);
    if (TapRing* taps = g_Audio.Taps.load(std::memory_order_acquire)) {
        const unsigned long long w = taps->Written.load(std::memory_order_relaxed);
        for (ma_uint32 i = 0; i < frames; ++i) {
            const size_t at = (size_t)((w + i) & (TapRing::kFrames - 1)) * 2;
            taps->Wet[at] += out[2 * (size_t)i];
            taps->Wet[at + 1] += out[2 * (size_t)i + 1];
        }
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    n->Callbacks.fetch_add(1, std::memory_order_relaxed);
    n->TotalMicros.store(n->TotalMicros.load(std::memory_order_relaxed) + us, std::memory_order_relaxed);
    if (us > n->MaxMicros.load(std::memory_order_relaxed)) n->MaxMicros.store(us, std::memory_order_relaxed);
    n->LastFrames.store((int)frames, std::memory_order_relaxed);
}

constexpr float kClipKnee = 0.708f;    // -3 dBFS
constexpr float kClipCeiling = 0.966f; // -0.3 dBFS

void SoftClipProcess(ma_node*, const float** ppIn, ma_uint32* pFrameCountIn, float** ppOut, ma_uint32* pFrameCountOut) {
    const ma_uint32 frames = std::min(*pFrameCountIn, *pFrameCountOut);
    const float* in = ppIn[0];
    float* out = ppOut[0];
    constexpr float range = kClipCeiling - kClipKnee;
    for (size_t i = 0; i < 2 * (size_t)frames; ++i) {
        const float x = in[i];
        const float a = std::fabs(x);
        out[i] = a <= kClipKnee ? x : std::copysign(kClipKnee + range * std::tanh((a - kClipKnee) / range), x);
    }
    *pFrameCountIn = frames;
    *pFrameCountOut = frames;
}
} // namespace

namespace AudioInternal {

ma_node_vtable OcclusionVtable = {OcclusionProcess, nullptr, 1, 1, 0};
ma_node_vtable MeterVtable = {MeterProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};
ma_node_vtable LimiterVtable = {LimiterProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};
ma_node_vtable ReverbVtable = {ReverbProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};
ma_node_vtable SoftClipVtable = {SoftClipProcess, nullptr, 1, 1, 0}; // (not continuous: no editor sound, no processing)

void OcclusionUninit(OcclusionNode& n) {
    ma_node_uninit(&n.Base, nullptr);
    ma_lpf_uninit(&n.Lpf, nullptr);
}

// The game's mix ends in the limiter, so master and Mute go on its output: the editor's sounds beside it on the endpoint keep
// their own level (and so does the Asset Browser preview, which plays at the master level by its own volume). Without a limiter
// (a mono device) everything shares the endpoint and master stays the engine volume. TARTARUS_MUTE is the endpoint: all of it.
void ApplyMasterVolume(ma_engine& engine) {
    const float master = g_Audio.MutedFlag ? 0.0f : g_Audio.MasterVolume;
    if (g_Audio.Limiter) {
        g_Audio.Limiter->Gain.store(master, std::memory_order_relaxed);
        ma_engine_set_volume(&engine, g_Audio.EnvMuted ? 0.0f : 1.0f);
    } else {
        ma_engine_set_volume(&engine, g_Audio.EnvMuted ? 0.0f : master);
    }
    if (g_Audio.PreviewSound) ma_sound_set_volume(g_Audio.PreviewSound.get(), g_Audio.Limiter ? master : 1.0f);
}

void UpdateReturnGains() {
    for (ReverbNode* r : g_Audio.Reverbs)
        if (r) r->Return.store(g_Audio.ReverbReturn * g_Audio.BusVolume[0], std::memory_order_relaxed); // the returns follow the SFX bus
}

bool InitNode(ma_node_vtable* vt, ma_node_base* node) {
    ma_node_config nc = ma_node_config_init();
    ma_uint32 ch = 2;
    nc.vtable = vt;
    nc.pInputChannels = &ch;
    nc.pOutputChannels = &ch;
    return ma_node_init(ma_engine_get_node_graph(&g_Audio.Engine), &nc, nullptr, node) == MA_SUCCESS;
}

} // namespace AudioInternal

void AudioEngine::SetMasterLimiter(const LimiterSettings& s) {
    if (!g_Audio.Limiter) return;
    std::lock_guard<std::mutex> lk(g_Audio.Limiter->Lock);
    LimiterSettings& p = g_Audio.Limiter->Pending;
    p = s;
    p.CeilingDb = std::min(s.CeilingDb, 0.0f);
    p.LookaheadMs = std::clamp(s.LookaheadMs, 0.1f, 10.0f);
    p.ReleaseMs = std::max(s.ReleaseMs, 1.0f);
    g_Audio.Limiter->Dirty.store(true, std::memory_order_release);
}

LimiterSettings AudioEngine::GetMasterLimiter() {
    if (!g_Audio.Limiter) return LimiterSettings{};
    std::lock_guard<std::mutex> lk(g_Audio.Limiter->Lock);
    return g_Audio.Limiter->Pending;
}

float AudioEngine::TakeLimiterGainReductionDb() { return g_Audio.Limiter ? g_Audio.Limiter->LastGrDb.exchange(0.0f, std::memory_order_relaxed) : 0.0f; }
float AudioEngine::TakeGlueGainReductionDb() { return g_Audio.Limiter ? g_Audio.Limiter->LastGlueGrDb.exchange(0.0f, std::memory_order_relaxed) : 0.0f; }

AudioEngine::MeterReading AudioEngine::GetMeter(int id) {
    MeterReading m;
    const LoudnessMeter* lm = nullptr;
    if (id == MeterMaster) lm = g_Audio.MasterMeter;
    else if (id >= MeterBusFirst && id < MeterBusFirst + kBusCount) lm = g_Audio.BusLoudness[id - MeterBusFirst];
    else if (id == MeterWet) {
        const LoudnessMeter* a = g_Audio.Reverbs[0] ? g_Audio.Reverbs[0]->Meter : nullptr;
        const LoudnessMeter* b = g_Audio.Reverbs[1] ? g_Audio.Reverbs[1]->Meter : nullptr;
        lm = (b && (!a || b->MomentaryLufs() > a->MomentaryLufs())) ? b : a;
    }
    if (!lm) return m;
    m.PeakDb = lm->PeakDb();
    m.MomentaryLufs = lm->MomentaryLufs();
    m.ShortTermLufs = lm->ShortTermLufs();
    return m;
}

void AudioEngine::EnableTaps(bool on) {
    if (!g_Audio.Initialized) return;
    g_Audio.TapsExternal = on;
    if (on) {
        if (!g_Audio.TapStorage) {
            g_Audio.TapStorage = std::make_unique<TapRing>();
            g_Audio.TapStorage->Master.assign(2 * (size_t)TapRing::kFrames, 0.0f);
            g_Audio.TapStorage->Pre.assign(2 * (size_t)TapRing::kFrames, 0.0f);
            g_Audio.TapStorage->Wet.assign(2 * (size_t)TapRing::kFrames, 0.0f);
        }
        g_Audio.Taps.store(g_Audio.TapStorage.get(), std::memory_order_release);
    } else if (!g_Audio.EncoderOpen) {
        g_Audio.Taps.store(nullptr, std::memory_order_release);
    }
}

size_t AudioEngine::DrainTaps(std::vector<float>& master, std::vector<float>& pre, std::vector<float>& wet) {
    TapRing* t = g_Audio.TapStorage.get();
    if (!t || !g_Audio.Taps.load()) return 0;
    const unsigned long long w = t->Written.load(std::memory_order_acquire);
    if (w - t->Read > TapRing::kFrames) {
        // Overrun: the frames skipped were never read, so the returns have added a lap onto their unread Wet data. Zero those slots
        // so the next lap starts from silence instead of stale audio.
        const unsigned long long skipped = std::min<unsigned long long>(w - TapRing::kFrames - t->Read, TapRing::kFrames);
        for (unsigned long long i = 0; i < skipped; ++i) {
            const size_t slot = (size_t)((t->Read + i) & (TapRing::kFrames - 1)) * 2;
            t->Wet[slot] = t->Wet[slot + 1] = 0.0f;
        }
        t->Read = w - TapRing::kFrames;
    }
    const size_t n = (size_t)(w - t->Read);
    const size_t at0 = master.size();
    master.resize(at0 + 2 * n);
    pre.resize(at0 + 2 * n);
    wet.resize(at0 + 2 * n);
    for (size_t i = 0; i < n; ++i) {
        const size_t slot = (size_t)((t->Read + i) & (TapRing::kFrames - 1)) * 2;
        master[at0 + 2 * i] = t->Master[slot];
        master[at0 + 2 * i + 1] = t->Master[slot + 1];
        pre[at0 + 2 * i] = t->Pre[slot];
        pre[at0 + 2 * i + 1] = t->Pre[slot + 1];
        wet[at0 + 2 * i] = t->Wet[slot];
        wet[at0 + 2 * i + 1] = t->Wet[slot + 1];
        t->Wet[slot] = t->Wet[slot + 1] = 0.0f; // the returns add into zeros next lap
    }
    if (g_Audio.EncoderOpen && n > 0) {
        ma_uint64 written = 0;
        ma_encoder_write_pcm_frames(&g_Audio.Encoder, master.data() + at0, (ma_uint64)n, &written);
    }
    t->Read = w;
    return n;
}

bool AudioEngine::StartCapture(const std::string& wavPath) {
    if (!g_Audio.Initialized || g_Audio.EncoderOpen || wavPath.empty()) return false;
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, ma_engine_get_sample_rate(&g_Audio.Engine));
    if (ma_encoder_init_file(wavPath.c_str(), &cfg, &g_Audio.Encoder) != MA_SUCCESS) {
        Log::Warn("Audio: could not open the capture file '" + wavPath + "'.");
        return false;
    }
    g_Audio.EncoderOpen = true;
    Log::Info("Audio: capturing the master output to '" + wavPath + "'.");
    if (!g_Audio.TapStorage) {
        g_Audio.TapStorage = std::make_unique<TapRing>();
        g_Audio.TapStorage->Master.assign(2 * (size_t)TapRing::kFrames, 0.0f);
        g_Audio.TapStorage->Pre.assign(2 * (size_t)TapRing::kFrames, 0.0f);
        g_Audio.TapStorage->Wet.assign(2 * (size_t)TapRing::kFrames, 0.0f);
    }
    g_Audio.Taps.store(g_Audio.TapStorage.get(), std::memory_order_release);
    return true;
}

void AudioEngine::StopCapture() {
    if (!g_Audio.EncoderOpen) return;
    std::vector<float> a, b, c;
    DrainTaps(a, b, c); // (the last frames)
    ma_encoder_uninit(&g_Audio.Encoder);
    g_Audio.EncoderOpen = false;
    if (!g_Audio.TapsExternal) g_Audio.Taps.store(nullptr, std::memory_order_release);
}
