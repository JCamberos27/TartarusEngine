#pragma once
#include <glm/glm.hpp>
#include "LayerRegistry.h"
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
    // Rigid bodies use all three axes (the PhysX scene gravity, set when Play starts). Earth's,
    // like Unity's default: it used to be -18 (the first-person player's snappy game-feel
    // value), which made every crate, domino and ball fall at almost twice real speed. The
    // player's own fall is the First Person Controller's Gravity (Player::Gravity).
    glm::vec3 Gravity{0.0f, -9.81f, 0.0f};
    // Stored + shown, not yet consumed (Player integrates on a collision-safety substep cap,
    // not a fixed sim step). Lands with the #185 physics step.
    float FixedTimestep = 1.0f / 60.0f;
    int   SolverIterations = 8;

    // #185 PR 8 — collision matrix over the 8 LayerRegistry slots. Bit j of LayerCollisionMask[i]
    // set == entities on layer i and layer j collide. Symmetric (the editor keeps both bits in
    // sync); default all-on. Copied into the PhysX scene at Play-enter, so edits apply next Play.
    // #150: one row per LayerRegistry slot (32), default all-on.
    unsigned LayerCollisionMask[LayerRegistry::kCount];
    PhysicsSettings() { for (unsigned& m : LayerCollisionMask) m = 0xFFFFFFFFu; }

    // #185 hardening — Play-mode Player tuning.
    float PlayerPushStrength = 2.0f; // impulse the walking Player imparts to a dynamic body it hits
    int   PlayerLayer        = 0;    // collision-matrix layer the Player capsule is on (0-31)

    bool LayersCollide(int a, int b) const {
        if (!LayerRegistry::IsValid(a) || !LayerRegistry::IsValid(b)) return true;
        return (LayerCollisionMask[a] >> b) & 1u;
    }
    void SetLayersCollide(int a, int b, bool on) {
        if (!LayerRegistry::IsValid(a) || !LayerRegistry::IsValid(b)) return;
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

// #171 - Unity's Audio Mixer, as a fixed set of buses (AudioEngine::Bus order: SFX, Music,
// Ambient, UI, Voice) plus a master volume. Pushed into AudioEngine by ApplyAudio().
struct AudioSettings {
    float MasterVolume = 1.0f;
    float BusVolume[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
};
const AudioSettings& Audio();
AudioSettings&       MutableAudio();

// #174 - Build Settings + the Player settings a build bakes into player.json.
struct BuildSettings {
    std::string ProductName = "My Game";
    std::string CompanyName;
    std::string Version = "1.0";
    std::string OutputDir;           // empty = <folder above the project>/Builds/<ProductName>
    std::vector<std::string> Scenes; // project-relative; index 0 is the startup scene
    int  Width = 1280, Height = 720; // windowed size
    bool Fullscreen = true;
    bool VSync = true;
    bool DevelopmentBuild = false;   // keep the stats overlay + physics debug keys in the player
};
const BuildSettings& Build();
BuildSettings&       MutableBuild();

// User-managed tag vocabulary. Additive to the free-text Tag field: the Inspector's Tag
// dropdown unions this list with whatever tags are actually in use in the open scene, so a tag
// defined here shows up even before anything wears it. Order is preserved; duplicates and blank
// entries are dropped on add.
const std::vector<std::string>& Tags();
void AddTag(const std::string& name);
void RemoveTag(const std::string& name);

// #121 - the Asset Browser's virtual folders (including empty ones, so they persist).
// Project data, not scene data: these used to be written into every scene file, so opening a
// second scene forked the organisation and creating a new one lost it. Folder membership for
// an individual asset lives in that asset's .meta ("folder"); this is only the folder list.
const std::vector<std::string>& AssetFolders();
void SetAssetFolders(std::vector<std::string> folders);

// project/settings.json. A missing or unparseable file leaves the defaults in place and is not
// treated as an error (same policy as LayerRegistry / EditorSettings).
void Load();
void Save();

} // namespace ProjectSettings
