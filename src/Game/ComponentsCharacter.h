#pragma once
// The player's body and first-person controller, and character outfits.
// Part of the component set; Components.h includes every part.

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>
#include "IK.h"        // IK::SpineDistribution
#include <algorithm>

class SkinHideBuffer; // OutfitHideTag, PlayerBodyTag
class VisibleIndexBuffer; // OutfitHideTag
class Model;

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

// #165 - the built-in first-person player, as a component. In Play the player spawns at this
// entity's position (feet) facing its forward (-Z), with these settings. Without one, Play
// renders through the scene's Camera (a fixed/animated shot, no player); with neither, it falls
// back to the old behaviour (a default player dropped in at the editor camera).
struct FirstPersonControllerComponent {
    std::string PrimaryWeaponPrefab;
    std::string SecondaryWeaponPrefab;
    std::string SidearmWeaponPrefab;
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
    // Clipping a doorway's jamb while walking mostly into it eases the capsule sideways past it (0..1 of its radius;
    // 0 = off), when the jamb is met within this many degrees of head on.
    float DoorwayAssist = 0.5f;
    float DoorwayAssistMaxAngle = 35.0f;
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
    // More weapons (.fpsanim) for the same arms: key 1 draws AnimationSet, 2 the secondary, 3 the
    // sidearm, 4 goes unarmed. Switching holsters the one in hand, then draws the other; each keeps its ammo.
    std::string SecondaryAnimationSet;
    std::string SidearmAnimationSet;
    // The rig bone the play camera sits on. Placement parks this bone's world position exactly on
    // the camera, so ViewModelOffset below is only a residual nudge - which matters because these
    // rigs are authored standing in their own scene (feet at y=0, head near y=1.56): without a
    // bone anchor the model's root goes where the camera is and the whole rig floats ~1 m above
    // the view. Leave empty to fall back to positioning the model's root directly on the camera.
    std::string CameraBone = "head";
    glm::vec3 ViewModelOffset{};
    glm::vec3 ViewModelRotation{};
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
// Animator Controller drives; every other piece follows it. The root may itself be that piece.
// In Play the body stands at the player's feet facing the view. Motion matching (MotionMatching/)
// poses the driver from the player's movement each step - the controller's own update is held -
// and the input moves the capsule at the matcher's gait speeds (capsule-led: the clips never move
// it). The play camera sits in the body's head.
struct FirstPersonBodyComponent {
    // Motion matching A/B switches (for judging the pose in Play):
    // MmPostLayers off draws the matcher's pose with foot IK only - no Spine Aim / Twist, arm hang or hands-from-eye.
    bool MmPostLayers = true;
    // Motion matching, unarmed: how much of the mocap head's own motion the eye rides (0 = its slow average, within Eye Slack -
    // the stabilized camera; 1 = the head as captured: the jog's bob and sway). Armed, the eye stays the rig's.
    float MmHeadRide = 1.0f;
    float WalkSpeed = 1.53f;       // m/s asked of the blend tree when walking - the default gait (the walk clips' speed)
    float RunSpeed = 3.26f;        // ... and with Shift held: the jog (the jog clips' speed)
    float SprintSpeed = 4.72f;     // ... and while sprinting (the run clip's)
    // The capsule's own run speed when it differs from the clips' (0 = the clips' Run Speed).
    float PlayerRunSpeed = 0.0f;
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
    // A modular body's hands, when they are a piece of their own (a long sleeve hides the arms and leaves
    // the hands): treated as part of the arms - drawn with them, hidden with them.
    std::string HandsPiece = "Hands";
    // A body with no flesh on its hands (the Realistic Skeleton): the rig's grip is tuned for a skinned hand, whose palm
    // and finger pads sit a centimetre or two out from the bones, so a bare-bone hand in the same pose floats off the
    // gun. Grip Inset moves each hand that far (metres) toward its palm; Finger Curl Extra closes the fingers by that
    // many more degrees per joint. Both 0 for a skinned body. Copied to NPCs at Play like the other hold settings.
    float GripInset = 0.0f;
    float FingerCurlExtra = 0.0f;
    // How much of the camera's pitch the spine takes (0 = the body stays upright, 1 = the chest
    // tilts as far as the view), so the shoulders follow the view and the hands stay in reach.
    float SpineAim = 0.0f;
    // How the spine's turns (view pitch, twist, shoulder line) are shared over spine_01..05, pelvis alpha:
    // the default is the even spread. Used by the player body and, copied at Play, by NPCs.
    IK::SpineDistribution Spine;
    // The same looking down. Armed, the eye hangs off the shoulders as the arms rig's does, so looking
    // down it comes over the chest only as far as the chest pitches with it.
    float SpineAimDown = 0.9f;
    // Unarmed, the most (degrees) the spine's aim pitches the chest looking up / down, eased in (0 = no limit). Past it the
    // view pitches at the neck alone: unlimited, a look straight up arched the body back like a limbo and a look straight
    // down folded it over (the owner's play test 2, Phase 4). Armed, the arms rig needs the full aim to reach the gun.
    // (Defaults: the values tuned on AnimTestbed - a scene that didn't save them got no cap, the fold back.)
    float SpineAimMaxUp = 20.0f;
    float SpineAimMaxDown = 25.0f;
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
    float TurnEndAngle = 8.0f; // Turn End Angle
    // Foot lock (IK::FootLock): a planted foot held in the world; the player body and the soldiers
    bool FootLockEnabled = true; // Foot Lock
    float FootLockHeight = 0.02f; // Foot Lock Height (m over the foot's lowest)
    float FootLockLift = 0.045f; // Foot Lock Lift (m over the foot's lowest: lets go)
    float FootLockSpeed = 2.0f; // Foot Lock Speed (m/s the animated foot may move over the ground, the body's turn left out, and still plant)
    float FootLockDrift = 0.25f; // Foot Lock Drift (m the animation may wander before the foot lets go: a safety, the leg's reach goes first)
    float FootLockRelease = 0.2f; // Foot Lock Release (s a let-go foot takes to catch up)
    float FootRayUp = 0.5f; // Foot Ray Up
    float FootRayLength = 1.0f; // Foot Ray Length
    float FootMaxRaise = 0.25f; // Foot Max Raise
    float PelvisMaxRaise = 0.0f; // Pelvis Max Raise
    float FootOffsetEase = 0.06f; // Foot Offset Ease
    float FootNormalEase = 0.08f; // Foot Normal Ease
    float FootIKFade = 0.1f; // Foot IK Fade
    float FootTiltMax = 25.0f; // Foot Tilt Max
    float StairEase = 0.2f; // Stair Ease
    // --- Arm IK (Player body: elbow tracking and clearance) ---
    float ElbowEase = 0.06f; // Elbow Ease
    float ElbowMaxRate = 540.0f; // Elbow Max Rate (degrees/s)
    // --- NPC body tuning (copied to NPC soldiers from the scene's player body) ---
    // NPC body heading and turns
    float NpcTurnThreshold = glm::degrees(1.15f); // NPC Turn Threshold (degrees; ~66 deg body can lag before turning on the spot)
    float NpcFaceEase = 0.09f; // NPC Face Ease (seconds; heading easing while moving)
    // NPC spine and body aim
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
    bool BareBody = false;           // debug: in Play the character drops its clothing (its own body parts only)
    int Version = 0;                 // runtime: bumped on every change (FirstPersonBody re-reads its pieces)
    std::uint64_t HideSignature = 0; // runtime: the pieces the hiding was last worked out for
    int LinkedVersion = -1;          // runtime: the Version whose pieces were last linked to the driving animator
    std::uint64_t HideWaiting = 0;   // runtime: the pieces the hiding is waiting on coverage for ...
    std::uint64_t HideRetryFrame = 0;// ... and when UpdateHiding looks again
    int BodyChecked = -1;            // runtime: the wardrobe generation its body parts were last checked against
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
