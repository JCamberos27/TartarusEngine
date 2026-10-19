#pragma once
// The Glass motion runtime: ImAnim driven through a small gate map so animation state only exists
// while something is actually moving.
//
// ImAnim (im_anim.h) keeps one tween channel per (ImGuiID, channel) for as long as something keeps
// calling it; a sleeping channel that is still touched every frame is never collected. Calling
// iam_tween_* for every hovered-once row or every open section on every frame would therefore pin
// a channel per widget for the life of the panel. ImAnimContext sits in front of it:
//
//   - A value at rest has no entry here and makes no ImAnim call. Animate() returns the target
//     directly. Zero bytes, zero work.
//   - When a target departs from the rest value, the id gets an entry and ImAnim gets a channel.
//   - The frame the tween lands on its rest value, the entry is erased and ImAnim is no longer
//     called for it; iam_gc() reclaims the orphaned channel a little later.
//   - When the map is empty, its storage is freed (ImVector::clear releases the buffer), and once
//     it has stayed empty for kIdleFramesBeforePoolRelease frames ImAnim's pools are released too
//     (iam_pool_clear). An idle editor holds no animation memory at all.
//
// Timers (Pulse) follow the same rule: an entry exists from Start until the frame after it
// reports 1.0, then it's gone, and a missing timer reads as "finished". Colour and 2D tweens and
// keyframe-clip instances are gated the same way.
//
// Procedural motion (Noise, Wiggle, Oscillate) is deliberately *not* ImAnim's oscillator / shake /
// noise channels: those keep a per-id state block forever (im_anim.cpp g_osc_map, g_shake_map,
// g_noise_states). Glass evaluates them as pure functions of time instead (ImAnim's stateless
// iam_noise_2d, SpringImpulse), so they hold nothing.
//
// Clip and path *definitions* (DefineClip, DefinePath) are the one fixed exception: a handful of
// small authored shapes registered once at startup and kept for the life of the editor. They are
// not animation state and are not counted by HeldBytes.
//
// Host-only (EditorLayer_Glass.cpp, the unit tests). One instance per process: Glass::Motion().

#include "GlassTokens.h"

#include <imgui.h>

namespace Glass {

// Under-damped spring impulse response in real seconds: displacement of a mass m on spring k with
// damping c, kicked at t = 0 so that its first peak is about `amplitude`. Decays to 0. Used for
// the contextual shake, which needs no channel at all: it's a pure function of the time since
// the event.
float SpringImpulse(float tSeconds, float mass, float stiffness, float damping, float amplitude);
// Damping ratio of (m, k, c): < 1 under-damped (overshoots), 1 critical, > 1 over-damped.
float DampingRatio(float mass, float stiffness, float damping);

// A keyframe of an authored clip (DefineClip): `channel` reaches `value` at `timeMs`, eased from
// the previous key on that channel with `ease` (an iam_ease_type; EaseId mirrors the common ones).
struct ClipKey {
    ImGuiID channel;
    float   timeMs;
    float   value;
    int     ease;
};

// One segment of an authored path (DefinePath), in whatever unit space the caller scales from
// (Glass paths are defined in a 0..1 box). Line uses `end`; Cubic uses c1, c2, end.
struct PathSeg {
    enum Kind : int { Line, Cubic } kind;
    ImVec2 c1, c2, end;
};

// Motion v2 categories, each switchable in Preferences > Viewport > Motion. Reduce Motion turns all
// of them off. A category that's off resolves its motion instantly (or draws it static).
enum class MotionCategory : int {
    ThemeTweens = 0, // theme switch, play / prefab / focus moods
    Inertia,         // coasting scroll, flings, value scrubbing
    Gradients,       // busy sheen, travelling rims, cursor glow
    RollingNumbers,  // odometer readouts
    Viewport,        // gizmo springs, outline sweep, radial menu, view cube
    Startup,         // the startup sequence
    Count
};

// A 2D affine transform for a layer of draw commands (BeginLayer / EndLayer): scale and rotate
// about `pivot`, then translate, then fade. Composed with ImAnim's iam_transform.
struct LayerTransform {
    ImVec2 pivot     = ImVec2(0, 0);
    ImVec2 scale     = ImVec2(1, 1);
    float  rotation  = 0.0f;           // radians
    ImVec2 translate = ImVec2(0, 0);
    float  alpha     = 1.0f;
};

class ImAnimContext {
public:
    // Call once per frame, after ImGui::NewFrame (EditorLayer::BeginFrame). Advances ImAnim's
    // clock, updates the pointer velocity, sweeps entries nobody asked about for a couple of frames
    // (their window closed mid-animation) and releases memory once idle.
    void NewFrame();

