#pragma once

#include <glm/glm.hpp>

#include <string>

// Per-weapon gameplay numbers, authored on the weapon prefab's Weapon Definition. Defaults are the AKS-74U's.
struct FirstPersonWeaponGameplay {
    int Magazine = 30;
    float RoundsPerMinute = 700.0f;  // full-auto cadence
    bool AllowFullAuto = true;       // false: B does nothing (semi-only weapon)
    int BurstRounds = 0; // >1: a semi trigger schedules this many rounds; release interrupts it
    float ReloadHoldSeconds = 0.35f; // R held this long checks the magazine instead of reloading
    float RegripMin = 10.0f, RegripMax = 20.0f; // seconds of settled Idle before a Fidget
    // Each round shoves the dynamic body the bore hits: ImpactImpulse N*s at the hit point
    // (so it spins as well as flies), capped at ImpactMaxSpeed m/s of velocity change per round
    // so light props don't rocket off. 0 = rounds push nothing.
    float ImpactImpulse = 0.0f;
    float ImpactMaxSpeed = 8.0f;
    // The holes rounds leave: radius in metres. Game-sized rather than calibre-sized (a real
    // 5.45 mm hole, 4.5 mm, was too small to see past a few metres).
    float BulletHoleRadius = 0.004f; // metres: the round's radius (the hole is ~1.15x its calibre)
    // Zeroing: rounds (and the laser) leave the muzzle aimed to cross the sight line
    // ZeroDistance metres out, like a sighted-in rifle - dead on the front post there, a little
    // low closer, a little high past it. 0 = straight down the bore as modelled.
    float ZeroDistance = 25.0f;
    // The sight line in weapon-root space (the eye's position and look direction with the sights
    // up, measured in Play). Without one it's measured the first time the sights settle, and the
    // log prints the values to save here.
    bool HasSightLine = false;
    glm::vec3 SightOrigin{0.0f}, SightDirection{0.0f, 0.0f, -1.0f};

    // A shotgun: each round is Pellets rays, each inside a cone of SpreadHip / SpreadAds degrees
    // (the cone's half angle; hip applies whenever the sights aren't up) about the zeroed bore.
    // ImpactImpulse and ImpactMaxSpeed are the whole round's, shared between its pellets.
    int Pellets = 1;
    float SpreadHip = 0.0f, SpreadAds = 0.0f;
    // How a reload fills the gun. Magazine: the reload state's Refill event fills it at once.
    // PerRound (a tube): each LoadRound event adds one round; the controller loops its load state
    // until the LastRound parameter is set, and exits early when StopReload is (the trigger was
    // pulled mid-reload). See FirstPersonAnimatorContract.
    enum class ReloadMode : int { Magazine, PerRound };
    ReloadMode Reload = ReloadMode::Magazine;
    // A manual action (pump, bolt, lever): after every round the Cycle trigger is set CycleDelay
    // seconds later, until a state tagged Cycling has played through; the gun can't fire in
    // between. A round loaded into an empty gun (LoadRound at 0 rounds) is chambered by its clip.
    bool CycleAfterShot = false;
    float CycleDelay = 0.1f;
    // What a hit on a character costs it: Damage health per round (per pellet for a shotgun),
    // times HeadMultiplier / LimbMultiplier by where it lands, scaled from 1 at FalloffStart
    // metres down to FalloffMin at FalloffEnd and beyond. See Combat/Damage.h.
    float Damage = 34.0f;
    float HeadMultiplier = 3.0f, LimbMultiplier = 0.75f;
    float FalloffStart = 25.0f, FalloffEnd = 60.0f, FalloffMin = 0.6f;
};

// Clamps authored values into range and rejects ones no weapon can use (error says which).
bool NormalizeWeaponGameplay(FirstPersonWeaponGameplay& gameplay, std::string& error);
