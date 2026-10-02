#include "NpcRagdoll.h"

#include "Components.h" // RagdollSettingsComponent
#include "IK.h"
#include "Model.h"
#include "NpcBody.h"
#include "PhysicsWorld.h"

#include <glm/gtc/matrix_transform.hpp>
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace {

// Masses add up to ~75 kg (the Ragdoll Settings' defaults; these are the fallback and the shared shape table). Swing / Twist
// here are the old generous symmetric cones, kept for AnatomicalLimits off; the anatomical ranges are below.
const NpcPartDef kDefs[NpcRagdoll::kParts] = {
    {"pelvis", "spine_02", 0.0f, 0.13f, 14.0f, -1, 0.0f, 0.0f},
    {"spine_03", "neck_01", 0.0f, 0.15f, 18.0f, 0, 28.0f, 22.0f},
    {"head", nullptr, 0.2f, 0.1f, 5.0f, 1, 40.0f, 45.0f},
    {"upperarm_l", "lowerarm_l", 0.0f, 0.055f, 2.5f, 1, 75.0f, 45.0f},
    {"lowerarm_l", "hand_l", 0.1f, 0.045f, 1.8f, 3, 70.0f, 25.0f},
    {"upperarm_r", "lowerarm_r", 0.0f, 0.055f, 2.5f, 1, 75.0f, 45.0f},
    {"lowerarm_r", "hand_r", 0.1f, 0.045f, 1.8f, 5, 70.0f, 25.0f},
    {"thigh_l", "calf_l", 0.0f, 0.075f, 8.0f, 0, 50.0f, 18.0f},
    {"calf_l", "foot_l", 0.06f, 0.055f, 4.5f, 7, 60.0f, 8.0f},
    {"thigh_r", "calf_r", 0.0f, 0.075f, 8.0f, 0, 50.0f, 18.0f},
    {"calf_r", "foot_r", 0.06f, 0.055f, 4.5f, 9, 60.0f, 8.0f},
};

// Anatomical ranges of motion (degrees, from the neutral pose: limb hanging, spine and head upright), the defaults of
// RagdollSettingsComponent. Flexion is toward the front for the spine, neck, shoulder and hip; elbows and knees are hinges
// that fold one way only. References (AAOS / Kapandji, healthy adult, active ROM; trimmed where one ragdoll part stands for
// several joints, or where the full range would let the limb fold through the body):
//   joint        flex  ext  adduct abduct  twist in / out
//   spine (T+L)   45    20    25     25      30 / 30      (lumbar+thoracic combined: flexion 60-80, extension 20-30)
//   head (neck)   50    60    40     40      70 / 70      (cervical: flexion 45-50, extension 55-70, lateral 40, rotation 70-80)
//   shoulder     170    60    40    150      70 / 80      (flexion 180, extension 60, adduction 30-50, abduction 180, IR 70, ER 90)
//   elbow        145     0     3      3      10 / 10      (flexion 140-150, hyperextension 0; the forearm's own rotation is the wrist's)
//   hip          120    20    30     45      40 / 45      (flexion 120, extension 20, adduction 30, abduction 45, IR 40, ER 45)
//   knee         140     0     3      3       5 /  5      (flexion 135-145, no hyperextension, ~5 deg of rotation when bent)
// "Lateral" for a hinge is slack so the joint doesn't bind. Hitbox shape (radius, extend) is NOT tunable: it changes gameplay.
enum class Region { Pelvis, Spine, Head, UpperArm, Forearm, Thigh, Calf };
constexpr Region kRegionOf[NpcRagdoll::kParts] = {Region::Pelvis, Region::Spine, Region::Head, Region::UpperArm, Region::Forearm,
                                                   Region::UpperArm, Region::Forearm, Region::Thigh, Region::Calf, Region::Thigh, Region::Calf};

struct Ranges { float Flex, Ext, LatIn, LatOut, TwistIn, TwistOut; };
Ranges RangesOf(const RagdollSettingsComponent& c, Region r) {
    switch (r) {
    case Region::Spine: return {c.SpineFlexMax, c.SpineExtMax, c.SpineLatIn, c.SpineLatOut, c.SpineTwistIn, c.SpineTwistOut};
    case Region::Head: return {c.HeadFlexMax, c.HeadExtMax, c.HeadLatIn, c.HeadLatOut, c.HeadTwistIn, c.HeadTwistOut};
    case Region::UpperArm: return {c.UpperArmFlexMax, c.UpperArmExtMax, c.UpperArmLatIn, c.UpperArmLatOut, c.UpperArmTwistIn, c.UpperArmTwistOut};
    case Region::Forearm: return {c.ForearmFlexMax, c.ForearmExtMax, c.ForearmLatIn, c.ForearmLatOut, c.ForearmTwistIn, c.ForearmTwistOut};
    case Region::Thigh: return {c.ThighFlexMax, c.ThighExtMax, c.ThighLatIn, c.ThighLatOut, c.ThighTwistIn, c.ThighTwistOut};
    case Region::Calf: return {c.CalfFlexMax, c.CalfExtMax, c.CalfLatIn, c.CalfLatOut, c.CalfTwistIn, c.CalfTwistOut};
    default: return {0, 0, 0, 0, 0, 0};
    }
}

glm::vec3 PerpNormalized(const glm::vec3& v, const glm::vec3& axis) {
    const glm::vec3 p = v - axis * glm::dot(v, axis);
    const float l = glm::length(p);
    return l > 1e-5f ? p / l : glm::vec3(0.0f);
}

// Part `i`'s joint frame (X = neutral bone direction, Y = positive flexion) and range of motion, from the build pose.
// `pd` / `cd`: the parent's and the part's bone directions; `left`, `up`, `fwd`: the body's axes (world).
void FillAnatomical(int i, const RagdollSettingsComponent& cfg, const glm::vec3& pd, const glm::vec3& cd, const glm::vec3& left,
                    const glm::vec3& up, const glm::vec3& fwd, PhysicsWorld::RagdollPart& out) {
    const Region r = kRegionOf[i];
    const bool hinge = r == Region::Forearm || r == Region::Calf;
    const bool leftSide = i == 3 || i == 4 || i == 7 || i == 8;
    const bool centre = r == Region::Spine || r == Region::Head;
    const Ranges lim = RangesOf(cfg, r);
    // Neutral: a hinge's is the bone above it straight on, a limb otherwise hangs, a spine stands.
    const glm::vec3 x = glm::normalize(hinge ? pd : centre ? up : -up);
    // The flexion direction: forward (back for a knee); a hinge folds the way the animated pose already bends when it does.
    glm::vec3 y = PerpNormalized(r == Region::Calf ? -fwd : fwd, x);
    if (hinge) {
        const glm::vec3 bent = PerpNormalized(cd, x);
        if (glm::length(cd - pd) > 0.17f && glm::length(bent) > 0.0f) y = bent;
    }
    if (glm::length(y) < 0.5f) y = PerpNormalized(std::fabs(x.y) < 0.9f ? up : left, x);
    const glm::vec3 z = glm::cross(x, y);
    out.Anatomical = true;
    const glm::quat q = glm::quat_cast(glm::mat3(x, y, z));
    out.LimitFrame[0] = q.x; out.LimitFrame[1] = q.y; out.LimitFrame[2] = q.z; out.LimitFrame[3] = q.w;
    out.SwingZMin = -lim.Ext;
    out.SwingZMax = lim.Flex;
    // Sideways: a positive swing about Y leans toward -Z; "in" is toward the midline, "out" away from it.
    if (centre) {
        const float lat = std::max(lim.LatIn, lim.LatOut);
        out.SwingYMin = -lat; out.SwingYMax = lat;
        out.TwistMin = -lim.TwistIn; out.TwistMax = lim.TwistOut;
        return;
    }
    const glm::vec3 outward = leftSide ? left : -left;
    if (glm::dot(-z, outward) > 0.0f) { out.SwingYMin = -lim.LatIn; out.SwingYMax = lim.LatOut; }
    else { out.SwingYMin = -lim.LatOut; out.SwingYMax = lim.LatIn; }
    // Twist about the bone (down a hanging limb): positive turns the left limb inward, the right outward.
    if (leftSide) { out.TwistMin = -lim.TwistOut; out.TwistMax = lim.TwistIn; }
    else { out.TwistMin = -lim.TwistIn; out.TwistMax = lim.TwistOut; }
}

glm::mat4 PoseOf(const glm::vec3& p, const glm::quat& q) { return glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q); }

} // namespace

