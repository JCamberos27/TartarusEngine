#pragma once

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

private:
    float m_HoldDistance = 3.0f; // world units from the eye to the carried body; scroll-adjustable
    bool m_LmbPrev = false;
    bool m_RmbPrev = false;
    bool m_Charging = false;
    float m_Charge = 0.0f;
};
