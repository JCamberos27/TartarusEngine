#include "GlassMotion.h"

#include "GlassLayout.h" // LayoutMotionNewFrame
#include "GlassTheme.h"  // Scale

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

#include <cmath>

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
    m_Time += (double)io.DeltaTime;
    iam_update_begin_frame();
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

ImAnimContext& Motion() {
    static ImAnimContext s_Context;
    return s_Context;
}

} // namespace Glass
