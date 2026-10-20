#include "GlassMotion.h"

#include "GlassLayout.h" // LayoutMotionNewFrame
#include "GlassTheme.h"  // Scale
#include "../EditorSound.h" // FlingRelease

// Vendored: compiled at the default warning level like the rest of extern code, so its header
// must not trip this TU's /W4 /WX.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <im_anim.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <imgui_internal.h> // scroll inertia: the wheeling window's ScrollTarget

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

// GlassTokens.h mirrors these ids so it doesn't need im_anim.h; keep them honest.
static_assert(Glass::EaseId::InCubic == iam_ease_in_cubic, "GlassTokens.h EaseId drifted from im_anim.h");
static_assert(Glass::EaseId::OutCubic == iam_ease_out_cubic, "GlassTokens.h EaseId drifted from im_anim.h");
static_assert(Glass::EaseId::OutQuart == iam_ease_out_quart, "GlassTokens.h EaseId drifted from im_anim.h");
static_assert(Glass::EaseId::CubicBezier == iam_ease_cubic_bezier, "GlassTokens.h EaseId drifted from im_anim.h");
static_assert(Glass::EaseId::Spring == iam_ease_spring, "GlassTokens.h EaseId drifted from im_anim.h");

namespace Glass {

namespace {
constexpr float kSettleEps = 1e-3f;      // a tween this close to its target has landed
constexpr ImGuiID kChannelValue = 0x474C5331u; // "GLS1": the float channel each id uses
constexpr ImGuiID kChannelColor = 0x474C5332u; // "GLS2": colour tweens
constexpr ImGuiID kChannelVec2  = 0x474C5333u; // "GLS3": 2D tweens
constexpr ImGuiID kScrollSalt   = 0x5C011E7Au; // window id ^ this = its scroll coast's entry id
constexpr ImGuiID kRollPrevSalt = 0x0D0E7E21u; // odometer: last frame's value in window storage
constexpr ImGuiID kRollFromSalt = 0xF1A5F1A5u; // odometer: the value a slide departs from

// Inertia entries reuse Entry fields: from4.xy velocity (px/s), duration the phase.
constexpr float kInertiaTracking = 0.0f;
constexpr float kInertiaCoasting = 1.0f;
constexpr float kInertiaScroll   = 2.0f; // a window's wheel coast, advanced by NewFrame itself

// A double in ImGuiStorage (void* slot, 64-bit): the odometer's last value, exact.
double LoadDouble(ImGuiStorage* st, ImGuiID key, double fallback) {
    static_assert(sizeof(void*) >= sizeof(double), "64-bit only");
    void* raw = st->GetVoidPtr(key);
    if (!raw) return fallback;
    double d;
    std::memcpy(&d, &raw, sizeof(d));
    return d;
}
void StoreDouble(ImGuiStorage* st, ImGuiID key, double d) {
    void* raw = nullptr;
    std::memcpy(&raw, &d, sizeof(d));
    if (!raw) raw = (void*)(uintptr_t)1; // +0.0 is all zero bits: keep it distinguishable from "absent"
    st->SetVoidPtr(key, raw);
}

// A point at distance `s` around the rectangle's perimeter, clockwise from the top-left.
ImVec2 PerimeterPoint(ImVec2 mn, ImVec2 mx, float s) {
    const float w = mx.x - mn.x, h = mx.y - mn.y;
    const float p = 2.0f * (w + h);
    s = std::fmod(s, p);
    if (s < 0.0f) s += p;
    if (s < w) return ImVec2(mn.x + s, mn.y);
    s -= w;
    if (s < h) return ImVec2(mx.x, mn.y + s);
    s -= h;
    if (s < w) return ImVec2(mx.x - s, mx.y);
    s -= w;
    return ImVec2(mn.x, mx.y - s);
}

float Fract(float x) { return x - std::floor(x); }

// Clip definitions are tiny and few; reserve accordingly instead of ImAnim's default 256 clips /
// 4096 instances, which would otherwise sit allocated for the life of the editor.
constexpr int kClipCapacity     = 16;
constexpr int kInstanceCapacity = 8;
void EnsureClipSystem() {
    static bool s_Init = false;
    if (!s_Init) { iam_clip_init(kClipCapacity, kInstanceCapacity); s_Init = true; }
}

bool NearColor(const ImVec4& a, const ImVec4& b) {
    return std::fabs(a.x - b.x) < kSettleEps && std::fabs(a.y - b.y) < kSettleEps &&
           std::fabs(a.z - b.z) < kSettleEps && std::fabs(a.w - b.w) < kSettleEps;
}
bool NearVec2(const ImVec2& a, const ImVec2& b) {
    // Screen space: a tenth of a pixel is landed.
    return std::fabs(a.x - b.x) < 0.1f && std::fabs(a.y - b.y) < 0.1f;
}

iam_ease_desc ToDesc(const MotionSpec& s) {
    iam_ease_desc d = {s.ease, s.p0, s.p1, s.p2, s.p3};
    return d;
}

// Cubic bezier easing (CSS semantics): solve x(u) = t for u by Newton with a bisection fallback,
// return y(u). Used for EaseProgress only; tweens go through ImAnim's own LUT.
float CubicBezierY(float x1, float y1, float x2, float y2, float t) {
    auto bez = [](float a, float b, float u) {
        const float v = 1.0f - u;
        return 3.0f * v * v * u * a + 3.0f * v * u * u * b + u * u * u;
    };
    auto dbez = [](float a, float b, float u) {
        const float v = 1.0f - u;
        return 3.0f * v * v * a + 6.0f * v * u * (b - a) + 3.0f * u * u * (1.0f - b);
    };
    float u = t;
    for (int i = 0; i < 8; ++i) {
        const float err = bez(x1, x2, u) - t;
        if (std::fabs(err) < 1e-5f) return bez(y1, y2, u);
        const float d = dbez(x1, x2, u);
        if (std::fabs(d) < 1e-6f) break;
        u -= err / d;
    }
    float lo = 0.0f, hi = 1.0f;
    u = t;
    for (int i = 0; i < 24; ++i) {
        const float x = bez(x1, x2, u);
        if (std::fabs(x - t) < 1e-5f) break;
        if (x < t) lo = u; else hi = u;
        u = (lo + hi) * 0.5f;
    }
    return bez(y1, y2, u);
}

// The same normalised-time spring ImAnim evaluates (im_anim.cpp spring_unit).
float SpringUnit(float u, float mass, float k, float c, float v0) {
    const float m = mass <= 0.0f ? 1.0f : mass;
    const float wn = std::sqrt(k / m);
    const float zeta = c / (2.0f * std::sqrt(k * m));
    if (zeta < 1.0f) {
        const float wd = wn * std::sqrt(1.0f - zeta * zeta);
        const float b = (zeta * wn + v0) / wd;
        return 1.0f - std::exp(-zeta * wn * u) * (std::cos(wd * u) + b * std::sin(wd * u));
    }
    if (zeta == 1.0f) return 1.0f - std::exp(-wn * u) * (1.0f + wn * u);
    const float wd = wn * std::sqrt(zeta * zeta - 1.0f);
    return 1.0f - 0.5f * (std::exp(-(zeta * wn - wd) * u) + std::exp(-(zeta * wn + wd) * u));
}
} // namespace

float DampingRatio(float mass, float stiffness, float damping) {
    return damping / (2.0f * std::sqrt(stiffness * (mass <= 0.0f ? 1.0f : mass)));
}

float SpringImpulse(float t, float mass, float stiffness, float damping, float amplitude) {
    if (t <= 0.0f || stiffness <= 0.0f) return 0.0f;
    const float m = mass <= 0.0f ? 1.0f : mass;
    const float wn = std::sqrt(stiffness / m);
    const float zeta = DampingRatio(m, stiffness, damping);
    if (zeta >= 1.0f) {
        // No oscillation: the critically damped bump t*e^(-wn t), normalised to peak at `amplitude`.
        const float peak = (1.0f / wn) * std::exp(-1.0f);
        return amplitude * t * std::exp(-wn * t) / peak;
    }
    const float wd = wn * std::sqrt(1.0f - zeta * zeta);
    const float decay = zeta * wn;
    // First peak of e^(-decay t) sin(wd t) is where its derivative is 0: tan(wd t) = wd / decay.
    const float tPeak = std::atan2(wd, decay) / wd;
    const float peak = std::exp(-decay * tPeak) * std::sin(wd * tPeak);
    return amplitude * std::exp(-decay * t) * std::sin(wd * t) / peak;
}

float EaseProgress(const MotionSpec& spec, float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    switch (spec.ease) {
        case iam_ease_cubic_bezier: return CubicBezierY(spec.p0, spec.p1, spec.p2, spec.p3, t);
        case iam_ease_spring:       return SpringUnit(t, spec.p0, spec.p1, spec.p2, spec.p3);
        default:                    return iam_eval_preset(spec.ease, t);
    }
}

ImVec4 MixOklab(ImVec4 a, ImVec4 b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return iam_get_blended_color(a, b, t, iam_col_oklab);
}

// --- ImAnimContext -----------------------------------------------------------------------------------
ImAnimContext::Entry* ImAnimContext::Find(ImGuiID id) {
    const int slot = m_Index.GetInt(id, 0);
    return slot > 0 ? &m_Entries[slot - 1] : nullptr;
}
const ImAnimContext::Entry* ImAnimContext::Find(ImGuiID id) const {
    const int slot = m_Index.GetInt(id, 0);
    return slot > 0 ? &m_Entries[slot - 1] : nullptr;
}

ImAnimContext::Entry& ImAnimContext::Add(ImGuiID id, Kind kind) {
    Entry e;
    e.id = id;
    e.kind = kind;
    e.from = 0.0f;
    e.from4 = ImVec4(0, 0, 0, 0);
    if (kind == Kind::Clip) ++m_LiveClips;
    e.aux = -1.0f;
    e.start = m_Time;
    e.duration = 0.0f;
    e.touched = m_Frame;
    m_Entries.push_back(e);
    m_Index.SetInt(id, m_Entries.Size);
    m_IdleFrames = 0;
    m_PoolsReleased = false;
    return m_Entries.back();
}

void ImAnimContext::Remove(ImGuiID id) {
    const int slot = m_Index.GetInt(id, 0);
    if (slot <= 0) return;
    const int idx = slot - 1;
    if (m_Entries[idx].kind == Kind::Clip) {
        // Free the instance's values now; its slot is reclaimed by iam_clip_gc once no clip plays.
        iam_instance inst = iam_get_instance(id);
        if (inst.valid()) inst.destroy();
        --m_LiveClips;
        m_ClipSlotsDirty = true;
    }
    const int last = m_Entries.Size - 1;
    if (idx != last) {
        m_Entries[idx] = m_Entries[last];
        m_Index.SetInt(m_Entries[idx].id, idx + 1);
    }
    m_Entries.pop_back();
    m_Index.SetInt(id, 0);
    ++m_DeadKeys;
    if (m_Entries.Size == 0) {
        // Idle: give the memory back now, not at the next resize.
        m_Entries.clear();
        m_Index.Clear();
        m_DeadKeys = 0;
    } else if (m_DeadKeys > m_Entries.Size * 2 + 16) {
        CompactIndex();
    }
}

void ImAnimContext::CompactIndex() {
    m_Index.Clear();
    for (int i = 0; i < m_Entries.Size; ++i) m_Index.SetInt(m_Entries[i].id, i + 1);
    m_DeadKeys = 0;
}

size_t ImAnimContext::HeldBytes() const {
    return (size_t)m_Entries.Capacity * sizeof(Entry) + (size_t)m_Index.Data.Capacity * sizeof(ImGuiStoragePair);
}

void ImAnimContext::Reset() {
    for (int i = m_Entries.Size - 1; i >= 0; --i)
        if (m_Entries[i].kind == Kind::Clip) Remove(m_Entries[i].id);
    ReclaimClipSlots();
    m_LiveClips = 0;
    m_Entries.clear();
    m_Index.Clear();
    m_DeadKeys = 0;
    m_IdleFrames = 0;
    iam_pool_clear();
    m_PoolsReleased = true;
}

bool ImAnimContext::SweepGateClosed() const {
    return m_PointerSpeed > HoverGatePxPerSec * Scale();
}

void ImAnimContext::NewFrame() {
    ImGuiIO& io = ImGui::GetIO();
    ++m_Frame;
    // The speed preference (and the debugger's slow-mo) scale the motion clock and ImAnim's own.
    m_Dt = io.DeltaTime * TimeScale();
    m_Time += (double)m_Dt;
    iam_set_global_time_scale(TimeScale());
    iam_update_begin_frame();
    if (m_DebuggerOpenLastFrame) iam_profiler_begin_frame();
    LayoutMotionNewFrame();

    // Pointer velocity (px/s), smoothed so a single jittery frame can't open or close the gate.
    float speed = 0.0f;
    if (io.DeltaTime > 0.0f && ImGui::IsMousePosValid(&io.MousePos) && ImGui::IsMousePosValid(&io.MousePosPrev)) {
        const ImVec2 d = io.MouseDelta;
        speed = std::sqrt(d.x * d.x + d.y * d.y) / io.DeltaTime;
    }
    m_PointerSpeed += (speed - m_PointerSpeed) * PointerVelocitySmoothing;

    // Abandoned entries: their widget wasn't drawn for a couple of frames (window closed, row
    // scrolled away, section collapsed under its parent). They'd be at rest the next time anyone
    // looks, so drop them. Walk backwards: Remove swaps the last entry into the hole.
    for (int i = m_Entries.Size - 1; i >= 0; --i)
        if (m_Entries[i].touched < m_Frame - kStaleFrames) Remove(m_Entries[i].id);

    if (m_Frame % kImAnimGcInterval == 0) iam_gc((unsigned)(kImAnimGcInterval * 2));

    // Clips advance only while one plays; a finished instance is forgotten (its final values read
    // through ClipFloat's fallback from then on).
    if (m_LiveClips > 0) {
        iam_clip_update(io.DeltaTime);
        for (int i = m_Entries.Size - 1; i >= 0; --i) {
            if (m_Entries[i].kind != Kind::Clip) continue;
            if (!iam_get_instance(m_Entries[i].id).is_playing()) Remove(m_Entries[i].id);
        }
    }
    if (m_LiveClips == 0 && m_ClipSlotsDirty) ReclaimClipSlots();

    ScrollInertiaFrame();

    // The startup sequence settles at its end, or at once on any click or key.
    if (m_StartupRunning) {
        bool input = ImGui::IsAnyMouseDown() || io.InputQueueCharacters.Size > 0;
        for (int k = ImGuiKey_NamedKey_BEGIN; !input && k < ImGuiKey_NamedKey_END; ++k) {
            if (k >= ImGuiKey_MouseLeft && k <= ImGuiKey_MouseWheelY) continue;
            if (ImGui::IsKeyPressed((ImGuiKey)k, false)) input = true;
        }
        if (input || !CategoryOn(MotionCategory::Startup) || StartupElapsed() > StartupSeconds + 0.4f)
            m_StartupRunning = false;
    }

    if (m_Entries.Size == 0) {
        if (++m_IdleFrames >= kIdleFramesBeforePoolRelease && !m_PoolsReleased) {
            iam_pool_clear();
            m_PoolsReleased = true;
        }
    }
}

float ImAnimContext::Animate(ImGuiID id, float target, float rest, const MotionSpec& in, const MotionSpec& out) {
    const bool atRest = std::fabs(target - rest) < kSettleEps;
    Entry* e = Find(id);
    if (!e) {
        if (atRest || m_ReduceMotion) return target; // nothing moving: no entry, no ImAnim call
        e = &Add(id, Kind::Tween);
        e->from = rest;
        // ImAnim may still hold an orphaned channel for this id (not collected yet) whose value is
        // wherever it last landed. Snap it to the real starting point first so the tween departs
        // from what is on screen, not from stale state.
        iam_tween_float(id, kChannelValue, rest, 0.0f, ToDesc(in), iam_policy_cut, 0.0f, rest);
    }
    if (m_ReduceMotion) {
        Remove(id);
        return target;
    }
    e->touched = m_Frame;
    const MotionSpec& spec = atRest ? out : in;
    const float v = iam_tween_float(id, kChannelValue, target, spec.durationMs * 0.001f, ToDesc(spec),
                                    iam_policy_crossfade, ImGui::GetIO().DeltaTime, e->from);
    if (atRest && std::fabs(v - target) < kSettleEps) {
        Remove(id); // landed back on rest: forget it
        return target;
    }
    return v;
}

float ImAnimContext::AnimateToggle(ImGuiID id, bool open, bool wasOpen, const MotionSpec& opening, const MotionSpec& closing) {
    const float target = open ? 1.0f : 0.0f;
    // On the flip frame the rest value is where it came from, so an entry starts there; after that
    // rest == target and the entry lives exactly until the tween lands.
    const float rest = wasOpen ? 1.0f : 0.0f;
    const MotionSpec& spec = open ? opening : closing;
    return Animate(id, target, Find(id) ? target : rest, spec, spec);
}

float ImAnimContext::HoverGlow(ImGuiID id, bool hovered) {
    bool lit = hovered;
    if (lit && SweepGateClosed() && !Find(id)) lit = false; // a fast sweep doesn't light new rows
    return Animate(id, lit ? 1.0f : 0.0f, 0.0f, HoverIn, HoverOut);
}

void ImAnimContext::Pulse(ImGuiID id, float durationSeconds, float delaySeconds) {
    if (m_ReduceMotion) { Remove(id); return; }
    Entry* e = Find(id);
    if (!e) e = &Add(id, Kind::Pulse);
    e->kind = Kind::Pulse;
    e->start = m_Time + (double)delaySeconds;
    e->duration = durationSeconds > 1e-4f ? durationSeconds : 1e-4f;
    e->touched = m_Frame;
}

float ImAnimContext::PulseElapsed(ImGuiID id, float fallback) {
    Entry* e = Find(id);
    if (!e || e->kind != Kind::Pulse) return fallback;
    e->touched = m_Frame;
    return (float)(m_Time - e->start);
}

float ImAnimContext::PulseProgress(ImGuiID id) {
    Entry* e = Find(id);
    if (!e || e->kind != Kind::Pulse) return 1.0f;
    e->touched = m_Frame;
    const float p = (float)((m_Time - e->start) / (double)e->duration);
    if (p >= 1.0f) {
        Remove(id); // reported finished once; from now on "absent" means the same thing
        return 1.0f;
    }
    return p < 0.0f ? 0.0f : p;
}

bool ImAnimContext::PulseActive(ImGuiID id) const {
    const Entry* e = Find(id);
    return e && e->kind == Kind::Pulse;
}

float ImAnimContext::Aux(ImGuiID id, float fallback) const {
    const Entry* e = Find(id);
    return e ? e->aux : fallback;
}

void ImAnimContext::SetAux(ImGuiID id, float value) {
    if (Entry* e = Find(id)) e->aux = value;
}

void ImAnimContext::ReclaimClipSlots() {
    // iam_instance::destroy clears an instance's values but leaves its slot; iam_clip_gc drops
    // slots not seen since the clip clock last ticked. Tick it once (nothing is playing, so this
    // evaluates nothing) and sweep everything older than that.
    iam_clip_update(0.0f);
    iam_clip_gc(0);
    m_ClipSlotsDirty = false;
}

ImVec4 ImAnimContext::AnimateColor(ImGuiID id, ImVec4 target, ImVec4 rest, const MotionSpec& in, const MotionSpec& out) {
    const bool atRest = NearColor(target, rest);
    Entry* e = Find(id);
    if (!e) {
        if (atRest || m_ReduceMotion) return target;
        e = &Add(id, Kind::Color);
        e->from4 = rest;
        iam_tween_color(id, kChannelColor, rest, 0.0f, ToDesc(in), iam_policy_cut, iam_col_oklab, 0.0f, rest);
    }
    if (m_ReduceMotion) {
        Remove(id);
        return target;
    }
    e->touched = m_Frame;
    const MotionSpec& spec = atRest ? out : in;
    const ImVec4 v = iam_tween_color(id, kChannelColor, target, spec.durationMs * 0.001f, ToDesc(spec),
                                     iam_policy_crossfade, iam_col_oklab, ImGui::GetIO().DeltaTime, e->from4);
    if (atRest && NearColor(v, target)) {
        Remove(id);
        return target;
    }
    return v;
}

ImVec2 ImAnimContext::Animate2(ImGuiID id, ImVec2 target, ImVec2 rest, const MotionSpec& spec) {
    const bool atRest = NearVec2(target, rest);
    Entry* e = Find(id);
    if (!e) {
        if (atRest || m_ReduceMotion) return target;
        e = &Add(id, Kind::Vec2);
        e->from4 = ImVec4(rest.x, rest.y, 0, 0);
        iam_tween_vec2(id, kChannelVec2, rest, 0.0f, ToDesc(spec), iam_policy_cut, 0.0f, rest);
    }
    if (m_ReduceMotion) {
        Remove(id);
        return target;
    }
    e->touched = m_Frame;
    const ImVec2 v = iam_tween_vec2(id, kChannelVec2, target, spec.durationMs * 0.001f, ToDesc(spec),
                                    iam_policy_crossfade, ImGui::GetIO().DeltaTime, ImVec2(e->from4.x, e->from4.y));
    if (atRest && NearVec2(v, target)) {
        Remove(id);
        return target;
    }
    return v;
}

float ImAnimContext::Noise(ImGuiID seed, float speedHz, float amplitude) const {
    if (m_ReduceMotion) return 0.0f;
    iam_noise_opts opts;
    opts.type = iam_noise_simplex;
    opts.octaves = 2;
    opts.persistence = 0.5f;
    // Each seed walks its own row of the 2D field, so two indicators never move in lockstep.
    const float row = (float)(seed % 1024u) * 7.31f;
    return amplitude * iam_noise_2d((float)m_Time * speedHz, row, opts);
}

float ImAnimContext::Oscillate(float hz, float phase) const {
    if (m_ReduceMotion) return 0.0f;
    return std::sin(((float)m_Time * hz + phase) * 6.28318530718f);
}

void ImAnimContext::TriggerWiggle(ImGuiID id, const ShakeSpec& spec) {
    Pulse(id, spec.durationMs * 0.001f);
}

float ImAnimContext::Wiggle(ImGuiID id, const ShakeSpec& spec) {
    if (!PulseActive(id)) return 0.0f;
    const float t = PulseElapsed(id, 0.0f);
    if (PulseProgress(id) >= 1.0f) return 0.0f;
    return SpringImpulse(t, spec.mass, spec.stiffness, spec.damping, spec.amplitudePx * Scale());
}

void ImAnimContext::DefineClip(ImGuiID clipId, const ClipKey* keys, int count) {
    EnsureClipSystem();
    if (iam_clip_exists(clipId)) return;
    iam_clip clip = iam_clip::begin(clipId);
    for (int i = 0; i < count; ++i)
        clip.key_float(keys[i].channel, keys[i].timeMs * 0.001f, keys[i].value, keys[i].ease);
    clip.end();
}

bool ImAnimContext::ClipDefined(ImGuiID clipId) { return iam_clip_exists(clipId); }
float ImAnimContext::ClipSeconds(ImGuiID clipId) { return iam_clip_exists(clipId) ? iam_clip_duration(clipId) : 0.0f; }

void ImAnimContext::PlayClip(ImGuiID id, ImGuiID clipId) {
    if (m_ReduceMotion || !iam_clip_exists(clipId)) { Remove(id); return; }
    Entry* e = Find(id);
    if (e && e->kind != Kind::Clip) { Remove(id); e = nullptr; }
    if (!e) e = &Add(id, Kind::Clip);
    e->touched = m_Frame;
    iam_play(clipId, id);
}

float ImAnimContext::ClipFloat(ImGuiID id, ImGuiID channel, float fallback) {
    Entry* e = Find(id);
    if (!e || e->kind != Kind::Clip) return fallback;
    e->touched = m_Frame;
    float v = fallback;
    if (!iam_get_instance(id).get_float(channel, &v)) return fallback;
    return v;
}

bool ImAnimContext::ClipPlaying(ImGuiID id) const {
    const Entry* e = Find(id);
    return e && e->kind == Kind::Clip;
}

void ImAnimContext::DefinePath(ImGuiID pathId, ImVec2 start, const PathSeg* segs, int count) {
    if (iam_path_exists(pathId)) return;
    iam_path p = iam_path::begin(pathId, start);
    for (int i = 0; i < count; ++i) {
        if (segs[i].kind == PathSeg::Cubic) p.cubic_to(segs[i].c1, segs[i].c2, segs[i].end);
        else                                p.line_to(segs[i].end);
    }
    p.end();
    iam_path_build_arc_lut(pathId, 48); // constant speed along the stroke: a tick draws evenly
}

bool ImAnimContext::PathDefined(ImGuiID pathId) { return iam_path_exists(pathId); }

ImVec2 ImAnimContext::PathPoint(ImGuiID pathId, float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return iam_path_evaluate_at_distance(pathId, t * iam_path_length(pathId));
}

ImVec2 ImAnimContext::PathMorph(ImGuiID pathA, ImGuiID pathB, float t, float blend) {
    return iam_path_morph(pathA, pathB, t, blend);
}

void ImAnimContext::TextStagger(ImGuiID id, const char* text, ImVec2 pos, ImU32 col, float progress,
                                float charDelayMs, float charMs, float risePx) {
    iam_text_stagger_opts o;
    o.pos = pos;
    o.effect = iam_text_fx_slide_up;
    o.char_delay = charDelayMs * 0.001f;
    o.char_duration = charMs * 0.001f;
    o.effect_intensity = risePx * Scale();
    o.ease = iam_ease_preset(iam_ease_out_cubic);
    o.color = col;
    iam_text_stagger(id, text, m_ReduceMotion ? 1.0f : progress, o);
}

float ImAnimContext::TextStaggerSeconds(const char* text, float charDelayMs, float charMs) {
    iam_text_stagger_opts o;
    o.char_delay = charDelayMs * 0.001f;
    o.char_duration = charMs * 0.001f;
    return iam_text_stagger_duration(text, o);
}

void ImAnimContext::ScrollToY(float y, const MotionSpec& spec) {
    if (m_ReduceMotion) { ImGui::SetScrollY(y); return; }
    iam_scroll_to_y(y, spec.durationMs * 0.001f, ToDesc(spec));
}

void ImAnimContext::ScrollToBottom(const MotionSpec& spec) {
    if (m_ReduceMotion) { ImGui::SetScrollY(ImGui::GetScrollMaxY()); return; }
    iam_scroll_to_bottom(spec.durationMs * 0.001f, ToDesc(spec));
}

// --- Motion v2: rolling numbers ----------------------------------------------------------------------
// A change starts a short slide (a Pulse on `id`) and remembers the number it left in window storage;
// the readout itself is always the new value. Only the digits that differ move, one step each.
double ImAnimContext::RollingValue(ImGuiID id, double value) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID prevKey = id ^ kRollPrevSalt;
    const double prev = LoadDouble(st, prevKey, value);
    StoreDouble(st, prevKey, value);
    if (!CategoryOn(MotionCategory::RollingNumbers)) {
        Remove(id);
        return value;
    }
    if (prev != value) {
        // A change mid-slide departs from the last value, not the half-moved digits: it stays one step.
        StoreDouble(st, id ^ kRollFromSalt, prev);
        Pulse(id, DigitSlide.durationMs * 0.001f);
    }
    return value;
}

