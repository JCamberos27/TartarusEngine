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

// Presence of this component means the entity participates in collision/raycasting.
//
// PhysX path (#185, active only while playing): on Play-enter a static PhysX actor is built
// per collider. `Shape` picks box/sphere/capsule; `HalfExtents` gives its dimensions and
// `Center` its offset from the entity origin, both in the entity's local space. `HalfExtents
// == 0` (the default, and every collider World::CreateBox makes) means "derive an axis-aligned
// box from the mesh bounds", so existing scenes keep their shape without a migration.
//
// The editor's AABB Raycast (World::Raycast — drop-to-surface / snap) still reads these via
// the ColliderWorldBounds helper in World.cpp: axis-aligned, rotation-ignored, sized from the
// RenderableComponent bounds (or a unit box).
struct ColliderComponent {
    // Values are fixed — serialized by index, and PhysicsWorld switches on them. Capsule's axis
    // is Y (its height runs along local up). ConvexHull / Mesh cook a shape from the entity's
    // RenderableComponent geometry (#185 PR 6): ConvexHull works on any body; Mesh is a
    // triangle mesh and PhysX only allows it on static/kinematic bodies (a Mesh collider with a
    // non-kinematic Rigidbody falls back to a convex hull).
    enum class Shape { Box = 0, Sphere = 1, Capsule = 2, ConvexHull = 3, Mesh = 4 };
    Shape Kind = Shape::Box;

    // Box/Sphere/Capsule only. Per-shape meaning when non-zero: Box -> the three half-extents;
    // Sphere -> .x is the radius; Capsule -> .x is the radius, .y is the half-height of the
    // cylindrical section. All-zero means auto-derive a box from the entity's render bounds
    // (see above). Ignored by ConvexHull / Mesh, which take their size from the mesh + scale.
    glm::vec3 HalfExtents{0.0f};

    // Shape centre offset from the entity origin, entity-local space.
    glm::vec3 Center{0.0f};

    // A trigger reports overlaps but doesn't block movement. Built as a PhysX trigger shape
    // (non-blocking); enter/stay/exit event dispatch is #185 PR 5.
    bool IsTrigger = false;

    // Surface response (#185 PR 7). Bounciness is restitution: 0 = dead stop, 1 = no energy
    // lost. Friction is the combined static+dynamic coefficient (0 = ice). PhysicsWorld shares
    // one PxMaterial per distinct (Friction, Bounciness) pair.
    float Bounciness = 0.0f;
    float Friction   = 0.6f;
};

// Makes a collider entity a *dynamic* PhysX body while playing (#185 PR 4) instead of the
// static actor a lone ColliderComponent gets: it falls, tumbles, and is pushed around, and its
// simulated pose is written back to the TransformComponent every frame (Play -> Stop restores
// the authored pose from the scene snapshot, like every other Play-mode change). Needs a
// ColliderComponent for its shape — a Rigidbody with no Collider does nothing. Registered
// through the reflection system (#184), so its Inspector section, serialization and Add
// Component entry are all generated.
struct RigidbodyComponent {
    float Mass = 1.0f;               // kg; ignored when kinematic
    bool  UseGravity = true;         // when false the body still collides but doesn't fall
    bool  IsKinematic = false;       // pose driven from TransformComponent each frame, not by forces
    glm::vec3 InitialVelocity{0.0f}; // linear velocity applied once, on Play-mode entry
    float LinearDamping = 0.05f;     // per-second velocity bleed (0 = frictionless drift)
    float AngularDamping = 0.05f;
};

