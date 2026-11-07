#include "AudioEngineInternal.h"
#include "Log.h"
#include "AssetDatabase.h"
#include <algorithm>
#include <cmath>

using namespace AudioInternal;

namespace {
// Decodes an impulse response (any wav / flac / ogg / mp3) to the engine's rate and prepares it. Cached by path; null on failure.
std::shared_ptr<const IrData> LoadIr(const std::string& path) {
    const auto it = g_Audio.IrCache.find(path);
    if (it != g_Audio.IrCache.end()) return it->second;
    std::shared_ptr<const IrData> ir;
    if (g_Audio.Initialized) {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, ma_engine_get_sample_rate(&g_Audio.Engine));
        ma_uint64 frames = 0;
        void* pcm = nullptr;
        if (ma_decode_file(path.c_str(), &cfg, &frames, &pcm) == MA_SUCCESS && pcm && frames > 0) {
            const float* in = static_cast<const float*>(pcm);
            std::vector<float> l((size_t)frames), r((size_t)frames);
            for (size_t i = 0; i < (size_t)frames; ++i) {
                l[i] = in[2 * i];
                r[i] = in[2 * i + 1];
            }
            ma_free(pcm, nullptr);
            ir = IrData::FromSamples(l.data(), r.data(), (int)frames, (int)ma_engine_get_sample_rate(&g_Audio.Engine));
        } else {
            if (AssetDatabase::IsUnfetchedExternal(path)) return nullptr; // a Drive file not fetched yet: not remembered, it may arrive
            Log::Warn("Audio: could not load the impulse response '" + path + "'.", LogContext::Asset(path));
        }
    }
    g_Audio.IrCache[path] = ir; // (a failure is remembered too: one warning, not one per frame; Reload(path) forgets it)
    return ir;
}

// The convolver of `path` on a bus (registered on first use).
// `keepMask`: slots the caller is already using this round (never recycled for this IR).
int IrIndexFor(int bus, const std::string& path, unsigned keepMask = 0) {
    ReverbNode* r = g_Audio.Reverbs[bus];
    if (!r || path.empty()) return -1;
    const auto it = r->IrIndex.find(path);
    if (it != r->IrIndex.end()) return it->second;
    const std::shared_ptr<const IrData> ir = LoadIr(path);
    if (!ir) {
        if (!AssetDatabase::IsUnfetchedExternal(path)) r->IrIndex[path] = -1; // (an unfetched file is asked again next time)
        return -1;
    }
    const int idx = r->Verb->AddIr(ir, keepMask);
    if (idx >= 0) {
        r->IrIndex[path] = idx;
        if ((int)r->IrPaths.size() <= idx) r->IrPaths.resize((size_t)idx + 1);
        std::string& was = r->IrPaths[(size_t)idx];
        if (!was.empty() && was != path) r->IrIndex.erase(was); // the slot was recycled: its old IR is no longer registered
        was = path;
    } else if (r->IrFullWarned.insert(path).second) { // (not remembered as -1: a slot may free up; the warning is once per path)
        Log::Warn("Audio: too many impulse responses in use on one reverb bus; '" + path + "' is not used for now.");
    }
    return idx;
}
} // namespace

void AudioEngine::SetReverbEnabled(bool enabled, int bus) {
    if (ReverbNode* r = g_Audio.Reverbs[bus == 1 ? 1 : 0]) r->Enabled.store(enabled);
}
bool AudioEngine::ReverbEnabled(int bus) { return g_Audio.Reverbs[bus == 1 ? 1 : 0] && g_Audio.Reverbs[bus == 1 ? 1 : 0]->Enabled.load(); }

bool AudioEngine::PreloadIr(const std::string& path) {
    if (!g_Audio.Initialized || path.empty()) return false;
    const bool ok = LoadIr(path) != nullptr;
    if (ok) IrIndexFor(0, path);
    return ok;
}

void AudioEngine::SetReverb(const ReverbSpec& spec, int bus) {
    const int b = bus == 1 ? 1 : 0;
    ReverbNode* r = g_Audio.Reverbs[b];
    if (!r) return;
    ReverbRtTarget t;
    t.GlideSeconds = g_Audio.ReverbGlide;
    float total = 0.0f;
    unsigned inUse = 0; // the slots this spec's earlier layers took: a later layer's new IR must not recycle them
    for (int i = 0; i < spec.Count && i < 2; ++i) {
        const ReverbLayerSpec& L = spec.Layers[i];
        const int idx = IrIndexFor(b, L.Ir, inUse);
        if (idx >= 0) inUse |= 1u << idx;
        if (idx < 0 || L.Weight <= 0.0f) continue;
        ReverbRtLayer& o = t.Layers[t.Count++];
        o.Instance = idx;
        o.Weight = L.Weight;
        o.WetLin = std::pow(10.0f, L.WetDb / 20.0f);
        o.PreDelayMs = L.PreDelayMs;
        o.HfDampDb = L.HfDampDb;
        o.LowCutHz = L.LowCutHz;
        total += L.Weight;
    }
    for (int i = 0; i < t.Count; ++i) t.Layers[i].Weight /= total; // (the layers that could be loaded carry the whole weight)
    r->Verb->SetTarget(t);
}

AudioEngine::ReverbInfo AudioEngine::GetReverbInfo(int bus) {
    ReverbInfo out;
    ReverbNode* r = g_Audio.Reverbs[bus == 1 ? 1 : 0];
    if (!r) return out;
    const ConvolutionReverb::Info info = r->Verb->GetInfo();
    out.Instances = info.Instances;
    for (int k = 0; k < info.Instances; ++k) {
        ReverbInfo::Entry e;
        if ((size_t)k < r->IrPaths.size()) e.Ir = r->IrPaths[(size_t)k];
        e.Running = info.On[k];
        e.Weight = info.Weight[k];
        out.Entries.push_back(e);
    }
    return out;
}

void AudioEngine::SetReverbGlide(float seconds) {
    g_Audio.ReverbGlide = std::max(seconds, 0.01f);
    for (ReverbNode* r : g_Audio.Reverbs)
        if (r) {
            ReverbRtTarget t = r->Verb->Target();
            t.GlideSeconds = g_Audio.ReverbGlide;
            r->Verb->SetTarget(t);
        }
}

void AudioEngine::SetReverbReturn(float level) {
    g_Audio.ReverbReturn = std::max(level, 0.0f);
    UpdateReturnGains();
}

AudioEngine::ReverbStats AudioEngine::GetReverbStats(int bus) {
    ReverbStats st;
    ReverbNode* r = g_Audio.Reverbs[bus == 1 ? 1 : 0];
    if (!r) return st;
    st.Callbacks = r->Callbacks.load();
    st.TotalMicros = r->TotalMicros.load();
    st.MaxMicros = r->MaxMicros.load();
    st.Frames = r->LastFrames.load();
    st.WorkerJobs = r->Verb->WorkerJobs();
    st.WorkerMicros = r->Verb->WorkerMicros();
    st.LateBlocks = r->Verb->LateBlocks();
    st.ActiveConvolvers = r->Verb->GetInfo().Active;
    return st;
}

void AudioEngine::ResetReverbStats() {
    for (ReverbNode* r : g_Audio.Reverbs)
        if (r) {
            r->Callbacks.store(0);
            r->TotalMicros.store(0.0);
            r->MaxMicros.store(0.0);
        }
}