const NpcPartDef& NpcPartDefOf(int part) { return kDefs[std::clamp(part, 0, NpcRagdoll::kParts - 1)]; }

NpcPartShape NpcPartShapeOf(const NpcPartDef& d, const glm::vec3& a, const glm::vec3& bIn, const glm::vec3& neck) {
    glm::vec3 b = bIn;
    if (!d.End) b = a + glm::normalize(a - neck + glm::vec3(0, 1e-4f, 0)) * 0.01f;
    glm::vec3 dir = b - a;
    float len = glm::length(dir);
    dir = len > 1e-4f ? dir / len : glm::vec3(0, 1, 0);
    len += d.Extend;
    b = a + dir * len;
    NpcPartShape s;
    s.Rotation = glm::rotation(glm::vec3(1, 0, 0), dir);
    s.Centre = 0.5f * (a + b);
    s.Radius = d.Radius;
    s.HalfLength = std::max(0.01f, 0.5f * len - d.Radius * 0.5f);
    s.Anchor = a;
    return s;
}

float NpcRagdoll::PartMass(const RagdollSettingsComponent* cfg, int part) {
    part = std::clamp(part, 0, kParts - 1);
    if (!cfg) return kDefs[part].Mass;
    switch (kRegionOf[part]) {
    case Region::Pelvis: return cfg->PelvisMass;
    case Region::Spine: return cfg->SpineMass;
    case Region::Head: return cfg->HeadMass;
    case Region::UpperArm: return cfg->UpperArmMass;
    case Region::Forearm: return cfg->ForearmMass;
    case Region::Thigh: return cfg->ThighMass;
    default: return cfg->CalfMass;
    }
}