    // A float that eases to `target`. `rest` is the value this id holds when it is not animating
    // (0 for a hover glow; the previous open state for a foldout, see AnimateToggle). Returns the
    // current value. `in` is used while heading away from rest, `out` while heading back.
    float Animate(ImGuiID id, float target, float rest, const MotionSpec& in, const MotionSpec& out);
    float Animate(ImGuiID id, float target, float rest, const MotionSpec& spec) { return Animate(id, target, rest, spec, spec); }

    // A value that glides to `target` whenever the target moves (a selection panel sliding from
    // row to row). `previousTarget` is what the caller asked for last frame: when it differs, the
    // glide departs from there; when they match and nothing is in flight, no state exists.
    float Follow(ImGuiID id, float target, float previousTarget, const MotionSpec& spec) {
        return Animate(id, target, Live(id) ? target : previousTarget, spec, spec);
    }

    // A 0..1 openness for something whose open flag can flip: `open` now, `wasOpen` last frame.
    // Animates only around a flip; at rest it's exactly 0 or 1 with no state.
    float AnimateToggle(ImGuiID id, bool open, bool wasOpen, const MotionSpec& opening, const MotionSpec& closing);

    // Hover glow with the velocity gate (GlassTokens.h HoverGatePxPerSec): while the pointer sweeps
    // faster than the gate, a row that isn't already lit doesn't light (no channel is created).
    float HoverGlow(ImGuiID id, bool hovered);

    // A colour that eases to `target` in OKLAB (ImAnim's iam_tween_color), so grey to white or
    // white to red never passes through a muddy midpoint. Same rest rules as Animate.
    ImVec4 AnimateColor(ImGuiID id, ImVec4 target, ImVec4 rest, const MotionSpec& in, const MotionSpec& out);
    ImVec4 AnimateColor(ImGuiID id, ImVec4 target, ImVec4 rest, const MotionSpec& spec) { return AnimateColor(id, target, rest, spec, spec); }

    // A point that eases to `target` (ImAnim's iam_tween_vec2): a tab underline's x and width, a
    // drag ghost trailing the cursor. Follow2 is Follow for two components.
    ImVec2 Animate2(ImGuiID id, ImVec2 target, ImVec2 rest, const MotionSpec& spec);
    ImVec2 Follow2(ImGuiID id, ImVec2 target, ImVec2 previousTarget, const MotionSpec& spec) {
        return Animate2(id, target, Live(id) ? target : previousTarget, spec);
    }

    // --- Procedural, stateless (pure functions of the motion clock; no entry, no channel) ---
    // Smooth noise in [-amplitude, amplitude], drifting at `speedHz`. `seed` picks the stream.
    float Noise(ImGuiID seed, float speedHz, float amplitude) const;
    // A sine in [-1, 1] at `hz` (play-mode breathing). Returns 0 under Reduce Motion.
    float Oscillate(float hz, float phase = 0.0f) const;
    // A one-shot horizontal wiggle (invalid input): TriggerWiggle starts it, Wiggle reads the
    // offset in px (0 when idle). Rides on a Pulse, evaluated with SpringImpulse.
    void  TriggerWiggle(ImGuiID id, const ShakeSpec& spec);
    float Wiggle(ImGuiID id, const ShakeSpec& spec);

