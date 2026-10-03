#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <memory>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "RotationMath.h"
#include "Animation.h" // LocalTRS
#include "IK.h"        // IK::SpineDistribution
#include <entt/entt.hpp>

class SkinHideBuffer; // OutfitHideTag, PlayerBodyTag
class VisibleIndexBuffer; // OutfitHideTag
class Model;
struct MaterialAsset; // full definition in MaterialAsset.h; shared_ptr<MaterialAsset> is valid here

// The ECS component set every placed entity (former level-geometry box, imported model, or
// procedural primitive) is built from. Replacing the old WorldBox/PlacedModel split — see
// World.h for the registry and the free functions (ComposeTransform, systems) that operate on
// these.

struct TransformComponent {
    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f};

    // Editor presentation only; never serialized. Keeping the last authored/displayed Euler
    // triple lets the Inspector show the representation the user entered instead of flipping to
    // another equivalent decomposition. Runtime orientation always comes from Rotation.
    glm::vec3 EulerHint{0.0f};

    TransformComponent() = default;
    TransformComponent(const glm::vec3& position, const glm::vec3& eulerDegrees,
                       const glm::vec3& scale)
        : Position(position), Scale(scale) { SetRotationEuler(eulerDegrees); }

    glm::vec3 EulerDegrees() const {
        return NearestEquivalentEuler(EulerYXZFromQuaternion(Rotation), EulerHint);
    }
    void SetRotationEuler(const glm::vec3& eulerDegrees) {
        Rotation = QuaternionFromEulerYXZ(eulerDegrees);
        EulerHint = eulerDegrees;
    }
    void SetRotationQuaternion(const glm::quat& rotation) {
        Rotation = NormalizeRotation(rotation);
        EulerHint = NearestEquivalentEuler(EulerYXZFromQuaternion(Rotation), EulerHint);
    }
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
// Materials: per-submesh MaterialAsset overrides. A null or missing slot falls through to the
// submesh's imported Material. Empty vector (level-geometry boxes) → all imported materials.
struct RenderableComponent {
    std::shared_ptr<Model> ModelRef;
    std::vector<std::shared_ptr<MaterialAsset>> Materials;

    // #163 - Unity's Mesh Renderer lighting flags. Values are serialized by index.
    // On: casts from the light-facing side. TwoSided: both sides (thin/open meshes like planes and
    // leaves that otherwise leak light). ShadowsOnly: invisible, but still casts.
    enum class ShadowCasting { Off = 0, On = 1, TwoSided = 2, ShadowsOnly = 3 };
    ShadowCasting CastShadows = ShadowCasting::On;
    bool ReceiveShadows = true;
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
    // lost. Friction is the sliding (dynamic) coefficient, StaticFriction what it takes to
    // start sliding (0 = ice). PhysicsWorld shares one PxMaterial per distinct combination.
    float Bounciness = 0.0f;
    float Friction   = 0.6f;
    // #170 / #204 - Unity's Physic Material: separate static friction, and how two touching
    // colliders' values combine (PxCombineMode order: 0 Average, 1 Minimum, 2 Multiply,
    // 3 Maximum). When the two sides disagree, the higher mode wins (Maximum > Multiply > ...).
    float StaticFriction = 0.6f;
    int   FrictionCombine = 0;
    int   BounceCombine = 0;
    // #170 - a shared .physicmaterial asset (project-relative). When set and readable, its values
    // replace the five surface fields above for the PhysX shape.
    std::string Material;
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
    // #185 PR 9 — swept CCD for this body, so a fast small object can't tunnel a thin wall.
    // Costs a little; leave off for slow / large bodies.
    bool  ContinuousCollision = false;
    // #185 hardening — per-axis constraints (PxRigidDynamicLockFlag). Freeze linear motion or
    // rotation on any world axis: e.g. lock rotation X+Z to keep a barrel upright, or lock
    // position Y to pin a body to a horizontal plane. Ignored while Kinematic.
    bool  FreezePositionX = false, FreezePositionY = false, FreezePositionZ = false;
    bool  FreezeRotationX = false, FreezeRotationY = false, FreezeRotationZ = false;
    // #168 - how the rendered pose follows the fixed-rate simulation between physics steps:
    // 0 None (snaps to the last step - judders above the physics rate), 1 Interpolate (blends
    // the last two steps, one step behind, smooth), 2 Extrapolate (predicts from velocity, no
    // lag, can overshoot on impact).
    int   Interpolation = 1;
};

// A PhysX joint constraining this entity's body to another (or to a fixed world frame) while
// playing (#185 PR 11). Needs a RigidbodyComponent on this entity; the other end is the entity
// whose OrderComponent value is ConnectedOrder (also needs a Rigidbody), or the world when
// ConnectedOrder < 0. Hand-serialised like ColliderComponent (a joint's "other end" isn't a
// plain reflectable field). Play -> Stop restores the authored scene as usual.
struct JointComponent {
    enum class Type { Fixed = 0, Hinge = 1, Ball = 2, Slider = 3, Distance = 4 };
    Type Kind = Type::Fixed;
    int  ConnectedOrder = -1;        // OrderComponent.Value of the other body; <0 = world
    glm::vec3 Anchor{0.0f};          // joint point, this entity's local space
    glm::vec3 Axis{1.0f, 0.0f, 0.0f}; // hinge / slider axis, this entity's local space
    float BreakForce  = 0.0f;        // 0 = unbreakable
    float BreakTorque = 0.0f;
    // Optional motion limits. Hinge: LimitLower/Upper are degrees about Axis. Slider: units
    // along Axis. Distance: LimitLower = min separation, LimitUpper = max. Ignored by Fixed/Ball.
    bool  UseLimit   = false;
    float LimitLower = -45.0f;
    float LimitUpper =  45.0f;
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
    int Output = 0; // #171 - mixer bus (AudioEngine::Bus): 0 SFX, 1 Music, 2 Ambient, 3 UI, 4 Voice
    // #171 - Unity's 3D Sound Settings. Spatial = heard from this entity's position (panned and
    // attenuated); off = plain 2D, same volume everywhere (music, UI). Rolloff: 0 Logarithmic
    // (Unity's default, inverse-distance), 1 Linear (silent at Max Distance).
    bool  Spatial = true;
    int   Rolloff = 0;
    float MinDistance = 1.0f;
    float MaxDistance = 500.0f;
    float DopplerLevel = 1.0f; // #171 - 0 = no pitch shift from motion, 1 = physical
};

// #162 / #203 - Unity's post-processing Volume. A Global volume applies everywhere; a local one
// applies inside its box (Size, in the object's local units, scaled by its transform) and fades
// in over Blend Distance outside it. Each ticked override pulls that setting toward its value by
// the volume's weight; volumes apply in Priority order (higher wins), on top of the scene's own
// Lighting > Post-processing values.
struct PostProcessVolumeComponent {
    bool      Global = false;
    glm::vec3 Size{10.0f};
    float     BlendDistance = 2.0f;
    float     Weight = 1.0f;
    int       Priority = 0;
    bool OverrideExposure = false;    float ExposureEV = 0.0f;
    bool OverrideTemperature = false; float Temperature = 0.0f;
    bool OverrideTint = false;        float Tint = 0.0f;
    bool OverrideContrast = false;    float Contrast = 0.0f;
    bool OverrideSaturation = false;  float Saturation = 0.0f;
    bool OverrideVignette = false;    float Vignette = 0.3f;
    bool OverrideBloom = false;       float BloomIntensity = 0.1f;
    bool OverrideChromatic = false;   float ChromaticAberration = 0.5f;
    bool OverrideGrain = false;       float FilmGrain = 0.3f;
};

