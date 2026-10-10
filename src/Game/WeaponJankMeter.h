#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

// The third-person weapon animations' smoothness, frame by frame (--stock-probe's jank sweep, the Weapon Lab panel).
// It is fed the world gun and the world hands with the third-person clips' own motion taken out, so what is left is what
// the procedural hold adds - the shoulder lock, the chest ride, the free hand's anchor - and flags where that jumps or keeps
// moving on its own:
//   Pop     - an acceleration spike (a step, a snap) the clips don't have: the gun, or a hand (its path on the body
//             against the clips' own hand on their gun: a reach the clips make fast isn't one, a hold snapping is).
//   Settle  - the gun still drifting into place well after a state has begun (a move of its own once the clip has ended).
//   Grip    - a world hand off its grip on the world gun (the arm couldn't reach).
// Consecutive frames of one kind in one state are one event, at their worst.
struct WeaponJankFrame {
    float Dt = 0.0f;
    std::string State;
    bool Valid = true;           // false: nothing to measure this frame (holstered, no gun)
    bool Recoil = false;         // a shot's kick this frame: its pop is the recoil's (not flagged)
    float SinceShot = 1e3f;      // seconds since a round left: a settle in the recoil's recovery is the recoil's
    float Speed = 0.0f;          // the player's planar speed (m/s): a settle needs the player still ...
    glm::quat View{1.0f, 0.0f, 0.0f, 0.0f}; // ... and the view (the clips' gun rides it: turning, the body turns under it)
    bool Resting = false;        // a held state (idle, walk, aim, sprint): the gun should be still past its start
    glm::vec3 GunPos{0.0f};      // the world gun, the clips' gun taken out (m, the clips' gun frame)
    glm::quat GunRot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Hand[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};     // each world hand off the chest (world axes, m)
    glm::vec3 HandClip[2] = {glm::vec3(0.0f), glm::vec3(0.0f)}; // ... and the clips' hand on their gun (m)
    float HandGap[2] = {0.0f, 0.0f}; // each world hand off where the hold put it (m)
};

struct WeaponJankEvent {
    enum class Kind { GunPop, HandPop, Settle, Grip };
    Kind What = Kind::GunPop;
    std::string State;
    float Time = 0.0f;   // seconds since the meter was reset, at the worst frame
    float Size = 0.0f;   // Pop: the frame's jump beyond a smooth path (cm, or degrees for a turn); Settle: cm / degrees
                         // travelled after it should have been still; Grip: cm off
    int Side = -1;       // HandPop / Grip: 0 left, 1 right
    bool Turn = false;   // GunPop / Settle: degrees (a turn), not cm
    static const char* KindName(Kind k);
    std::string Describe() const;
};

class WeaponJankMeter {
public:
    // Thresholds: a pop is a jump past a smooth path of more than PopCm in a frame (at 60 Hz; scaled to the step), or a
    // turn of more than PopDeg; a settle starts SettleAfter seconds into a state and counts motion faster than SettleCmPerS /
    // SettleDegPerS; a grip is a hand more than GripCm off.
    float PopCm = 0.4f, PopDeg = 0.5f;
    float SettleAfter = 0.2f, SettleCmPerS = 3.0f, SettleDegPerS = 4.0f, SettleMinCm = 0.5f, SettleMinDeg = 0.6f;
    float StillFor = 0.6f; // a settle is only measured once the player and the view have been still this long (s)
    float GripCm = 1.5f;

    void Reset();
    void Push(const WeaponJankFrame& f);
    const std::vector<WeaponJankEvent>& Events() const { return m_Events; }
    float Time() const { return m_Time; }

private:
    void Flag(WeaponJankEvent::Kind kind, float size, int side = -1, bool turn = false);
    void CloseSettle();
    std::vector<WeaponJankEvent> m_Events;
    std::vector<WeaponJankFrame> m_Prev; // the last two valid frames (oldest first)
    float m_Time = 0.0f, m_StateTime = 0.0f, m_Still = 0.0f;
    std::string m_State;
    // The settle being measured in this state: travel since SettleAfter, while moving.
    float m_SettleCm = 0.0f, m_SettleDeg = 0.0f, m_SettleStart = -1.0f;
    // Each event kind's last frame, so a run of frames is one event.
    float m_LastFlag[4][2] = {{-1.0f, -1.0f}, {-1.0f, -1.0f}, {-1.0f, -1.0f}, {-1.0f, -1.0f}};
};