    // --- Keyframe clips (ImAnim's iam_clip) ---
    // Register an authored clip once (startup). Keys are grouped by channel in time order.
    static void DefineClip(ImGuiID clipId, const ClipKey* keys, int count);
    static bool ClipDefined(ImGuiID clipId);
    static float ClipSeconds(ImGuiID clipId);
    // Play `clipId` on instance `id` (restarts it if it's already playing). The instance is an
    // entry like any other: gone the frame after it finishes. Reduce Motion skips it.
    void  PlayClip(ImGuiID id, ImGuiID clipId);
    // A channel's value on a playing instance; `fallback` once it has finished or never ran (pass
    // the clip's final value, so a finished clip reads exactly like a settled one).
    float ClipFloat(ImGuiID id, ImGuiID channel, float fallback);
    bool  ClipPlaying(ImGuiID id) const;

    // --- Motion paths (ImAnim's iam_path) ---
    static void   DefinePath(ImGuiID pathId, ImVec2 start, const PathSeg* segs, int count);
    static bool   PathDefined(ImGuiID pathId);
    // A point at arc-length fraction t (0..1) of the path, in its definition space.
    static ImVec2 PathPoint(ImGuiID pathId, float t);
    // A point on the blend of two paths (iam_path_morph): `blend` 0 is path a, 1 is path b.
    static ImVec2 PathMorph(ImGuiID pathA, ImGuiID pathB, float t, float blend);

    // --- Text and scroll ---
    // Per-character stagger (iam_text_stagger: fade + rise) into the current window's draw list.
    // `progress` 0..1 over TextStaggerSeconds. Under Reduce Motion it draws settled.
    void  TextStagger(ImGuiID id, const char* text, ImVec2 pos, ImU32 col, float progress, float charDelayMs, float charMs, float risePx);
    static float TextStaggerSeconds(const char* text, float charDelayMs, float charMs);
    // Ease the current window's scroll (iam_scroll_to_y). Reduce Motion jumps.
    void ScrollToY(float y, const MotionSpec& spec);
    void ScrollToBottom(const MotionSpec& spec);

    // One-shot timers for staggered sequences and notifications.
    void  Pulse(ImGuiID id, float durationSeconds, float delaySeconds = 0.0f);
    // 0..1 progress of a pulse, 1 when it has finished or never existed. Touching it keeps it alive.
    float PulseProgress(ImGuiID id);
    // Seconds since the pulse started (negative during its delay); `fallback` if there's none.
    float PulseElapsed(ImGuiID id, float fallback);
    bool  PulseActive(ImGuiID id) const;

    // A scratch float riding on a live entry (a reveal keeps its measured natural height here while
    // it animates). Gone with the entry; `fallback` when there is none.
    float Aux(ImGuiID id, float fallback) const;
    void  SetAux(ImGuiID id, float value);
    bool  Live(ImGuiID id) const { return Find(id) != nullptr; }

    // Pointer speed in screen px/s, smoothed. Exposed for the overlays' debug readout and tests.
    float PointerSpeed() const { return m_PointerSpeed; }
    bool  SweepGateClosed() const;

    // Reduce Motion (Preferences): every tween resolves instantly, the shake and pops are skipped.
    void SetReduceMotion(bool on) { m_ReduceMotion = on; }
    bool ReduceMotion() const { return m_ReduceMotion; }

    // --- Motion v2 -----------------------------------------------------------------------------
    // Preferences: one switch per category, and a speed multiplier (0.5..2) on the motion clock and
    // ImAnim's global time scale. A debug override (the motion debugger's slow-mo) multiplies on top.
    void SetCategoryEnabled(MotionCategory c, bool on) { m_CategoryOn[(int)c] = on; }
    // On, and Reduce Motion is off.
    bool CategoryOn(MotionCategory c) const { return !m_ReduceMotion && m_CategoryOn[(int)c]; }
    void  SetSpeed(float s) { m_Speed = s < 0.1f ? 0.1f : (s > 4.0f ? 4.0f : s); }
    float Speed() const { return m_Speed; }
    void  SetDebugTimeScale(float s) { m_DebugScale = s < 0.05f ? 0.05f : (s > 4.0f ? 4.0f : s); }
    float DebugTimeScale() const { return m_DebugScale; }
    float TimeScale() const { return m_Speed * m_DebugScale; }
    // This frame's step on the motion clock (DeltaTime * TimeScale).
    float Dt() const { return m_Dt; }