// #171 - Unity's Audio Listener: in Play, 3D sounds are heard from this entity (position and
// facing) instead of from the game camera. The first active one wins.
struct AudioListenerComponent {
    bool Enabled = true;
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

// Unity's activeInHierarchy, as a tag: an inactive entity is skipped by rendering,
// collision/raycasting, and editor picking, but keeps all its components and stays listed
// (greyed out) in the Hierarchy so it can be switched back on. Deliberately a tag rather than
// a bool field so "active" is the zero-cost default that needs no component at all.
// DERIVED - never set it directly: World::SyncActiveInHierarchy() puts it on every entity
// that has DeactivatedTag itself or on any ancestor, so switching a parent off hides its
// whole subtree (#201). Runtime systems only ever test this one.
struct InactiveTag {};

// #163 - Unity's LOD Group. The entity's direct children are its levels, in order: child 0 is
// LOD 0 (full detail), child 1 is LOD 1, and so on (up to 4; any further children are ignored
// by LOD and always drawn). Each level is used while the group's height on screen, as a fraction
// of the view height, is at least that level's threshold; below the last used level's threshold
// the whole group is culled. Evaluated per view by World::ApplyLod.
struct LODGroupComponent {
    float Lod0 = 0.6f, Lod1 = 0.3f, Lod2 = 0.1f, Lod3 = 0.03f; // screen-height thresholds, 0..1
    float Size = 0.0f; // world-space size used for the screen height; 0 = automatic from LOD 0's bounds
};

// Runtime only, never saved: this renderer belongs to an LOD level the current view isn't
// using (see LODGroupComponent). Draw and shadow passes skip it; unlike InactiveTag it doesn't
// affect physics, scripts or the Hierarchy.
struct LodCulledTag {};
// Unity's activeSelf == false: the authored "active" checkbox. Set by the editor and the
// scene/prefab loader, saved as "active": false. See InactiveTag for what it causes.
struct DeactivatedTag {};

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

// Runtime only, never saved: set by FirstPersonPresentation on the arms and weapon entities it
// creates for Play, and read by the renderer's view-model sub-pass. Tagged entities are drawn
// last, after a depth clear, projected with FirstPersonControllerComponent::ViewModelFov instead
// of the view's own FOV — the depth clear is what stops world geometry (metres away) from
// clipping hands that sit ~0.3 m from the eye, and the separate, usually narrower FOV is what
// makes a held weapon read as held rather than stretched into the scene.
//
// A caller that leaves RenderFrameContext::ViewModelFov at its default — the editor Scene tab,
// which previews through its own camera — has no sub-pass to run, so tagged entities are drawn
// there as ordinary scene geometry exactly as before.
struct ViewModelTag {};

// Animated but never drawn: no camera pass, no shadow, no SSAO depth, no search tint. The first-person
// arms rig once the body's arms take its hands (FirstPersonBody::ArmsLateUpdate): it keeps posing (its
// hands are the targets, the sights stay camera-locked) but the body's own arms are what is seen and
// what casts the shadow. Runtime only, like ViewModelTag.
struct PoseSourceTag {};

// Split first-person / world poses (FirstPersonBody with Weapon Arms, in Play). The player's own camera
// shows the first-person pose - the arms and gun exactly as the animations have them - while every other
// view (the Scene view, other players) and every shadow shows the world copy: the same animations, the gun
// placed on the body's shoulder and clear of its head, the body's hands on it there. Runtime only.
//   OwnerViewOnlyTag   - drawn only in the player's own camera; no other view, no shadow, no SSAO elsewhere.
//   HiddenFromOwnerTag - the world copy: every view but the player's own camera; casts the shadows.
struct OwnerViewOnlyTag {};
struct HiddenFromOwnerTag {};

// The player's own body (FirstPersonBody, in Play): the camera sits in it, so whatever of it comes within
// NearHide metres of the eye is not drawn in the camera's world pass (shadows keep it). Near the eye the
// near plane would otherwise slice the mesh into slivers - the neck and shoulders on a landing, the chest
// looking down. Runtime only, like ViewModelTag.
//
// CameraHideBones: skin weighted mostly to these bones (palette IDs, -1 = unused) isn't drawn in that pass
// either - the torso's shoulders, which move with the arms (so the arms stay on them in every other view)
// but which the view-model arms draw over in the camera's, at their own FOV.
//
// Sleeves (an outfit's clothing: a top, a jacket, gloves): while the arms are in the view-model pass, the
// clothing's arm part must be too, or it's projected at the world's FOV and hangs off the arms - a sleeve
// floating beside the hand. SleeveBones marks the arm's palette bones (the upper arms and everything under
// them); with SleevesInViewModel set, the renderer draws the piece twice: the world pass without the
// vertices weighted mostly to them, the view-model pass with only those. The seam is at the shoulder.
struct PlayerBodyTag {
    // Not drawn in the camera's world pass at all (the head, and what an outfit hangs on it): the camera is
    // inside it. Shadows, the Scene view and every other view still draw it.
    bool CameraHidden = false;
    float NearHide = 0.1f;
    // Clothing (not a body part): its Near Hide is FirstPersonBodyComponent::ClothingNearHide and it
    // stretches NearHideWidth metres to either side of the view (0 = a sphere of NearHide).
    bool Clothing = false;
    float NearHideWidth = 0.0f;
    // Clothing's vertices around the neck in its bind pose (FirstPersonBodyCollarVertices), not drawn in
    // the camera's world pass. Null = none.
    std::shared_ptr<SkinHideBuffer> CollarVerts;
    std::shared_ptr<const std::vector<std::uint8_t>> CollarBits; // the same, on the CPU (FirstPersonBody's camera probe)
    int CameraHideBones[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    bool HasSleeves = false;            // SleeveBones is set (a clothing piece with arm bones)
    bool SleevesInViewModel = false;    // FirstPersonBody: the arms are in the view-model pass right now
    bool SleevesHidden = false;         // ... or hidden from the camera (easing off a holstered gun): the sleeves too
    std::uint32_t SleeveBones[16] = {}; // bit per palette bone (MAX_BONES = 512)
    // Clothing around the camera, never drawn in its world pass (shadows and other views keep it): what the
    // neck and everything above it move (a hood, a collar, a scarf), the collarbones' cloth (the shoulder
    // tops, which turn up toward the eye with the gun) and the neck's parent alone (the collar's base). The
    // camera sits inside the head; this clothing swept across the view as dark flaps, swinging with the walk.
    bool HasHeadBones = false;
    std::uint32_t HeadBones[16] = {};   // bit per palette bone
};

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

    // #203 - Unity's Culling Mask: bit N set = objects on layer N (Layer component) receive this
    // light. -1 (all bits) = everything, the default. Shadows are unaffected.
    int CullingMask = -1;

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
// these (in creation order) while editing, so you can frame a shot without walking there.
// In Play (#165) it's also the game camera, unless the scene has a First Person Controller.
// Absent == the Game view falls back to the editor camera and shows a "No camera in scene"
// hint (#36 B10).
struct CameraComponent {
    float FovDegrees = 60.0f;
    float NearPlane = 0.1f;
    float FarPlane = 1000.0f;
};

// #177 - a basic Unity-style Particle System: emits camera-facing soft sprites from the entity's
// origin in a cone around its local +Y, fading/shrinking over their lifetime. Simulated every
// frame (edit mode too, so you can tune it without pressing Play); drawn after transparent
// geometry. Everything below `Live` is runtime state, never serialized.
struct ParticleSystemComponent {
    bool  Emitting = true;
    float Rate = 20.0f;           // particles per second
    int   MaxParticles = 500;
    float Lifetime = 2.0f;        // seconds
    float StartSpeed = 3.0f;      // metres per second
    float Spread = 25.0f;         // cone half-angle around local +Y, degrees (0 = a straight jet)
    float StartSize = 0.25f;      // metres
    float EndSize = 0.05f;
    glm::vec3 StartColor{1.0f, 0.75f, 0.3f};
    glm::vec3 EndColor{1.0f, 0.2f, 0.05f};
    float StartAlpha = 1.0f;
    float EndAlpha = 0.0f;
    float Intensity = 1.0f;       // HDR brightness multiplier (above 1 feeds Bloom)
    float GravityModifier = 0.0f; // x project gravity (1 = falls like a rigidbody)
    int   BlendMode = 1;          // 0 Alpha Blended, 1 Additive
    // Runtime only (not serialized): a project-relative texture that turns particles with a Length
    // into muzzle-flame tongues (see Particle.Length). Empty = the plain soft discs.
    std::string Texture;

