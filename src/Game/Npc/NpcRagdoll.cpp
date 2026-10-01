#include "NpcRagdoll.h"

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
#include <string>

namespace {

// Masses add up to ~75 kg; limits are generous rather than anatomical, so a body settles naturally.
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

// The joints' slerp drives at the moment of death (acceleration units: independent of the parts' masses).
constexpr float kDriveStiffness = 700.0f;
constexpr float kDriveDamping = 60.0f;

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

NpcRagdoll::~NpcRagdoll() { Stop(); }

void NpcRagdoll::Stop() {
    if (m_Id >= 0) PhysicsWorld::DestroyRagdoll(m_Id);
    m_Id = -1;
    m_Pieces.clear();
    m_Drive = 0.0f;
}

bool NpcRagdoll::Start(NpcBody& body, const glm::vec3& velocity, const glm::vec3& impulse, const glm::vec3& point, int hitPart) {
    Stop();
    const auto& models = body.Models();
    if (models.empty()) return false;
    const glm::mat4 root = body.RootMatrix();
    // Bone world positions from the driver's finished pose.
    auto bone = [&](const char* name, glm::vec3& out) { return name && body.BoneWorld(name, out); };
    PhysicsWorld::RagdollPart parts[kParts];
    glm::mat4 partWorld[kParts];
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
        p.Parent = d.Parent;
        p.Position[0] = s.Centre.x; p.Position[1] = s.Centre.y; p.Position[2] = s.Centre.z;
        p.Rotation[0] = s.Rotation.x; p.Rotation[1] = s.Rotation.y; p.Rotation[2] = s.Rotation.z; p.Rotation[3] = s.Rotation.w;
        p.Radius = s.Radius;
        p.HalfLength = s.HalfLength;
        p.Mass = d.Mass;
        p.Anchor[0] = a.x; p.Anchor[1] = a.y; p.Anchor[2] = a.z;
        p.SwingDeg = d.Swing;
        p.TwistDeg = d.Twist;
        p.Velocity[0] = velocity.x; p.Velocity[1] = velocity.y; p.Velocity[2] = velocity.z;
        partWorld[i] = PoseOf(s.Centre, s.Rotation);
    }
    m_Id = PhysicsWorld::CreateRagdoll((unsigned)entt::to_integral(body.Root()), parts, kParts);
    if (m_Id < 0) return false;
    // Powered at first: the joints hold the death pose (their drive targets are the pose they were built in).
    PhysicsWorld::SetRagdollDrive(m_Id, kDriveStiffness, kDriveDamping);
    m_Drive = 1.0f;
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
    const float cap = kDefs[target].Mass * 6.0f;
    const float onPart = std::min(total, cap);
    if (total > 1e-6f) {
        const glm::vec3 dir = impulse / total;
        const glm::vec3 a = dir * onPart, rest = dir * std::min(total - onPart, kDefs[1].Mass * 5.0f);
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
    // The drives fade out: stiff at the moment of death, limp a quarter second on.
    if (m_Drive > 0.0f && dt > 0.0f) {
        m_Drive = std::max(0.0f, m_Drive - dt / kDriveFade);
        const float k = m_Drive * m_Drive;
        PhysicsWorld::SetRagdollDrive(m_Id, kDriveStiffness * k, kDriveDamping * k);
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