namespace {
// Scaled-integer digits of |v| at `decimals` places, at least decimals + 1 of them.
int DigitCount(double scaledAbs, int decimals) {
    int n = 1;
    double p = 10.0;
    while (scaledAbs >= p - 0.5 && n < 18) { ++n; p *= 10.0; }
    return n < decimals + 1 ? decimals + 1 : n;
}
double Pow10(int k) { double p = 1.0; while (k-- > 0) p *= 10.0; return p; }
int DigitAt(double scaled, int k) { return (int)std::fmod(std::floor(scaled / Pow10(k)), 10.0); }
} // namespace

float ImAnimContext::OdometerWidth(double value, int decimals, const char* suffix) {
    decimals = decimals < 0 ? 0 : (decimals > 6 ? 6 : decimals);
    const float w0 = ImGui::CalcTextSize("0").x;
    const double scaled = std::fabs(value) * Pow10(decimals);
    float w = (float)DigitCount(std::floor(scaled + 0.5), decimals) * w0;
    if (decimals > 0) w += ImGui::CalcTextSize(".").x;
    if (value < 0.0) w += ImGui::CalcTextSize("-").x;
    if (suffix && *suffix) w += ImGui::CalcTextSize(suffix).x;
    return w;
}

float ImAnimContext::Odometer(ImGuiID id, ImDrawList* dl, ImVec2 pos, double value, int decimals, ImU32 col, const char* suffix) {
    decimals = decimals < 0 ? 0 : (decimals > 6 ? 6 : decimals);
    RollingValue(id, value);
    const bool sliding = PulseActive(id);
    const float p = sliding ? EaseProgress(DigitSlide, PulseProgress(id)) : 1.0f;
    const float lineH = ImGui::GetTextLineHeight();
    const float w0 = ImGui::CalcTextSize("0").x;
    const float dotW = ImGui::CalcTextSize(".").x;
    const double unit = Pow10(decimals);
    const double scaledTo = std::floor(std::fabs(value) * unit + 0.5);
    const double from = sliding ? LoadDouble(ImGui::GetStateStorage(), id ^ kRollFromSalt, value) : value;
    const double scaledFrom = std::floor(std::fabs(from) * unit + 0.5);
    const int nFrom = DigitCount(scaledFrom, decimals);
    // Up when the number grows (the new digit rises in from below), down when it shrinks.
    const float dir = value >= from ? 1.0f : -1.0f;
    const float travel = lineH * DigitSlideTravel;
    const int alpha = (int)((col >> IM_COL32_A_SHIFT) & 0xFF);
    auto withAlpha = [&](float a) { return (col & ~IM_COL32_A_MASK) | ((ImU32)(alpha * a + 0.5f) << IM_COL32_A_SHIFT); };

    // Digits sit in fixed-width slots so the readout doesn't jitter, laid out for the new value.
    float x = pos.x;
    if (value < 0.0) {
        dl->AddText(ImVec2(x, pos.y), col, "-");
        x += ImGui::CalcTextSize("-").x;
    }
    if (sliding) dl->PushClipRect(ImVec2(pos.x, pos.y), ImVec2(pos.x + 4096.0f, pos.y + lineH), true);
    for (int k = DigitCount(scaledTo, decimals) - 1; k >= 0; --k) {
        const int to = DigitAt(scaledTo, k);
        const int was = k < nFrom ? DigitAt(scaledFrom, k) : -1; // -1: the old number had no digit here
        char a[2] = {(char)('0' + to), 0};
        const float ax = x + (w0 - ImGui::CalcTextSize(a).x) * 0.5f;
        if (!sliding || was == to) {
            dl->AddText(ImVec2(ax, pos.y), col, a);
        } else {
            if (was >= 0) {
                char b[2] = {(char)('0' + was), 0};
                const float bx = x + (w0 - ImGui::CalcTextSize(b).x) * 0.5f;
                dl->AddText(ImVec2(bx, pos.y - dir * p * travel), withAlpha(1.0f - p), b);
            }
            dl->AddText(ImVec2(ax, pos.y + dir * (1.0f - p) * travel), withAlpha(p), a);
        }
        x += w0;
        if (k == decimals && decimals > 0) {
            dl->AddText(ImVec2(x, pos.y), col, ".");
            x += dotW;
        }
    }
    if (sliding) dl->PopClipRect();
    if (suffix && *suffix) {
        dl->AddText(ImVec2(x, pos.y), col, suffix);
        x += ImGui::CalcTextSize(suffix).x;
    }
    return x - pos.x;
}