float NpcRagdoll::PartFade(const RagdollSettingsComponent* cfg, int part) {
    part = std::clamp(part, 0, kParts - 1);
    if (!cfg) return kDriveFade;
    float scale = 1.0f;
    switch (kRegionOf[part]) {
    case Region::Pelvis: scale = cfg->PelvisFadeScale; break;
    case Region::Spine: scale = cfg->SpineFadeScale; break;
    case Region::Head: scale = cfg->HeadFadeScale; break;
    case Region::UpperArm: scale = cfg->UpperArmFadeScale; break;
    case Region::Forearm: scale = cfg->ForearmFadeScale; break;
    case Region::Thigh: scale = cfg->ThighFadeScale; break;
    default: scale = cfg->CalfFadeScale; break;
    }
    return std::max(cfg->DriveFade * std::max(scale, 0.0f), 1e-3f);
}

float NpcRagdoll::DriveFadeTime() const {
    float t = 0.0f;
    for (int i = 0; i < kParts; ++i) t = std::max(t, m_PartFade[i]);
    return t;
}

namespace {
// The bones a snapshot holds: every part's start and end bone, and the neck.
const char* const kSnapNames[NpcRagdoll::kSnapBones] = {"pelvis", "spine_02", "spine_03", "neck_01", "head", "upperarm_l", "lowerarm_l", "hand_l",
                                                        "upperarm_r", "lowerarm_r", "hand_r", "thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r"};
} // namespace

void NpcRagdoll::Capture(const NpcBody& body, BoneSnapshot& out) {
    const glm::mat4 inv = glm::inverse(body.RootMatrix());
    out.Valid = true;
    for (int i = 0; i < kSnapBones; ++i) {
        glm::vec3 w;
        if (!body.BoneWorld(kSnapNames[i], w)) { out.Valid = false; return; }
        out.P[i] = glm::vec3(inv * glm::vec4(w, 1.0f));
    }
}

void NpcRagdoll::InheritVelocity(const glm::mat4* prevWorld, const glm::mat4* nowWorld, float dt, const RagdollSettingsComponent& cfg,
                                 PhysicsWorld::RagdollPart* parts) {
    if (dt < 0.002f || dt > 0.12f || cfg.LimbVelocityScale <= 0.0f) return;
    for (int i = 0; i < kParts; ++i) {
        glm::vec3 v = (glm::vec3(nowWorld[i][3]) - glm::vec3(prevWorld[i][3])) / dt * cfg.LimbVelocityScale;
        const float sp = glm::length(v);
        if (sp > cfg.MaxLimbSpeed) v *= cfg.MaxLimbSpeed / sp;
        glm::quat dq = glm::quat_cast(glm::mat3(nowWorld[i])) * glm::inverse(glm::quat_cast(glm::mat3(prevWorld[i])));
        if (dq.w < 0.0f) dq = -dq;
        const float angle = 2.0f * std::atan2(glm::length(glm::vec3(dq.x, dq.y, dq.z)), dq.w);
        glm::vec3 w(0.0f);
        const float s = glm::length(glm::vec3(dq.x, dq.y, dq.z));
        if (s > 1e-6f) w = glm::vec3(dq.x, dq.y, dq.z) / s * (angle / dt * cfg.LimbVelocityScale);
        const float spin = glm::length(w);
        if (spin > cfg.MaxLimbSpin) w *= cfg.MaxLimbSpin / spin;
        for (int k = 0; k < 3; ++k) { parts[i].Velocity[k] += v[k]; parts[i].AngularVelocity[k] += w[k]; }
    }
}

