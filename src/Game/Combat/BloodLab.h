#pragma once
#include <functional>
#include <string>

class BloodFx;
class FxSprites;
class ImpactFx;
class ScreenBlood;

// The Blood Lab (docs/BLOOD_FX.md, v2): an ImGui window off the F7 dev panel for tuning the blood in Play. Fire any
// effect at the crosshair - a body hit (rifle / shotgun / headshot gore), a pool, wall drips, a surface impact, being
// hurt - move the Blood Settings live, read what's alive and what it costs, clear it all. The host wires the actions
// (it owns the world, the squad and the camera); the window only calls them.
class BloodLab {
public:
    bool Open = false;
    struct Hooks {
        // Rounds at the crosshair: a body (or the world), with this damage / pellets / to the head.
        std::function<void(float damage, int pellets, bool head)> FireAtCrosshair;
        std::function<void()> PoolAtCrosshair;      // a corpse's pool where you look (the floor)
        std::function<void()> DripsAtCrosshair;     // a wall spatter with its drips
        std::function<void()> ImpactAtCrosshair;    // a round into the surface you look at (its burst and hole)
        std::function<void(float damage)> Hurt;     // the player hit (screen blood), from the right
        std::function<void()> ClearAll;             // every stain, splat, spray, sprite, hole and stump gone
    };
    Hooks Actions;
    struct Stats {
        int Sprays = 0, Stains = 0, Splats = 0, Sprites = 0, Holes = 0;
        float SprayMs = 0.0f, DecalMs = 0.0f, SpriteMs = 0.0f;
        bool BloodData = false, KnifeData = false, GoreMeshes = false;
    };
    // Draws the window (inside an ImGui frame) while Open. `blood` / `screen` settings are edited in place.
    void Draw(BloodFx& blood, ScreenBlood& screen, const Stats& stats);
};