// --- Motion v2: inertia -------------------------------------------------------------------------------
void ImAnimContext::InertiaTrack(ImGuiID id, ImVec2 delta) {
    const float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f) return;
    Entry* e = Find(id);
    if (e && e->kind != Kind::Inertia) { Remove(id); e = nullptr; }
    if (!e) {
        if (!CategoryOn(MotionCategory::Inertia)) return;
        e = &Add(id, Kind::Inertia);
    }
    e->duration = kInertiaTracking;
    e->touched = m_Frame;
    const ImVec2 sample(delta.x / dt, delta.y / dt);
    e->from4.x += (sample.x - e->from4.x) * VelocitySmoothing;
    e->from4.y += (sample.y - e->from4.y) * VelocitySmoothing;
}

void ImAnimContext::InertiaRelease(ImGuiID id) {
    Entry* e = Find(id);
    if (!e || e->kind != Kind::Inertia) return;
    const float speed = std::sqrt(e->from4.x * e->from4.x + e->from4.y * e->from4.y);
    if (!CategoryOn(MotionCategory::Inertia) || speed < FlingMinPxPerSec * Scale()) { Remove(id); return; }
    e->duration = kInertiaCoasting;
    e->start = -1.0;
    e->touched = m_Frame;
    // A soft whoosh, louder and a touch higher for a faster fling.
    const float k = ImClamp(speed / (FlingMinPxPerSec * Scale() * 8.0f), 0.0f, 1.0f);
    EditorSound::Play(EditorSound::Event::FlingRelease, ImGui::GetIO().MousePos.x / ImMax(1.0f, ImGui::GetIO().DisplaySize.x),
                      0.94f + 0.12f * k, -6.0f + 6.0f * k);
}

