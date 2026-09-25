#include "FirstPersonBody.h"

#include "Camera.h"
#include "Components.h"
#include "Log.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "World.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace {

// Model space: the Quantum / UE mannequin family faces +Z with +Y up, so its right is -X.
constexpr glm::vec3 kForward{0.0f, 0.0f, 1.0f};
constexpr glm::vec3 kRight{-1.0f, 0.0f, 0.0f};

glm::quat YawRotation(float yaw) { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }

// Frame-rate independent approach toward a target over `seconds` (0 = snap).
float Follow(float dt, float seconds) { return seconds > 1e-4f ? 1.0f - std::exp(-dt / seconds) : 1.0f; }

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

} // namespace

float FirstPersonBodyYaw(const glm::vec3& front, float fallback) {
    const glm::vec2 flat(front.x, front.z);
    if (glm::dot(flat, flat) < 1e-8f) return fallback;
    return std::atan2(front.x, front.z); // rotating +Z by this about +Y gives (sin, 0, cos)
}

glm::vec2 FirstPersonBodyLocalMove(const glm::vec3& worldVelocity, float yaw) {
    const glm::quat r = YawRotation(yaw);
    const glm::vec3 v(worldVelocity.x, 0.0f, worldVelocity.z);
    return {glm::dot(v, r * kRight), glm::dot(v, r * kForward)};
}

glm::vec3 FirstPersonBodyEye(const glm::vec3& restHead, const glm::vec3& head, float bob, const glm::vec3& offset) {
    const float b = std::clamp(bob, 0.0f, 1.0f);
    return restHead + (head - restHead) * b + kRight * offset.x + glm::vec3(0.0f, offset.y, 0.0f) + kForward * offset.z;
}

void FirstPersonBody::Fail(const std::string& message) {
    m_LastError = message;
    Log::Warn("First Person Body: " + message);
    m_Body = entt::null;
}

bool FirstPersonBody::Start(World& world, Player& player) {
    *this = FirstPersonBody{};
    auto& reg = world.Registry;
    auto bodies = reg.view<FirstPersonBodyComponent>(entt::exclude<InactiveTag>);
    if (bodies.begin() == bodies.end()) return false;
    const entt::entity body = *bodies.begin(); // the first; a scene has one player
    const auto& cfg = reg.get<FirstPersonBodyComponent>(body);

    // The pieces: the root itself and its children, each a rigged model on the one skeleton.
    auto rigged = [&](entt::entity e) {
        const auto* rc = reg.try_get<RenderableComponent>(e);
        return rc && rc->ModelRef && rc->ModelRef->NodeCount() > 0;
    };
    std::vector<entt::entity> pieces;
    if (rigged(body)) pieces.push_back(body);
    if (const auto* h = reg.try_get<HierarchyComponent>(body))
        for (entt::entity child : h->Children)
            if (reg.valid(child) && !reg.all_of<InactiveTag>(child) && rigged(child)) pieces.push_back(child);
    // The first piece with an Animator Controller drives; the others follow it.
    for (entt::entity e : pieces)
        if (reg.all_of<AnimatorControllerComponent>(e)) { m_Driver = e; break; }
    if (pieces.empty()) { Fail("needs rigged body pieces (the object's own model, or child objects with models)."); return false; }
    if (m_Driver == entt::null) {
        Fail("needs an Animator Controller on one of its pieces (e.g. animations/fps_body_locomotion.controller).");
        return false;
    }
    m_Body = body;

    // The clips' travel comes out of the pose and is handed to the Player - never applied to
    // an object: this class places the body itself. Turns stay in the pose: the view owns the yaw.
    auto& ac = reg.get<AnimatorControllerComponent>(m_Driver);
    ac.RootMotion.Mode = (int)RootMotionMode::InPlace;
    ac.RootMotion.Rotation = false;
    ac.RootMotion.Vertical = false;

    Model& model = *reg.get<RenderableComponent>(m_Driver).ModelRef;
    m_HeadNode = model.NodeIndex(cfg.HeadBone);
    if (m_HeadNode < 0)
        Log::Warn("First Person Body: the body has no bone '" + cfg.HeadBone + "' - the camera stays at the eye height.");
    else
        m_RestHead = glm::vec3(model.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, m_HeadNode)[3]);

    auto names = [](const std::string& csv) {
        std::vector<std::string> out;
        std::stringstream list(csv);
        for (std::string name; std::getline(list, name, ',');)
            if (!(name = Trim(name)).empty()) out.push_back(name);
        return out;
    };
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::vector<std::string> hiddenBones = names(cfg.HiddenBones), hiddenParts = names(cfg.HiddenParts);
    int shadowOnly = 0;
    for (entt::entity e : pieces) {
        Model* m = reg.get<RenderableComponent>(e).ModelRef.get();
        std::vector<int> nodes;
        for (const std::string& b : hiddenBones) {
            const int n = m->NodeIndex(b);
            if (n >= 0) nodes.push_back(n);
            else if (e == m_Driver) Log::Warn("First Person Body: Hidden Bones names '" + b + "', which the body doesn't have.");
        }
        m->SetHiddenNodes(nodes);
        m_Models.push_back(reg.get<RenderableComponent>(e).ModelRef);
        if (e != m_Driver)
            if (auto* pac = reg.try_get<AnimatorControllerComponent>(e)) pac->Driver = m_Driver;
        // A hidden piece still casts its shadow; the camera is inside the head.
        const std::string name = lower(reg.all_of<NameComponent>(e) ? reg.get<NameComponent>(e).Name : std::string());
        for (const std::string& part : hiddenParts)
            if (e != body && name.find(lower(part)) != std::string::npos) {
                reg.get<RenderableComponent>(e).CastShadows = RenderableComponent::ShadowCasting::ShadowsOnly;
                ++shadowOnly;
                break;
            }
    }

    // The input asks the blend tree for speeds its clips have.
    m_RunSpeed = std::max(cfg.RunSpeed, 0.01f);
    player.MoveSpeed = m_RunSpeed;
    player.SprintMultiplier = std::max(cfg.SprintSpeed, 0.01f) / m_RunSpeed;

    Log::Info("First Person Body: '" + (reg.all_of<NameComponent>(body) ? reg.get<NameComponent>(body).Name : std::string("body")) +
              "' is the player's body - " + std::to_string(pieces.size()) + " pieces, " + std::to_string(shadowOnly) +
              " shadow-only, root motion at responsiveness " + std::to_string(cfg.Responsiveness).substr(0, 4) + ".");
    return true;
}

