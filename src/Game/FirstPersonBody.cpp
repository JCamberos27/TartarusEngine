#include "FirstPersonBody.h"

#include "Camera.h"
#include "Components.h"
#include "IK.h"
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

glm::vec3 FirstPersonBodyViewModelToWorldFov(const glm::vec3& c, float worldFovDeg, float viewModelFovDeg) {
    if (!(worldFovDeg > 0.0f) || !(viewModelFovDeg > 0.0f)) return c;
    const float k = std::tan(glm::radians(worldFovDeg) * 0.5f) / std::tan(glm::radians(viewModelFovDeg) * 0.5f);
    return {c.x * k, c.y * k, c.z};
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
        m_Pieces.push_back(e);
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

    // The chest follows the view's pitch first, so the head (and the camera on it) and the
    // shoulders are where they will be drawn.
    if (cfg.SpineAim > 0.0f) ApplySpineAim(camera, cfg.SpineAim);

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

// Tilts the spine by `amount` of the camera's pitch, spread evenly over spine_01..spine_05 and taken
// about the model's X axis, on every piece so they stay one skeleton.
void FirstPersonBody::ApplySpineAim(const Camera& camera, float amount) {
    const float pitch = std::asin(std::clamp(camera.Front().y, -1.0f, 1.0f)); // up is positive
    static const char* const kSpine[] = {"spine_01", "spine_02", "spine_03", "spine_04", "spine_05"};
    std::vector<int> parents;
    std::vector<glm::mat4> globals;
    for (const auto& mp : m_Models) {
        Model& m = *mp;
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        std::vector<int> bones;
        for (const char* name : kSpine)
            if (const int i = m.NodeIndex(name); i >= 0) bones.push_back(i);
        if (bones.empty()) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        // Rotating +Z about +X by theta gives (0, -sin, cos): looking up needs a negative angle.
        const glm::quat step = glm::angleAxis(-pitch * amount / (float)bones.size(), glm::vec3(1.0f, 0.0f, 0.0f));
        for (int i : bones) IK::OffsetBone(pose, parents, globals, i, glm::vec3(0.0f), step, IK::Position(globals[i]));
        m.ApplyLocalPose(pose);
    }
}

void FirstPersonBody::ArmsLateUpdate(World& world, const Camera& camera, entt::entity weaponArms, float viewModelFov, float dt) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) return;
    const bool enabled = reg.get<FirstPersonBodyComponent>(m_Body).WeaponArms;
    const bool haveRig = enabled && weaponArms != entt::null && reg.valid(weaponArms) &&
                         reg.all_of<RenderableComponent>(weaponArms) && viewModelFov > 0.0f;
    // The rig keeps posing (its hands are the targets) but is no longer drawn.
    if (haveRig) reg.get<RenderableComponent>(weaponArms).CastShadows = RenderableComponent::ShadowCasting::ShadowsOnly;
    // Holstered, the rig is inactive: the arms ease back to the locomotion clips'.
    const bool follow = haveRig && !reg.all_of<InactiveTag>(weaponArms);
    m_ArmsWeight += ((follow ? 1.0f : 0.0f) - m_ArmsWeight) * Follow(dt, 0.1f);
    if (m_ArmsWeight < 1e-3f || !haveRig) return;

    const Model* rig = reg.get<RenderableComponent>(weaponArms).ModelRef.get();
    if (!rig || rig->AppliedLocalPose().empty()) return;
    const IK::Pose& rigPose = rig->AppliedLocalPose();
    if ((int)rigPose.size() != rig->NodeCount()) return;
    const glm::mat4 rigWorld = world.ComposeWorldTransform(weaponArms);

    struct Side { const char *Clavicle, *Upper, *Lower, *Hand; };
    static const Side kSides[2] = {{"clavicle_l", "upperarm_l", "lowerarm_l", "hand_l"},
                                   {"clavicle_r", "upperarm_r", "lowerarm_r", "hand_r"}};
    // Where each of the rig's hands is drawn in the world pass: seen from the camera, in the same place.
    const glm::vec3 right = camera.Right(), up = camera.Up(), front = camera.Front();
    glm::vec3 handPos[2]{};
    glm::quat handRot[2]{};
    bool haveHand[2] = {false, false};
    for (int s = 0; s < 2; ++s) {
        glm::mat4 h(1.0f);
        if (!rig->NodeTransform(kSides[s].Hand, h)) continue;
        const glm::mat4 w = rigWorld * h;
        const glm::vec3 d = glm::vec3(w[3]) - camera.Position;
        const glm::vec3 c = FirstPersonBodyViewModelToWorldFov({glm::dot(d, right), glm::dot(d, up), glm::dot(d, front)},
                                                               camera.Fov, viewModelFov);
        handPos[s] = camera.Position + right * c.x + up * c.y + front * c.z;
        handRot[s] = IK::Rotation(w);
        haveHand[s] = true;
    }

    std::vector<int> parents;
    std::vector<glm::mat4> globals;
    for (size_t k = 0; k < m_Models.size(); ++k) {
        Model& m = *m_Models[k];
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount() || !reg.valid(m_Pieces[k])) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);

        // The rig's arm shapes first (rotations only: the body keeps its own bone lengths), so the
        // elbows bend the way the animation has them; the solve then only fixes the hands.
        std::vector<char> under(pose.size(), 0);
        for (const Side& sd : kSides)
            if (const int c = m.NodeIndex(sd.Clavicle); c >= 0) under[c] = 1;
        for (int i = 0; i < (int)pose.size(); ++i) {
            if (!under[i] && parents[i] >= 0 && under[parents[i]]) under[i] = 1;
            if (!under[i]) continue;
            const int r = rig->NodeIndex(m.NodeName(i));
            if (r >= 0) pose[i].R = glm::normalize(glm::slerp(pose[i].R, rigPose[r].R, m_ArmsWeight));
        }

        const glm::mat4 toModel = glm::inverse(world.ComposeWorldTransform(m_Pieces[k]));
        const glm::quat toModelRot = IK::Rotation(toModel);
        IK::ComputeGlobals(pose, parents, globals);
        for (int s = 0; s < 2; ++s) {
            if (!haveHand[s]) continue;
            const int upper = m.NodeIndex(kSides[s].Upper), lower = m.NodeIndex(kSides[s].Lower),
                      hand = m.NodeIndex(kSides[s].Hand);
            if (upper < 0 || lower < 0 || hand < 0) continue;
            const glm::vec3 target = glm::vec3(toModel * glm::vec4(handPos[s], 1.0f));
            const glm::quat rot = glm::normalize(toModelRot * handRot[s]);
            IK::SolveTwoBone(pose, parents, globals, upper, lower, hand, target, &rot, m_ArmsWeight);
        }
        m.ApplyLocalPose(pose);
    }
}
