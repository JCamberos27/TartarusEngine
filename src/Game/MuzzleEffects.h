#pragma once
#include <string>
#include <glm/glm.hpp>

// Authored on the weapon prefab. Each shot snapshots these values so a weapon switch
// cannot change a flash that is already alive.
struct MuzzleEffectSettings {
    bool PrefabParticles = false; // runtime: the prefab's generic systems own the flash
    std::string FiringSounds, FiringSoundProfile; // runtime: selected muzzle attachment
    bool Enabled = true;
    float FlashTime = 0.055f;
    float LightIntensity = 18.0f;
    float PlayerFlashScale = 0.35f;
    float LightRange = 7.0f;
    glm::vec3 LightColor{1.0f, 0.72f, 0.38f};
    int MuzzleStyle = 1;
    std::string FlameTexture = "assets/Effects/Muzzle/T_MuzzleFlame.png";
    float FlameGlow = 150.0f;
    float FlameScale = 1.75f;
    glm::vec3 FlameColor{1.0f, 0.147f, 0.0177f};
    float FlameLifetimeMin = 0.125f, FlameLifetimeMax = 0.175f;
    float FlameLengthMin = 0.16f, FlameLengthMax = 0.32f;
    float FlameWidthMin = 0.04f, FlameWidthMax = 0.06f;
    std::string FlashSprite = "muzzle_star";
    float FlashLifetime = 0.065f;
    float FlashSizeMin = 0.26f, FlashSizeMax = 0.34f;
    float FlashIntensity = 14.0f;
    bool SideJets = true, CoreGlow = true;
    bool Smoke = true, AfterfireSmoke = true;
    float SmokeScale = 1.0f;
    float SmokeLifetimeMin = 2.0f, SmokeLifetimeMax = 3.0f;
    float SmokeAlpha = 0.18f;
    int SparkCount = 9;
};
