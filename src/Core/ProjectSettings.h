#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>

// Project-scoped settings — the "-lite" Project Settings shell (#236 A4).
//
// Distinct from EditorSettings (per-user editor preferences, editor_prefs.json). These belong
// to the *content* and version-control with it: persisted to project/settings.json alongside
// the scenes, the way Unity's ProjectSettings/*.asset do. Two groups so far — Physics and Tags.
// Layer names are their own file (LayerRegistry / project/layers.json), surfaced in the same
// Project Settings window's "Tags & Layers" tab.
namespace ProjectSettings {

struct PhysicsSettings {
    // Rigid bodies use all three axes (the PhysX scene gravity, set when Play starts); the
    // Play-mode Player's own fall uses the Y component (Player::Gravity).
    glm::vec3 Gravity{0.0f, -18.0f, 0.0f};
    // Stored + shown, not yet consumed (Player integrates on a collision-safety substep cap,
    // not a fixed sim step). Lands with the #185 physics step.
    float FixedTimestep = 1.0f / 60.0f;
    int   SolverIterations = 8;

    // #185 PR 8 — collision matrix over the 8 LayerRegistry slots. Bit j of LayerCollisionMask[i]
    // set == entities on layer i and layer j collide. Symmetric (the editor keeps both bits in
    // sync); default all-on. Copied into the PhysX scene at Play-enter, so edits apply next Play.
    unsigned LayerCollisionMask[8] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};

    // #185 hardening — Play-mode Player tuning.
    float PlayerPushStrength = 2.0f; // impulse the walking Player imparts to a dynamic body it hits
    int   PlayerLayer        = 0;    // collision-matrix layer the Player capsule is on (0-7)

    bool LayersCollide(int a, int b) const {
        if (a < 0 || a > 7 || b < 0 || b > 7) return true;
        return (LayerCollisionMask[a] >> b) & 1u;
    }
    void SetLayersCollide(int a, int b, bool on) {
        if (a < 0 || a > 7 || b < 0 || b > 7) return;
        auto set = [&](int i, int j) {
            if (on) LayerCollisionMask[i] |= (1u << j);
            else    LayerCollisionMask[i] &= ~(1u << j);
        };
        set(a, b); set(b, a);
    }
};

const PhysicsSettings& Physics();
PhysicsSettings&       MutablePhysics();

// #144 - Unity's Project Settings > Time. The fixed step itself stays PhysicsSettings::FixedTimestep.
struct TimeSettings {
    // Longest frame the game simulates in one go; a longer hitch runs slower instead of making
    // physics / gameplay take one huge step. Unity's "Maximum Allowed Timestep" (default 1/3 s;
    // this engine has always used 0.1 s).
    float MaximumDeltaTime = 0.1f;
    // Time.timeScale when Play starts (the game module can change it while playing).
    float TimeScale = 1.0f;
};

const TimeSettings& Time();
TimeSettings&       MutableTime();

// User-managed tag vocabulary. Additive to the free-text Tag field: the Inspector's Tag
// dropdown unions this list with whatever tags are actually in use in the open scene, so a tag
// defined here shows up even before anything wears it. Order is preserved; duplicates and blank
// entries are dropped on add.
const std::vector<std::string>& Tags();
void AddTag(const std::string& name);
void RemoveTag(const std::string& name);

// project/settings.json. A missing or unparseable file leaves the defaults in place and is not
// treated as an error (same policy as LayerRegistry / EditorSettings).
void Load();
void Save();

} // namespace ProjectSettings
