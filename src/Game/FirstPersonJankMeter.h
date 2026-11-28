#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

// The first-person arms' smoothness, frame by frame (WeaponJankMeter's counterpart for the player's own view; the combo
// matrix's first-person instrument). It is fed what the player SEES - each hand in the camera's space - plus the body
// debug's numbers (grip gap, reach, eye-to-arm), and flags:
//   Bounce  - one gait cycle's vertical hand travel (peak to peak, mm) past its line while moving: the walking arm bounce.
//             The line is BounceMm, or BounceFastMm for a cycle run at FastSpeed or more (a jog, a sprint).
//   HandPop - an acceleration spike a hand makes in the view (a step, a snap) that a recoil kick does not explain.
//   Grip    - a hand off the rig's grip (the arm could not reach, or the solve lags): more than GripCm.
//   Stretch - a hand asked past the arm's reach (Reach > 1): the shoulder shrug and reach lean are what cover it.
//   Clip    - the body's upper arm within ClipCm of the eye, or the camera seeing the inside of the body.
// Consecutive frames of one kind in one state are one event, at their worst. Plain data in, no engine types: a harness
// feeds it from BodyDebug::Info() and the camera, a unit test from a script.
struct FirstPersonJankSample {
    float Dt = 0.0f;
    std::string State;
    bool Valid = true;           // false: nothing to measure (no hands); the next valid frame starts a fresh path
    // The arms aren't drawn (holstered): nothing is scored, but the hands' path carries on, so the first drawn frame is
    // measured against where they were (a draw that snaps the hands counts from its first frame).
    bool Hidden = false;
    bool Recoil = false;         // a shot's kick this frame: its pop is the recoil's
    float Speed = 0.0f;          // the player's planar speed (m/s)
    float GaitPhase = -1.0f;     // 0..1 the body's step cycle; < 0 none (standing, in the air)
    glm::vec3 Hand[2] = {glm::vec3(0.0f), glm::vec3(0.0f)}; // each hand in the camera's space (m; x right, y up, z forward)
    float HandGap[2] = {0.0f, 0.0f};   // m each body hand is off the rig's (BodyDebug HandGap)
    float Reach[2] = {0.0f, 0.0f};     // each hand's distance from its shoulder, of the arm's length (BodyDebug Reach)
    float EyeToArm[2] = {1e3f, 1e3f};  // m from the eye to each upper arm's bone line (BodyDebug EyeToUpperArmBody)
    int InsideRays = 0;                // view rays that hit the body's inside (BodyDebug ProbeInsideRays); <= 0: none
};

struct FirstPersonJankEvent {
    enum class Kind { Bounce, HandPop, Grip, Stretch, Clip };
    Kind What = Kind::Bounce;
    std::string State;
    float Time = 0.0f;  // seconds since the meter was reset, at the worst frame
    float Size = 0.0f;  // Bounce: mm peak to peak; HandPop / Grip: cm; Stretch: % past the arm's length; Clip: cm eye to arm (0 = inside rays)
    int Side = -1;      // 0 left, 1 right; -1 both / none
    float Line = 0.0f;  // Bounce: the line this cycle was held to (mm; BounceMm or BounceFastMm)
    static const char* KindName(Kind k);
    std::string Describe() const;
};

class FirstPersonJankMeter {
public:
    // Thresholds. A pop is a jump past a smooth path of more than PopCm in a frame (at 60 Hz, scaled to the step).
    float BounceMm = 15.0f;      // a cycle's vertical travel
    // ... and a cycle run at FastSpeed (m/s, its mean) or more: a jog or a sprint, where Body Motion's gun trail (#1309,
    // ~0.01 m per m/s) is designed - 1.5x its worst measured (870 jog, 33 mm).
    float BounceFastMm = 50.0f, FastSpeed = 2.4f;
    float MinBounceSpeed = 0.5f; // m/s: below it no cycle is measured
    float PopCm = 0.6f;
    float GripCm = 1.0f;
    float StretchAt = 1.0f;      // Reach over this
    float ClipCm = 6.0f;

    void Reset();
    void Push(const FirstPersonJankSample& s);
    const std::vector<FirstPersonJankEvent>& Events() const { return m_Events; }
    float Time() const { return m_Time; }
    // The largest Size of one kind (0 when none), for a matrix row.
    float Worst(FirstPersonJankEvent::Kind kind) const;
    // The last finished gait cycle's vertical hand travel (mm, the larger of the two hands); < 0 until one finishes.
    float LastCycleMm() const { return m_LastCycleMm; }

private:
    void Flag(FirstPersonJankEvent::Kind kind, float size, int side = -1, float line = 0.0f);
    void EndCycle();
    std::vector<FirstPersonJankEvent> m_Events;
    std::vector<FirstPersonJankSample> m_Prev; // the last two valid samples (oldest first)
    float m_Time = 0.0f;
    std::string m_State;
    float m_LastFlag[5][3] = {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}}; // by kind, then side + 1 (-1 both / none)
    // The gait cycle being measured: each hand's vertical range since the phase last wrapped.
    float m_PrevPhase = -1.0f, m_Lo[2] = {1e9f, 1e9f}, m_Hi[2] = {-1e9f, -1e9f};
    int m_CycleFrames = 0;
    float m_CycleSpeed = 0.0f; // the cycle's summed speed (its mean: / m_CycleFrames)
    bool m_CycleClean = true; // moving, not recoiling, one state throughout
    float m_LastCycleMm = -1.0f;
};
