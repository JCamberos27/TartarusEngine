#include "ThirdPersonWeapon.h"

#include "AnimationSystem.h"
#include "Model.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>

const char* const ThirdPersonWeapon::kClipFolder = "assets/Animations/Rifle01/";

namespace {

const char* const kClipFiles[] = {
    "In_Place/W2_Stand_Aim_Idle_v2_IPC.fbx",   "In_Place/W2_Crouch_Aim_Idle_v2_IPC.fbx",
    "In_Place/W2_Stand_Relaxed_Idle_v2_IPC.fbx", "In_Place/W2_Crouch_Idle_v2_IPC.fbx",
    "Root_Motion/W2_Stand_Aim_Point_U90.fbx",  "Root_Motion/W2_Stand_Aim_Point_Center.fbx", "Root_Motion/W2_Stand_Aim_Point_D90.fbx",
    "Root_Motion/W2_Crouch_Aim_Point_U90.fbx", "Root_Motion/W2_Crouch_Aim_Point_Center.fbx", "Root_Motion/W2_Crouch_Aim_Point_D90.fbx",
    "In_Place/W2_Stand_Fire_Single_IPC.fbx",   "In_Place/W2_Crouch_Fire_Single_IPC.fbx",
};

float Follow(float dt, float seconds) { return seconds > 1e-4f ? 1.0f - std::exp(-dt / seconds) : 1.0f; }

glm::mat4 FromTR(const glm::vec3& t, const glm::quat& r) { return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r); }

// `m` a share `t` of the way from identity (rotation slerped, translation scaled).
glm::mat4 Partial(const glm::mat4& m, float rot, float pos) {
    const glm::quat q = glm::normalize(glm::quat_cast(glm::mat3(m)));
    return FromTR(glm::vec3(m[3]) * pos, glm::slerp(glm::quat(1, 0, 0, 0), q, rot));
}

// The rotation slerped between two rigid frames' and their translations lerped.
glm::mat4 Ease(const glm::mat4& from, const glm::mat4& to, float t) {
    const glm::quat a = glm::normalize(glm::quat_cast(glm::mat3(from))), b = glm::normalize(glm::quat_cast(glm::mat3(to)));
    return FromTR(glm::mix(glm::vec3(from[3]), glm::vec3(to[3]), t), glm::slerp(a, b, t));
}

bool Under(const Model& m, int node, int ancestor) {
    for (int p = node; p >= 0; p = m.NodeParent(p))
        if (p == ancestor) return true;
    return false;
}

} // namespace

bool ThirdPersonWeapon::Bound(const Model& m) const {
    const auto it = m_Rigs.find(&m);
    return it != m_Rigs.end() && it->second.Ok;
}

bool ThirdPersonWeapon::Bind(Model& m, AssetLibrary& assets, const std::string& standIdle, const std::string& crouchIdle) {
    Rig& r = m_Rigs[&m];
    if (r.Ok && (int)r.Parents.size() == m.NodeCount()) return true;
    r = Rig{};
    for (int c = 0; c < FireCrouch + 1; ++c) r.Clips[c] = ResolveAnimationClip(m, std::string(kClipFolder) + kClipFiles[c], assets);
    r.Clips[IdleStand] = standIdle.empty() ? -1 : ResolveAnimationClip(m, standIdle, assets);
    r.Clips[IdleCrouch] = crouchIdle.empty() ? r.Clips[IdleStand] : ResolveAnimationClip(m, crouchIdle, assets);
    for (int c = 0; c < ClipCount; ++c)
        if (r.Clips[c] < 0) return false;
    r.Parents.resize(m.NodeCount());
    for (int i = 0; i < m.NodeCount(); ++i) r.Parents[i] = m.NodeParent(i);
    static const char* const kLayered[] = {"spine_01", "spine_02", "spine_03", "spine_04", "spine_05", "neck_01", "neck_02", "head"};
    for (const char* name : kLayered)
        if (const int i = m.NodeIndex(name); i >= 0) r.Layered.push_back(i);
    const int clav[2] = {m.NodeIndex("clavicle_l"), m.NodeIndex("clavicle_r")};
    static const char* const kSide[2] = {"_l", "_r"};
    for (int s = 0; s < 2; ++s) {
        r.Upper[s] = m.NodeIndex(std::string("upperarm") + kSide[s]);
        r.Lower[s] = m.NodeIndex(std::string("lowerarm") + kSide[s]);
        r.Hand[s] = m.NodeIndex(std::string("hand") + kSide[s]);
    }
    if (clav[0] < 0 || clav[1] < 0 || r.Hand[0] < 0 || r.Hand[1] < 0 || r.Upper[0] < 0 || r.Lower[0] < 0 || r.Upper[1] < 0 ||
        r.Lower[1] < 0 || r.Layered.empty())
        return false;
    for (int i = 0; i < m.NodeCount(); ++i) {
        const std::string& name = m.NodeName(i);
        if (name.rfind("ik_", 0) == 0) continue;
        if (Under(m, i, clav[0]) || Under(m, i, clav[1])) r.Arms.push_back(i);
        if ((i != r.Hand[0] && Under(m, i, r.Hand[0])) || (i != r.Hand[1] && Under(m, i, r.Hand[1]))) r.Fingers.push_back(i);
    }
    r.Posed = r.Layered;
    r.Posed.insert(r.Posed.end(), r.Arms.begin(), r.Arms.end());
    r.FireLength[0] = m.AnimationLength(r.Clips[FireStand]);
    r.FireLength[1] = m.AnimationLength(r.Clips[FireCrouch]);
    r.Ok = true;
    return true;
}