PhysicsWorld::RagdollParams NpcRagdoll::BodyParams(const RagdollSettingsComponent& c) {
    PhysicsWorld::RagdollParams p;
    p.LinearDamping = c.LinearDamping; p.AngularDamping = c.AngularDamping;
    p.SolverPosIters = c.SolverPosIters; p.SolverVelIters = c.SolverVelIters;
    p.Depenetration = c.Depenetration; p.SleepThreshold = c.SleepThreshold;
    p.StaticFriction = c.StaticFriction; p.DynamicFriction = c.DynamicFriction; p.Restitution = c.Restitution;
    return p;
}

bool NpcRagdoll::BuildParts(const std::function<bool(const char*, glm::vec3&)>& bone, const glm::mat4& root, const glm::vec3& velocity,
                            const RagdollSettingsComponent& cfg, PhysicsWorld::RagdollPart* parts, glm::mat4* partWorldOut) {
    glm::mat4 partWorld[kParts];
    glm::vec3 dir[kParts];
    glm::vec3 neck;
    if (!bone("neck_01", neck)) neck = glm::vec3(0.0f);
    for (int i = 0; i < kParts; ++i) {
        const NpcPartDef& d = kDefs[i];
        glm::vec3 a, b(0.0f);
        if (!bone(d.Bone, a)) return false;
        if (d.End && !bone(d.End, b)) return false;
        if (!d.End && glm::length(neck) < 1e-6f) neck = a - glm::vec3(0, 0.1f, 0);
        const NpcPartShape s = NpcPartShapeOf(d, a, b, neck);
        auto& p = parts[i];
        p = PhysicsWorld::RagdollPart();
        p.Parent = d.Parent;
        p.Position[0] = s.Centre.x; p.Position[1] = s.Centre.y; p.Position[2] = s.Centre.z;
        p.Rotation[0] = s.Rotation.x; p.Rotation[1] = s.Rotation.y; p.Rotation[2] = s.Rotation.z; p.Rotation[3] = s.Rotation.w;
        p.Radius = s.Radius;
        p.HalfLength = s.HalfLength;
        p.Mass = PartMass(&cfg, i);
        p.Anchor[0] = a.x; p.Anchor[1] = a.y; p.Anchor[2] = a.z;
        p.SwingDeg = d.Swing;
        p.TwistDeg = d.Twist;
        p.Velocity[0] = velocity.x; p.Velocity[1] = velocity.y; p.Velocity[2] = velocity.z;
        p.InertiaScale = cfg.InertiaScale;
        if (cfg.ShapedTorsoInertia && (kRegionOf[i] == Region::Pelvis || kRegionOf[i] == Region::Spine)) {
            // A trunk is wider than it is deep: a box (the hips a little narrower than the shoulders), not a round capsule.
            p.InertiaHalfWidth = cfg.TorsoHalfWidth * (kRegionOf[i] == Region::Pelvis ? 0.9f : 1.0f);
            p.InertiaHalfDepth = cfg.TorsoHalfDepth;
            const glm::vec3 left = glm::normalize(glm::vec3(root[0]));
            p.InertiaLateral[0] = left.x; p.InertiaLateral[1] = left.y; p.InertiaLateral[2] = left.z;
        }
        partWorld[i] = PoseOf(s.Centre, s.Rotation);
        dir[i] = s.Rotation * glm::vec3(1, 0, 0);
    }
    if (cfg.AnatomicalLimits) {
        const glm::vec3 left = glm::normalize(glm::vec3(root[0])), up = glm::normalize(glm::vec3(root[1])), fwd = glm::normalize(glm::vec3(root[2]));
        for (int i = 1; i < kParts; ++i) FillAnatomical(i, cfg, dir[kDefs[i].Parent], dir[i], left, up, fwd, parts[i]);
    }
    if (partWorldOut) for (int i = 0; i < kParts; ++i) partWorldOut[i] = partWorld[i];
    return true;
}