    // Rolling numbers: draws `value` at `pos` (top-left, current font) with each digit sliding
    // vertically like a mechanical counter while the value changes, and a brief white flash.
    // Remembers last frame's value in the current window's storage (ImGui's memory, not this map).
    // `decimals` fixed digits after the point; `suffix` drawn after (unrolled). Returns the width
    // drawn. RollingNumbers off: plain text.
    float Odometer(ImGuiID id, ImDrawList* dl, ImVec2 pos, double value, int decimals, ImU32 col, const char* suffix = nullptr);
    // The width Odometer would take for `value`.
    static float OdometerWidth(double value, int decimals, const char* suffix = nullptr);
    // The value as it reads right now (a rolling readout that isn't drawn as digits).
    double RollingValue(ImGuiID id, double value);

    // Inertia: track a drag's motion, release it into a coast that decays like a trackpad fling.
    // While dragging call InertiaTrack with the frame's delta (px); on release InertiaRelease. Each
    // frame after that, InertiaDelta returns how far the coast moved (0 once it has stopped, and
    // the entry is gone). Inertia off: release stops at once.
    void   InertiaTrack(ImGuiID id, ImVec2 delta);
    void   InertiaRelease(ImGuiID id);
    ImVec2 InertiaDelta(ImGuiID id);
    void   InertiaStop(ImGuiID id) { Remove(id); }
    bool   InertiaCoasting(ImGuiID id) const;
    // Start a coast directly with a velocity (px/s), e.g. a wheel notch.
    void   Fling(ImGuiID id, ImVec2 velocity);

    // Living gradients (stateless; pure functions of the motion clock; Gradients off: nothing drawn).
    // A soft white band sweeping across [min, max] (busy sheen). `strength` 0..1 fades it.
    void DrawSheen(ImDrawList* dl, ImVec2 min, ImVec2 max, ImGuiID seed, float strength) const;
    // A white gradient segment travelling around the rectangle's perimeter.
    void DrawTravellingRim(ImDrawList* dl, ImVec2 min, ImVec2 max, float lapSeconds, float arc, float alpha,
                           float thickness, ImGuiID seed) const;
    // A radial white glow centred on `cursor`, clipped to [min, max] (accent hover).
    void DrawCursorGlow(ImDrawList* dl, ImVec2 min, ImVec2 max, ImVec2 cursor, float strength, float rounding) const;

    // Layers: everything drawn into `dl` between BeginLayer and EndLayer is transformed (card flips,
    // folder zoom, punch-ins). Returns the vertex index to pass to EndLayer.
    static int  BeginLayer(ImDrawList* dl);
    static void EndLayer(ImDrawList* dl, int vtxStart, const LayerTransform& t);

    // Grid ripple: seconds before tile (col, row) starts, spreading from (originCol, originRow)
    // (iam_stagger_grid_delay), capped at `capSteps` steps.
    static float GridRippleDelay(int col, int row, int cols, int rows, int originCol, int originRow, float stepMs, int capSteps);

    // Text on paths. TextOnPath follows a path made with DefinePath (in pixels, offset by
    // `origin`), via iam_text_path. TextAlongCubic places glyphs along a cubic bezier (a node-graph
    // wire) centred at arc fraction `centerT`, with no path definition (stateless).
    static void TextOnPath(ImGuiID pathId, const char* text, ImVec2 origin, ImU32 col, float offsetPx, bool centred);
    static void TextAlongCubic(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, const char* text,
                               ImU32 col, float centerT, float liftPx);

