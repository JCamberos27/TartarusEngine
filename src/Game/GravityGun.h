#pragma once
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

class Player;

// The gravity gun's predicted throw: the flight split into legs at each bounce (Legs[0] starts at
// the held body), and where each leg meets a surface (the last one is where the prediction stops).
struct ThrowPrediction {
    std::vector<std::vector<glm::vec3>> Legs;
    std::vector<glm::vec3> ContactPoints;
    std::vector<glm::vec3> ContactNormals;
    float BodyRadius = 0.0f;
    void Clear() { Legs.clear(); ContactPoints.clear(); ContactNormals.clear(); BodyRadius = 0.0f; }
};

// How the gravity gun throws. Filled from the scene's First Person Controller when there is one.
struct GravityGunSettings {
    float MinThrowSpeed = 4.0f;   // m/s for a quick click
    float MaxThrowSpeed = 18.0f;  // m/s fully charged
    float ChargeTime = 1.0f;      // seconds of holding left mouse to reach MaxThrowSpeed
    float BackspinRevPerSec = 2.0f; // spin given to a thrown round body (a basketball shot's backspin)
};

// The player's default physics-interaction tool while in Play mode — a Half-Life-2-style gravity
// gun. Right-click grabs the dynamic body under the crosshair and carries it in front of the eye;
// scroll while holding pulls it nearer / pushes it out; holding left mouse charges a throw and
// releasing it launches the body forward (harder the longer it was held). With nothing held,
// left-click gives whatever it hits a shove.
//
// Grabbing is forgiving: a body the crosshair ray misses is still picked up if it's within a few
// degrees of the aim and in plain sight. A held body follows smoothly (PhysicsWorld servos it every
// physics substep, leading it by the player's own motion), turns with the view, and Alt+scroll /
// Ctrl+scroll rotate it left-right / up-down. While a throw charges, Prediction() is the predicted
// flight - the arc and up to two bounces off whatever it hits - which the renderer draws in red.
//
// Promoted out of the ad-hoc harness that used to live inline in main.cpp, gated behind the
// "Physics debug input" Preferences toggle (#185 hardening). That harness only ever exercised
// PhysicsWorld's grab/force API "until gameplay code does" — this is that gameplay code: a real,
// always-available ability, not a debug tool. (The G-key explosion shockwave stays behind the
// debug toggle in main.cpp — a separate test tool, not part of the gravity gun.)
class GravityGun {
public:
    GravityGunSettings Settings;

    // Call once per simulated Play frame while the game has input focus. Reads mouse buttons and
    // scroll directly from Input::, and rays/forces the currently-active PhysX world (both
    // no-ops outside Play, so this is safe to call unconditionally whenever `player` is ticking).
    void Update(float dt, const Player& player);
    // Forget any held button / charge (Play started or stopped).
    void Reset();

    bool IsHolding() const;
    bool IsCharging() const { return m_Charging; }
    float Charge() const { return m_Charge; } // 0..1 while charging

    // While a throw charges: where the held body would go if released now (world space), else
    // empty. The held body's own shape is swept along the arc, and each bounce uses the combined
    // bounciness / friction of the two colliders, as PhysX will.
    const ThrowPrediction& Prediction() const { return m_Prediction; }

private:
    unsigned FindGrabTarget(const glm::vec3& eye, const glm::vec3& fwd) const;
    void PredictThrow(const glm::vec3& fwd, float speed);

    float m_HoldDistance = 3.0f; // world units from the eye to the carried body; scroll-adjustable
    bool m_LmbPrev = false;
    bool m_RmbPrev = false;
    bool m_Charging = false;
    float m_Charge = 0.0f;

    // Hold state: the hold point last frame (for its velocity), the orientation the body is held
    // at, and the view yaw it was last turned with.
    bool m_HaveHoldPoint = false;
    glm::vec3 m_PrevHoldPoint{0.0f};
    glm::vec3 m_HoldVelocity{0.0f}; // smoothed: raw per-frame mouse-look deltas are spiky
    glm::quat m_HoldRotation{1.0f, 0.0f, 0.0f, 0.0f};
    float m_HoldYaw = 0.0f;

    ThrowPrediction m_Prediction;
};