    // The flame extras: a particle with Length > 0 (and a Texture set) is drawn as a tongue standing
    // on Pos and pointing along Axis, growing from nothing to Length x Width metres over its life
    // (the system's sizes and colours are ignored). Seed (0..1) picks its shape, Glow scales its
    // emission and Alpha its smoky body.
    struct Particle {
        glm::vec3 Pos{0.0f}, Vel{0.0f};
        float Age = 0.0f, Life = 1.0f;
        glm::vec3 Axis{0.0f, 0.0f, -1.0f};
        float Length = 0.0f, Width = 0.0f, Seed = 0.0f, Glow = 1.0f, Alpha = 1.0f;
    };
    std::vector<Particle> Live;
    float EmitAccumulator = 0.0f;
    std::uint32_t Rng = 0x9E3779B9u;
};

// #165 - the built-in first-person player, as a component. In Play the player spawns at this
// entity's position (feet) facing its forward (-Z), with these settings. Without one, Play
// renders through the scene's Camera (a fixed/animated shot, no player); with neither, it falls
// back to the old behaviour (a default player dropped in at the editor camera).
struct FirstPersonControllerComponent {
    float MoveSpeed = 6.0f;
    float SprintMultiplier = 1.6f;
    float JumpSpeed = 5.5f;
    float JumpBufferTime = 0.12f; // a jump pressed this long (seconds) before landing still happens on landing
    float CoyoteTime = 0.10f;     // a jump pressed this long after stepping off an edge still counts
    // Seconds (time constants) the move takes to reach the input's speed on the ground, to slow down,
    // and to steer in the air. 0 = instant.
    float GroundAccelTime = 0.0f;
    float GroundDecelTime = 0.0f;
    float AirAccelTime = 0.0f;
    float EyeHeight = 1.6f;
    float CapsuleRadius = 0.3f;
    float CapsuleHeight = 1.8f;
    float MouseSensitivity = 0.1f;  // degrees per pixel
    bool  InvertY = false;
    // HORIZONTAL degrees on a 16:9 screen - the "FOV 90" of a shooter's settings menu. The
    // camera itself is vertical (VerticalFov); a wider screen sees more at the sides (Hor+).
    float FieldOfView = 90.0f;
    float VerticalFov() const {
        const float h = glm::radians(std::clamp(FieldOfView, 1.0f, 179.0f));
        return glm::degrees(2.0f * std::atan(std::tan(0.5f * h) * (9.0f / 16.0f)));
    }
    // Gamepad right stick look speed (turn rate, not a delta)
    float StickLookDegPerSec = 180.0f;  // degrees per second; Gamepad Input group
    // Camera lean collision: the sphere radius used for wall-detection during camera lean
    float EyeRadius = 0.12f;            // metres; keeps the near plane off the wall; Camera group
    float KillY = -20.0f;           // falling below this respawns at the spawn point
    bool  GravityGun = true;        // the built-in pick-up/throw tool (right/left mouse)
    float Gravity = 18.0f;          // m/s^2 pulling the player down - game feel, separate from the physics world's
    // Gravity gun throw: hold left mouse to charge from Min to Max Throw Speed over Charge Time.
    float MinThrowSpeed = 4.0f;     // m/s, a tap
    float MaxThrowSpeed = 18.0f;    // m/s, fully charged
    float ThrowChargeTime = 1.0f;   // seconds to full power
    float ThrowBackspin = 2.0f;     // revolutions per second given to a thrown ball (round bodies only)
    // Gravity gun grab: aiming distance for the primary pick-up ray
    float GrabRange = 100.0f;       // metres; Gravity Gun group
    // Gravity gun aim assist: search radius when no object is under the exact crosshair
    float AssistRange = 30.0f;      // metres; Gravity Gun group
    // Gravity gun assist cone: within this many degrees of the crosshair
    float AssistConeDeg = 7.0f;     // degrees; Gravity Gun group
    // Gravity gun scroll-wheel tuning: rotation applied per scroll notch while holding an object
    float ScrollTurnDeg = 15.0f;    // degrees per notch; Gravity Gun group

