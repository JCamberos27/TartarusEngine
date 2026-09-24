#pragma once

#include "Curve.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <json.hpp>

#include <random>
#include <string>
#include <vector>

// Procedural first-person weapon animation, in the spirit of Kinemation's CAS: the animated
// pose plays as authored, and on top of it the GUN BONE is moved by a stack of procedural
// layers - recoil, sway, bob, breathing, an ADS aim offset, per-state pose offsets and lean -
// while two-bone IK keeps both hands on the gun (IK.h, IKRigComponent).
//
// Everything here is in CAMERA space: +X right, +Y up, +Z back toward the eye (the camera looks
// down -Z). Positions are metres, rotations are degrees as (pitch about X, yaw about Y, roll
// about Z); +pitch lifts the muzzle. FirstPersonPresentation converts the result to the arms
// rig's model space. Every number lives in the weapon definition (.fpsanim, "procedural").

// Default-shape builders, over 0..1 (also used to migrate pre-curve .fpsanim numbers).
// A recoil shot: a straight rise to `amplitude` at `peak`, then an exponential settle with time
// constant `tau`, ~0 by 1.
Curve FirstPersonKickCurve(float amplitude, float peak = 0.09f, float tau = 0.2f);
// amplitude * sin(2 pi cycles p + phase).
Curve FirstPersonSineCurve(float amplitude, float cycles = 1.0f, float phase = 0.0f);

struct FirstPersonCurve3 {
    Curve X, Y, Z;
    glm::vec3 Evaluate(float t) const { return {X.Evaluate(t), Y.Evaluate(t), Z.Evaluate(t)}; }
};

// Critically- (or under-) damped spring toward a moving target; the smoothing every layer uses.
struct FirstPersonSpring {
    float Frequency = 12.0f; // Hz: how fast it follows
    float Damping = 1.0f;    // 1 = no overshoot, < 1 = springy
};

struct WeaponRecoilSettings {
    bool Enabled = true;
    // Seconds one shot's curves span; they are keyed over 0..1 of it.
    float Duration = 0.4f;
    // Per-shot shape. Rotation curves are degrees, position curves metres. Kickback is +Z (into
    // the shoulder).
    FirstPersonCurve3 Rotation; // pitch, yaw, roll
    FirstPersonCurve3 Position; // side, up, kickback
    // Each shot scales its curves by a random pick in [min, max] per channel, so no two shots
    // match. A negative min lets yaw/roll/side kick either way.
    glm::vec2 PitchRange{0.85f, 1.15f};
    glm::vec2 YawRange{-1.0f, 1.0f};
    glm::vec2 RollRange{-1.0f, 1.0f};
    glm::vec2 SideRange{-1.0f, 1.0f};
    glm::vec2 UpRange{0.9f, 1.1f};
    glm::vec2 KickRange{0.9f, 1.1f};
    float HipScale = 0.6f;    // hip fire also plays the Fire clip, so the kick is smaller
    float AdsScale = 1.0f;
    glm::vec3 Pivot{0.0f, 0.0f, 0.0f}; // rotation centre, camera frame, relative to the gun bone
    FirstPersonSpring Smoothing{22.0f, 0.7f};
    // Camera recoil: a view punch (degrees) that recovers on its own. Doesn't move where the
    // player aims, only what they see.
    Curve CameraPitch;
    Curve CameraYaw;
    glm::vec2 CameraYawRange{-1.0f, 1.0f};
    float CameraScale = 1.0f;
    // Smooths the punch (rounds' curves summed) so full auto reads as one rolling push instead
    // of a jolt per round. Frequency 0 = unsmoothed (the older behaviour).
    FirstPersonSpring CameraSmoothing{0.0f, 1.0f};
    // Aim climb: each round moves where the player actually aims (degrees, a random pick in
    // [min, max] scaled like the kick), unlike the camera punch above, which only moves what they
    // see. Once the trigger has rested AimRecoveryDelay seconds, AimRecovery (0..1) of the climb
    // eases back at AimRecoverySpeed degrees/second; the rest is the player's to pull down.
    glm::vec2 AimPitch{0.0f};
    glm::vec2 AimYaw{0.0f};
    float AimRecovery = 0.0f;
    float AimRecoveryDelay = 0.12f;
    float AimRecoverySpeed = 8.0f;
    // Hip fire kicks procedurally, like ADS, instead of restarting the controller's Fire state
    // every round. Off plays the Fire clip (the older behaviour).
    bool HipProcedural = false;
    // Seconds the bolt takes to slam back and return each round (the weapon rig's BoltBone,
    // along the travel its Fire clip authors). 0 = no procedural bolt.
    float BoltCycle = 0.0f;
    std::string BoltBone = "bolt";
};

