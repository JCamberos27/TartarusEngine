#pragma once

#include "IK.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <map>
#include <string>
#include <vector>

class AssetLibrary;
class Model;

// The third-person weapon hold: how a body that holds a rifle looks from outside (every view but the
// player's own, shadows, and the enemy soldiers). The first-person rigs' clips are authored for a camera -
// the gun under the eye, the arms stretched to it, a reload staged in frame - so the body's own posture comes
// from third-person rifle clips instead (the MoCap Online Rifle set, a UE4 pack baked onto the rig by
// AnimRetarget), and only what is relative to the gun is taken from the first-person rig:
//
//   - The stance: an aim (or low ready) idle over the locomotion. The spine and head as an additive on the
//     locomotion's own (relative to its idle), so the gait's lean and turn stay; the arms outright.
//   - The aim: the clips' aim offsets for the pitch, and a fire clip as an additive on each shot.
//   - The gun: in the right hand, held as the first-person rig holds it at the ready (its grip). During an
//     action (a reload, a draw) the gun moves in the hand as the first-person one moves before its camera.
//   - The hands: each onto the gun where the first-person rig's is on its gun - a mag out and in, a bolt
//     pulled, the support hand on the handguard - and the fingers shaped as the rig's.
struct ThirdPersonWeaponFrame {
    bool Valid = false;
    glm::mat4 Gun{1.0f};                      // the first-person gun, world, rigid (no scale)
    glm::mat4 Hand[2] = {glm::mat4(1.0f), glm::mat4(1.0f)}; // the rig's hands (l, r), world, rigid
    glm::mat4 Camera{1.0f};                   // the rig's camera, world, rigid
    glm::vec3 Bore{0.0f, 0.0f, -1.0f};        // down the barrel, world
    const Model* Rig = nullptr;               // the arms rig (finger shapes); null = keep the clips'
    bool Ready = false;                       // simply held (idle, walk, hip fire, sights): the grip's reference
    bool Action = false;                      // a reload, a draw or holster, a melee, an inspect: the gun moves in the hand
    bool Sprint = false;                      // the low ready
    int Shots = 0;                            // rounds fired so far (a change = a shot)
};

struct ThirdPersonWeaponAim {
    float Pitch = 0.0f;           // radians, + up: the aim offset's
    float Yaw = 0.0f;             // radians, + left (about +Y): the aim's heading off the body's, turned into the spine
    glm::vec3 Target{0.0f};       // where the gun points (world)
    bool HaveTarget = false;
    float Crouch = 0.0f;          // 0..1
    float Weight = 1.0f;          // 0..1: the whole layer (0 = the locomotion as it is)
};

class ThirdPersonWeapon {
public:
    // The clips, relative to the project's assets.
    static const char* const kClipFolder;
    // Attaches the clips to `m` (once per model) and the locomotion's standing / crouched idles the spine is
    // layered against. False until every clip resolves.
    bool Bind(Model& m, AssetLibrary& assets, const std::string& standIdle, const std::string& crouchIdle);
    bool Bound(const Model& m) const;
    // On `m`'s local `pose` (the locomotion's, the model at `modelWorld`): the stance, the aim and the hands on
    // the gun. `gunOut` gets the gun's world transform (rigid). False (pose untouched) when `m` isn't bound.
    bool Apply(const Model& m, const glm::mat4& modelWorld, IK::Pose& pose, const ThirdPersonWeaponFrame& frame,
               const ThirdPersonWeaponAim& aim, float time, float dt, glm::mat4& gunOut);
    void Reset();
    // The nodes Apply poses on `m` (the spine, neck, head and arms), for handing its result to pieces that share the skeleton.
    const std::vector<int>* PosedNodes(const Model& m) const;

    // Diagnostics: the left / right hand's distance from where it should be on the gun after the solve (m), and
    // the gun's bore off the aim (degrees).
    float HandGap(int side) const { return m_HandGap[side & 1]; }
    float BoreError() const { return m_BoreError; }

private:
    enum Clip { AimStand, AimCrouch, LowStand, LowCrouch, UpStand, CenterStand, DownStand, UpCrouch, CenterCrouch, DownCrouch,
                FireStand, FireCrouch, IdleStand, IdleCrouch, ClipCount };
    struct Rig {
        std::array<int, ClipCount> Clips{};
        std::vector<int> Parents;
        std::vector<int> Layered;   // spine, neck, head: additive on the locomotion
        std::vector<int> Arms;      // clavicles and under: the clips' outright
        std::vector<int> Fingers;   // under the hands
        std::vector<int> Posed;     // Layered + Arms
        int Upper[2] = {-1, -1}, Lower[2] = {-1, -1}, Hand[2] = {-1, -1};
        float FireLength[2] = {0.0f, 0.0f};
        bool Ok = false;
    };
    std::map<const Model*, Rig> m_Rigs;
    std::map<std::pair<const Model*, const Model*>, std::vector<std::pair<int, int>>> m_FingerLinks; // (body, rig) -> node pairs
    std::vector<IK::Pose> m_Samples;
    std::map<const Model*, std::vector<IK::Pose>> m_Still, m_FireStart; // single poses, sampled once per model
    std::vector<glm::mat4> m_Globals;

    // Per character.
    glm::mat4 m_Grip{1.0f};           // the gun in the right hand, as held at the ready
    glm::mat4 m_GunInCamera{1.0f};    // ... and before the camera
    bool m_HaveGrip = false;
    float m_Action = 0.0f, m_Low = 0.0f; // eased weights
    int m_LastShots = -1;
    float m_SinceShot = 1e9f;
    float m_HandGap[2] = {0.0f, 0.0f}, m_BoreError = 0.0f;
};