    // Optional camera-bound arms + weapon presentation. The .fpsanim asset defines paired clips;
    // leaving this empty preserves the existing controller exactly (including Sandbox gravity gun
    // playtests). The fields below are authored setup, not a second physics character.
    std::string AnimationSet;
    // A second weapon (.fpsanim) for the same arms: key 1 draws AnimationSet, 3 this one, 2 goes
    // unarmed. Switching holsters the one in hand, then draws the other; each keeps its ammo.
    std::string SecondaryAnimationSet;
    // The rig bone the play camera sits on. Placement parks this bone's world position exactly on
    // the camera, so ViewModelOffset below is only a residual nudge - which matters because these
    // rigs are authored standing in their own scene (feet at y=0, head near y=1.56): without a
    // bone anchor the model's root goes where the camera is and the whole rig floats ~1 m above
    // the view. Leave empty to fall back to positioning the model's root directly on the camera.
    std::string CameraBone = "head";
    glm::vec3 ViewModelOffset{0.0f};
    glm::vec3 ViewModelRotation{0.0f};
    float ViewModelScale = 1.0f;
    float ViewModelFov = 60.0f;
    // The player's health (Combat/PlayerVitals.h): enemies' rounds take it, it comes back
    // RegenRate per second after RegenDelay seconds unhurt, and at 0 the player dies and respawns
    // here RespawnDelay seconds later, untouchable for SpawnProtection seconds.
    float MaxHealth = 100.0f;
    float RegenDelay = 5.0f;
    float RegenRate = 30.0f;
    float RespawnDelay = 3.0f;
    float SpawnProtection = 2.0f;
};

// True first person (FirstPersonBody.h): the player's own full body, drawn in the world under the
// play camera. Put it on the body's root object, anywhere in a scene with a First Person
// Controller. The body is modular: the root's children are its pieces (head, torso, legs, feet -
// or clothing in their place), each a rigged model on the same skeleton. The first piece with an
// Animator Controller (a locomotion controller - see assets/Animations/Controllers/fps_body_locomotion.controller)
// drives; every other piece follows it. The root may itself be that piece.
// In Play the body stands at the player's feet facing the view, the controller gets the player's
// movement as parameters (MoveX / MoveY / Speed / Sprint / Grounded / Airborne / Jump), and the
// clips' root motion walks the player's capsule. The play camera sits in the body's head.
struct FirstPersonBodyComponent {
    // 0 = the capsule moves exactly as the clips travel (root motion), 1 = as the input asks
    // (the clips only animate). Between: a blend - heavier and more grounded toward 0.
    float Responsiveness = 0.0f;
    float ParamSmoothing = 0.12f;  // seconds the MoveX / MoveY parameters take to follow the input
    float RunSpeed = 3.26f;        // m/s asked of the blend tree when moving (the jog clips' speed)
    float SprintSpeed = 4.72f;     // ... and while sprinting (the run clip's)
    // The capsule's own run / sprint speeds when they differ from the clips' (0 = the clips' Run /
    // Sprint Speed). With Responsiveness up, the gait clips then play faster to keep pace.
    float PlayerRunSpeed = 0.0f;
    float PlayerSprintSpeed = 0.0f;
    float MaxPlayRate = 1.5f;      // the fastest the gait clips play, x authored
    std::string HeadBone = "head";
    // The eyes, from the head bone, in the body's frame (x right, y up, z forward), metres.
    glm::vec3 CameraOffset{0.0f, 0.08f, 0.1f};
    // With the gun out the eye is placed from the arms rig - where its camera sits against its shoulders, put on
    // the body's - so the hands are within reach. A first-person rig's camera sits low and back (the AKS74U's:
    // 8 cm over its shoulders, a real eye's ~27): the eye sank into the neck and the chest's top was in front of
    // it, in the view's corner. This lifts it back toward the eyes (body frame: X right, Y up, Z forward, metres).
    // The gun and arms stay on the camera; the body sits lower under them, so the hands reach a little further.
    // All of it within 25 degrees of level; none past 60 up or down (the chest under the eye looking down, the
    // support hand past its reach looking up), eased between.
    glm::vec3 ArmedEyeOffset{0.0f, 0.10f, 0.04f}; // Armed Eye Offset
    // How much of the head's own motion (bob, sway, lean) the camera follows: 0 = it rides at
    // the head's standing height, steady; 1 = locked to the head bone.
    float HeadBob = 0.5f;
    float CameraSmoothing = 0.06f; // seconds; smooths the head motion in the body's frame
    // Pieces that cast a shadow but aren't drawn in first person, comma separated: a child is
    // hidden when its name contains one of these (case-insensitive). The camera is in the head.
    std::string HiddenParts = "Head";
    // Bones collapsed while the body is the player's, comma separated (empty = none) - e.g.
    // "upperarm_l, upperarm_r" on a one-piece body whose arms a first-person arms model draws.
    std::string HiddenBones;
    // Phase 2 (#405): the body's own arms hold the weapon. While the first-person arms rig is
    // up, the body's arms take its pose and IK their hands onto its hands (seen in the world
    // pass, at the view model's screen position), and the arms rig itself is no longer drawn -
    // its gun is. Off leaves the arms rig drawing the arms, as before.
    bool WeaponArms = false;
    // The body piece that is the arms (a child whose name contains this, case-insensitive). While
    // Weapon Arms holds it is drawn with the gun in the view-model pass, so its hands sit exactly
    // where the first-person rig's do.
    std::string ArmsPiece = "Arms";
    // How much of the camera's pitch the spine takes (0 = the body stays upright, 1 = the chest
    // tilts as far as the view), so the shoulders follow the view and the hands stay in reach.
    float SpineAim = 0.0f;
    // ---- lane A ----
    // How the spine's turns (view pitch, twist, shoulder line) are shared over spine_01..05, pelvis alpha:
    // the default is the even spread. Used by the player body and, copied at Play, by NPCs.
    IK::SpineDistribution Spine;
    // ---- end lane A ----
    // The same looking down. Armed, the eye hangs off the shoulders as the arms rig's does, so looking
    // down it comes over the chest only as far as the chest pitches with it.
    float SpineAimDown = 0.9f;
    // Armed, how far the chest takes the arms rig's stance (its shoulder line, e.g. bladed with the
    // support shoulder forward) rather than squaring to the view: 1 = the rig's, so both hands reach.
    float ShoulderLineMatch = 1.0f;
    // Standing still, the body keeps its heading until the view is this many degrees off it, then
    // turns on the spot (the turn clips). 0 = the body always faces the view.
    float TurnThreshold = 0.0f;
    // How much of the view's twist off the body's heading the spine takes (0..1), so the chest
    // faces the view while the feet have not turned yet.
    float SpineTwist = 0.0f;
    // While the body stands still, the view turns no faster than this many degrees a second once it
    // is past the Turn Threshold (where the feet must step round) - what the turn clips can keep up
    // with, so they don't slide. Aiming within the threshold is free. 0 = no limit.
    float MaxTurnRate = 0.0f;
    // Starting and stopping play their own clips (a push-off, a braking step) instead of blending
    // straight between idle and the gait; the clips' root motion then eases the capsule up to
    // speed and down again.
    bool StartStopClips = false;
    // Crouching (hold the Crouch action): the capsule's height while crouched (0 = no crouching) and
    // the move speed as a fraction of Run Speed (the crouch-walk clips travel about 1.35 m/s).
    float CrouchHeight = 0.0f;
    float CrouchSpeed = 0.42f;
    // Foot IK: each foot is put on the ground under it (a ray down), the pelvis drops to the lower
    // foot (at most FootIKMaxDrop metres) and the legs are re-solved, so the feet meet stairs and slopes.
    bool FootIK = false;
    float FootIKMaxDrop = 0.35f;
    // The body's bones are found by the UE5 mannequin's names. A rig that names them differently maps
    // them here, "standard = theirs", comma or line separated: "pelvis = Hips, foot_l = LeftFoot, foot_r = RightFoot".
    // (The weapon-arms feature still needs the body and the arms rig to share bone names.)
    std::string BoneMap;
    // --- Advanced tuning (defaults are the values the body was tuned with) ---
    float EyeSlack = 0.035f; // Eye Slack
    float ReachSlack = 0.04f; // Reach Slack
    float ShrugStart = 0.98f; // Shrug Start
    float ShrugMax = 0.12f; // Shrug Max
    // The most (degrees) a collarbone turns to move its shoulder (shrug and steadying together): the shoulder
    // turns about the collarbone's inner end, like a real one, rather than sliding.
    float ShoulderMaxAngle = 25.0f; // Shoulder Max Angle
    // How much of the arms rig's collarbone turn the body's arms (and the chest's stance, Shoulder Line Match)
    // take, 0..1. The rig has no torso or head to hit, so its clips swing the collarbones freely - a reload's
    // reach hunched the body's shoulder up at the camera. 1 = all of it, 0 = the body's own clip pose; the
    // hands stay on the rig's either way (the shrug turns a collarbone as far as a hand needs).
    float ClavicleFollow = 0.35f; // Clavicle Follow
    // The most (degrees) the chest leans toward a hand still out of reach after its collarbone has turned.
    float ReachLeanMax = 15.0f; // Reach Lean Max
    // Arm Steadiness (Weapon Arms): how much the arms ignore the body's locomotion sway, 0..1. The shoulders
    // are held at a slow average of where they sit relative to the view (Arm Steady Time), by at most Arm
    // Steady Max metres, and the elbows bend in the rig's plane. 0 = the arms follow the chest (the old behaviour).
    float ArmSteadiness = 1.0f; // Arm Steadiness
    float ArmSteadyTime = 0.5f; // Arm Steady Time
    float ArmSteadyMax = 0.04f; // Arm Steady Max
    // Split poses: each world elbow swings out about its shoulder-to-hand line (the hand stays on the gun) until
    // the arm around it is this far (metres, the sleeve's thickness) from the drawn torso. A view-model rig tucks
    // its elbows to a narrower body than this one; in its bend plane they went into the chest. 0 = off.
    float ElbowClearance = 0.06f; // Elbow Clearance
    // Looking down, the eye moves this far (metres) forward over the chest, eased in from Look Down Start
    // degrees below level to straight down - the head pitching at the neck. Keeps the camera out of the torso.
    float LookDownPush = 0.0f; // Look Down Push
    float LookDownStart = 0.0f; // Look Down Start
    // Whatever of the body comes within this many metres of the eye isn't drawn in the camera's view (its
    // shadow stays): the near plane would slice it into slivers. 0 = off.
    float NearHide = 0.1f; // Near Hide
    // Clothing (outfit pieces that aren't body parts) is hidden further out than the body: within this many
    // metres of the eye above, below and ahead (never less than Near Hide), and Clothing Near Hide Width to
    // either side. Clothing is bulkier than skin and swings with the walk, so a hood's rim or a shoulder top
    // would otherwise sweep across the view's edges.
    float ClothingNearHide = 0.2f; // Clothing Near Hide
    float ClothingNearHideWidth = 0.28f; // Clothing Near Hide Width
    // Clothing that sits around the neck in the bind pose - at or above the shoulder joints, less this many
    // metres, and between them - isn't drawn in the camera's view whatever bones move it (a hood or a
    // scarf skinned to the chest). < 0 = off.
    float CollarHideDrop = 0.02f; // Collar Hide Drop
    // Bones whose skin isn't drawn in the camera's own view on the body pieces other than the arms,
    // comma separated (at most 8): the torso's shoulders. They follow the arms' shoulders (so the arms
    // stay attached), which reach toward the gun - in view they'd bulge into the camera.
    std::string CameraHiddenBones = "clavicle_l, clavicle_r, upperarm_l, upperarm_r"; // Camera Hidden Bones
    float ArmsEaseOut = 0.1f; // Arms Ease Out
    float TurnLagFloor = 90.0f; // Turn Lag Floor
    float TurnLagMargin = 5.0f; // Turn Lag Margin
    float TurnEndAngle = 8.0f; // Turn End Angle
    float TurnMinTime = 0.3f; // Turn Min Time
    float TurnTimeout = 4.0f; // Turn Timeout
    float TurnMoveEase = 0.08f; // Turn Move Ease
    float StartIdleTime = 0.25f; // Start Idle Time
    float StartMaxMove = 0.6f; // Start Max Move
    float StopMinRunTime = 0.6f; // Stop Min Run Time
    float StopMinRunTimeCrouched = 0.7f; // Stop Min Run Time (Crouched)
    float StopMinSpeed = 1.2f; // Stop Min Speed
    float StopMinSpeedCrouched = 0.6f; // Stop Min Speed (Crouched)
    float StopDebounce = 0.05f; // Stop Debounce
    float StopRunForward = 0.7f; // Stop Run Forward
    float AirborneDelay = 0.15f; // Airborne Delay
    float FootLockDrift = 0.12f; // Foot Lock Drift
    float FootPlantedHeight = 0.05f; // Foot Planted Height
    float FootRayUp = 0.5f; // Foot Ray Up
    float FootRayLength = 1.0f; // Foot Ray Length
    float FootMaxRaise = 0.25f; // Foot Max Raise
    float PelvisMaxRaise = 0.0f; // Pelvis Max Raise
    float FootOffsetEase = 0.06f; // Foot Offset Ease
    float FootNormalEase = 0.08f; // Foot Normal Ease
    float FootIKFade = 0.1f; // Foot IK Fade
    float FootTiltMax = 25.0f; // Foot Tilt Max
    float FootLockEaseIn = 0.04f; // Foot Lock Ease In
    float FootLockEaseOut = 0.08f; // Foot Lock Ease Out
    float StairPopRise = 0.03f; // Stair Pop Rise
    float StairPopRate = 2.5f; // Stair Pop Rate
    float StairEase = 0.09f; // Stair Ease
    // --- Arm IK (Player body: elbow tracking and clearance) ---
    float ElbowEase = 0.06f; // Elbow Ease
    float ElbowMaxRate = 540.0f; // Elbow Max Rate (degrees/s)
    // --- NPC body tuning (copied to NPC soldiers from the scene's player body) ---
    // NPC body heading and turns
    float NpcTurnThreshold = glm::degrees(1.15f); // NPC Turn Threshold (degrees; ~66 deg body can lag before turning on the spot)
    float NpcMoveEase = 0.1f; // NPC Move Ease (seconds; blend tree parameter easing)
    float NpcFaceEase = 0.09f; // NPC Face Ease (seconds; heading easing while moving)
    // NPC spine and body aim
    float NpcMaxTwist = glm::degrees(1.2f); // NPC Max Twist (degrees; spine twists toward aim)
    float NpcAimLean = glm::degrees(0.1f); // NPC Aim Lean (degrees; torso forward lean aiming, standing)
    float NpcAimLeanCrouched = glm::degrees(0.22f); // NPC Aim Lean Crouched (degrees; ~13 deg)
    float NpcReadyLeanCrouched = glm::degrees(0.4f); // NPC Ready Lean Crouched (degrees; ~23 deg, low ready stance)
    float NpcCowerHunch = glm::degrees(0.35f); // NPC Cower Hunch (degrees; spine curls forward ducking)
    // NPC head look
    float NpcHeadMaxYaw = glm::degrees(1.2f); // NPC Head Max Yaw (degrees; ~70 deg head turns past chest)
    float NpcHeadMaxPitch = glm::degrees(0.6f); // NPC Head Max Pitch (degrees; ~35 deg nod up/down)
    // NPC foot IK
    float NpcFootIKMaxDrop = 0.35f; // NPC Foot IK Max Drop (metres)
    float NpcFootIKMaxRaise = 0.35f; // NPC Foot IK Max Raise (metres)
    float NpcFootIKPelvisRaise = 0.08f; // NPC Foot IK Pelvis Raise (metres; pelvis height adjustment)
    float NpcFootIKTiltMax = glm::degrees(0.5f); // NPC Foot IK Tilt Max (degrees; max foot angle to ground normal)
    float NpcFootOffsetEase = 0.05f; // NPC Foot Offset Ease (seconds; vertical foot adjustment easing)
    float NpcFootNormalEase = 0.08f; // NPC Foot Normal Ease (seconds; ground normal easing)
    float NpcFootIKFade = 0.15f; // NPC Foot IK Fade (seconds; foot IK enable/disable easing)
    // ---- lane A ----
    // Foot slide correction (docs/CAS_PARITY.md #8); both layers off by default, NPCs use the same numbers
    bool FootPinEnabled = false; // Foot Pin Enabled
    float FootPinWeight = 1.0f; // Foot Pin Weight
    float FootPinRelease = 0.06f; // Foot Pin Release (seconds)
    float FootPinMaxDrift = 0.25f; // Foot Pin Max Drift (metres)
    bool StrideWarpEnabled = false; // Stride Warp Enabled
    float StrideWarpWeight = 1.0f; // Stride Warp Weight
    float StrideScaleMin = 0.75f; // Stride Scale Min
    float StrideScaleMax = 1.35f; // Stride Scale Max
    float StridePelvisAdjust = 1.0f; // Stride Pelvis Adjust
    // ---- end lane A ----
};

// A character dressed from a wardrobe (docs/CHARACTER_OUTFITS.md): put it on the body's root. Its children
// tagged with an Outfit Piece are the outfit - body parts (torso, arms, legs, feet, head) and items
// (hair, tops, pants, shoes, hats ...) - and the Inspector's outfit editor builds and swaps them from
// the wardrobe's catalog. The children are the record of what is worn (each piece's model and its
// materials, i.e. its colourway); this holds the choices the pieces don't: gender and race.
struct CharacterOutfitComponent {
    std::string Wardrobe = "assets/Characters/Quantum/Quantum.wardrobe"; // the pack's .wardrobe file
    int Gender = 0;                  // Wardrobe::Gender: 0 male, 1 female
    std::string Race = "European";   // a race of that gender's body (head + skin)
    std::string Locks;               // slots Randomize leaves alone, comma separated
    bool AutoHide = true;            // skin (and under-layers) covered by clothing isn't drawn
    bool RandomizeOnPlay = false;    // Play starts in a fresh random outfit (reverted on Stop, like any Play change)
    int Version = 0;                 // runtime: bumped on every change (FirstPersonBody re-reads its pieces)
    std::uint64_t HideSignature = 0; // runtime: the pieces the hiding was last worked out for
    int LinkedVersion = -1;          // runtime: the Version whose pieces were last linked to the driving animator
    std::uint64_t HideWaiting = 0;   // runtime: the pieces the hiding is waiting on coverage for ...
    std::uint64_t HideRetryFrame = 0;// ... and when UpdateHiding looks again
};

// One piece of a Character Outfit (a child of the object with the Character Outfit component). Set by
// the outfit editor; `Item` is the model the piece was built from, so a piece whose model was swapped
// by hand is noticed and rebuilt.
struct OutfitPieceComponent {
    std::string Slot;                // "Torso", "Hair", "Top", "Wrist L" ...
    std::string Item;                // project-relative model path
    int Flags = 0;                   // OutfitPieceFlags
    float Fit = 0.0f;                // runtime: Wardrobe::FitOf (0 = not looked up yet; UpdateHiding does)
};
enum OutfitPieceFlags { OutfitPieceBodyPart = 1, OutfitPieceHeadAttached = 2 };

// The vertices of an outfit piece that clothing covers (OutfitSystem::UpdateHiding): not drawn, so skin
// can't poke through the cloth. Runtime only - rebuilt from the outfit whenever its pieces change.
struct OutfitHideTag {
    std::shared_ptr<SkinHideBuffer> Buffer; // one bit per vertex (SkinHideBuffer.h)
    std::shared_ptr<const std::vector<std::uint32_t>> Bits; // the same bits on the CPU
    std::shared_ptr<VisibleIndexBuffer> Visible;            // the triangles not covered entirely, drawn in its place
    int Hidden = 0, Total = 0;              // vertices hidden / in the model (the Inspector's readout)
};

// How far an outfit piece is drawn toward the camera (OutfitSystem::UpdateHiding): the layers it's worn over
// times OutfitSystem::kLayerPull. Depth only - each vertex slides along its own view ray, so nothing moves on
// screen - so cloth lying on the layer under it wins the depth test instead of flickering with it, and a few
// millimetres of the layer under swaying out through it stay behind. Runtime only.
struct OutfitLayerTag {
    float Pull = 0.0f; // metres
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
// #175 / #113 — Unity's (legacy) Animation component: plays one of the entity's model clips in
// Play mode. Serialized fields are the authored setup; game code drives it at runtime by
// changing Clip (crossfades to it) or IsPlaying. SkeletalAnimationSystem applies it each frame.
// Root motion (RootMotion.h): what a component does with the travel its clips author on the root
// bone. Off plays the clips as authored (a walk drags the mesh away from the object and snaps
// back each loop). Apply takes the travel out of the pose and moves the object by it. In Place
// takes it out of the pose too but leaves the object where it is: game code reads the motion
// (RootMotionDeltaPosition / Yaw) and moves the object itself, or a second rig follows one that
// applies it.
enum class RootMotionMode : int { Off = 0, Apply = 1, InPlace = 2 };

struct RootMotionOptions {
    int  Mode = 0;              // RootMotionMode
    std::string Bone;           // the root bone; empty = auto ("root", else the hips / pelvis)
    bool Rotation = true;       // turns in the clip turn the object (off: they stay in the pose)
    bool Vertical = false;      // height changes move the object (off: jumps, crouches stay in the pose)

