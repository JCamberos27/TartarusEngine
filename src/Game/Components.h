#pragma once
#include <string>
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include <entt/entt.hpp>

class Model;

// The ECS component set every placed entity (former level-geometry box, imported model, or
// procedural primitive) is built from. Replacing the old WorldBox/PlacedModel split — see
// World.h for the registry and the free functions (ComposeTransform, systems) that operate on
// these.

struct TransformComponent {
    glm::vec3 Position{0.0f};
    glm::vec3 RotationEuler{0.0f}; // degrees
    glm::vec3 Scale{1.0f};
};

struct NameComponent {
    std::string Name; // empty = display falls back to a positional name in the Hierarchy
};

// Stable per-entity sequence number, assigned once when the entity is created (World's factory
// methods) and preserved through save/load. The Hierarchy and the scene serializer both order
// entities by this instead of trusting EnTT's live creation order, which a snapshot round-trip
// (undo/redo, Play->Stop) would otherwise flatten - it re-creates every entity in file order,
// so lights/empties (written last) sank permanently below every mesh. Legacy scene files with
// no "order" field fall back to file order on load, matching the old behaviour for them.
struct OrderComponent {
    int Value = 0;
};

// What to draw. Every entity — including former "boxes", which now render through the same
// cube-primitive Model + PBR material as everything else instead of a separate flat-shaded mesh.
struct RenderableComponent {
    std::shared_ptr<Model> ModelRef;
};

// Holds the mesh that was on a RenderableComponent when the user removed "Mesh Renderer" from
// the Inspector, so re-adding the component restores the same mesh instead of a default cube.
// Editor-only and NOT serialized: saving the scene with the mesh removed makes that permanent.
struct DetachedMeshComponent {
    std::shared_ptr<Model> ModelRef;
};

// Presence of this component means the entity participates in collision/raycasting. The actual
// AABB is derived from the entity's RenderableComponent bounds (or, absent one, from
// TransformComponent Position/Scale as a plain unit box) — axis-aligned, ignoring rotation; see
// World::ColliderWorldBounds. Nothing to keep in sync with the visual size as a result.
struct ColliderComponent {
    bool IsTrigger = false; // reserved for future trigger-volume use; solid (blocking) for now
};

// Optional clip triggered from the editor Inspector; formerly PlacedModel-only, now any entity.
struct AudioSourceComponent {
    std::string SoundPath;
};

// Tag only: which Hierarchy section an entity lists under ("Level Geometry" vs "Models") and
// which JSON array (SceneSerializer's "boxes" vs "models") it round-trips through. Carries no
// data of its own — an entity's actual shape/material comes from its RenderableComponent same
// as any other entity.
struct LevelGeometryTag {};

// Free-text classification, mirroring Unity's Tag field — gameplay code and editor search can
// both filter on it ("t:Enemy" in the Hierarchy search box). Absent component == "Untagged".
struct TagComponent {
    std::string Tag = "Untagged";
};

// Unity's GameObject "active" checkbox as a tag: an inactive entity is skipped by rendering,
// collision/raycasting, and editor picking, but keeps all its components and stays listed
// (greyed out) in the Hierarchy so it can be switched back on. Deliberately a tag rather than
// a bool field so "active" is the zero-cost default that needs no component at all.
struct InactiveTag {};

// Unity's "Static" checkbox: marks geometry that never moves at runtime. Purely declarative
// today (nothing in the engine batches or bakes yet) — it's here so scenes can be authored with
// the distinction already recorded, rather than needing a re-pass once that optimization lands.
struct StaticTag {};

// A dynamic light. Point/Spot use the entity's world position; Directional (the sun) ignores
// position and takes its travel direction from the entity's -Z axis (rotate the entity to aim
// it), matching the spot-cone convention. Every kind goes through the same LightBuffer SSBO and
// the same clustered-forward path; the renderer no longer has a hard-coded sun.
struct LightComponent {
    enum class Type { Point, Spot, Directional };
    Type Kind = Type::Point;
    glm::vec3 Color{1.0f, 0.95f, 0.85f};
    float Intensity = 5.0f;
    float Range = 12.0f;             // Point/Spot: distance at which the contribution reaches zero
    float SpotAngleDegrees = 35.0f;  // Spot: half-angle of the cone
    // Directional: angular diameter of the sun disc, in degrees (~0.53 for Earth's sun). Drives
    // soft-shadow penumbra width once CSM lands; ignored by the other kinds.
    float AngularSizeDegrees = 0.53f;
    // 0 = author Color directly (the swatch is live). >0 = Color is driven from this colour
    // temperature in Kelvin (1500-15000 typical); the Inspector shows a Kelvin slider instead of
    // the raw swatch until you hit "Custom RGB", which zeroes this again.
    float ColorTempK = 0.0f;

