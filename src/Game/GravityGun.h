#pragma once

class Player;

// The player's default physics-interaction tool while in Play mode — a Half-Life-2-style gravity
// gun. Right-click grabs the dynamic body under the crosshair and carries it in front of the eye;
// scroll while holding pulls it nearer / pushes it out; left-click launches it forward. With
// nothing held, left-click gives whatever it hits a shove.
//
// Promoted out of the ad-hoc harness that used to live inline in main.cpp, gated behind the
// "Physics debug input" Preferences toggle (#185 hardening). That harness only ever exercised
// PhysicsWorld's grab/force API "until gameplay code does" — this is that gameplay code: a real,
// always-available ability, not a debug tool. (The G-key explosion shockwave stays behind the
// debug toggle in main.cpp — a separate test tool, not part of the gravity gun.)
class GravityGun {
public:
    // Call once per simulated Play frame while the game has input focus. Reads mouse buttons and
    // scroll directly from Input::, and rays/forces the currently-active PhysX world (both
    // no-ops outside Play, so this is safe to call unconditionally whenever `player` is ticking).
    void Update(float dt, const Player& player);

private:
    float m_HoldDistance = 3.0f; // world units from the eye to the carried body; scroll-adjustable
    bool m_LmbPrev = false;
    bool m_RmbPrev = false;
};