    // --- runtime (not serialized): the last update's motion ---
    glm::vec3 DeltaPosition{0.0f}; // world-space metres (the parent's space when parented)
    float DeltaYaw = 0.0f;         // degrees about +Y
    float Speed = 0.0f;            // horizontal m/s, for readouts and game code
    float TurnRate = 0.0f;         // degrees / s
    int ResolvedBone = -1;         // node index on the model, -1 = not found
};

struct SkeletalAnimationComponent {
    std::string Clip;                // clip name; empty = the model's first clip
    bool PlayAutomatically = true;   // start when Play begins
    int  WrapMode = 1;               // AnimationWrapMode: 0 Once, 1 Loop, 2 PingPong, 3 ClampForever
    float Speed = 1.0f;              // playback rate; negative plays backwards
    float CrossFade = 0.25f;         // seconds to blend when the clip changes
    RootMotionOptions RootMotion;

    // --- runtime (not serialized) ---
    bool Started = false;            // PlayAutomatically has been applied this run
    bool IsPlaying = false;          // set by the system at start; game code may set it
    std::string PlayingClip;         // what the system last asked the model for
};

// #175 Part B - one Animator Controller parameter's live value (Type matches
// AnimatorController::ParamType: 0 Float, 1 Int, 2 Bool, 3 Trigger; Bool/Trigger are 0 or 1).
struct AnimatorParam {
    std::string Name;
    int Type = 0;
    float Value = 0.0f;
};

// One layer's live playback (Animator v2). Stack[0] is the oldest pose still contributing;
// the last entry is the current state. Every entry above the first fades in over the one below
// it, so interrupting a crossfade keeps the pose it had reached instead of popping.
struct AnimatorLayerRuntime {
    struct Item {
        int   State = -1;
        float Phase = 0.0f;        // normalized time in the state: 1 = one pass, >1 counts loops
        float Fade = 1.0f;         // 0..1 blend-in weight over the entries below
        float FadeDuration = 0.0f; // seconds for Fade 0 -> 1 (0 = instant)
        float PrevPhase = -1.0f;   // Phase before the last advance (root motion); -1 = just entered
    };
    std::vector<Item> Stack;
    int  Transition = -1;          // the transition whose crossfade is running, or -1
    bool Interruptible = true;     // false while a non-interruptible transition fades
};

// One transition the controller took, for the Animator window's History tab.
struct AnimatorTransitionLog {
    float Time = 0.0f;       // seconds since the controller started
    int   Layer = 0;
    int   Transition = -1;   // index in the layer's transition list
    std::string From, To;    // state names
    std::string Why;         // the transition's conditions with the parameter values at that moment
};

// #175 Part B - drives this entity's model from an Animator Controller asset (.controller):
// clips (or 1D blend trees) as states on one or more layers, crossfaded transitions on parameter
// conditions. While playing it takes over from an Animation component on the same entity. Game
// code steers it with the setters below and reads back state, tags and fired events.
struct AnimatorControllerComponent {
    std::string Controller;  // project-relative .controller path
    float Speed = 1.0f;      // multiplies every state's speed
    // Which of the controller's tracks this entity plays (empty = the first). A controller can
    // hold several clip sets per state - e.g. "arms" and "weapon" - for rigs animated together.
    std::string Track;
    RootMotionOptions RootMotion; // from the base layer; a state can opt out (State::RootMotion)