struct WeaponSwaySettings {
    bool Enabled = true;
    // Look sway: the gun lags behind the view. Per 100 degrees/second of turn.
    float LookRotation = 1.2f;     // degrees
    float LookPosition = 0.004f;   // metres
    float MaxRotation = 4.0f;      // degrees
    float MaxPosition = 0.02f;     // metres
    // Move sway: the gun trails against movement and tilts into strafes. Per m/s.
    float MovePosition = 0.0015f;  // metres
    float MoveRoll = 0.6f;         // degrees
    float AdsScale = 0.25f;
    FirstPersonSpring Spring{6.0f, 0.55f};
};

struct WeaponBobSettings {
    bool Enabled = true;
    // One cycle = one stride (two steps). Curves keyed over 0..1 of a cycle: side, up (metres),
    // roll (degrees).
    float WalkStride = 2.4f;       // metres per cycle
    float SprintStride = 3.4f;
    FirstPersonCurve3 Walk;        // X side, Y up, Z roll
    FirstPersonCurve3 Sprint;
    float WalkFullSpeed = 3.5f;    // m/s at full amplitude
    float HipScale = 1.0f;
    float AdsScale = 1.0f;
    float Ease = 8.0f;             // 1/s fade in and out
};

struct WeaponBreathSettings {
    bool Enabled = true;
    float Period = 4.0f;           // seconds per breath
    FirstPersonCurve3 Position;    // metres, over 0..1 of a breath
    Curve Pitch;                   // degrees
    float HipScale = 1.0f;
    float AdsScale = 0.35f;
};

struct WeaponAimSettings {
    // Added to the gun while sights are up (states tagged ADS), on top of the Aim clip. Zero
    // leaves the authored sight picture exactly as it is.
    glm::vec3 Position{0.0f};
    glm::vec3 Rotation{0.0f};
    float BlendTime = 0.18f;       // seconds in and out
    Curve Blend;                   // 0..1 easing over the blend
};

// A pose tweak while a state (or any state with a tag) is playing - e.g. lower the gun and roll
// it in Sprint, or pull it in closer while walking.
struct WeaponStateOffset {
    std::string Match;             // state name or tag
    glm::vec3 Position{0.0f};
    glm::vec3 Rotation{0.0f};
    float BlendIn = 0.2f;
    float BlendOut = 0.25f;
};

// Locomotion clips play at a rate that follows the player's actual speed (the controller's Walk
// and Sprint states use the WalkRate / SprintRate parameters as their speed): rate 1 at the
// reference speed, slower below it.
struct WeaponLocomotionSettings {
    bool MatchSpeed = true;
    // m/s at which each clip plays at rate 1. 0 = the player's own full speed (the controller's
    // Move Speed, and Move Speed x Sprint Multiplier), so any scene's tuning plays at 1x.
    float WalkReference = 0.0f;
    float SprintReference = 0.0f;
    float MinRate = 0.6f;
    float MaxRate = 1.4f;
};

struct WeaponLeanSettings {
    bool Enabled = true;
    float Angle = 12.0f;           // camera roll at full lean, degrees
    float Offset = 0.28f;          // camera side shift at full lean, metres
    float WeaponRoll = 6.0f;       // extra gun roll into the lean, degrees
    float Speed = 6.0f;            // 1/s
    bool WhileSprinting = false;   // off: sprinting straightens up
};

struct WeaponIKSettings {
    bool Enabled = true;
    std::string GunBone = "ik_hand_gun";
    std::string RightUpper = "upperarm_r", RightLower = "lowerarm_r", RightHand = "hand_r";
    std::string LeftUpper = "upperarm_l", LeftLower = "lowerarm_l", LeftHand = "hand_l";
    std::string OffTag = "IKOff";  // states with this tag play purely as authored
    // Off, or when the arms rig lacks any of the bones above: the procedural motion moves the
    // whole view model about the eye instead of the gun bone.
    float BlendTime = 0.12f;
};

struct WeaponProceduralSettings {
    WeaponRecoilSettings Recoil;
    WeaponSwaySettings Sway;
    WeaponBobSettings Bob;
    WeaponBreathSettings Breath;
    WeaponAimSettings Aim;
    std::vector<WeaponStateOffset> StateOffsets;
    WeaponLocomotionSettings Locomotion;
    WeaponLeanSettings Lean;
    WeaponIKSettings IK;

