#pragma once
// Weapon definitions and their attachment points.
// Part of the component set; Components.h includes every part.

#include <string>
#include <glm/glm.hpp>
#include "MuzzleEffects.h"
#include "FirstPersonWeaponGameplay.h"

// A weapon prefab's root: what the weapon is and how it plays. The .fpsanim Animation Set owns the
// animator, rigs, recoil, shake and audio; the gameplay numbers and the muzzle effects live here.
struct WeaponDefinitionComponent {
    std::string Description;
    std::string AnimationSet;
    MuzzleEffectSettings Muzzle;
    FirstPersonWeaponGameplay Gameplay;
};

// Interchangeable children of a weapon prefab: one active per category (the first active one, or
// the player's pick). References are hierarchy paths relative to the attachment's own object.
struct MuzzleAttachmentComponent {
    bool Enabled = true;
    std::string MuzzleTransform = "Muzzle"; // its local -Z is the bore direction
    std::string MuzzleFlash = "Muzzle";     // the flash Particle System
    std::string FiringSounds;               // audio assets; empty uses the gun's layered audio
    std::string FiringSoundProfile;         // "", "ak", "870" or "revolver38": the layered profile when Firing Sounds is empty
};

struct GripAttachmentComponent {
    bool Enabled = true;
};

struct OpticAttachmentComponent {
    bool Enabled = true;
    std::string AimPoint = "Aim Point"; // its local -Z points along the sight line
    bool DefaultOptic = false;          // the iron-sight aim point the weapon's ADS animation was made for
};