NpcRagdoll::~NpcRagdoll() { Stop(); }

void NpcRagdoll::Stop() {
    if (m_Id >= 0) PhysicsWorld::DestroyRagdoll(m_Id);
    m_Id = -1;
    m_Pieces.clear();
    m_Drive = 0.0f;
}

bool NpcRagdoll::Start(NpcBody& body, const glm::vec3& velocity, const glm::vec3& impulse, const glm::vec3& point, int hitPart,
                       const RagdollSettingsComponent* cfgIn, const BoneSnapshot* prev, float prevDt) {
    Stop();
    const RagdollSettingsComponent defaults;
    const RagdollSettingsComponent& cfg = cfgIn ? *cfgIn : defaults;
    const auto& models = body.Models();
    if (models.empty()) return false;
    const glm::mat4 root = body.RootMatrix();
    // Bone world positions from the driver's finished pose.
    PhysicsWorld::RagdollPart parts[kParts];
    glm::mat4 partWorld[kParts];
    if (!BuildParts([&](const char* name, glm::vec3& out) { return name && body.BoneWorld(name, out); }, root, velocity, cfg, parts, partWorld))
        return false;
    if (prev && prev->Valid) { // each part's own motion: its pose against the one before, both in the body's (fixed) root frame
        auto before = [&](const char* name, glm::vec3& out) {
            for (int k = 0; k < kSnapBones; ++k)
                if (std::strcmp(kSnapNames[k], name) == 0) { out = glm::vec3(root * glm::vec4(prev->P[k], 1.0f)); return true; }
            return false;
        };
        PhysicsWorld::RagdollPart prevParts[kParts];
        glm::mat4 prevWorld[kParts];
        if (BuildParts(before, root, velocity, cfg, prevParts, prevWorld)) InheritVelocity(prevWorld, partWorld, prevDt, cfg, parts);
    }
    const PhysicsWorld::RagdollParams bodyParams = BodyParams(cfg);
    m_Id = PhysicsWorld::CreateRagdoll((unsigned)entt::to_integral(body.Root()), parts, kParts, &bodyParams);
    if (m_Id < 0) return false;
    // Powered at first: the joints hold the death pose (their drive targets are the pose they were built in).
    m_Stiffness = cfg.DriveStiffness; m_Damping = cfg.DriveDamping;
    for (int i = 0; i < kParts; ++i) m_PartFade[i] = PartFade(&cfg, i);
    PhysicsWorld::SetRagdollDrive(m_Id, m_Stiffness, m_Damping);
    m_Drive = 1.0f;
    m_Time = 0.0f;
    m_Settled = false;
    m_RootInv = glm::inverse(root);
    // Each piece's bones, relative to the part they ride on.
    for (const auto& mp : models) {
        PieceBones pb;
        pb.M = mp;
        pb.Node.assign(kParts, -1);
        pb.Off.assign(kParts, glm::mat4(1.0f));
        bool any = false;
        for (int i = 0; i < kParts; ++i) {
            const int n = mp->NodeIndex(kDefs[i].Bone);
            glm::mat4 g(1.0f);
            if (n < 0 || !mp->NodeTransform(kDefs[i].Bone, g)) continue;
            pb.Node[(size_t)i] = n;
            pb.Off[(size_t)i] = glm::inverse(partWorld[i]) * (root * g);
            any = true;
        }
        if (any) m_Pieces.push_back(std::move(pb));
    }
    // The round's shove, on the part it struck (else the one nearest where it did).
    int target = hitPart;
    if (target < 0 || target >= kParts) {
        target = 1;
        float best = 1e9f;
        for (int i = 0; i < kParts; ++i) {
            const float d2 = glm::length(glm::vec3(partWorld[i][3]) - point);
            if (d2 < best) { best = d2; target = i; }
        }
    }
    // A light part (a forearm, a skull) shoved with the whole round's momentum would leave the body at 30 m/s and tear
    // its joints: the part takes what it can (6 m/s), the rest goes into the chest, so the body still moves as hard.
    const float total = glm::length(impulse);
    const float cap = PartMass(&cfg, target) * cfg.PartImpulseSpeed;
    const float onPart = std::min(total, cap);
    if (total > 1e-6f) {
        const glm::vec3 dir = impulse / total;
        const glm::vec3 a = dir * onPart, rest = dir * std::min(total - onPart, PartMass(&cfg, 1) * cfg.ChestImpulseSpeed);
        const float ja[3] = {a.x, a.y, a.z}, at[3] = {point.x, point.y, point.z};
        PhysicsWorld::RagdollImpulse(m_Id, target, ja, at);
        if (glm::dot(rest, rest) > 1e-8f && target != 1) {
            const float jr[3] = {rest.x, rest.y, rest.z}, centre[3] = {partWorld[1][3][0], partWorld[1][3][1], partWorld[1][3][2]};
            PhysicsWorld::RagdollImpulse(m_Id, 1, jr, centre);
        }
    }
    return true;
}

