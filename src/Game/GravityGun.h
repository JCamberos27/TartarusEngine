#pragma once
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

class Player;

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
// Ctrl+scroll rotate it left-right / up-down. While a throw charges, Trajectory() is the predicted
// flight path, which the renderer draws as a red arc.
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

    // While a throw charges: the predicted path of the held body (world space, from the body to
    // where it first hits something, or ~4 s of flight), else empty. TrajectoryHitNormal() is the
    // surface normal where it lands, or zero when the arc ends in the air.
    const std::vector<glm::vec3>& Trajectory() const { return m_Trajectory; }
    glm::vec3 TrajectoryHitNormal() const { return m_TrajectoryHitNormal; }

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
    glm::quat m_HoldRotation{1.0f, 0.0f, 0.0f, 0.0f};
    float m_HoldYaw = 0.0f;

    std::vector<glm::vec3> m_Trajectory;
    glm::vec3 m_TrajectoryHitNormal{0.0f};
};