void ImAnimContext::Fling(ImGuiID id, ImVec2 velocity) {
    if (!CategoryOn(MotionCategory::Inertia)) return;
    Entry* e = Find(id);
    if (e && e->kind != Kind::Inertia) { Remove(id); e = nullptr; }
    if (!e) e = &Add(id, Kind::Inertia);
    if (e->duration != kInertiaCoasting) e->from4 = ImVec4(0, 0, 0, 0);
    e->from4.x += velocity.x;
    e->from4.y += velocity.y;
    e->duration = kInertiaCoasting;
    e->start = -1.0;
    e->touched = m_Frame;
}

bool ImAnimContext::InertiaCoasting(ImGuiID id) const {
    const Entry* e = Find(id);
    return e && e->kind == Kind::Inertia && e->duration == kInertiaCoasting;
}

ImVec2 ImAnimContext::InertiaDelta(ImGuiID id) {
    Entry* e = Find(id);
    if (!e || e->kind != Kind::Inertia || e->duration != kInertiaCoasting) return ImVec2(0, 0);
    e->touched = m_Frame;
    // Once per frame: a second read in the same frame returns the same step, not a second one.
    if (e->start == (double)m_Frame) return ImVec2(e->from, e->aux);
    if (!CategoryOn(MotionCategory::Inertia)) { Remove(id); return ImVec2(0, 0); }
    const float decay = std::exp(-InertiaFriction * m_Dt);
    e->from4.x *= decay;
    e->from4.y *= decay;
    const ImVec2 d(e->from4.x * m_Dt, e->from4.y * m_Dt);
    const float speed = std::sqrt(e->from4.x * e->from4.x + e->from4.y * e->from4.y);
    if (speed < InertiaStopPxPerSec * Scale()) { Remove(id); return d; }
    e->start = (double)m_Frame;
    e->from = d.x;
    e->aux = d.y;
    return d;
}

