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

struct PartDef {
    const char* Bone;
    const char* End;     // the bone the capsule reaches to (null = along the parent direction)
    float Extend;        // metres past End (a hand, a skull)
    float Radius, Mass;
    int Parent;
    float Swing, Twist;
};
// Masses add up to ~75 kg; limits are generous rather than anatomical, so a body settles naturally.
const PartDef kDefs[NpcRagdoll::kParts] = {
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

glm::mat4 PoseOf(const glm::vec3& p, const glm::quat& q) { return glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q); }

} // namespace

NpcRagdoll::~NpcRagdoll() { Stop(); }

void NpcRagdoll::Stop() {
    if (m_Id >= 0) PhysicsWorld::DestroyRagdoll(m_Id);
    m_Id = -1;
    m_Pieces.clear();
}

bool NpcRagdoll::Start(NpcBody& body, const glm::vec3& velocity, const glm::vec3& impulse, const glm::vec3& point) {
    Stop();
    const auto& models = body.Models();
    if (models.empty()) return false;
    const glm::mat4 root = body.RootMatrix();
    // Bone world positions from the driver's finished pose.
    auto bone = [&](const char* name, glm::vec3& out) { return name && body.BoneWorld(name, out); };
    PhysicsWorld::RagdollPart parts[kParts];
    glm::mat4 partWorld[kParts];
    for (int i = 0; i < kParts; ++i) {
        const PartDef& d = kDefs[i];
        glm::vec3 a, b;
        if (!bone(d.Bone, a)) return false;
        if (d.End) {
            if (!bone(d.End, b)) return false;
        } else {
            glm::vec3 neck;
            if (!bone("neck_01", neck)) neck = a - glm::vec3(0, 0.1f, 0);
            b = a + glm::normalize(a - neck + glm::vec3(0, 1e-4f, 0)) * 0.01f;
        }
        glm::vec3 dir = b - a;
        float len = glm::length(dir);
        dir = len > 1e-4f ? dir / len : glm::vec3(0, 1, 0);
        len += d.Extend;
        b = a + dir * len;
        const glm::quat q = glm::rotation(glm::vec3(1, 0, 0), dir);
        const glm::vec3 c = 0.5f * (a + b);
        auto& p = parts[i];
        p.Parent = d.Parent;
        p.Position[0] = c.x; p.Position[1] = c.y; p.Position[2] = c.z;
        p.Rotation[0] = q.x; p.Rotation[1] = q.y; p.Rotation[2] = q.z; p.Rotation[3] = q.w;
        p.Radius = d.Radius;
        p.HalfLength = std::max(0.01f, 0.5f * len - d.Radius * 0.5f);
        p.Mass = d.Mass;
        p.Anchor[0] = a.x; p.Anchor[1] = a.y; p.Anchor[2] = a.z;
        p.SwingDeg = d.Swing;
        p.TwistDeg = d.Twist;
        p.Velocity[0] = velocity.x; p.Velocity[1] = velocity.y; p.Velocity[2] = velocity.z;
        partWorld[i] = PoseOf(c, q);
    }
    m_Id = PhysicsWorld::CreateRagdoll((unsigned)entt::to_integral(body.Root()), parts, kParts);
    if (m_Id < 0) return false;
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
    // The round's shove, on the part nearest where it struck.
    int nearest = 1;
    float best = 1e9f;
    for (int i = 0; i < kParts; ++i) {
        const float d2 = glm::length(glm::vec3(partWorld[i][3]) - point);
        if (d2 < best) { best = d2; nearest = i; }
    }
    const float j[3] = {impulse.x, impulse.y, impulse.z}, at[3] = {point.x, point.y, point.z};
    PhysicsWorld::RagdollImpulse(m_Id, nearest, j, at);
    return true;
}

void NpcRagdoll::Update() {
    if (m_Id < 0) return;
    glm::mat4 partWorld[kParts];
    for (int i = 0; i < kParts; ++i) {
        float p[3], q[4];
        if (!PhysicsWorld::GetRagdollPart(m_Id, i, p, q)) return;
        partWorld[i] = PoseOf(glm::vec3(p[0], p[1], p[2]), glm::quat(q[3], q[0], q[1], q[2]));
    }
    std::vector<int> parents;
    std::vector<glm::mat4> globals;
    std::vector<std::pair<int, int>> order; // (node, part), parents first
    for (PieceBones& pb : m_Pieces) {
        Model& m = *pb.M;
        IK::Pose pose = m.AppliedLocalPose();
        if ((int)pose.size() != m.NodeCount()) m.BindLocalPose(pose);
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[(size_t)i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        order.clear();
        for (int i = 0; i < kParts; ++i)
            if (pb.Node[(size_t)i] >= 0) order.push_back({pb.Node[(size_t)i], i});
        std::sort(order.begin(), order.end());
        for (const auto& [node, part] : order) {
            const glm::mat4 want = m_RootInv * partWorld[part] * pb.Off[(size_t)part];
            IK::SetGlobal(pose, parents, globals, node, glm::vec3(want[3]), IK::Rotation(want));
        }
        m.ApplyLocalPose(pose);
    }
}

bool NpcRagdoll::Asleep() const { return m_Id < 0 || PhysicsWorld::RagdollAsleep(m_Id); }

glm::vec3 NpcRagdoll::Root() const {
    float p[3], q[4];
    if (m_Id < 0 || !PhysicsWorld::GetRagdollPart(m_Id, 0, p, q)) return glm::vec3(0.0f);
    return glm::vec3(p[0], p[1], p[2]);
}