    void  SetFloat(const std::string& name, float v)  { Param(name, 0).Value = v; }
    void  SetInt(const std::string& name, int v)      { Param(name, 1).Value = (float)v; }
    void  SetBool(const std::string& name, bool v)    { Param(name, 2).Value = v ? 1.0f : 0.0f; }
    void  SetTrigger(const std::string& name)         { Param(name, 3).Value = 1.0f; }
    void  ResetTrigger(const std::string& name)       { Param(name, 3).Value = 0.0f; }
    float GetFloat(const std::string& name) const {
        for (const auto& p : Params) if (p.Name == name) return p.Value;
        return 0.0f;
    }
    const std::string& CurrentState() const { return StateName; }
    // Base-layer queries, valid after the controller's first update.
    bool InState(const std::string& name) const { return StateName == name; }
    bool HasTag(const std::string& tag) const {
        for (const auto& t : StateTags) if (t == tag) return true;
        return false;
    }
    // Events whose time was crossed during the last update (cleared at the start of each one).
    bool EventFired(const std::string& name) const {
        for (const auto& e : FiredEvents) if (e == name) return true;
        return false;
    }

    // --- runtime (not serialized) ---
    std::vector<AnimatorParam> Params; // seeded from the controller's defaults at start
    bool  Started = false;
    int   State = -1;                  // base layer: current state index
    std::string StateName;             // base layer: current state name
    std::vector<std::string> StateTags;// base layer: current state's tags
    float StateTime = 0.0f;            // base layer: normalized time in the current state
    bool  InTransition = false;        // base layer: a crossfade is still running
    std::vector<std::string> FiredEvents;
    std::vector<AnimatorLayerRuntime> Layers;
    std::vector<AnimatorTransitionLog> History; // newest last, capped
    float Clock = 0.0f;                         // seconds since the controller started
    // Follower mode: when set, this entity mirrors the driver's layers/states/times exactly and
    // evaluates no transitions of its own - the way a weapon rig stays locked to the arms.
    entt::entity Driver = entt::null;
    // Follower only (runtime, not saved): take the driver's finished local pose, matched by node
    // name, instead of sampling every clip again - for modular pieces cut from one skeleton (an
    // NPC's outfit). Ignored when either rig has an IK Rig.
    bool CopyDriverPose = false;
    // Runtime, not saved: skip this entity's update this frame (the enemy squad's animation LOD). The time still
    // counts - the next update advances by all of it - and the pose holds meanwhile.
    bool SkipUpdate = false;
    float SkippedTime = 0.0f;
    // Keep updating while the entity is inactive (hidden rigs that must still leave a state).
    bool UpdateWhenInactive = false;

    // A parameter by name, created (with `type`) if the controller hasn't declared it yet -
    // game code may set values before the first frame.
    AnimatorParam& Param(const std::string& name, int type) {
        for (auto& p : Params) if (p.Name == name) return p;
        Params.push_back({name, type, 0.0f});
        return Params.back();
    }
};

// One two-bone chain of an IK Rig (shoulder-elbow-hand, hip-knee-foot).
struct IKLimb {
    bool Enabled = false;
    std::string Upper, Lower, End; // node names on this entity's model
    std::string Target;            // node the end reaches for
    // Reach for where the end sat relative to Target in the ANIMATED pose, rather than for Target
    // itself: a hand keeps its authored grip while the gun it holds is moved procedurally.
    bool KeepAnimatedOffset = true;
    bool MatchRotation = true;     // also turn the end to the goal's rotation
    float Weight = 1.0f;
    // ---- lane A ----
    // Elbow/knee hint (IK.h TwoBoneHint). HintWeight 0 keeps the animated bend plane, as before.
    std::string PoleBone;          // node whose position the elbow points toward (empty = the animated elbow)
    float HintWeight = 0.0f;       // 0..1
    glm::vec3 HintOffset{0.0f};    // model-space move of the pole point
    float MaxLimbScale = 1.0f;     // longest the limb may stretch toward an out-of-reach target (1 = never)
    float CurveWeight = 1.0f;      // runtime: clip weight curve scale on Weight (game code writes it; 1 = none)
    // The end's grip on the Target, moved in the Target bone's own frame (model units, degrees pitch/yaw/roll);
    // zero = the grip as animated. First-person weapon IK writes the hand-vs-gun offsets here.
    glm::vec3 GripPosition{0.0f};
    glm::vec3 GripRotation{0.0f};
    // ---- end lane A ----
    // Runtime: radians to swing the solved limb about its root->end line (the elbow's "door"),
    // after the solve - the end stays put. Game code writes it (first-person ADS actions do).
    float Swivel = 0.0f;
    // Runtime: a model-space move applied to the end's goal after it is found (identity = none).
    // First-person ADS writes it to keep a hand that has left the gun where the body is.
    glm::mat4 GoalMove{1.0f};
};

// A procedural rigid move of one bone and everything under it, in model space. Runtime only:
// game code writes these each frame (the first-person recoil/sway stack does).
struct IKBoneOffset {
    std::string Bone;
    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Pivot{0.0f};         // rotation centre, relative to the bone's own position
    // When set, the rotation centre is this bone's position (plus Pivot) in the same pose,
    // instead of the offset bone's own.
    std::string PivotBone;
};

// Post-process IK on an Animator Controller's output pose (IK.h): procedural bone offsets, up to
// two two-bone limbs and a look-at, run after the layers blend and before the pose is applied.
struct IKRigComponent {
    bool Enabled = true;
    float Weight = 1.0f;           // master blend, offsets included (0 = the animated pose)
    IKLimb LimbA;
    IKLimb LimbB;
    bool LookAtEnabled = false;
    std::string LookAtBone;
    std::string LookAtTarget;
    glm::vec3 LookAtAxis{0.0f, 0.0f, 1.0f}; // the bone's local axis that should face the target
    float LookAtMaxAngle = 60.0f;
    float LookAtWeight = 1.0f;
    float LookCurveWeight = 1.0f;   // runtime: clip weight curve scale on LookAtWeight (lane A; 1 = none)

    // --- runtime (not serialized) ---
    std::vector<IKBoneOffset> Offsets;
    // Extra local rotations (pre-multiplied onto a bone's animated local rotation, scaled by
    // Weight), applied before everything else - e.g. twist bones the limbs don't solve.
    std::vector<std::pair<std::string, glm::quat>> LocalRotations;
    // A pose the bones are pulled toward first, each by its HoldWeights entry (node order; empty
    // = none) - e.g. the aim pose keeping the gun and right arm on the sights while a reload
    // plays on the left arm. A limb whose hand is held keeps the held pose's grip on its target;
    // one that isn't keeps its animated grip, so it plays its motion relative to the target.
    std::vector<LocalTRS> HoldPose;
    std::vector<float> HoldWeights;
};

struct AnimatorComponent {
    glm::vec3 SpinDegPerSec{0.0f};   // continuous local rotation: angular velocity, degrees/second, about this vector's own direction

    glm::vec3 OrbitAxis{0.0f, 1.0f, 0.0f};
    float OrbitDegPerSec = 0.0f;     // revolve around Base position on this axis
    float OrbitRadius = 0.0f;

    float BobAmplitude = 0.0f;       // vertical sine offset from Base position, world units
    float BobFreqHz = 0.0f;

    float ColorCycleHzPerSec = 0.0f; // LightComponent hue revolutions/second (0 = leave color alone)