void NpcRagdoll::Update(float dt) {
    if (m_Id < 0) return;
    // The drives fade out, each region on its own clock: stiff at the moment of death, limp a quarter second on (by default).
    if (m_Drive > 0.0f && dt > 0.0f) {
        m_Time += dt;
        m_Drive = 0.0f;
        for (int i = 1; i < kParts; ++i) {
            const float f = DriveAt(m_Time, m_PartFade[i]);
            m_Drive = std::max(m_Drive, f);
            PhysicsWorld::SetRagdollPartDrive(m_Id, i, m_Stiffness * f * f, m_Damping * f * f);
        }
    }
    // A body at rest stays as it lies: one last write once it sleeps, then nothing per frame.
    const bool asleep = Asleep();
    if (asleep && m_Settled && m_Drive <= 0.0f) return;
    m_Settled = asleep;
    glm::mat4 partWorld[kParts];
    for (int i = 0; i < kParts; ++i) {
        float p[3], q[4];
        if (!PhysicsWorld::GetRagdollPart(m_Id, i, p, q)) return;
        partWorld[i] = PoseOf(glm::vec3(p[0], p[1], p[2]), glm::quat(q[3], q[0], q[1], q[2]));
    }
    auto& pose = m_Pose;
    auto& globals = m_Globals;
    thread_local std::vector<IK::GlobalTarget> targets; // (node, part), parents first
    for (PieceBones& pb : m_Pieces) {
        Model& m = *pb.M;
        pose = m.AppliedLocalPose();
        if ((int)pose.size() != m.NodeCount()) m.BindLocalPose(pose);
        if (pb.Parents.size() != pose.size()) {
            pb.Parents.resize(pose.size());
            for (int i = 0; i < (int)pose.size(); ++i) pb.Parents[(size_t)i] = m.NodeParent(i);
        }
        IK::ComputeGlobals(pose, pb.Parents, globals);
        targets.clear();
        for (int i = 0; i < kParts; ++i)
            if (pb.Node[(size_t)i] >= 0) {
                const glm::mat4 want = m_RootInv * partWorld[i] * pb.Off[(size_t)i];
                targets.push_back({pb.Node[(size_t)i], glm::vec3(want[3]), IK::Rotation(want)});
            }
        std::sort(targets.begin(), targets.end(), [](const IK::GlobalTarget& a, const IK::GlobalTarget& b) { return a.Node < b.Node; });
        IK::SetGlobals(pose, pb.Parents, globals, targets);
        m.ApplyLocalPose(pose);
    }
}

bool NpcRagdoll::Asleep() const { return m_Id < 0 || PhysicsWorld::RagdollAsleep(m_Id); }

void NpcRagdoll::Shove(int part, const glm::vec3& impulse, const glm::vec3& point) {
    if (m_Id < 0) return;
    const float j[3] = {impulse.x, impulse.y, impulse.z}, at[3] = {point.x, point.y, point.z};
    PhysicsWorld::RagdollImpulse(m_Id, std::clamp(part, 0, kParts - 1), j, at);
    m_Settled = false; // awake again: the pose follows the parts from here
}

glm::vec3 NpcRagdoll::PartPosition(int part) const {
    float p[3], q[4];
    if (m_Id < 0 || !PhysicsWorld::GetRagdollPart(m_Id, part, p, q)) return glm::vec3(0.0f);
    return glm::vec3(p[0], p[1], p[2]);
}

glm::vec3 NpcRagdoll::Root() const { return PartPosition(0); }
