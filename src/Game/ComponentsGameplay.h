#pragma once
// Interactables and gameplay state: transform controllers, doors, lights, health, NPC spawns, squads.
// Part of the component set; Components.h includes every part.

#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

struct MaterialAsset; // full definition in MaterialAsset.h; shared_ptr<MaterialAsset> is valid here

// The first drag-and-drop component script. Its editable settings are intentionally data-only:
// a Script asset attaches this component, and this runtime system applies its motion only while
// playing. That gives the Inspector a familiar script workflow without requiring a compiler or
// exposing native engine code to scene authors.
// Registered via ComponentRegistry (#184) rather than hand-coded serializer/Inspector code.
// Reflected fields are addressed by pointer-to-member (ComponentReflection.h), so the
// std::string member is no problem. The
// runtime-scratch tail is deliberately excluded from the reflected field list, same as it was
// excluded from the old hand-written serializer.
struct TransformControllerComponent {
    // Informational only (the system never reads it): the .tescript this was dropped from. Empty
    // when added from Add Component; no script ships, so a made-up default showed as missing.
    std::string ScriptPath;
    bool Enabled = true;
    glm::vec3 RotationDegPerSec{0.0f};
    glm::vec3 TranslationUnitsPerSec{0.0f};
    float ScalePulseAmplitude = 0.0f; // fraction of the authored scale (0.2 = +/-20%)
    float ScalePulseFrequencyHz = 0.5f;

    // Runtime scratch, never serialized. Play -> Stop reloads the authored scene snapshot.
    bool Initialized = false;
    glm::vec3 BasePosition{0.0f};
    glm::quat BaseRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 BaseScale{1.0f};
    float Elapsed = 0.0f;
};

// A door, drawer or lid the player opens and closes with Interact (E) while playing (Game/Openable.h). The motion is
// worked out from the model's own bounds, with its front facing local +Z: a hinge runs along the matching edge, a
// drawer slides out along +Z by most of its parent's depth. Moves this entity's own transform, so a part sits as a
// child of its body. Play -> Stop reloads the authored scene, so everything comes back shut.
struct OpenableComponent {
    enum class MotionKind : int {
        HingeLeft = 0,  // a door hinged on its left edge (seen from the front), swinging out
        HingeRight = 1, // ... on its right edge
        Lid = 2,        // hinged along its back edge, lifting (a box or toilet lid, a washer's top)
        FlapDown = 3,   // hinged along its bottom edge, dropping forward (an oven door)
        FlapUp = 4,     // hinged along its top edge, lifting outward (a wall cabinet's flap)
        SlideOut = 5,   // a drawer
        SlideLeft = 6,  // a sliding door, toward local -X
        SlideRight = 7, // ... toward local +X
    };
    int Motion = 0;          // MotionKind
    // A door leaf (hinged or sliding), as opposed to a lid, flap or drawer. Leaves sit in a doorway between two rooms:
    // the renderer damps their GI's directional term (GIVolumes::SetMask flatL1) so the face toward the other room isn't black.
    bool IsLeaf() const {
        const MotionKind m = (MotionKind)Motion;
        return m == MotionKind::HingeLeft || m == MotionKind::HingeRight || m == MotionKind::SlideLeft || m == MotionKind::SlideRight;
    }
    float Amount = 0.0f;     // degrees for a hinge, metres for a slide; 0 = the motion's usual amount
    float OpenTime = 0.5f;   // seconds from shut to open
    bool StartOpen = false;
    float StartAjar = 0.0f;  // with StartOpen: how far it starts, 0..1 of Amount (0 = all the way); one Interact opens it fully
    bool SwingBack = false;  // a hinge or flap opens toward local -Z (its back face) instead: a front door whose inside is its back
    // Audio assets, ';'-separated variants (one picked per play), played at the part. Empty = silent.
    std::string OpenSound;  // as it starts to open (the knob turning, the hinge)
    std::string CloseSound; // when it lands shut (the thud)
    std::string LatchSound; // with it, the latch snapping home

    // Runtime, never serialized.
    bool Started = false;
    glm::vec3 BasePosition{0.0f};
    glm::quat BaseRotation{1.0f, 0.0f, 0.0f, 0.0f};
    float Progress = 0.0f; // 0 shut .. 1 open
    float Target = 0.0f;
    float ResolvedAmount = 0.0f; // Amount, or the motion's usual amount for this model
    glm::vec3 PoseMin{0.0f}, PoseMax{0.0f}; // the box it turns about: its box collider, else the model's bounds
};

