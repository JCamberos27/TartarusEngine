#include "NpcHitboxes.h"

#include "IK.h"
#include "Model.h"
#include "NpcBody.h"

#include <glm/gtc/matrix_transform.hpp>

NpcHitboxes::~NpcHitboxes() { Stop(); }

void NpcHitboxes::Stop() {
    if (m_Id >= 0) PhysicsWorld::DestroyNpcHitboxes(m_Id);
    m_Id = -1;
    m_Active = m_HavePose = false;
}

bool NpcHitboxes::Start(const NpcBody& body, PhysicsWorld::CharacterId capsule) {
    Stop();
    const Model* m = body.DriverModel();
    if (!m || capsule == PhysicsWorld::kNoCharacter) return false;
    m_Neck = m->NodeIndex("neck_01");
    for (int i = 0; i < NpcRagdoll::kParts; ++i) {
        const NpcPartDef& d = NpcPartDefOf(i);
        m_Node[i] = m->NodeIndex(d.Bone);
        m_End[i] = d.End ? m->NodeIndex(d.End) : -1;
        if (m_Node[i] < 0 || (d.End && m_End[i] < 0)) return false;
    }
    m_Parents.resize((size_t)m->NodeCount());
    for (int i = 0; i < m->NodeCount(); ++i) m_Parents[(size_t)i] = m->NodeParent(i);
    std::vector<char> needed((size_t)m->NodeCount(), 0);
    auto mark = [&](int n) {
        for (; n >= 0 && !needed[(size_t)n]; n = m_Parents[(size_t)n]) needed[(size_t)n] = 1;
    };
    mark(m_Neck);
    for (int i = 0; i < NpcRagdoll::kParts; ++i) { mark(m_Node[i]); mark(m_End[i]); }
    m_Path.clear();
    for (int i = 0; i < m->NodeCount(); ++i)
        if (needed[(size_t)i]) m_Path.push_back(i);
    m_Globals.resize((size_t)m->NodeCount());
    Pose(body);
    if (!m_HavePose) return false;
    PhysicsWorld::HitCapsule caps[NpcRagdoll::kParts];
    for (int i = 0; i < NpcRagdoll::kParts; ++i) {
        caps[i].Radius = m_Local[i].Radius * kFatten;
        caps[i].HalfLength = m_Local[i].HalfLength;
    }
    m_Id = PhysicsWorld::CreateNpcHitboxes(capsule, caps, NpcRagdoll::kParts);
    if (m_Id < 0) return false;
    m_Active = true;
    Place(body);
    return true;
}

void NpcHitboxes::SetActive(bool active) {
    if (m_Id < 0 || active == m_Active) return;
    m_Active = active;
    PhysicsWorld::SetNpcHitboxesActive(m_Id, active);
}

void NpcHitboxes::Pose(const NpcBody& body) {
    const Model* m = body.DriverModel();
    if (!m || m_Path.empty() || (int)m->AppliedLocalPose().size() != m->NodeCount()) return;
    const auto& pose = m->AppliedLocalPose();
    for (int i : m_Path) {
        const int p = m_Parents[(size_t)i];
        const glm::mat4 local = pose[(size_t)i].ToMatrix();
        m_Globals[(size_t)i] = p >= 0 ? AffineMul(m_Globals[(size_t)p], local) : local;
    }
    const glm::vec3 neck = m_Neck >= 0 ? IK::Position(m_Globals[(size_t)m_Neck]) : glm::vec3(0.0f);
    for (int i = 0; i < NpcRagdoll::kParts; ++i) {
        const NpcPartDef& d = NpcPartDefOf(i);
        const glm::vec3 a = IK::Position(m_Globals[(size_t)m_Node[i]]);
        const glm::vec3 b = m_End[i] >= 0 ? IK::Position(m_Globals[(size_t)m_End[i]]) : glm::vec3(0.0f);
        m_Local[i] = NpcPartShapeOf(d, a, b, neck);
    }
    m_HavePose = true;
    if (m_Id >= 0) Place(body);
}

void NpcHitboxes::Follow(const NpcBody& body) {
    if (m_Id >= 0 && m_HavePose) Place(body);
}

glm::vec3 NpcHitboxes::Centre(const NpcBody& body, int part) const {
    if (part < 0 || part >= NpcRagdoll::kParts || !m_HavePose) return glm::vec3(0.0f);
    return glm::vec3(body.RootMatrix() * glm::vec4(m_Local[part].Centre, 1.0f));
}

void NpcHitboxes::Place(const NpcBody& body) {
    const glm::mat4 root = body.RootMatrix();
    const glm::quat rootRot = glm::quat_cast(glm::mat3(root));
    PhysicsWorld::HitCapsule caps[NpcRagdoll::kParts];
    for (int i = 0; i < NpcRagdoll::kParts; ++i) {
        const glm::vec3 c = glm::vec3(root * glm::vec4(m_Local[i].Centre, 1.0f));
        const glm::quat q = glm::normalize(rootRot * m_Local[i].Rotation);
        caps[i].Position[0] = c.x; caps[i].Position[1] = c.y; caps[i].Position[2] = c.z;
        caps[i].Rotation[0] = q.x; caps[i].Rotation[1] = q.y; caps[i].Rotation[2] = q.z; caps[i].Rotation[3] = q.w;
    }
    PhysicsWorld::SetNpcHitboxPoses(m_Id, caps, NpcRagdoll::kParts);
}