    // Per-light shadow tuning. Always present (not a separate ECS component) so every light can be
    // authored with shadow settings recorded even when it isn't currently casting. `Enabled`
    // replaces the old `CastShadows` bool; the legacy `"castShadows"` scene key still loads into
    // it and is still written for one release. Bias/NormalBias/Softness are multipliers on the
    // shader's existing texel-proportional depth bias / normal-offset / PCF-radius terms — all
    // default 1.0, i.e. byte-identical to pre-phase-2 output. NearPlane feeds the perspective
    // near for spot/point depth passes. Resolution + UpdateMode are authored and serialized now
    // but don't take effect until the shadow maps support per-slot sizes.
    struct ShadowSettings {
        bool  Enabled    = false;   // was LightComponent::CastShadows
        float Bias       = 1.0f;    // x the shader's texel-proportional depth bias
        float NormalBias = 1.0f;    // x the normal-offset term
        float Softness   = 1.0f;    // x the PCF radius
        float NearPlane  = 0.05f;   // perspective near for spot/point depth
        int   Resolution = 0;       // 0 = follow global; else 512/1024/2048/4096 (authored-only v1)
        int   UpdateMode = 0;       // 0 Dynamic, 1 Static (bake once), 2 Off        (authored-only v1)
    } Shadow;
};

// A game camera placed in the scene. The Game view renders through the first active one of
// these (in creation order) while editing, so you can frame a shot without walking there in
// Play mode; Play mode still uses the first-person Player controller. Absent == the Game view
// falls back to the editor camera and shows a "No camera in scene" hint (#36 B10).
struct CameraComponent {
    float FovDegrees = 60.0f;
    float NearPlane = 0.1f;
    float FarPlane = 1000.0f;
};

// Procedural runtime animation: spin, orbit, bob, and (for a LightComponent entity) hue
// cycling. Applied only while the scene is playing — edit mode always shows the authored
// pose. All parameters are authored/serialized; the Base* fields and Initialized are runtime
// scratch, captured from the entity's authored transform the first frame play runs and never
// written to the scene file, so Play -> Stop restores everything cleanly.
struct AnimatorComponent {
    glm::vec3 SpinDegPerSec{0.0f};   // continuous local rotation, degrees/second per axis

    glm::vec3 OrbitAxis{0.0f, 1.0f, 0.0f};
    float OrbitDegPerSec = 0.0f;     // revolve around Base position on this axis
    float OrbitRadius = 0.0f;

    float BobAmplitude = 0.0f;       // vertical sine offset from Base position, world units
    float BobFreqHz = 0.0f;

    float ColorCycleHzPerSec = 0.0f; // LightComponent hue revolutions/second (0 = leave color alone)

    // --- runtime scratch (not serialized) ---
    bool Initialized = false;
    glm::vec3 BasePosition{0.0f};
    glm::vec3 BaseRotation{0.0f};   // authored RotationEuler, so spin is reversible like orbit/bob (#109)
    glm::vec3 BaseColor{1.0f};
    float Elapsed = 0.0f;
};

// Present only on entities that are parented, or that have at least one child — an entity with
// no relationships at all simply lacks this component, so the common (flat) case pays no cost.
// When Parent is not entt::null, this entity's TransformComponent is interpreted as LOCAL space
// (relative to the parent's world transform, via World::ComposeWorldTransform) instead of world
// space; every other system (physics, picking, rendering) that used to read TransformComponent
// directly must go through ComposeWorldTransform once an entity can be parented.
struct HierarchyComponent {
    entt::entity Parent = entt::null;
    std::vector<entt::entity> Children;
};