    // --- runtime scratch (not serialized) ---
    bool Initialized = false;
    glm::vec3 BasePosition{0.0f};
    glm::quat BaseRotation{1.0f, 0.0f, 0.0f, 0.0f}; // authored orientation, so spin is reversible like orbit/bob (#109)
    glm::vec3 BaseColor{1.0f};
    float Elapsed = 0.0f;
};

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
    std::string ScriptPath = "scripts/TransformController.tescript";
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

// --- Scoring and impact sounds (Sandbox basketball court). Plain data, run by ScoringSystem /
// ImpactSoundSystem in TartarusGame.dll; generic enough for any "ball through a hoop / into a
// goal" game.

// A trigger volume that scores when an object with `Tag` enters it (moving downward, if
// RequireDownward - a ball dropping through a rim, not one pushed up from below). Adds Points
// (ThreePoints when the ball was thrown from ThreePointDistance or further, measured flat from
// where the gravity gun let go of it) to Team on the scene's Scoreboard, bursts every Particle
// System among this object's children, flashes their Lights and plays ScoreSound.
struct GoalTriggerComponent {
    std::string Tag = "Ball";
    int Team = 0;                    // 0 = Home, 1 = Away
    int Points = 2;
    int ThreePoints = 3;
    float ThreePointDistance = 0.0f; // 0 = every basket is worth Points
    bool RequireDownward = true;
    std::string ScoreSound;          // played at the trigger when it scores (optional)
    float FlashIntensity = 40.0f;    // child Lights jump to this, then fade back
};

// The running score. The first one in the scene is the one goals add to; Play -> Stop resets it.
struct ScoreboardComponent {
    int Home = 0;
    int Away = 0;
};

// A seven-segment digit showing one place of a team's score. Its children named "Seg A" .. "Seg G"
// (the standard segment letters: A top, B top right, C bottom right, D bottom, E bottom left,
// F top left, G middle) light up by raising their material's emission.
// Hit points. Rounds that hit an entity with one take Current down (Combat/Damage.h); at 0 it
// is dead. Current is runtime-only: it starts at Max each Play.
struct HealthComponent {
    float Max = 100.0f;
    bool Invulnerable = false;
    // --- runtime (not serialized)
    float Current = -1.0f; // < 0 = not started yet (reads as Max)
};

// Where an enemy soldier appears in Play (Npc/NpcSpawner.h). The enemy squad is built from these.
struct NpcSpawnComponent {
    int Weapon = 0;        // 0 = AKS-74U, 1 = Remington 870, 2 = either, picked at random
    int Squad = 0;         // NPCs with the same Squad fight together
    float Skill = 0.5f;    // 0 (green) .. 1 (veteran): reaction, accuracy, aggression
    int OutfitSeed = 0;    // 0 = a random outfit each Play
    int Brain = 0;         // 0 = the squad AI, 1 = a training dummy (stands still, takes hits)
};

// The fight's rules, on any object in the scene (the first one counts).
struct SquadSettingsComponent {
    int SquadSize = 4;            // NPCs alive at once, refilled from the spawns
    float RespawnDelay = 8.0f;    // seconds before a dead NPC's replacement appears
    float Difficulty = 1.0f;      // scales the NPCs' accuracy and reaction
    float NpcDamageScale = 0.45f; // NPC rounds do this much of the weapon's damage to the player
    bool Respawn = true;          // false: dead NPCs stay dead
    // Combat / AI tunables (Npc/NpcDirector, CoverSystem). Defaults are the values these were hard-coded to.
    float HeavyHitDamage = 40.0f;
    float StaggerTime = 0.4f;
    float BleedOutTime = 20.0f;
    float CrawlSpeed = 0.6f;
    float LimpSpeedScale = 0.6f;
    float LimpTime = 6.0f;
    float CorpseTime = 14.0f;
    float FallGravity = 18.0f;
    float MeleeDamage = 25.0f;
    float MeleeTime = 0.55f;
    float MeleeHitTime = 0.22f;
    float HitboxRange = 60.0f;
    float FootIKRange = 25.0f;
    float MeshCheckRange = 12.0f;
    float CoverSpacing = 0.9f;
    float CoverReach = 0.85f;
    float CoverKneeHeight = 0.85f;
    float CoverHeadHeight = 1.55f;
    float CoverStep = 0.8f;
};

struct ScoreDigitComponent {
    int Team = 0;             // 0 = Home, 1 = Away
    int Place = 0;            // 0 = ones, 1 = tens (blank while the score is under 10)
    float OnStrength = 6.0f;  // emissive strength of a lit segment
    float OffStrength = 0.04f;
};

// Plays a sound where this object hits something, louder the harder the hit (the contact's
// closing speed between MinSpeed and MaxSpeed), with a little random pitch so repeats don't sound
// identical. Both objects in a contact play their own sound (a ball's bounce + a rim's clang).
struct ImpactSoundComponent {
    std::string Clip;
    float Volume = 1.0f;
    float MinSpeed = 0.6f;        // m/s; softer contacts are silent
    float MaxSpeed = 8.0f;        // m/s; full volume from here
    float PitchVariation = 0.08f; // +/- fraction
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

// PR14 (#333): placed reflection probe. Position comes from TransformComponent; this component
// adds the box volume for parallax correction and a blend priority. The bake captures the sky
// IBL (procedural or HDRI) at this probe's position; the shader corrects reflection vectors
// to the box boundary, giving room-appropriate reflections without per-probe scene captures.
// No probes in a scene → the shader falls back to the global sky IBL path bit-identically.
struct ReflectionProbeComponent {
    glm::vec3 Size{5.0f, 5.0f, 5.0f}; // full extents of the capture volume, in world units
    float Importance{1.0f};            // higher wins 2-probe selection tie-breaks
};

// ---- lane P ----
// Scene-level visual effects and HUD settings (Lane P quality pass).
// Add one to the scene to tune muzzle flash, laser beam and HUD display parameters.
// Defaults match the values these effects had before they were tunable; read once when Play starts.
struct FxHudSettingsComponent {
    // Muzzle flash parameters (Combat/CombatFx.cpp)
    float FlashTime = 0.055f;               // seconds the muzzle flash light stays on; Muzzle Flash group
    float PlayerFlashScale = 0.35f;         // player's flash light scale relative to soldier's; Muzzle Flash group
    float FlameGlow = 150.0f;               // flame peak emission intensity (red channel); Muzzle Flash group
    float FlameScale = 1.75f;               // flame tongue length/width scale vs. tactical shooter pack; Muzzle Flash group

    // Laser beam parameters (src/Renderer/WeaponFxRenderer.cpp)
    float BeamRange = 150.0f;               // metres drawn; past that it's gone in the haze; Laser Beam group
    float BeamHalfWidth = 0.0015f;          // beam width in metres (3 mm); Laser Beam group
    float BeamFalloff = 2.5f;               // glow falloff distance in metres near the emitter; Laser Beam group
    float BeamBend = 4.0f;                  // metres over which a view-model emitter eases onto the true path; Laser Beam group