void FirstPersonBody::Stop(World& world) {
    (void)world;
    for (const auto& m : m_Models)
        if (m) m->SetHiddenNodes({}); // model instances outlive Play; the rest is snapshot state
    *this = FirstPersonBody{};
}

void FirstPersonBody::BeforePlayerMove(Player& player, Camera& camera) {
    camera.Position -= m_CameraApplied;
    m_CameraApplied = glm::vec3(0.0f);
    if (!IsActive()) return;
    // Airborne, the input steers (there is no travel in a fall clip to follow).
    player.RootMotionVelocity = m_RootVelocity;
    player.RootMotionWeight = player.Grounded ? 1.0f - std::clamp(m_Responsiveness, 0.0f, 1.0f) : 0.0f;
}

void FirstPersonBody::Tick(World& world, const Player& player, const Camera& camera, float dt) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) { m_Body = entt::null; return; }
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    m_Responsiveness = cfg.Responsiveness;

    // Stand at the capsule's feet, facing the view.
    if (PhysicsWorld::HasCharacter()) {
        float f[3];
        PhysicsWorld::GetCharacterFootPosition(f);
        m_Feet = glm::vec3(f[0], f[1], f[2]);
    } else {
        m_Feet = camera.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f);
    }
    m_Yaw = FirstPersonBodyYaw(camera.Front(), m_Yaw);
    world.SetWorldPose(m_Body, m_Feet, YawRotation(m_Yaw));

    // The movement, in the body's frame, eased so the gait changes smoothly.
    const glm::vec2 target = FirstPersonBodyLocalMove(player.WishVelocity, m_Yaw);
    m_Move += (target - m_Move) * Follow(dt, cfg.ParamSmoothing);
    m_AirTime = player.Grounded ? 0.0f : m_AirTime + dt;

    if (!reg.valid(m_Driver)) { m_Body = entt::null; return; }
    auto& ac = reg.get<AnimatorControllerComponent>(m_Driver);
    ac.SetFloat("MoveX", m_Move.x);
    ac.SetFloat("MoveY", m_Move.y);
    ac.SetFloat("Speed", glm::length(m_Move));
    ac.SetBool("Sprint", glm::length(glm::vec2(player.WishVelocity.x, player.WishVelocity.z)) > m_RunSpeed * 1.05f);
    ac.SetBool("Grounded", player.Grounded);
    // Off the ground for a moment (not a step down a stair): falling.
    ac.SetBool("Airborne", m_AirTime > 0.15f);
    if (player.Jumped) ac.SetTrigger("Jump");
}

void FirstPersonBody::LateUpdate(World& world, Camera& camera, float dt) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) { m_Body = entt::null; return; }
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);

    // This step's root motion, for the capsule's next move.
    if (!reg.valid(m_Driver)) { m_Body = entt::null; return; }
    const auto& ac = reg.get<AnimatorControllerComponent>(m_Driver);
    const glm::vec3 d = ac.RootMotion.DeltaPosition;
    m_RootVelocity = dt > 0.0f ? glm::vec3(d.x, 0.0f, d.z) / dt : glm::vec3(0.0f);

    // The camera into the head: smoothed in the body's own frame, so it never trails the move.
    const auto* rc = reg.try_get<RenderableComponent>(m_Driver);
    const Model* model = rc ? rc->ModelRef.get() : nullptr;
    if (!model || m_HeadNode < 0) return;
    glm::mat4 head(1.0f);
    if (!model->NodeTransform(model->NodeName(m_HeadNode), head)) return;
    const glm::vec3 target = FirstPersonBodyEye(m_RestHead, glm::vec3(head[3]), cfg.HeadBob, cfg.CameraOffset);
    if (!m_HaveEye) { m_Eye = target; m_HaveEye = true; }
    m_Eye += (target - m_Eye) * Follow(dt, cfg.CameraSmoothing);

    const auto& t = reg.get<TransformComponent>(m_Body);
    const glm::vec3 eyeWorld = m_Feet + YawRotation(m_Yaw) * (t.Scale * m_Eye);
    m_CameraApplied = eyeWorld - camera.Position;
    camera.Position = eyeWorld;
}