const std::vector<int>* ThirdPersonWeapon::PosedNodes(const Model& m) const {
    const auto it = m_Rigs.find(&m);
    return it != m_Rigs.end() && it->second.Ok ? &it->second.Posed : nullptr;
}

void ThirdPersonWeapon::Reset() {
    m_HaveGrip = false;
    m_Action = m_Low = 0.0f;
    m_LastShots = -1;
    m_SinceShot = 1e9f;
}

bool ThirdPersonWeapon::Apply(const Model& m, const glm::mat4& modelWorld, IK::Pose& pose, const ThirdPersonWeaponFrame& frame,
                              const ThirdPersonWeaponAim& aim, float time, float dt, glm::mat4& gunOut) {
    const auto it = m_Rigs.find(&m);
    if (it == m_Rigs.end() || !it->second.Ok || (int)pose.size() != m.NodeCount() || !frame.Valid) return false;
    const Rig& r = it->second;
    const float w = std::clamp(aim.Weight, 0.0f, 1.0f);
    const float crouch = std::clamp(aim.Crouch, 0.0f, 1.0f);

    // The first-person rig's references, taken while the gun is simply held: the grip (gun in the right hand) and where
    // the gun sits before the camera. Eased, so a state's edge doesn't jump them.
    const glm::mat4 gripNow = glm::inverse(frame.Hand[1]) * frame.Gun;
    const glm::mat4 inCameraNow = glm::inverse(frame.Camera) * frame.Gun;
    if (!m_HaveGrip) {
        m_Grip = gripNow;
        m_GunInCamera = inCameraNow;
        m_HaveGrip = true;
    } else if (frame.Ready && !frame.Action) {
        const float k = Follow(dt, 0.15f);
        m_Grip = Ease(m_Grip, gripNow, k);
        m_GunInCamera = Ease(m_GunInCamera, inCameraNow, k);
    }
    m_Action += ((frame.Action ? 1.0f : 0.0f) - m_Action) * Follow(dt, 0.08f);
    m_Low += ((frame.Sprint ? 1.0f : 0.0f) - m_Low) * Follow(dt, 0.12f);
    if (m_LastShots >= 0 && frame.Shots != m_LastShots) m_SinceShot = 0.0f;
    else m_SinceShot += dt;
    m_LastShots = frame.Shots;

    // 1. The stance, sampled.
    // The idle loops every frame; the aim offsets and the locomotion idles are single poses, sampled once per model;
    // the fire clip only while a shot plays.
    m_Samples.resize(ClipCount);
    const float pitch = glm::degrees(aim.Pitch);
    const bool firing = m_SinceShot < std::max(r.FireLength[0], r.FireLength[1]);
    auto& still = m_Still[&m];
    if (still.size() != ClipCount) {
        still.resize(ClipCount);
        for (int c : {UpStand, CenterStand, DownStand, UpCrouch, CenterCrouch, DownCrouch, IdleStand, IdleCrouch})
            m.SampleLocalPose(r.Clips[c], 0.0f, AnimationWrapMode::ClampForever, still[c]);
    }
    for (int c = 0; c < ClipCount; ++c) {
        if (c == AimStand || c == AimCrouch || c == LowStand || c == LowCrouch) {
            m.SampleLocalPose(r.Clips[c], time, AnimationWrapMode::Loop, m_Samples[c]);
        } else if (c == FireStand || c == FireCrouch) {
            if (firing) m.SampleLocalPose(r.Clips[c], std::min(m_SinceShot, r.FireLength[c == FireCrouch ? 1 : 0]),
                                          AnimationWrapMode::ClampForever, m_Samples[c]);
        } else {
            m_Samples[c] = still[c];
        }
    }
    const float up = std::clamp(pitch / 90.0f, 0.0f, 1.0f), down = std::clamp(-pitch / 90.0f, 0.0f, 1.0f);
    const float aimShare = 1.0f - m_Low;
    auto stance = [&](int i) {
        const LocalTRS stand = LocalTRS::Blend(m_Samples[AimStand][i], m_Samples[LowStand][i], m_Low);
        const LocalTRS crouched = LocalTRS::Blend(m_Samples[AimCrouch][i], m_Samples[LowCrouch][i], m_Low);
        LocalTRS s = LocalTRS::Blend(stand, crouched, crouch);
        // The aim offset: the pitch's pose against the centre one, on top (none of it at the low ready).
        const glm::quat cs = m_Samples[CenterStand][i].R, cc = m_Samples[CenterCrouch][i].R;
        const glm::quat aoS = glm::slerp(glm::slerp(cs, m_Samples[UpStand][i].R, up), m_Samples[DownStand][i].R, down);
        const glm::quat aoC = glm::slerp(glm::slerp(cc, m_Samples[UpCrouch][i].R, up), m_Samples[DownCrouch][i].R, down);
        const glm::quat add = glm::slerp(glm::inverse(cs) * aoS, glm::inverse(cc) * aoC, crouch);
        s.R = glm::normalize(s.R * glm::slerp(glm::quat(1, 0, 0, 0), add, aimShare));
        return s;
    };
    // The fire clip's motion against its own first frame, as an additive.
    auto& fireStarts = m_FireStart[&m];
    if (fireStarts.size() != 2) {
        fireStarts.resize(2);
        m.SampleLocalPose(r.Clips[FireStand], 0.0f, AnimationWrapMode::ClampForever, fireStarts[0]);
        m.SampleLocalPose(r.Clips[FireCrouch], 0.0f, AnimationWrapMode::ClampForever, fireStarts[1]);
    }
    const IK::Pose& fireStart = fireStarts[crouch > 0.5f ? 1 : 0];
    auto fire = [&](int i) {
        if (!firing || i >= (int)fireStart.size()) return glm::quat(1, 0, 0, 0);
        const IK::Pose& f = m_Samples[crouch > 0.5f ? FireCrouch : FireStand];
        return glm::slerp(glm::quat(1, 0, 0, 0), glm::normalize(glm::inverse(fireStart[i].R) * f[i].R), aimShare);
    };

    // 2. Onto the locomotion: the spine and head layered on its own (relative to its idle), the arms outright.
    for (int i : r.Layered) {
        const glm::quat idle = glm::slerp(m_Samples[IdleStand][i].R, m_Samples[IdleCrouch][i].R, crouch);
        const LocalTRS s = stance(i);
        const glm::quat want = glm::normalize(pose[i].R * glm::inverse(idle) * s.R * fire(i));
        pose[i].R = glm::normalize(glm::slerp(pose[i].R, want, w));
    }
    for (int i : r.Arms) {
        const LocalTRS s = stance(i);
        pose[i].R = glm::normalize(glm::slerp(pose[i].R, glm::normalize(s.R * fire(i)), w));
    }
    IK::ComputeGlobals(pose, r.Parents, m_Globals);
    // The aim's heading off the body's, spread down the spine (model space, about up).
    if (std::abs(aim.Yaw) > 1e-4f) {
        std::vector<int> spine;
        for (int i : r.Layered)
            if (m.NodeName(i).rfind("spine", 0) == 0) spine.push_back(i);
        if (!spine.empty()) {
            const glm::quat step = glm::angleAxis(aim.Yaw * w / (float)spine.size(), glm::vec3(0.0f, 1.0f, 0.0f));
            for (int i : spine) IK::OffsetBone(pose, r.Parents, m_Globals, i, glm::vec3(0.0f), step, IK::Position(m_Globals[i]));
        }
    }

    // 3. The gun in the right hand as the clips hold it, by the first-person grip; moved in the hand as the first-person gun
    // moves before its camera during an action (rotation mostly, a little of the travel); turned (a little) onto the aim.
    const glm::mat4 toModel = glm::inverse(modelWorld);
    const glm::quat toModelRot = IK::Rotation(toModel);
    const glm::mat4 handRWorld = FromTR(glm::vec3(modelWorld * glm::vec4(IK::Position(m_Globals[r.Hand[1]]), 1.0f)),
                                        glm::normalize(IK::Rotation(modelWorld) * IK::Rotation(m_Globals[r.Hand[1]])));
    glm::mat4 gun = handRWorld * m_Grip;
    const glm::vec3 boreLocal = glm::inverse(glm::mat3(frame.Gun)) * frame.Bore;
    // The aim, at the chest: what's left between the bore and the target after the clips' aim offset is turned out by the
    // spine (spread down it, at most 45 degrees), so the gun comes onto the target with the shoulders behind it.
    std::vector<int> spine;
    for (int i : r.Layered)
        if (m.NodeName(i).rfind("spine", 0) == 0) spine.push_back(i);
    const float aimWeight = aimShare * (1.0f - m_Action) * w;
    if (aim.HaveTarget && !spine.empty() && aimWeight > 1e-3f) {
        for (int pass = 0; pass < 2; ++pass) {
            const glm::vec3 bore = glm::normalize(glm::mat3(gun) * boreLocal);
            const glm::vec3 to = aim.Target - glm::vec3(gun[3]);
            if (glm::length(to) < 0.5f) break;
            glm::quat q = glm::rotation(bore, glm::normalize(to));
            const float most = glm::radians(45.0f), angle = 2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f));
            if (angle < glm::radians(0.5f)) break;
            if (angle > most) q = glm::slerp(glm::quat(1, 0, 0, 0), q, most / angle);
            q = glm::slerp(glm::quat(1, 0, 0, 0), q, aimWeight);
            const glm::quat qModel = glm::normalize(toModelRot * q * glm::inverse(toModelRot));
            const glm::quat step = glm::slerp(glm::quat(1, 0, 0, 0), qModel, 1.0f / (float)spine.size());
            for (int i : spine) IK::OffsetBone(pose, r.Parents, m_Globals, i, glm::vec3(0.0f), step, IK::Position(m_Globals[i]));
            const glm::mat4 h = FromTR(glm::vec3(modelWorld * glm::vec4(IK::Position(m_Globals[r.Hand[1]]), 1.0f)),
                                       glm::normalize(IK::Rotation(modelWorld) * IK::Rotation(m_Globals[r.Hand[1]])));
            gun = h * m_Grip;
        }
    }
    const glm::vec3 handPivot(gun * glm::inverse(m_Grip)[3]);
    if (m_Action > 1e-3f) {
        glm::mat4 d = glm::inverse(m_GunInCamera) * inCameraNow;
        glm::vec3 t(d[3]);
        if (const float len = glm::length(t); len > 0.25f) t *= 0.25f / len;
        d[3] = glm::vec4(t, 1.0f);
        gun = gun * Partial(d, 0.7f * m_Action, 0.5f * m_Action);
    }
    m_BoreError = 0.0f;
    if (aim.HaveTarget) {
        const glm::vec3 bore = glm::normalize(glm::mat3(gun) * boreLocal);
        const glm::vec3 to = aim.Target - glm::vec3(gun[3]);
        if (glm::length(to) > 0.5f) {
            const glm::vec3 want = glm::normalize(to);
            m_BoreError = glm::degrees(std::acos(std::clamp(glm::dot(bore, want), -1.0f, 1.0f)));
            glm::quat q = glm::rotation(bore, want);
            const float most = glm::radians(12.0f), angle = 2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f));
            if (angle > most) q = glm::slerp(glm::quat(1, 0, 0, 0), q, most / angle);
            q = glm::slerp(glm::quat(1, 0, 0, 0), q, aimShare * (1.0f - m_Action) * w);
            const glm::vec3 pivot = handPivot;
            gun = glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(q) * glm::translate(glm::mat4(1.0f), -pivot) * gun;
        }
    }
    gunOut = gun;

    // 4. Both hands onto the gun where the rig's are on its own, the fingers as the rig's.
    for (int s = 0; s < 2; ++s) {
        const glm::mat4 want = gun * glm::inverse(frame.Gun) * frame.Hand[s];
        const glm::vec3 target = glm::vec3(toModel * glm::vec4(glm::vec3(want[3]), 1.0f));
        const glm::quat rot = glm::normalize(toModelRot * IK::Rotation(want));
        IK::SolveTwoBone(pose, r.Parents, m_Globals, r.Upper[s], r.Lower[s], r.Hand[s], target, &rot, w);
        m_HandGap[s] = glm::length(glm::vec3(modelWorld * glm::vec4(IK::Position(m_Globals[r.Hand[s]]), 1.0f)) - glm::vec3(want[3]));
    }
    if (frame.Rig) {
        auto& links = m_FingerLinks[{&m, frame.Rig}];
        if (links.empty())
            for (int i : r.Fingers)
                if (const int j = frame.Rig->NodeIndex(m.NodeName(i)); j >= 0) links.push_back({i, j});
        const IK::Pose& rigPose = frame.Rig->AppliedLocalPose();
        for (const auto& [i, j] : links)
            if (j < (int)rigPose.size()) pose[i].R = glm::normalize(glm::slerp(pose[i].R, rigPose[j].R, w));
    }
    return true;
}
