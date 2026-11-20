#pragma once

// The scene's weapon-effects tuning (a component on any object: Components.h includes this). Here so the renderer
// that draws the effects (WeaponFxRenderer) does not include the game's component header.
// Scene-level visual effects and HUD settings (Lane P quality pass).
// Add one to the scene to tune muzzle flash, laser beam and HUD display parameters.
// Defaults match the values these effects had before they were tunable; read once when Play starts.
struct FxHudSettingsComponent {
    float FlashTime = 0.055f;
    float PlayerFlashScale = 0.35f;
    float FlameGlow = 150.0f;
    float FlameScale = 1.75f;
    int MuzzleStyle = 1;
    float BeamRange = 150.0f;
    float BeamHalfWidth = 0.0015f;
    float BeamFalloff = 2.5f;
    float BeamBend = 4.0f;
};