void ImAnimContext::ScrollInertiaFrame() {
    ImGuiContext& g = *GImGui;
    // A wheel notch this frame: ImGui has set the wheeling window's scroll target. Take it back and
    // turn it into velocity, so the list glides there (and a little past, for a fast run of notches).
    if (CategoryOn(MotionCategory::Inertia) && g.WheelingWindow && g.WheelingWindowScrolledFrame == g.FrameCount &&
        g.WheelingWindow->ScrollTarget.y != FLT_MAX && g.WheelingWindow->ScrollTargetCenterRatio.y == 0.0f) {
        ImGuiWindow* w = g.WheelingWindow;
        const float delta = w->ScrollTarget.y - w->Scroll.y;
        w->ScrollTarget.y = FLT_MAX;
        const ImGuiID key = w->ID ^ kScrollSalt;
        Entry* e = Find(key);
        if (e && e->kind != Kind::Inertia) { Remove(key); e = nullptr; }
        float boost = 1.0f;
        if (e && e->duration == kInertiaScroll && (e->from4.y > 0.0f) == (delta > 0.0f)) {
            // Notches close together accelerate, like a trackpad's momentum.
            const float sinceMs = (float)(m_Time - (double)e->aux) * 1000.0f / (TimeScale() > 0.0f ? TimeScale() : 1.0f);
            if (sinceMs < WheelBoostWindowMs) boost = std::min(WheelBoostMax, 1.0f + (WheelBoostWindowMs - sinceMs) / WheelBoostWindowMs * (WheelBoostMax - 1.0f));
        } else if (e) {
            e->from4.y = 0.0f; // reversing: stop the old glide first
        }
        if (!e) e = &Add(key, Kind::Inertia);
        e->duration = kInertiaScroll;
        // A coast from v travels v / friction: a notch's velocity is its distance times friction.
        e->from4.y += delta * InertiaFriction * boost;
        e->aux = (float)m_Time;
        e->touched = m_Frame;
    }
    for (int i = m_Entries.Size - 1; i >= 0; --i) {
        Entry& e = m_Entries[i];
        if (e.kind != Kind::Inertia || e.duration != kInertiaScroll) continue;
        const ImGuiID key = e.id;
        ImGuiWindow* w = ImGui::FindWindowByID(key ^ kScrollSalt);
        if (!w || !w->WasActive || !CategoryOn(MotionCategory::Inertia)) { Remove(key); continue; }
        e.touched = m_Frame;
        e.from4.y *= std::exp(-InertiaFriction * m_Dt);
        float y = w->Scroll.y + e.from4.y * m_Dt;
        bool edge = false;
        if (y <= 0.0f) { y = 0.0f; edge = true; }
        if (y >= w->ScrollMax.y) { y = w->ScrollMax.y; edge = true; }
        ImGui::SetScrollY(w, y);
        if (edge || std::fabs(e.from4.y) < InertiaStopPxPerSec * Scale()) Remove(key);
    }
}

