#pragma once
// Colliders, rigidbodies, joints and the ragdoll settings.
// Part of the component set; Components.h includes every part.

#include <string>
#include <vector>
#include <glm/glm.hpp>

// Presence of this component means the entity participates in collision/raycasting.
//
// PhysX path (#185, active only while playing): on Play-enter a static PhysX actor is built
// per collider. `Shape` picks box/sphere/capsule; `HalfExtents` gives its dimensions and
// `Center` its offset from the entity origin, both in the entity's local space. `HalfExtents
// == 0` (the default, and every collider World::CreateBox makes) means "derive an axis-aligned
// box from the mesh bounds", so existing scenes keep their shape without a migration.
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
    int SolverPosIters = 32;
    int SolverVelIters = 8;
    float Depenetration = 3.0f;
    float SleepThreshold = 0.08f;
    float StaticFriction = 0.8f;
    float DynamicFriction = 0.7f;
    float Restitution = 0.05f;
    float PartImpulseSpeed = 10.0f;
    float ChestImpulseSpeed = 8.0f;
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
    float HandTwistIn = 35.0f;
    float HandTwistOut = 35.0f;
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
    float DistalInertiaScale = 1.5f;
    float DistalJointDamping = 6.0f;
    // Weight: gravity on a dead body as a multiple of the scene's (a body reads light at 1 at game scale), the flesh over the capsules
    // (contacts rest this far out, so the skin lies on the floor rather than sunk into it), and whether the parts collide with each
    // other (an arm can't pass through the chest, a leg through the other).
    float GravityScale = 1.35f;
    float SkinOffset = 0.015f;
    bool SelfCollision = true;
    // Joint inertia (armature), as a fraction of what each joint turns: a limb has almost none about its own length, so a floor's or a
    // crate's friction spins it through its twist limit. Twist Armature on the twist axis, Swing Armature on the others.
    float TwistArmature = 0.5f;
    float SwingArmature = 0.05f;
    // The hands' friction, times the body's: a hand slides off an edge (a stair, a ledge, a crate) instead of hooking on it and
    // being levered past its range by the body hanging from it.
    float HandFriction = 0.35f;
    // The fingers let go of the grip they died in over Finger Relax Time seconds, to their bind pose with Finger Curl of the grip
    // kept (a dead hand's loose curl).
    float FingerRelaxTime = 0.35f;
    float FingerCurl = 0.3f;
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
    float ToneStiffness = 300.0f;      // joint spring at full strength (acceleration units)
    float ToneDamping = 30.0f;         // its damper at full strength
    float ToneResidual = 0.03f;        // the fraction of strength a dead body keeps (never zero)
    float JointFriction = 2.0f;        // damper floor on every joint: dead bodies have joint friction
    float LegsToneTime = 0.35f;        // seconds for each region's strength to decay (after the stagger, for the legs)
    float SpineToneTime = 0.6f;
    float NeckToneTime = 0.8f;
    float ArmsToneTime = 0.9f;
    float CollapseBlendTime = 0.35f;   // seconds the target pose takes to go from the death pose to the collapse pose
    float CollapseAmount = 1.0f;       // 0 = hold the death pose, 1 = the full collapse pose below
    float HipFlexCollapse = 35.0f;     // degrees, in the collapse pose
    float KneeFlexCollapse = 70.0f;
    float SpineCurlCollapse = 18.0f;
    float NeckCollapse = 20.0f;
    float ShoulderCollapse = 35.0f;
    float ElbowCollapse = 50.0f;
    // Stagger
    float StaggerLegStrength = 0.55f;  // the legs start at this fraction of the strength (they give out, they don't hold a pose)
    float StaggerTime = 0.15f;         // seconds the legs hold it before they decay
    float HitWeakness = 0.25f;         // the struck joint's strength (fraction): it takes the round instead of holding against it
    float HitImpulseScale = 2.2f;      // multiplies the round's shove (the body goes along the shot)
    float HitBodyShare = 0.35f;        // the share of the shove that pushes the whole body (by mass) instead of only the struck part
    // Settle
    float ImpactSettleDelay = 0.35f;   // seconds after the pelvis or chest first meets the ground before the body may settle: the limbs slump and roll on first
    float SettleSpeed = 0.5f;          // m/s: below this (after the delay) the body starts to settle
    float SettleDelay = 0.5f;          // seconds after death before it can
    float SettleRamp = 0.5f;           // seconds to reach the full settled values
    float SettleLinearDamping = 2.5f;
    float SettleAngularDamping = 3.5f;
    float SettleFriction = 2.0f;       // static and dynamic, against the world
    float SettleJointFriction = 12.0f; // the joint damper floor, settled
    float RestSpeed = 0.01f;           // m/s and rad/s x 10: below this for Rest Time the body is put to sleep
    float RestTime = 0.3f;
    float CreepSpeed = 0.2f;           // m/s and rad/s x 20: settled and no faster than this for Rest Timeout seconds, the body is put to sleep
    float RestTimeout = 1.0f;          // anyway (a gun or another body on it, a hand caught at two limits, keep it inching); 0 = off
    float StabilizationThreshold = 0.05f; // PhysX stabilization: contact jitter below this energy per mass is held still
    bool GripFloor = true;             // the body's friction wins against a slick floor and it never bounces
    // Lying: after the collapse the drive target keeps going, to a relaxed lying pose (legs extend, hips and arms relax), held by a modest
    // tone (RelaxTone) for a while, so a body does not end frozen kneeling or curled up. RelaxTone 0 = no lying phase (the target stays at the collapse pose).
    float RelaxDelay = 0.3f;           // seconds after death before the target starts toward the lying pose (about when the collapse has formed)
    float RelaxLoose = 0.3f;           // the most the settle (damping, friction, joint friction) may reach until the lying pose has formed: lower lets the limbs move on the floor
    float RelaxTime = 0.6f;            // seconds the target takes to go from the collapse pose to the lying pose (after the delay)
    float RelaxTone = 0.2f;            // the strength fraction that holds the lying pose while it forms
    float RelaxHold = 0.5f;            // seconds the lying tone holds once the target has arrived, then it fades out
    float RestPelvisMax = 0.22f;       // a body at rest whose pelvis is higher than this (m) is not lying (kneeling, sat up), whose knees are bent more than RestKneeMax or hips more than RestHipMax (degrees) is curled up:
    float RestChestMax = 0.3f;         // (or whose chest is higher than this: propped up on its arms or legs)
    float RestKneeMax = 55.0f;         // it is not put to sleep: its legs push toward the lying pose and the floor lets go of it, for up to RestFixTime seconds (propped against something, it is left as it is after that)
    float RestHipMax = 80.0f;
    float RestFixTime = 1.5f;
    float RestFixTone = 0.6f;          // the legs' strength fraction while they push
    float LyingHip = 8.0f;             // degrees, the lying pose's hip, knee and elbow flexion
    float LyingKnee = 10.0f;
    float LyingElbow = 25.0f;
    float LyingArms = 0.8f;            // how far the arms go back to hanging (0 = they keep the death pose, 1 = straight down)
    float LyingSpine = 0.6f;           // how far the spine, neck and head go back to upright
    // Directional falls: the leg on the side the body falls toward buckles first and folds further; a back fall folds the knees less and sits back.
    float BuckleAsymmetry = 0.5f;      // 0 = both knees fold alike, 1 = the lead leg folds 2x and the other not at all (at a pure sideways fall)
    float BuckleLead = 0.5f;           // the lead leg's stagger hold is shortened by this fraction (the other's lengthened)
    float BackKneeScale = 0.45f;       // knee fold when falling straight back, as a fraction of Knee Flexion
    float BackHipScale = 1.5f;         // hip fold when falling straight back (the legs go forward, the body sits down), as a multiple of Hip Flexion
    // Reactions: layers on the drive target while the body is still conscious (a head kill has none and goes limp at once).
    float BraceWeight = 1.0f;          // 0..1: arms reach toward the fall (the Shoulder Brace and Brace Sideways below, scaled by this)
    float BraceLateral = 30.0f;        // degrees the arms swing toward a sideways fall
    float HeadTuckWeight = 1.0f;       // 0..1: the head tucks forward as the body falls back (Neck Tuck degrees, scaled)
    float HeadKillLimp = 0.4f;         // a head kill: the arms' and neck's tone times are scaled by this (0.4 = they let go in under half the time)
    // Corpse hits: a round into a body at rest.
    float CorpseWakeTime = 0.45f;      // seconds the body stays loose (no settling) after a round, so the limb the round struck visibly reacts
    float CorpseShotMaxSpeed = 4.0f;   // m/s: the most a round adds to the struck part, whatever the damage
};
