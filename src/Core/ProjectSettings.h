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
    // Only .y is consumed today — Player's Play-mode walk collider (Player::Gravity). X/Z are
    // stored for forward-compatibility with rigid bodies (#185) and edited in the tab, but do
    // nothing yet.
    glm::vec3 Gravity{0.0f, -18.0f, 0.0f};
    // Stored + shown, not yet consumed (Player integrates on a collision-safety substep cap,
    // not a fixed sim step). Lands with the #185 physics step.
    float FixedTimestep = 1.0f / 60.0f;
    int   SolverIterations = 8;

    // #185 PR 8 — collision matrix over the 8 LayerRegistry slots. Bit j of LayerCollisionMask[i]
    // set == entities on layer i and layer j collide. Symmetric (the editor keeps both bits in
    // sync); default all-on. Copied into the PhysX scene at Play-enter, so edits apply next Play.
    unsigned LayerCollisionMask[8] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};

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