// --- Motion v2: living gradients ------------------------------------------------------------------
void ImAnimContext::DrawSheen(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImGuiID seed, float strength) const {
    if (!dl || strength <= 0.0f || !CategoryOn(MotionCategory::Gradients)) return;
    const float w = mx.x - mn.x;
    if (w <= 1.0f) return;
    const float band = w * SheenBandFraction;
    const float phase = Fract((float)m_Time / SheenSweepSeconds + (float)(seed % 997u) / 997.0f);
    const float cx = mn.x - band + phase * (w + 2.0f * band);
    // The band breathes a little on smooth noise so two bars never pulse in step.
    const float a = SheenAlpha * strength * (0.85f + 0.15f * Noise(seed, 0.6f, 1.0f));
    const ImU32 clear = IM_COL32(255, 255, 255, 0);
    const ImU32 lit = IM_COL32(255, 255, 255, (int)(ImClamp(a, 0.0f, 1.0f) * 255.0f));
    dl->PushClipRect(mn, mx, true);
    dl->AddRectFilledMultiColor(ImVec2(cx - band, mn.y), ImVec2(cx, mx.y), clear, lit, lit, clear);
    dl->AddRectFilledMultiColor(ImVec2(cx, mn.y), ImVec2(cx + band, mx.y), lit, clear, clear, lit);
    dl->PopClipRect();
}