    // HUD display parameters (Combat/CombatHud.cpp)
    float FeedLife = 4.5f;                  // seconds a kill feed line stays on screen; HUD group
    float StreakWindow = 4.0f;               // seconds to count consecutive kills for streak display; HUD group
    float SubLinger = 1.1f;                 // seconds a subtitle lingers after its clip ends; HUD group
};
// ---- end lane P ----
// ---- lane R ----
// A soldier's ragdoll (Npc/NpcRagdoll): masses, joint ranges of motion, drives, body physics, impulse caps. On any object in
// the scene (the first one counts); without one the NPCs use these same defaults. Hitbox shapes are not exposed (they change gameplay).
// Joint limits are degrees from each joint's neutral pose (arm and leg hanging, spine and head upright).
struct RagdollSettingsComponent {
    bool AnatomicalLimits = true;
    float DriveStiffness = 700.0f;
    float DriveDamping = 60.0f;
    float DriveFade = 0.25f;
    float LinearDamping = 0.08f;
    float AngularDamping = 0.25f;
    int SolverPosIters = 16;
    int SolverVelIters = 4;
    float Depenetration = 3.0f;
    float SleepThreshold = 0.08f;
    float StaticFriction = 0.8f;
    float DynamicFriction = 0.7f;
    float Restitution = 0.05f;
    float PartImpulseSpeed = 6.0f;
    float ChestImpulseSpeed = 5.0f;
    float CorpseShotBase = 1.5f;
    float CorpseShotPerDamage = 0.04f;
    float PelvisMass = 15.0f;
    float SpineMass = 19.0f;
    float SpineFlexMax = 45.0f;
    float SpineExtMax = 20.0f;
    float SpineLatIn = 25.0f;
    float SpineLatOut = 25.0f;
    float SpineTwistIn = 30.0f;
    float SpineTwistOut = 30.0f;
    float HeadMass = 4.5f;
    float HeadFlexMax = 25.0f;
    float HeadExtMax = 30.0f;
    float HeadLatIn = 20.0f;
    float HeadLatOut = 20.0f;
    float HeadTwistIn = 35.0f;
    float HeadTwistOut = 35.0f;
    float UpperArmMass = 2.5f;
    float UpperArmFlexMax = 170.0f;
    float UpperArmExtMax = 60.0f;
    float UpperArmLatIn = 40.0f;
    float UpperArmLatOut = 150.0f;
    float UpperArmTwistIn = 70.0f;
    float UpperArmTwistOut = 80.0f;
    float ForearmMass = 1.6f;
    float ForearmFlexMax = 145.0f;
    float ForearmExtMax = 0.0f;
    float ForearmLatIn = 3.0f;
    float ForearmLatOut = 3.0f;
    float ForearmTwistIn = 10.0f;
    float ForearmTwistOut = 10.0f;
    float ThighMass = 8.0f;
    float ThighFlexMax = 120.0f;
    float ThighExtMax = 20.0f;
    float ThighLatIn = 30.0f;
    float ThighLatOut = 45.0f;
    float ThighTwistIn = 40.0f;
    float ThighTwistOut = 45.0f;
    float CalfMass = 4.0f;
    float CalfFlexMax = 140.0f;
    float CalfExtMax = 0.0f;
    float CalfLatIn = 3.0f;
    float CalfLatOut = 3.0f;
    float CalfTwistIn = 5.0f;
    float CalfTwistOut = 5.0f;
    // Death momentum: each part starts with its own bone's velocity (from the last two animated poses), not just the body's.
    float LimbVelocityScale = 1.0f;
    float MaxLimbSpeed = 8.0f;
    float MaxLimbSpin = 30.0f;
    // Shaped inertia: the torso parts turn like a box wider than deep rather than a round capsule.
    bool ShapedTorsoInertia = true;
    float TorsoHalfWidth = 0.18f;
    float TorsoHalfDepth = 0.11f;
    float InertiaScale = 1.0f;
    // The drive fade per region: DriveFade times this (1 = all together).
    float PelvisFadeScale = 1.0f;
    float SpineFadeScale = 1.0f;
    float HeadFadeScale = 1.0f;
    float UpperArmFadeScale = 1.0f;
    float ForearmFadeScale = 1.0f;
    float ThighFadeScale = 1.0f;
    float CalfFadeScale = 1.0f;
    // The neck, hands and feet are ragdoll-only parts (the hitboxes stay the eleven): the head's range above is the head on the neck,
    // the neck's the neck on the chest (together the cervical range). Hand: flexion = palm side, lateral = radial / ulnar deviation.
    // Foot: flexion = dorsiflexion (toes up), extension = plantarflexion, lateral = toes in / out, twist = inversion / eversion.
    float NeckMass = 1.0f;
    float NeckFlexMax = 30.0f;
    float NeckExtMax = 35.0f;
    float NeckLatIn = 25.0f;
    float NeckLatOut = 25.0f;
    float NeckTwistIn = 40.0f;
    float NeckTwistOut = 40.0f;
    float HandMass = 0.5f;
    float HandFlexMax = 75.0f;
    float HandExtMax = 70.0f;
    float HandLatIn = 25.0f;
    float HandLatOut = 25.0f;
    float HandTwistIn = 15.0f;
    float HandTwistOut = 15.0f;
    float FootMass = 1.0f;
    float FootFlexMax = 20.0f;
    float FootExtMax = 50.0f;
    float FootLatIn = 15.0f;
    float FootLatOut = 15.0f;
    float FootTwistIn = 35.0f;
    float FootTwistOut = 15.0f;
    float NeckFadeScale = 1.0f;
    float HandFadeScale = 1.0f;
    float FootFadeScale = 1.0f;
    // Hands and feet are light end links that whip the limb above through their joint limit: their rotational inertia is multiplied by
    // Distal Inertia Scale (on top of Inertia Scale) and their joint carries a viscous Distal Joint Damping that stays on after the drives fade.
    float DistalInertiaScale = 6.0f;
    float DistalJointDamping = 40.0f;
    // Hit flinch: a round that doesn't kill kicks the struck region's bones (a damped spring, on top of the hit animation), scaled by
    // the damage, and they settle back within FlinchDuration. Visual only: aim, eye and hitboxes don't move.
    bool HitFlinch = true;
    float FlinchAngle = 7.0f;
    float FlinchDuration = 0.3f;
    float FlinchDamageRef = 40.0f;
    float FlinchMaxAngle = 28.0f;
    // Powered ragdoll (Euphoria-style "physical animation"): after death every joint keeps a spring toward a target pose (the pose it
    // died in, blending into a procedural collapse), its strength decaying per region to a small residual tone; the struck region gives way
    // first. Off: the old behaviour (the drives fade on the Death Drive clocks and the body is a rag).
    bool PoweredRagdoll = true;
    // Muscle Tone
    float ToneStiffness = 450.0f;      // joint spring at full strength (acceleration units)
    float ToneDamping = 50.0f;         // its damper at full strength
    float ToneResidual = 0.06f;        // the fraction of strength a dead body keeps (never zero)
    float JointFriction = 4.0f;        // damper floor on every joint: dead bodies have joint friction
    float LegsToneTime = 0.6f;         // seconds for each region's strength to decay (after the stagger, for the legs)
    float SpineToneTime = 1.1f;
    float NeckToneTime = 1.4f;
    float ArmsToneTime = 1.4f;
    float CollapseBlendTime = 0.55f;   // seconds the target pose takes to go from the death pose to the collapse pose
    float CollapseAmount = 1.0f;       // 0 = hold the death pose, 1 = the full collapse pose below
    float HipFlexCollapse = 35.0f;     // degrees, in the collapse pose
    float KneeFlexCollapse = 70.0f;
    float SpineCurlCollapse = 18.0f;
    float NeckCollapse = 20.0f;
    float ShoulderCollapse = 35.0f;
    float ElbowCollapse = 50.0f;
    // Stagger
    float StaggerLegStrength = 0.7f;   // the legs start at this fraction of the strength (they give out, they don't hold a pose)
    float StaggerTime = 0.35f;         // seconds the legs hold it before they decay
    float HitWeakness = 0.25f;         // the struck joint's strength (fraction): it takes the round instead of holding against it
    float HitImpulseScale = 2.2f;      // multiplies the round's shove (the body goes along the shot)
    float HitBodyShare = 0.8f;         // the share of the shove that pushes the whole body (by mass) instead of only the struck part
    // Settle
    float DownHeight = 0.5f;           // the body counts as down once its pelvis is below this fraction of its standing height: it settles whatever its speed
    float SettleSpeed = 0.5f;          // m/s: below this (after the delay) the body starts to settle
    float SettleDelay = 0.5f;          // seconds after death before it can
    float SettleRamp = 0.2f;           // seconds to reach the full settled values
    float SettleLinearDamping = 6.0f;
    float SettleAngularDamping = 8.0f;
    float SettleFriction = 2.5f;       // static and dynamic, against the world
    float SettleJointFriction = 30.0f; // the joint damper floor, settled
    float RestSpeed = 0.01f;           // m/s and rad/s x 10: below this for Rest Time the body is put to sleep
    float RestTime = 0.3f;
    float StabilizationThreshold = 0.05f; // PhysX stabilization: contact jitter below this energy per mass is held still
    bool GripFloor = true;             // the body's friction wins against a slick floor and it never bounces
};
// ---- end lane R ----
// ---- lane R-gear ----
// The gun a soldier drops when he dies (Npc/NpcDroppedWeapon): its own simulated body, tumbling to rest. On any object in the scene
// (the first one counts); without one the NPCs use these same defaults.
struct DroppedWeaponSettingsComponent {
    bool Enabled = true;               // the gun leaves the hands and lies where it falls (off: it vanishes with the soldier, as before)
    float Mass = 3.5f;                 // kg
    float ImpulseShare = 0.15f;        // the share of the killing round's impulse the gun takes (velocity change = share x impulse / mass)
    float MaxShotSpeed = 5.0f;         // m/s: the most speed the round can add
    float Spin = 0.5f;                 // tumble: rad/s of spin per m/s of speed leaving the hands (0 = none)
    float MaxSpin = 8.0f;              // rad/s
    float CollisionDelay = 0.1f;       // seconds the gun flies without colliding, clear of the falling body's arms and torso
    float Friction = 0.7f;
    float Bounciness = 0.1f;           // restitution
    float LinearDamping = 0.1f;
    float AngularDamping = 0.6f;
    float Lifetime = 0.0f;             // seconds before the gun goes (0 = it lies as long as the corpse does)
};
// ---- end lane R-gear ----
