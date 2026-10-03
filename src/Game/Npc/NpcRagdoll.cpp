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

// The hitbox table (NpcHitboxes): eleven capsules whose shapes are gameplay, so nothing here follows the ragdoll's tuning.
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

enum class Region { Pelvis, Spine, Head, UpperArm, Forearm, Thigh, Calf, Neck, Hand, Foot };

// The ragdoll's own table: the same eleven in the same order, then neck, hands and feet. The forearms and calves end at the wrist
// and ankle now (the hand and foot capsules carry the rest), and the head hangs off the neck. Masses add up to ~75 kg (the Ragdoll
// Settings' defaults; these are the fallback). Swing / Twist here are the old generous symmetric cones, kept for AnatomicalLimits
// off; the anatomical ranges are below. A hand / foot reaches to its End bone (middle_01 / ball); a rig without it falls back to
// AltLen metres along the forearm (hand) or the body's forward (foot) from the joint.
struct RagDef { NpcPartDef D; Region R; bool Left; float AltLen; };
const RagDef kRagDefs[NpcRagdoll::kRagParts] = {
    {{"pelvis", "spine_02", 0.0f, 0.13f, 15.0f, -1, 0.0f, 0.0f}, Region::Pelvis, false, 0.0f},
    {{"spine_03", "neck_01", 0.0f, 0.15f, 19.0f, 0, 28.0f, 22.0f}, Region::Spine, false, 0.0f},
    {{"head", nullptr, 0.2f, 0.1f, 4.5f, NpcRagdoll::kNeck, 30.0f, 35.0f}, Region::Head, false, 0.0f},
    {{"upperarm_l", "lowerarm_l", 0.0f, 0.055f, 2.5f, 1, 75.0f, 45.0f}, Region::UpperArm, true, 0.0f},
    {{"lowerarm_l", "hand_l", 0.0f, 0.045f, 1.6f, 3, 70.0f, 25.0f}, Region::Forearm, true, 0.0f},
    {{"upperarm_r", "lowerarm_r", 0.0f, 0.055f, 2.5f, 1, 75.0f, 45.0f}, Region::UpperArm, false, 0.0f},
    {{"lowerarm_r", "hand_r", 0.0f, 0.045f, 1.6f, 5, 70.0f, 25.0f}, Region::Forearm, false, 0.0f},
    {{"thigh_l", "calf_l", 0.0f, 0.075f, 8.0f, 0, 50.0f, 18.0f}, Region::Thigh, true, 0.0f},
    {{"calf_l", "foot_l", 0.0f, 0.055f, 4.0f, 7, 60.0f, 8.0f}, Region::Calf, true, 0.0f},
    {{"thigh_r", "calf_r", 0.0f, 0.075f, 8.0f, 0, 50.0f, 18.0f}, Region::Thigh, false, 0.0f},
    {{"calf_r", "foot_r", 0.0f, 0.055f, 4.0f, 9, 60.0f, 8.0f}, Region::Calf, false, 0.0f},
    {{"neck_01", "head", 0.0f, 0.05f, 1.0f, 1, 30.0f, 30.0f}, Region::Neck, false, 0.0f},
    {{"hand_l", "middle_01_l", 0.05f, 0.04f, 0.5f, 4, 60.0f, 20.0f}, Region::Hand, true, 0.06f},
    {{"hand_r", "middle_01_r", 0.05f, 0.04f, 0.5f, 6, 60.0f, 20.0f}, Region::Hand, false, 0.06f},
    {{"foot_l", "ball_l", 0.03f, 0.045f, 1.0f, 8, 40.0f, 10.0f}, Region::Foot, true, 0.12f},
    {{"foot_r", "ball_r", 0.03f, 0.045f, 1.0f, 10, 40.0f, 10.0f}, Region::Foot, false, 0.12f},
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
struct Ranges { float Flex, Ext, LatIn, LatOut, TwistIn, TwistOut; };
Ranges RangesOf(const RagdollSettingsComponent& c, Region r) {
    switch (r) {
    case Region::Spine: return {c.SpineFlexMax, c.SpineExtMax, c.SpineLatIn, c.SpineLatOut, c.SpineTwistIn, c.SpineTwistOut};
    case Region::Head: return {c.HeadFlexMax, c.HeadExtMax, c.HeadLatIn, c.HeadLatOut, c.HeadTwistIn, c.HeadTwistOut};
    case Region::UpperArm: return {c.UpperArmFlexMax, c.UpperArmExtMax, c.UpperArmLatIn, c.UpperArmLatOut, c.UpperArmTwistIn, c.UpperArmTwistOut};
    case Region::Forearm: return {c.ForearmFlexMax, c.ForearmExtMax, c.ForearmLatIn, c.ForearmLatOut, c.ForearmTwistIn, c.ForearmTwistOut};
    case Region::Thigh: return {c.ThighFlexMax, c.ThighExtMax, c.ThighLatIn, c.ThighLatOut, c.ThighTwistIn, c.ThighTwistOut};
    case Region::Calf: return {c.CalfFlexMax, c.CalfExtMax, c.CalfLatIn, c.CalfLatOut, c.CalfTwistIn, c.CalfTwistOut};
    case Region::Neck: return {c.NeckFlexMax, c.NeckExtMax, c.NeckLatIn, c.NeckLatOut, c.NeckTwistIn, c.NeckTwistOut};
    case Region::Hand: return {c.HandFlexMax, c.HandExtMax, c.HandLatIn, c.HandLatOut, c.HandTwistIn, c.HandTwistOut};
    case Region::Foot: return {c.FootFlexMax, c.FootExtMax, c.FootLatIn, c.FootLatOut, c.FootTwistIn, c.FootTwistOut};
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
    const Region r = kRagDefs[i].R;
    const bool hinge = r == Region::Forearm || r == Region::Calf;
    const bool leftSide = kRagDefs[i].Left;
    const bool centre = r == Region::Spine || r == Region::Head || r == Region::Neck;
    const Ranges lim = RangesOf(cfg, r);
    // Neutral: a hinge's (and a wrist's) is the bone above it straight on, a foot's is level, a limb otherwise hangs, a spine stands.
    const glm::vec3 level = PerpNormalized(fwd, up);
    const glm::vec3 x = glm::normalize(hinge || r == Region::Hand ? pd : r == Region::Foot ? (glm::length(level) > 0.5f ? level : fwd) : centre ? up : -up);
    // The flexion direction: forward (back for a knee, up for an ankle: toes toward the shin); a hinge folds the way the animated
    // pose already bends when it does.
    glm::vec3 y = PerpNormalized(r == Region::Calf ? -fwd : r == Region::Foot ? up : fwd, x);
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
const NpcPartDef& NpcRagdollDefOf(int part) { return kRagDefs[std::clamp(part, 0, NpcRagdoll::kRagParts - 1)].D; }

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
    part = std::clamp(part, 0, kRagParts - 1);
    if (!cfg) return kRagDefs[part].D.Mass;
    switch (kRagDefs[part].R) {
    case Region::Pelvis: return cfg->PelvisMass;
    case Region::Spine: return cfg->SpineMass;
    case Region::Head: return cfg->HeadMass;
    case Region::UpperArm: return cfg->UpperArmMass;
    case Region::Forearm: return cfg->ForearmMass;
    case Region::Thigh: return cfg->ThighMass;
    case Region::Neck: return cfg->NeckMass;
    case Region::Hand: return cfg->HandMass;
    case Region::Foot: return cfg->FootMass;
    default: return cfg->CalfMass;
    }
}

float NpcRagdoll::PartFade(const RagdollSettingsComponent* cfg, int part) {
    part = std::clamp(part, 0, kRagParts - 1);
    if (!cfg) return kDriveFade;
    float scale = 1.0f;
    switch (kRagDefs[part].R) {
    case Region::Pelvis: scale = cfg->PelvisFadeScale; break;
    case Region::Spine: scale = cfg->SpineFadeScale; break;
    case Region::Head: scale = cfg->HeadFadeScale; break;
    case Region::UpperArm: scale = cfg->UpperArmFadeScale; break;
    case Region::Forearm: scale = cfg->ForearmFadeScale; break;
    case Region::Thigh: scale = cfg->ThighFadeScale; break;
    case Region::Neck: scale = cfg->NeckFadeScale; break;
    case Region::Hand: scale = cfg->HandFadeScale; break;
    case Region::Foot: scale = cfg->FootFadeScale; break;
    default: scale = cfg->CalfFadeScale; break;
    }
    return std::max(cfg->DriveFade * std::max(scale, 0.0f), 1e-3f);
}

float NpcRagdoll::DriveFadeTime() const {
    float t = 0.0f;
    for (int i = 0; i < kRagParts; ++i) t = std::max(t, m_PartFade[i]);
    return t;
}

// ---- The muscles ----------------------------------------------------------------------------------------------------------

NpcRagdollMotor::Group NpcRagdollMotor::GroupOf(int part) {
    switch (kRagDefs[std::clamp(part, 0, NpcRagdoll::kRagParts - 1)].R) {
    case Region::Thigh: case Region::Calf: case Region::Foot: return Group::Legs;
    case Region::Head: case Region::Neck: return Group::Neck;
    case Region::UpperArm: case Region::Forearm: case Region::Hand: return Group::Arms;
    default: return Group::Spine;
    }
}

namespace {
float GroupTime(const RagdollSettingsComponent& c, NpcRagdollMotor::Group g) {
    switch (g) {
    case NpcRagdollMotor::Group::Legs: return c.LegsToneTime;
    case NpcRagdollMotor::Group::Neck: return c.NeckToneTime;
    case NpcRagdollMotor::Group::Arms: return c.ArmsToneTime;
    default: return c.SpineToneTime;
    }
}
} // namespace

float NpcRagdollMotor::PartStrength(const RagdollSettingsComponent& cfg, int part, float t, int hitPart) {
    part = std::clamp(part, 0, NpcRagdoll::kRagParts - 1);
    if (part == 0) return 1.0f; // the pelvis has no joint
    const Group g = GroupOf(part);
    const bool legs = g == Group::Legs;
    const float hold = legs ? std::max(cfg.StaggerTime, 0.0f) : 0.0f;
    const float decay = std::max(GroupTime(cfg, g), 1e-3f);
    const float x = std::clamp((t - hold) / decay, 0.0f, 1.0f);
    const float curve = (1.0f - x) * (1.0f - x);
    float scale = legs ? cfg.StaggerLegStrength : 1.0f;
    // The struck joint takes the round instead of holding against it: the part's own (a hit pelvis: both hips; a hit head: the neck too).
    const bool struck = part == hitPart || (hitPart == 0 && (part == 7 || part == 9)) || (hitPart == 2 && part == NpcRagdoll::kNeck);
    if (struck) scale *= cfg.HitWeakness;
    const float residual = std::clamp(cfg.ToneResidual, 0.0f, 1.0f);
    return residual + (1.0f - residual) * curve * std::clamp(scale, 0.0f, 1.0f);
}

float NpcRagdollMotor::CollapseFlexion(const RagdollSettingsComponent& cfg, int part, bool forward) {
    switch (kRagDefs[std::clamp(part, 0, NpcRagdoll::kRagParts - 1)].R) {
    case Region::Thigh: return cfg.HipFlexCollapse;
    case Region::Calf: return cfg.KneeFlexCollapse;
    case Region::Spine: return forward ? cfg.SpineCurlCollapse : -0.4f * cfg.SpineCurlCollapse;
    case Region::Neck: return forward ? -0.75f * cfg.NeckCollapse : cfg.NeckCollapse;
    case Region::Head: return forward ? -0.4f * cfg.NeckCollapse : 0.4f * cfg.NeckCollapse;
    case Region::UpperArm: return forward ? cfg.ShoulderCollapse : 0.3f * cfg.ShoulderCollapse;
    case Region::Forearm: return forward ? cfg.ElbowCollapse : 0.8f * cfg.ElbowCollapse;
    default: return 0.0f;
    }
}

glm::quat NpcRagdollMotor::TargetAt(const RagdollSettingsComponent& cfg, int part, float t, bool forward) {
    const float x = std::clamp(t / std::max(cfg.CollapseBlendTime, 1e-3f), 0.0f, 1.0f);
    const float blend = x * x * (3.0f - 2.0f * x) * cfg.CollapseAmount;
    // The joint frame's +Z is the flexion axis (X, the bone, turns toward +Y, the flexion direction).
    return glm::angleAxis(glm::radians(CollapseFlexion(cfg, part, forward) * blend), glm::vec3(0.0f, 0.0f, 1.0f));
}

void NpcRagdollMotor::Begin(int id, const PhysicsWorld::RagdollPart* parts, const glm::mat4& root, const glm::vec3& fallDir, int hitPart,
                            const RagdollSettingsComponent& cfg) {
    m_Id = id;
    m_HitPart = hitPart;
    m_Cfg = std::make_shared<RagdollSettingsComponent>(cfg);
    m_Time = m_Settle = m_Still = 0.0f;
    m_AppliedSettle = -1.0f;
    m_Resting = false;
    m_Frames = parts && parts[1].Anatomical;
    {
        float p[3], q[4];
        m_Height0 = PhysicsWorld::GetRagdollPart(id, 0, p, q) ? std::max(p[1], 0.3f) : 1.0f;
    }
    const glm::vec3 fwd = glm::normalize(glm::vec3(root[2]));
    const glm::vec3 flat(fallDir.x, 0.0f, fallDir.z);
    m_Forward = glm::dot(flat, flat) < 1e-8f || glm::dot(flat, glm::vec3(fwd.x, 0.0f, fwd.z)) >= 0.0f;
    m_Strongest = 1.0f;
    Update(0.0f);
}

void NpcRagdollMotor::Update(float dt) {
    if (m_Id < 0 || !m_Cfg) return;
    const RagdollSettingsComponent& c = *m_Cfg;
    m_Time += std::max(dt, 0.0f);
    float lin = 0.0f, ang = 0.0f;
    if (!PhysicsWorld::RagdollMotion(m_Id, &lin, &ang)) return;
    // Settling: once the parts are slow the body heavies up (damping, friction, joint friction); it unwinds fast if something moves it again.
    if (dt > 0.0f) {
        float pp[3], pq[4];
        const bool down = m_Time >= 0.2f && PhysicsWorld::GetRagdollPart(m_Id, 0, pp, pq) && pp[1] < c.DownHeight * m_Height0;
        const bool slow = down || (m_Time >= c.SettleDelay && lin < c.SettleSpeed && ang < 4.0f * c.SettleSpeed);
        const float ramp = std::max(c.SettleRamp, 1e-3f);
        m_Settle = slow ? std::min(1.0f, m_Settle + dt / ramp) : std::max(0.0f, m_Settle - 2.0f * dt / ramp);
        // At rest: still for RestTime, then asleep (and nothing writes to it until it is hit).
        if (lin < c.RestSpeed && ang < 10.0f * c.RestSpeed) m_Still += dt; else m_Still = 0.0f;
        if (!m_Resting && m_Time >= c.SettleDelay && m_Still >= c.RestTime) {
            PhysicsWorld::RagdollSleep(m_Id);
            m_Resting = true;
        }
        if (m_Resting && !PhysicsWorld::RagdollAsleep(m_Id)) m_Resting = false; // woken by something else
    }
    if (m_Resting) return;
    if (m_Settle != m_AppliedSettle) {
        const float s = m_Settle;
        PhysicsWorld::SetRagdollDamping(m_Id, glm::mix(c.LinearDamping, c.SettleLinearDamping, s), glm::mix(c.AngularDamping, c.SettleAngularDamping, s));
        PhysicsWorld::SetRagdollFriction(m_Id, glm::mix(c.StaticFriction, c.SettleFriction, s), glm::mix(c.DynamicFriction, c.SettleFriction * 0.92f, s));
    }
    // The joints: strength (spring and damper, with the joint friction floor) and target, until the tone has decayed and the settle is in.
    float driveEnd = c.CollapseBlendTime;
    for (int i = 1; i < NpcRagdoll::kRagParts; ++i) {
        const Group g = GroupOf(i);
        driveEnd = std::max(driveEnd, (g == Group::Legs ? c.StaggerTime : 0.0f) + GroupTime(c, g));
    }
    if (m_Time <= driveEnd + 0.05f || m_Settle != m_AppliedSettle) {
        m_Strongest = 0.0f;
        const float floorDamp = glm::mix(c.JointFriction, c.SettleJointFriction, m_Settle);
        for (int i = 1; i < NpcRagdoll::kRagParts; ++i) {
            const float s = PartStrength(c, i, m_Time, m_HitPart);
            m_Strongest = std::max(m_Strongest, s);
            const bool distal = kRagDefs[i].R == Region::Hand || kRagDefs[i].R == Region::Foot;
            PhysicsWorld::SetRagdollPartDrive(m_Id, i, c.ToneStiffness * s * s, std::max({c.ToneDamping * s, floorDamp, distal ? c.DistalJointDamping : 0.0f}));
            if (m_Frames && m_Time <= driveEnd + 0.05f) {
                const glm::quat q = TargetAt(c, i, m_Time, m_Forward);
                const float r[4] = {q.x, q.y, q.z, q.w};
                PhysicsWorld::SetRagdollDriveTarget(m_Id, i, r);
            }
        }
    }
    m_AppliedSettle = m_Settle;
}

void NpcRagdollMotor::Wake() {
    m_Settle = 0.0f;
    m_Still = 0.0f;
    m_Resting = false;
    m_AppliedSettle = -1.0f;
}

namespace {
// The bones a snapshot holds: every part's start and end bone, and the neck; the last four are optional.
const char* const kSnapNames[NpcRagdoll::kSnapBones] = {"pelvis", "spine_02", "spine_03", "neck_01", "head", "upperarm_l", "lowerarm_l", "hand_l",
                                                        "upperarm_r", "lowerarm_r", "hand_r", "thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r",
                                                        "middle_01_l", "middle_01_r", "ball_l", "ball_r"};
constexpr int kRequiredSnapBones = 17;
} // namespace

void NpcRagdoll::Capture(const NpcBody& body, BoneSnapshot& out) {
    const glm::mat4 inv = glm::inverse(body.RootMatrix());
    out.Valid = true;
    for (int i = 0; i < kSnapBones; ++i) {
        glm::vec3 w;
        out.Has[i] = body.BoneWorld(kSnapNames[i], w);
        if (!out.Has[i]) {
            out.P[i] = glm::vec3(0.0f);
            if (i < kRequiredSnapBones) { out.Valid = false; return; }
            continue;
        }
        out.P[i] = glm::vec3(inv * glm::vec4(w, 1.0f));
    }
}

void NpcRagdoll::InheritVelocity(const glm::mat4* prevWorld, const glm::mat4* nowWorld, float dt, const RagdollSettingsComponent& cfg,
                                 PhysicsWorld::RagdollPart* parts) {
    if (dt < 0.002f || dt > 0.12f || cfg.LimbVelocityScale <= 0.0f) return;
    for (int i = 0; i < kRagParts; ++i) {
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
    if (c.PoweredRagdoll) { p.Grip = c.GripFloor; p.StabilizationThreshold = c.StabilizationThreshold; }
    return p;
}

bool NpcRagdoll::BuildParts(const std::function<bool(const char*, glm::vec3&)>& bone, const glm::mat4& root, const glm::vec3& velocity,
                            const RagdollSettingsComponent& cfg, PhysicsWorld::RagdollPart* parts, glm::mat4* partWorldOut) {
    glm::mat4 partWorld[kRagParts];
    glm::vec3 dir[kRagParts];
    glm::vec3 neck;
    const glm::vec3 left = glm::normalize(glm::vec3(root[0])), up = glm::normalize(glm::vec3(root[1])), fwd = glm::normalize(glm::vec3(root[2]));
    if (!bone("neck_01", neck)) neck = glm::vec3(0.0f);
    for (int i = 0; i < kRagParts; ++i) {
        const RagDef& rd = kRagDefs[i];
        const NpcPartDef& d = rd.D;
        glm::vec3 a, b(0.0f);
        if (!bone(d.Bone, a)) return false;
        if (d.End && !bone(d.End, b)) {
            if (rd.AltLen <= 0.0f) return false;
            // A rig without the hand's or foot's tip bone: a short stub along the forearm (hand) or forward (foot).
            b = a + (rd.R == Region::Hand ? dir[d.Parent] : fwd) * rd.AltLen;
        }
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
        if (rd.R == Region::Hand || rd.R == Region::Foot) p.JointDamping = cfg.DistalJointDamping;
        p.InertiaScale = cfg.InertiaScale * (rd.R == Region::Hand || rd.R == Region::Foot ? std::max(cfg.DistalInertiaScale, 0.05f) : 1.0f);
        if (cfg.ShapedTorsoInertia && (rd.R == Region::Pelvis || rd.R == Region::Spine)) {
            // A trunk is wider than it is deep: a box (the hips a little narrower than the shoulders), not a round capsule.
            p.InertiaHalfWidth = cfg.TorsoHalfWidth * (rd.R == Region::Pelvis ? 0.9f : 1.0f);
            p.InertiaHalfDepth = cfg.TorsoHalfDepth;
            p.InertiaLateral[0] = left.x; p.InertiaLateral[1] = left.y; p.InertiaLateral[2] = left.z;
        }
        partWorld[i] = PoseOf(s.Centre, s.Rotation);
        dir[i] = s.Rotation * glm::vec3(1, 0, 0);
    }
    if (cfg.AnatomicalLimits)
        for (int i = 1; i < kRagParts; ++i) FillAnatomical(i, cfg, dir[kRagDefs[i].D.Parent], dir[i], left, up, fwd, parts[i]);
    if (partWorldOut) for (int i = 0; i < kRagParts; ++i) partWorldOut[i] = partWorld[i];
    return true;
}

NpcRagdoll::~NpcRagdoll() { Stop(); }

void NpcRagdoll::Stop() {
    if (m_Id >= 0) PhysicsWorld::DestroyRagdoll(m_Id);
    m_Id = -1;
    m_Pieces.clear();
    m_Drive = 0.0f;
    m_Powered = false;
}

int NpcRagdoll::Launch(int id, const PhysicsWorld::RagdollPart* parts, const glm::mat4* partWorld, const glm::mat4& root, const glm::vec3& velocity,
                       const glm::vec3& impulse, const glm::vec3& point, int hitPart, const RagdollSettingsComponent& cfg, NpcRagdollMotor* motor) {
    // The round's shove, on the part it struck (else the one nearest where it did).
    int target = hitPart;
    if (target < 0 || target >= kRagParts) {
        target = 1;
        float best = 1e9f;
        for (int i = 0; i < kRagParts; ++i) {
            const float d2 = glm::length(glm::vec3(partWorld[i][3]) - point);
            if (d2 < best) { best = d2; target = i; }
        }
    }
    const float scale = motor ? std::max(cfg.HitImpulseScale, 0.0f) : 1.0f;
    // A light part (a forearm, a skull) shoved with the whole round's momentum would leave the body at 30 m/s and tear
    // its joints: the part takes what it can (6 m/s), the rest goes into the chest, so the body still moves as hard.
    float total = glm::length(impulse) * scale;
    if (motor && total > 1e-6f) {
        // The body's share pushes every part by mass (the whole soldier is carried along the shot); the rest is the struck part's.
        const float share = std::clamp(cfg.HitBodyShare, 0.0f, 1.0f);
        float mass = 0.0f;
        for (int i = 0; i < kRagParts; ++i) mass += PartMass(&cfg, i);
        const glm::vec3 dv = impulse / glm::length(impulse) * (total * share / std::max(mass, 1.0f));
        for (int i = 0; i < kRagParts; ++i) {
            const glm::vec3 j = dv * PartMass(&cfg, i);
            const float ji[3] = {j.x, j.y, j.z}, at[3] = {partWorld[i][3][0], partWorld[i][3][1], partWorld[i][3][2]};
            PhysicsWorld::RagdollImpulse(id, i, ji, at);
        }
        total *= 1.0f - share;
    }
    const float cap = PartMass(&cfg, target) * cfg.PartImpulseSpeed;
    const float onPart = std::min(total, cap);
    if (total > 1e-6f) {
        const glm::vec3 dir = impulse / glm::length(impulse);
        const glm::vec3 a = dir * onPart, rest = dir * std::min(total - onPart, PartMass(&cfg, 1) * cfg.ChestImpulseSpeed);
        const float ja[3] = {a.x, a.y, a.z}, at[3] = {point.x, point.y, point.z};
        PhysicsWorld::RagdollImpulse(id, target, ja, at);
        if (glm::dot(rest, rest) > 1e-8f && target != 1) {
            const float jr[3] = {rest.x, rest.y, rest.z}, centre[3] = {partWorld[1][3][0], partWorld[1][3][1], partWorld[1][3][2]};
            PhysicsWorld::RagdollImpulse(id, 1, jr, centre);
        }
    }
    if (motor) {
        // Muscle tone from here: the struck joint weak, the legs partial, the target going to the collapse; the way it is going to fall is
        // the body's momentum plus the round's.
        float mass = 0.0f;
        for (int i = 0; i < kRagParts; ++i) mass += PartMass(&cfg, i);
        motor->Begin(id, parts, root, velocity * mass + impulse * scale, target, cfg);
    }
    return target;
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
    PhysicsWorld::RagdollPart parts[kRagParts];
    glm::mat4 partWorld[kRagParts];
    if (!BuildParts([&](const char* name, glm::vec3& out) { return name && body.BoneWorld(name, out); }, root, velocity, cfg, parts, partWorld))
        return false;
    if (prev && prev->Valid) { // each part's own motion: its pose against the one before, both in the body's (fixed) root frame
        auto before = [&](const char* name, glm::vec3& out) {
            for (int k = 0; k < kSnapBones; ++k)
                if (std::strcmp(kSnapNames[k], name) == 0 && prev->Has[k]) { out = glm::vec3(root * glm::vec4(prev->P[k], 1.0f)); return true; }
            return false;
        };
        PhysicsWorld::RagdollPart prevParts[kRagParts];
        glm::mat4 prevWorld[kRagParts];
        if (BuildParts(before, root, velocity, cfg, prevParts, prevWorld)) InheritVelocity(prevWorld, partWorld, prevDt, cfg, parts);
    }
    const PhysicsWorld::RagdollParams bodyParams = BodyParams(cfg);
    m_Id = PhysicsWorld::CreateRagdoll((unsigned)entt::to_integral(body.Root()), parts, kRagParts, &bodyParams);
    if (m_Id < 0) return false;
    // Powered at first: the joints hold the death pose (their drive targets are the pose they were built in).
    m_Stiffness = cfg.DriveStiffness; m_Damping = cfg.DriveDamping; m_DistalDamping = cfg.DistalJointDamping;
    m_Powered = cfg.PoweredRagdoll;
    for (int i = 0; i < kRagParts; ++i)
        m_PartFade[i] = m_Powered ? (NpcRagdollMotor::GroupOf(i) == NpcRagdollMotor::Group::Legs ? cfg.StaggerTime : 0.0f) + (NpcRagdollMotor::GroupOf(i) == NpcRagdollMotor::Group::Legs ? cfg.LegsToneTime : NpcRagdollMotor::GroupOf(i) == NpcRagdollMotor::Group::Neck ? cfg.NeckToneTime : NpcRagdollMotor::GroupOf(i) == NpcRagdollMotor::Group::Arms ? cfg.ArmsToneTime : cfg.SpineToneTime)
                                  : PartFade(&cfg, i);
    if (!m_Powered) PhysicsWorld::SetRagdollDrive(m_Id, m_Stiffness, m_Damping);
    m_Drive = 1.0f;
    m_Time = 0.0f;
    m_Settled = false;
    m_RootInv = glm::inverse(root);
    // Each piece's bones, relative to the part they ride on.
    for (const auto& mp : models) {
        PieceBones pb;
        pb.M = mp;
        pb.Node.assign(kRagParts, -1);
        pb.Off.assign(kRagParts, glm::mat4(1.0f));
        bool any = false;
        for (int i = 0; i < kRagParts; ++i) {
            const int n = mp->NodeIndex(kRagDefs[i].D.Bone);
            glm::mat4 g(1.0f);
            if (n < 0 || !mp->NodeTransform(kRagDefs[i].D.Bone, g)) continue;
            pb.Node[(size_t)i] = n;
            pb.Off[(size_t)i] = glm::inverse(partWorld[i]) * (root * g);
            any = true;
        }
        // The bones between parts: spine_01 / spine_02 sit a third and two thirds of the way from the pelvis to the chest (by where
        // the animated pose has them), the clavicles ride the chest.
        auto link = [&](const char* name, int A, int B) {
            const int n = mp->NodeIndex(name);
            glm::mat4 g(1.0f);
            if (n < 0 || !mp->NodeTransform(name, g)) return;
            const glm::mat4 world = root * g;
            float t = 0.0f;
            if (A != B) {
                const glm::vec3 pa(partWorld[A][3]), ab = glm::vec3(partWorld[B][3]) - pa;
                const float l2 = glm::dot(ab, ab);
                t = l2 > 1e-8f ? std::clamp(glm::dot(glm::vec3(world[3]) - pa, ab) / l2, 0.0f, 1.0f) : 0.0f;
            }
            pb.Links.push_back({n, A, B, t, glm::inverse(partWorld[A]) * world, glm::inverse(partWorld[B]) * world});
        };
        link("spine_01", 0, 1);
        link("spine_02", 0, 1);
        link("clavicle_l", 1, 1);
        link("clavicle_r", 1, 1);
        if (any) m_Pieces.push_back(std::move(pb));
    }
    Launch(m_Id, parts, partWorld, root, velocity, impulse, point, hitPart, cfg, m_Powered ? &m_Motor : nullptr);
    m_Drive = m_Powered ? m_Motor.Strength() : m_Drive;
    return true;
}

void NpcRagdoll::Update(float dt) {
    if (m_Id < 0) return;
    if (m_Powered) {
        m_Motor.Update(dt);
        m_Drive = m_Motor.Strength();
    }
    // The drives fade out, each region on its own clock: stiff at the moment of death, limp a quarter second on (by default).
    if (!m_Powered && m_Drive > 0.0f && dt > 0.0f) {
        m_Time += dt;
        m_Drive = 0.0f;
        for (int i = 1; i < kRagParts; ++i) {
            const float f = DriveAt(m_Time, m_PartFade[i]);
            m_Drive = std::max(m_Drive, f);
            const bool distal = kRagDefs[i].R == Region::Hand || kRagDefs[i].R == Region::Foot;
            PhysicsWorld::SetRagdollPartDrive(m_Id, i, m_Stiffness * f * f, std::max(m_Damping * f * f, distal ? m_DistalDamping : 0.0f));
        }
    }
    // A body at rest stays as it lies: one last write once it sleeps, then nothing per frame.
    const bool asleep = Asleep();
    if (asleep && m_Settled && (m_Powered || m_Drive <= 0.0f)) return;
    m_Settled = asleep;
    glm::mat4 partWorld[kRagParts];
    for (int i = 0; i < kRagParts; ++i) {
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
        for (int i = 0; i < kRagParts; ++i)
            if (pb.Node[(size_t)i] >= 0) {
                const glm::mat4 want = m_RootInv * partWorld[i] * pb.Off[(size_t)i];
                targets.push_back({pb.Node[(size_t)i], glm::vec3(want[3]), IK::Rotation(want)});
            }
        for (const PieceBones::Link& l : pb.Links) {
            const glm::mat4 a = m_RootInv * partWorld[l.A] * l.OffA, b = m_RootInv * partWorld[l.B] * l.OffB;
            targets.push_back({l.Node, glm::mix(glm::vec3(a[3]), glm::vec3(b[3]), l.T), glm::normalize(glm::slerp(IK::Rotation(a), IK::Rotation(b), l.T))});
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
    PhysicsWorld::RagdollImpulse(m_Id, std::clamp(part, 0, kRagParts - 1), j, at);
    m_Settled = false; // awake again: the pose follows the parts from here
    if (m_Powered) m_Motor.Wake();
}

glm::vec3 NpcRagdoll::PartPosition(int part) const {
    float p[3], q[4];
    if (m_Id < 0 || !PhysicsWorld::GetRagdollPart(m_Id, part, p, q)) return glm::vec3(0.0f);
    return glm::vec3(p[0], p[1], p[2]);
}

glm::vec3 NpcRagdoll::Root() const { return PartPosition(0); }

// ---- Hit flinch --------------------------------------------------------------------------------------------------------

namespace {
constexpr float kFlinchSettle = 5.5f; // the spring's omega * duration: the kick is ~6% of its peak at `duration`
const char* const kFlinchBones[NpcFlinch::kBones] = {"spine_01", "spine_02", "spine_03", "neck_01", "head", "upperarm_l", "upperarm_r",
                                                     "lowerarm_l", "lowerarm_r", "thigh_l", "thigh_r", "calf_l", "calf_r"};
enum FlinchBone { SP1, SP2, SP3, NECK, HEAD, UAL, UAR, LAL, LAR, THL, THR, CL, CR };
struct FlinchRegion { int Bone[3]; float Weight[3]; float Scale; };
// By hitbox part: the bones it kicks, their shares, and the region's own scale (a head or a forearm moves further than a chest).
const FlinchRegion kFlinchRegions[NpcRagdoll::kParts] = {
    {{SP1, -1, -1}, {0.6f, 0.0f, 0.0f}, 0.7f},         // pelvis
    {{SP2, SP3, -1}, {0.45f, 0.55f, 0.0f}, 0.8f},      // chest
    {{NECK, HEAD, -1}, {0.4f, 0.6f, 0.0f}, 1.3f},      // head
    {{UAL, SP3, -1}, {0.8f, 0.2f, 0.0f}, 1.4f},        // upper arm l
    {{LAL, UAL, SP3}, {0.8f, 0.3f, 0.1f}, 1.5f},       // forearm l
    {{UAR, SP3, -1}, {0.8f, 0.2f, 0.0f}, 1.4f},        // upper arm r
    {{LAR, UAR, SP3}, {0.8f, 0.3f, 0.1f}, 1.5f},       // forearm r
    {{THL, SP1, -1}, {0.8f, 0.2f, 0.0f}, 1.2f},        // thigh l
    {{CL, THL, -1}, {0.8f, 0.3f, 0.0f}, 1.4f},         // calf l
    {{THR, SP1, -1}, {0.8f, 0.2f, 0.0f}, 1.2f},        // thigh r
    {{CR, THR, -1}, {0.8f, 0.3f, 0.0f}, 1.4f},         // calf r
};
} // namespace

const char* NpcFlinch::BoneName(int bone) { return kFlinchBones[std::clamp(bone, 0, kBones - 1)]; }

float NpcFlinch::Curve(float age, float duration) {
    if (age <= 0.0f || duration <= 1e-4f) return 0.0f;
    const float x = kFlinchSettle * age / duration; // omega * t; t exp(-t) peaks at 1
    return x * std::exp(1.0f - x);
}

int NpcFlinch::PartBones(int part, int outBone[3], float outWeight[3]) {
    const FlinchRegion& r = kFlinchRegions[std::clamp(part, 0, NpcRagdoll::kParts - 1)];
    int n = 0;
    for (int i = 0; i < 3; ++i)
        if (r.Bone[i] >= 0) { outBone[n] = r.Bone[i]; outWeight[n] = r.Weight[i]; ++n; }
    return n;
}

float NpcFlinch::PeakAngle(const RagdollSettingsComponent& cfg, float damage, int part, float weight) {
    const float k = std::clamp(damage / std::max(cfg.FlinchDamageRef, 1.0f), 0.3f, 2.0f);
    const float deg = cfg.FlinchAngle * kFlinchRegions[std::clamp(part, 0, NpcRagdoll::kParts - 1)].Scale * weight * k;
    return glm::radians(std::min(deg, cfg.FlinchMaxAngle));
}

void NpcFlinch::Hit(int part, const glm::vec3& boneDir, const glm::vec3& dir, float damage, float now, const RagdollSettingsComponent& cfg) {
    if (!cfg.HitFlinch || cfg.FlinchAngle <= 0.0f || damage <= 0.0f || part < 0 || part >= NpcRagdoll::kParts) return;
    const float bl = glm::length(boneDir), dl = glm::length(dir);
    if (bl < 1e-5f || dl < 1e-5f) return;
    glm::vec3 axis = glm::cross(boneDir / bl, dir / dl);
    const float al = glm::length(axis);
    if (al < 0.05f) return; // along the bone: nothing to turn
    // Weaker as the round runs along the bone (sin of the angle between them is the lever).
    const float lever = std::min(al, 1.0f);
    axis /= al;
    int bone[3];
    float weight[3];
    const int n = PartBones(part, bone, weight);
    for (int i = 0; i < n; ++i)
        m_Kicks.push_back({bone[i], axis, PeakAngle(cfg, damage, part, weight[i]) * lever, now, std::max(cfg.FlinchDuration, 0.05f)});
    m_MaxAngle = glm::radians(std::max(cfg.FlinchMaxAngle, 1.0f));
    if (m_Kicks.size() > 24) m_Kicks.erase(m_Kicks.begin(), m_Kicks.begin() + (m_Kicks.size() - 24));
}

void NpcFlinch::Hit(const NpcBody& body, int part, const glm::vec3& dirWorld, float damage, float now, const RagdollSettingsComponent& cfg) {
    if (part < 0 || part >= NpcRagdoll::kParts) return;
    const glm::mat4 root = body.RootMatrix();
    const glm::mat3 toRoot = glm::transpose(glm::mat3(glm::normalize(glm::vec3(root[0])), glm::normalize(glm::vec3(root[1])), glm::normalize(glm::vec3(root[2]))));
    // The part's bone direction: a limb's from its bone to the next, the trunk and head's straight up.
    glm::vec3 boneDir(0.0f, 1.0f, 0.0f);
    const NpcPartDef& d = NpcPartDefOf(part);
    glm::vec3 a, b;
    if (part >= 3 && d.End && body.BoneWorld(d.Bone, a) && body.BoneWorld(d.End, b) && glm::length(b - a) > 1e-4f) boneDir = toRoot * (b - a);
    Hit(part, boneDir, toRoot * dirWorld, damage, now, cfg);
}

bool NpcFlinch::Active(float now) const {
    for (const Kick& k : m_Kicks)
        if (now - k.Start < k.Duration * 1.5f) return true;
    return false;
}

void NpcFlinch::Rotations(float now, glm::vec3 out[kBones]) const {
    for (int i = 0; i < kBones; ++i) out[i] = glm::vec3(0.0f);
    for (const Kick& k : m_Kicks) out[k.Bone] += k.Axis * (k.Peak * Curve(now - k.Start, k.Duration));
    for (int i = 0; i < kBones; ++i) {
        const float l = glm::length(out[i]);
        if (l > m_MaxAngle) out[i] *= m_MaxAngle / l;
    }
}

void NpcFlinch::Apply(NpcBody& body, float now) const {
    glm::vec3 rot[kBones];
    Rotations(now, rot);
    thread_local std::vector<int> parents;
    thread_local std::vector<glm::mat4> globals;
    thread_local std::vector<LocalTRS> pose;
    struct Item { int Node; glm::quat Q; };
    std::vector<Item> items;
    for (const auto& mp : body.Models()) {
        Model& m = *mp;
        pose = m.AppliedLocalPose();
        if ((int)pose.size() != m.NodeCount()) continue;
        items.clear();
        for (int i = 0; i < kBones; ++i) {
            const float l = glm::length(rot[i]);
            if (l < 1e-5f) continue;
            const int node = m.NodeIndex(kFlinchBones[i]);
            if (node >= 0) items.push_back({node, glm::angleAxis(l, rot[i] / l)});
        }
        if (items.empty()) continue;
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.Node < b.Node; }); // parents first
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[(size_t)i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        for (const Item& it : items) IK::OffsetBone(pose, parents, globals, it.Node, glm::vec3(0.0f), it.Q, IK::Position(globals[(size_t)it.Node]));
        m.ApplyLocalPose(pose);
    }
}