void ImAnimContext::DrawTravellingRim(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float lapSeconds, float arc, float alpha,
                                      float thickness, ImGuiID seed) const {
    if (!dl || alpha <= 0.0f || !CategoryOn(MotionCategory::Gradients)) return;
    const float w = mx.x - mn.x, h = mx.y - mn.y;
    if (w <= 2.0f || h <= 2.0f) return;
    const float perim = 2.0f * (w + h);
    const float head = Fract((float)m_Time / (lapSeconds > 0.1f ? lapSeconds : 0.1f) + (float)(seed % 991u) / 991.0f) * perim;
    const float len = perim * arc;
    // White in the middle of the segment, clear at both ends, blended in OKLAB (ImAnim's gradient).
    static const iam_gradient s_Grad = iam_gradient()
        .add(0.0f, ImVec4(1, 1, 1, 0))
        .add(0.5f, ImVec4(1, 1, 1, 1))
        .add(1.0f, ImVec4(1, 1, 1, 0));
    constexpr int kSteps = 40;
    ImVec2 prev = PerimeterPoint(mn, mx, head - len);
    for (int i = 1; i <= kSteps; ++i) {
        const float u = (float)i / (float)kSteps;
        const ImVec2 p = PerimeterPoint(mn, mx, head - len + u * len);
        ImVec4 c = s_Grad.sample(u - 0.5f / kSteps, iam_col_oklab);
        c.w *= alpha;
        dl->AddLine(prev, p, ImGui::ColorConvertFloat4ToU32(c), thickness);
        prev = p;
    }
}

void ImAnimContext::DrawCursorGlow(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImVec2 cursor, float strength, float rounding) const {
    (void)rounding;
    if (!dl || strength <= 0.0f || !CategoryOn(MotionCategory::Gradients)) return;
    const float r = CursorGlowRadiusPx * Scale();
    dl->PushClipRect(mn, mx, true);
    // Concentric discs, each a touch more transparent: a soft radial falloff without a texture.
    constexpr int kRings = 6;
    for (int i = kRings; i >= 1; --i) {
        const float t = (float)i / (float)kRings;
        const float a = CursorGlowAlpha * strength * (1.0f - t * t) / (float)kRings * 2.4f;
        dl->AddCircleFilled(cursor, r * t, IM_COL32(255, 255, 255, (int)(ImClamp(a, 0.0f, 1.0f) * 255.0f)), 24);
    }
    dl->PopClipRect();
}

// --- Motion v2: layers ------------------------------------------------------------------------------
int ImAnimContext::BeginLayer(ImDrawList* dl) { return dl ? dl->VtxBuffer.Size : 0; }

void ImAnimContext::EndLayer(ImDrawList* dl, int vtxStart, const LayerTransform& t) {
    if (!dl || vtxStart >= dl->VtxBuffer.Size) return;
    const bool identity = t.scale.x == 1.0f && t.scale.y == 1.0f && t.rotation == 0.0f &&
                          t.translate.x == 0.0f && t.translate.y == 0.0f;
    if (!identity) {
        // pivot + translate, after rotate * scale about the pivot (ImAnim's iam_transform, row-major).
        iam_transform xf(ImVec2(t.pivot.x + t.translate.x, t.pivot.y + t.translate.y), t.rotation, t.scale);
        float m[6];
        iam_transform_to_matrix(xf, m);
        for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            const float x = v.pos.x - t.pivot.x, y = v.pos.y - t.pivot.y;
            v.pos = ImVec2(m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]);
        }
    }
    if (t.alpha < 1.0f) {
        const float a = t.alpha < 0.0f ? 0.0f : t.alpha;
        for (int i = vtxStart; i < dl->VtxBuffer.Size; ++i) {
            ImU32& c = dl->VtxBuffer[i].col;
            const ImU32 ca = (ImU32)((float)((c >> IM_COL32_A_SHIFT) & 0xFF) * a + 0.5f);
            c = (c & ~IM_COL32_A_MASK) | (ca << IM_COL32_A_SHIFT);
        }
    }
}

float ImAnimContext::GridRippleDelay(int col, int row, int cols, int rows, int originCol, int originRow, float stepMs, int capSteps) {
    // Euclidean distance from the origin tile, so the wave is round. (ImAnim's iam_stagger_grid_delay
    // normalises by the farthest tile, an O(tiles) scan per call; a ripple wants a fixed speed.)
    (void)cols; (void)rows;
    const float dx = (float)(col - originCol), dy = (float)(row - originRow);
    float d = std::sqrt(dx * dx + dy * dy);
    if (d > (float)capSteps) d = (float)capSteps;
    return d * stepMs * 0.001f;
}