// Optional clip triggered from the editor Inspector; formerly PlacedModel-only, now any entity.
struct AudioSourceComponent {
    std::string SoundPath;
    float Volume = 1.0f;
    bool Loop = false;
    // Played automatically on Play-mode entry (see EditorLayer::OnEnterPlayMode) and stopped on
    // exit; false by default so placing a source in a scene doesn't start blaring the moment you
    // press Play unless you opt in.
    bool PlayOnStart = false;
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

// Unity's layer field, "-lite" (#236 A1): a small integer slot (0..LayerRegistry::kCount-1)
// grouping entities for editor visibility / pick-locking, camera culling, and (later) PhysX
// collision filtering. Slot 0 is "Default"; the component is omitted entirely for it, so an
// entity on the default layer costs nothing — same zero-default pattern as the tags above.
// Human-readable slot names live in project/layers.json (LayerRegistry), not on the component.
struct LayerComponent {
    int Layer = 0;
};
// SceneVis-lite (#236 B), the editor-only counterparts of Unity's Scene-view hand/lock columns.
// Unlike InactiveTag these change nothing about the object itself — it still renders in the
// Game view, still collides, still ticks, still saves:
//   HiddenInSceneTag — not drawn in the editor Scene viewport, and not click-/marquee-pickable
//                      there (a decluttering aid; the object is still "there").
//   SceneLockedTag   — still drawn in the Scene viewport, but not click-/marquee-pickable, so a
//                      finished piece of set dressing can't be grabbed by accident. Hierarchy
//                      selection and the gizmo still work once selected.
struct HiddenInSceneTag {};
struct SceneLockedTag {};

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
// Registered via ComponentRegistry (#184) rather than hand-coded serializer/Inspector code — the
// third such migration. Reflected fields are addressed by pointer-to-member
// (ComponentReflection.h); the runtime-scratch tail (Initialized, Base*, Elapsed) is left out of
// the reflected field list, same as it was left out of the old hand-written serializer. The flat
// reflected Inspector list loses the old section's "Spin"/"Orbit"/"Bob"/"Light Color Cycle"
// sub-headers, so field names below are qualified (e.g. "Orbit Speed", not "Speed") to stay
// unambiguous without them.
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

// The first drag-and-drop component script. Its editable settings are intentionally data-only:
// a Script asset attaches this component, and this runtime system applies its motion only while
// playing. That gives the Inspector a familiar script workflow without requiring a compiler or
// exposing native engine code to scene authors.
// Registered via ComponentRegistry (#184) rather than hand-coded serializer/Inspector code — the
// second such migration, after SpinComponent below. Reflected fields are addressed by
// pointer-to-member (ComponentReflection.h), so the std::string member is no problem. The
// runtime-scratch tail is deliberately excluded from the reflected field list, same as it was
// excluded from the old hand-written serializer.
struct TransformControllerComponent {
    std::string ScriptPath = "scripts/TransformController.tescript";
    bool Enabled = true;
    glm::vec3 RotationDegPerSec{0.0f};
    glm::vec3 TranslationUnitsPerSec{0.0f};
    float ScalePulseAmplitude = 0.0f; // fraction of the authored scale (0.2 = +/-20%)
    float ScalePulseFrequencyHz = 0.5f;

    // Runtime scratch, never serialized. Play -> Stop reloads the authored scene snapshot.
    bool Initialized = false;
    glm::vec3 BasePosition{0.0f};
    glm::vec3 BaseRotation{0.0f};
    glm::vec3 BaseScale{1.0f};
    float Elapsed = 0.0f;
};

// First component wired up purely through the native reflection system (#184): it is declared
// here, listed once in ComponentRegistry::RegisterEngineComponents(), and from that its JSON
// serialization, Inspector section and Add Component entry are all generated. The per-frame
// behaviour is SpinSystem in TartarusGame.dll. Reflected fields are addressed by pointer-to-
// member (ComponentReflection.h), so there's no standard-layout requirement on the component.
struct SpinComponent {
    glm::vec3 Axis{0.0f, 1.0f, 0.0f}; // local axis to spin around (normalised at use)
    float Speed = 90.0f;             // degrees per second, applied only while playing
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

// Marks the ROOT of a live prefab instance (#236 A2, stage 1-2). The scene file stores only a
// stub for this entity — the source path plus the root's own transform / name / active / layer
// (the "transform-only overrides") — and NOT its descendants; on load the subtree is rebuilt
// from SourcePath and those overrides re-applied on top. So edits to the prefab asset propagate
// to every instance on the next load, while each instance keeps its own placement.
// Descendants carry no marker: the serializer recomputes the owned set from this root each save.
// "Unpack Prefab" removes this component, turning the subtree into plain scene entities.
struct PrefabInstanceComponent {
    std::string SourcePath;
    // Runtime-only: set when SourcePath couldn't be opened on load, so the instance is shown as
    // a broken placeholder rather than silently vanishing. Never serialised.
    bool Missing = false;
    // Runtime-only (#236 A2 stage 3 / #302 Part B): every entity of this instance in
    // prefab-local order — index 0 is this root, the rest follow InstantiatePrefab's creation
    // order (which is deterministic: the .prefab's boxes, then models, then empties, each in
    // file order). Populated by InstantiatePrefab; used at save time to pair each live entity
    // with its pristine counterpart in the .prefab file so per-field overrides can be diffed.
    // Never serialised; entt::null for any descendant the user has since deleted.
    std::vector<entt::entity> InstanceEntities;
};