    // The startup sequence: Begin at launch; Progress 0..1 (1 when done, skipped or Startup off).
    // Any click or key settles it (NewFrame watches for one).
    void  BeginStartup();
    float StartupElapsed() const; // seconds; large when not running
    bool  StartupRunning() const { return m_StartupRunning; }

    // The motion debugger: ImAnim's unified inspector and profiler, plus the slow-mo override and
    // this map's live readout. The profiler only runs while the window is open.
    void DrawDebugger(bool* open);

    // Diagnostics: live entries, and the bytes this map holds (0 when idle).
    int    LiveCount() const { return m_Entries.Size; }
    size_t HeldBytes() const;
    // The i-th live entry's id and kind (0 tween, 1 pulse, 2 colour, 3 vec2, 4 clip, 5 inertia),
    // for a smoke check's failure message.
    ImGuiID LiveId(int i) const { return m_Entries[i].id; }
    int     LiveKind(int i) const { return (int)m_Entries[i].kind; }
    double Now() const { return m_Time; }

    // Drops everything, ImAnim's pools included (tests, theme switch).
    void Reset();

    static constexpr int kStaleFrames = 2;                  // untouched this long = abandoned
    static constexpr int kIdleFramesBeforePoolRelease = 90;  // ~1.5 s at 60 Hz
    static constexpr int kImAnimGcInterval = 30;             // frames between iam_gc() sweeps

private:
    enum class Kind : unsigned char { Tween, Pulse, Color, Vec2, Clip, Inertia };
    struct Entry {
        ImGuiID id;
        Kind    kind;
        float   from;      // tween: value at activation (ImAnim's init value); pulse: unused
        ImVec4  from4;     // colour / vec2 tween: value at activation
        float   aux;       // caller scratch (SetAux), e.g. a reveal's natural height
        double  start;     // pulse: start time (s)
        float   duration;  // pulse: seconds
        int     touched;   // last frame anything asked about it
    };
    Entry* Find(ImGuiID id);
    const Entry* Find(ImGuiID id) const;
    Entry& Add(ImGuiID id, Kind kind);
    void   Remove(ImGuiID id);
    void   CompactIndex();

    ImVector<Entry> m_Entries;
    ImGuiStorage    m_Index;        // id -> index + 1 (0 = absent, a removed key)
    int             m_DeadKeys = 0; // removed keys still sitting in m_Index
    int             m_Frame = 0;
    int             m_IdleFrames = 0;
    bool            m_PoolsReleased = true;
    double          m_Time = 0.0;
    float           m_PointerSpeed = 0.0f;
    bool            m_ReduceMotion = false;
    bool            m_CategoryOn[(int)MotionCategory::Count] = {true, true, true, true, true, true};
    float           m_Speed = 1.0f;
    float           m_DebugScale = 1.0f;
    float           m_Dt = 0.0f;
    double          m_StartupBegan = -1.0e9;
    bool            m_StartupRunning = false;
    bool            m_DebuggerOpenLastFrame = false;
    int             m_LiveClips = 0; // Clip entries: iam_clip_update only runs while one plays
    bool            m_ClipSlotsDirty = false; // a destroyed instance still holds its ImAnim slot
    void            ReclaimClipSlots();
    void            ScrollInertiaFrame();
};

ImAnimContext& Motion();

// Ease a 0..1 progress with a MotionSpec's curve (ImAnim's evaluator, so the curve is identical to
// what a tween with the same spec draws).
float EaseProgress(const MotionSpec& spec, float t);

// Perceptual colour blend (ImAnim's OKLAB mix): a hover tint that crosses from grey to white
// without dipping through a muddy midpoint. t in 0..1.
ImVec4 MixOklab(ImVec4 a, ImVec4 b, float t);

// The delay before item `index` of a staggered reveal starts, in seconds, honouring the cap.
inline float StaggerDelay(int index, float stepMs, int cap) {
    const int i = index < 0 ? 0 : (index > cap ? cap : index);
    return (float)i * stepMs * 0.001f;
}

} // namespace Glass