// A light, a fitting or a lamp on a circuit (Game/LightSwitch.h): every Light on the circuit goes dark and every model
// on it loses its glow (its materials' emission) when the circuit is off. Play only; Play -> Stop restores the scene.
struct LightCircuitComponent {
    std::string Circuit;     // the circuit's name; a Light Switch with the same name works it
    float Glow = 1.0f;       // models: the emission kept when on, x their materials' own (0 = leave them alone)
    bool GlowWhenOff = false; // models: glow while the circuit is OFF instead (a lit switch's toggle, found in the dark)

    // Runtime, never serialized.
    bool Started = false;
    std::vector<std::shared_ptr<MaterialAsset>> OnMaterials, OffMaterials;
    std::vector<std::shared_ptr<MaterialAsset>> AuthoredMaterials; // the slots as they were at start (LightSwitch::Stop)
    bool AuthoredOff = false;                                     // its light was deactivated at start
};

// Works a circuit with Interact (E): a wall switch (its Rocker child flips) or a lamp's own body. Every switch on the same
// circuit flips together. Play only.
struct LightSwitchComponent {
    std::string Circuit;      // the circuit it works
    bool StartOn = false;     // the circuit's state when Play starts (the first switch on it found decides)
    std::string Rocker;       // a child's name that tips over when it flips (empty = none)
    float RockerAngle = 14.0f; // degrees the rocker tips either way, about its local Z (across the plate)
    std::string OnSound;      // ';'-separated audio variants
    std::string OffSound;

    // Runtime, never serialized.
    bool Started = false;
    bool On = false;
    entt::entity RockerEntity = entt::null;
    glm::quat RockerBase{1.0f, 0.0f, 0.0f, 0.0f};
};

// Drives the Light on its entity: a fluorescent tube that stutters now and then, a TV screen's
// restless glow, or a candle's fast shimmer. Works around the light's authored intensity and colour
// while playing; Play -> Stop reloads the authored scene, so they come back untouched.
struct LightFlickerComponent {
    enum class FlickerMode : int { Fluorescent = 0, Tv = 1, Candle = 2 };
    int Mode = 0;              // FlickerMode
    float MinInterval = 4.0f;  // shortest wait between events, seconds
    float MaxInterval = 15.0f; // longest wait between events, seconds
    float Depth = 1.0f;        // how far the light dips: 1 = all the way off
    int Seed = 1;              // so lights with the same settings don't flicker in step

    // Runtime scratch, never serialized.
    bool Started = false;
    float BaseIntensity = 0.0f;
    glm::vec3 BaseColor{1.0f};
    float Wait = 0.0f, Level = 1.0f, Target = 1.0f, Time = 0.0f;
    glm::vec3 Tint{1.0f};
    int Pulses = 0;
    std::uint32_t Rng = 1;
};

// Hit points. Rounds that hit an entity with one take Current down (Combat/Damage.h); at 0 it
// is dead. Current is runtime-only: it starts at Max each Play.
struct HealthComponent {
    float Max = 100.0f;
    bool Invulnerable = false;
    // --- runtime (not serialized)
    float Current = -1.0f; // < 0 = not started yet (reads as Max)
};

// Where a training dummy appears in Play (NpcDirector), facing this object's forward: an unarmed body that wanders
// the navigation mesh, flinches when shot, bleeds, and dies into a ragdoll.
struct NpcSpawnComponent {
    bool Roam = true;      // wanders the navigation mesh near its spawn (off: stands where it was placed)
};

// The dummies' rules, on any object in the scene (the first one counts).
struct SquadSettingsComponent {
    bool Respawn = true;
    float RespawnDelay = 8.0f;
    float HeavyHitDamage = 40.0f;
    float StaggerTime = 0.4f;
    float CorpseTime = 180.0f;
    int MaxCorpses = 12;
    float UnseenTime = 3.0f;
    float FallGravity = 18.0f;
    float HitboxRange = 60.0f;
    float FootIKRange = 25.0f;
    std::string BodyPrefab = "assets/AI/Soldier.json";
};