// --- Motion v2: text on paths ---------------------------------------------------------------------
void ImAnimContext::TextOnPath(ImGuiID pathId, const char* text, ImVec2 origin, ImU32 col, float offsetPx, bool centred) {
    if (!text || !*text || !iam_path_exists(pathId)) return;
    iam_text_path_opts o;
    o.origin = origin;
    o.offset = offsetPx;
    o.align = centred ? iam_text_align_center : iam_text_align_start;
    o.color = col;
    iam_text_path(pathId, text, o);
}

void ImAnimContext::TextAlongCubic(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, const char* text,
                                   ImU32 col, float centerT, float liftPx) {
    if (!dl || !text || !*text) return;
    // Read left to right: walk the curve from whichever end is further left.
    if (p3.x < p0.x) { std::swap(p0, p3); std::swap(p1, p2); centerT = 1.0f - centerT; }
    auto at = [&](float u) {
        const float v = 1.0f - u;
        return ImVec2(v * v * v * p0.x + 3 * v * v * u * p1.x + 3 * v * u * u * p2.x + u * u * u * p3.x,
                      v * v * v * p0.y + 3 * v * v * u * p1.y + 3 * v * u * u * p2.y + u * u * u * p3.y);
    };
    constexpr int kLut = 32;
    float lut[kLut + 1];
    lut[0] = 0.0f;
    ImVec2 prev = p0;
    for (int i = 1; i <= kLut; ++i) {
        const ImVec2 p = at((float)i / kLut);
        lut[i] = lut[i - 1] + std::sqrt((p.x - prev.x) * (p.x - prev.x) + (p.y - prev.y) * (p.y - prev.y));
        prev = p;
    }
    const float total = lut[kLut];
    auto uAt = [&](float s) {
        if (s <= 0.0f) return 0.0f;
        if (s >= total) return 1.0f;
        int i = 1;
        while (i < kLut && lut[i] < s) ++i;
        const float seg = lut[i] - lut[i - 1];
        return ((float)(i - 1) + (seg > 0.0f ? (s - lut[i - 1]) / seg : 0.0f)) / kLut;
    };
    const float textW = ImGui::CalcTextSize(text).x;
    if (textW > total * 0.9f) return; // doesn't fit: no label rather than a squashed one
    const float lineH = ImGui::GetTextLineHeight();
    float s = centerT * total - textW * 0.5f;
    for (const char* c = text; *c;) {
        unsigned int cp = 0;
        const int bytes = ImTextCharFromUtf8(&cp, c, nullptr);
        if (bytes <= 0) break;
        char glyph[8] = {};
        std::memcpy(glyph, c, (size_t)ImMin(bytes, 7));
        const float adv = ImGui::CalcTextSize(glyph).x;
        const float u = uAt(s + adv * 0.5f);
        const ImVec2 p = at(u);
        const ImVec2 q = at(ImMin(1.0f, u + 0.01f)), r = at(ImMax(0.0f, u - 0.01f));
        const float ang = std::atan2(q.y - r.y, q.x - r.x);
        const ImVec2 n(std::sin(ang), -std::cos(ang)); // left normal: above the wire
        const ImVec2 centre(p.x + n.x * liftPx, p.y + n.y * liftPx);
        const int v0 = BeginLayer(dl);
        dl->AddText(ImVec2(centre.x - adv * 0.5f, centre.y - lineH * 0.5f), col, glyph);
        LayerTransform t;
        t.pivot = centre;
        t.rotation = ang;
        EndLayer(dl, v0, t);
        s += adv;
        c += bytes;
    }
}

// --- Motion v2: the startup sequence ---------------------------------------------------------------
void ImAnimContext::BeginStartup() {
    if (!CategoryOn(MotionCategory::Startup)) return;
    m_StartupBegan = m_Time;
    m_StartupRunning = true;
}

float ImAnimContext::StartupElapsed() const {
    return m_StartupRunning ? (float)(m_Time - m_StartupBegan) : 1.0e6f;
}

// --- Motion v2: the debugger -----------------------------------------------------------------------
void ImAnimContext::DrawDebugger(bool* open) {
    const bool isOpen = open && *open;
    if (isOpen != m_DebuggerOpenLastFrame) {
        iam_profiler_enable(isOpen); // the profiler only runs while someone is looking at it
        m_DebuggerOpenLastFrame = isOpen;
        if (!isOpen) m_DebugScale = 1.0f;
    }
    if (!isOpen) return;
    iam_profiler_end_frame();
    ImGui::SetNextWindowSize(ImVec2(360.0f * Scale(), 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Motion Debugger", open)) {
        ImGui::TextUnformatted("Slow motion (this session only)");
        float s = m_DebugScale;
        if (ImGui::SliderFloat("##slowmo", &s, 0.05f, 1.0f, "%.2fx")) SetDebugTimeScale(s);
        ImGui::SameLine();
        if (ImGui::SmallButton("1x")) m_DebugScale = 1.0f;
        ImGui::Separator();
        ImGui::Text("Live entries   %d", LiveCount());
        ImGui::Text("Held bytes     %u", (unsigned)HeldBytes());
        ImGui::Text("Pointer speed  %.0f px/s%s", PointerSpeed(), SweepGateClosed() ? "  (gate closed)" : "");
        ImGui::Text("Time scale     %.2fx (speed %.2fx)", TimeScale(), m_Speed);
        ImGui::Text("Startup        %s", m_StartupRunning ? "running" : "idle");
        static const char* kNames[] = {"Theme tweens", "Inertia", "Gradients", "Rolling numbers", "Viewport", "Startup"};
        for (int c = 0; c < (int)MotionCategory::Count; ++c)
            ImGui::Text("%-15s %s", kNames[c], CategoryOn((MotionCategory)c) ? "on" : "off");
        if (ImGui::CollapsingHeader("Live entries")) {
            static const char* kKinds[] = {"tween", "pulse", "colour", "vec2", "clip", "inertia"};
            for (int i = 0; i < m_Entries.Size && i < 64; ++i)
                ImGui::Text("%08X  %s", m_Entries[i].id, kKinds[(int)m_Entries[i].kind]);
        }
        static bool s_Inspector = false;
        ImGui::Checkbox("ImAnim inspector", &s_Inspector);
        if (s_Inspector) iam_show_unified_inspector(&s_Inspector);
    }
    ImGui::End();
}

ImAnimContext& Motion() {
    static ImAnimContext s_Context;
    return s_Context;
}

} // namespace Glass