    // The AKS-74U tuning: every curve filled in.
    static WeaponProceduralSettings Defaults();
    nlohmann::json ToJson() const;
    // Reads over `out` (missing keys keep their current values). False + reason on bad data.
    static bool FromJson(const nlohmann::json& j, WeaponProceduralSettings& out, std::string* error);
};

// What the stack produces each frame.
struct WeaponProceduralPose {
    glm::vec3 Position{0.0f};      // gun bone, camera frame, metres
    glm::vec3 Rotation{0.0f};      // gun bone, degrees (pitch, yaw, roll)
    glm::vec3 Pivot{0.0f};         // rotation centre relative to the gun bone, camera frame
    glm::vec2 CameraKick{0.0f};    // view punch, degrees (pitch, yaw)
    glm::vec2 AimKick{0.0f};       // aim climb this frame, degrees (pitch, yaw) - apply once, keep
    float Bolt = 0.0f;             // 0..1 of the bolt's travel toward the rear
    float CameraRoll = 0.0f;       // lean, degrees
    float CameraSide = 0.0f;       // lean, metres along the camera's right
    // 0..1, eased toward 0 in states tagged OffTag: how much of the procedural motion (and the IK
    // carrying it) applies. IK.Enabled doesn't change it; that picks IK vs whole-view-model.
    float IKWeight = 1.0f;
    float WalkRate = 1.0f;         // for the controller's WalkRate / SprintRate parameters
    float SprintRate = 1.0f;

    glm::quat RotationQuat() const; // yaw * pitch * roll
};

struct WeaponProceduralInput {
    float Dt = 0.0f;
    glm::vec2 LookRate{0.0f};      // degrees/second of view turn (yaw right +, pitch up +)
    glm::vec3 Velocity{0.0f};      // camera frame, m/s (horizontal plane)
    bool Sprinting = false;
    bool Ads = false;              // the current state is tagged ADS
    bool IKOff = false;            // the current state is tagged IKOff (or hidden)
    float Lean = 0.0f;             // -1 left .. +1 right
    // The player's full walk and sprint speeds (m/s), for references left at 0.
    float WalkSpeed = 0.0f;
    float SprintSpeed = 0.0f;
    // The current state's name and tags, for StateOffsets.
    const std::string* StateName = nullptr;
    const std::vector<std::string>* StateTags = nullptr;
};

class WeaponProceduralState {
public:
    void Reset();
    // A round was fired: starts one recoil instance.
    // `cycleBolt` false when the weapon's Fire clip is already cycling the bolt itself.
    void OnShot(const WeaponProceduralSettings& s, bool ads, bool cycleBolt = true);
    const WeaponProceduralPose& Update(const WeaponProceduralSettings& s, const WeaponProceduralInput& in);
    const WeaponProceduralPose& Pose() const { return m_Pose; }
    int ActiveShots() const { return (int)m_Shots.size(); }
    void Seed(unsigned seed) { m_Rng.seed(seed); }

    // The summed recoil curves `seconds` after one shot with every random pick at 1 - what the
    // Inspector's recoil preview plots.
    static void PreviewShot(const WeaponRecoilSettings& r, float seconds, glm::vec3& rotation, glm::vec3& position);

private:
    struct Shot {
        float Time = 0.0f;
        float Scale = 1.0f;
        glm::vec3 Rot{1.0f}, Pos{1.0f};
        float CamYaw = 1.0f;
    };
    struct Spring3 {
        glm::vec3 X{0.0f}, V{0.0f};
        void Step(const glm::vec3& target, float dt, const FirstPersonSpring& sp);
    };

    std::vector<Shot> m_Shots;
    glm::vec2 m_AimPending{0.0f};     // climb still to feed in (spread over a few frames)
    glm::vec2 m_AimRecoverable{0.0f}; // climb that recovery will hand back
    float m_SinceShot = 1e9f;
    float m_BoltTime = 1e9f;
    Spring3 m_RecoilRot, m_RecoilPos, m_SwayRot, m_SwayPos, m_Camera;
    float m_BobWeight = 0.0f, m_BobSprint = 0.0f, m_BobPhase = 0.0f;
    float m_BreathPhase = 0.0f;
    float m_Ads = 0.0f;            // 0..1 linear, eased by Aim.Blend
    float m_Lean = 0.0f;
    float m_IK = 1.0f;
    std::vector<float> m_StateWeights;
    WeaponProceduralPose m_Pose;
    std::mt19937 m_Rng{0x5eedu};
};
