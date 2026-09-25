#include "AnimationSystem.h"
#include "Model.h"
#include "AssetLibrary.h"
#include "ProjectPaths.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include "World.h"
#include "RotationMath.h"
#include "Components.h"
#include "PhysicsWorld.h"
#include "GameModuleAPI.h" // BodyState

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>

namespace {

// h in [0,1), s and v in [0,1]. Standard HSV->RGB.
glm::vec3 HsvToRgb(float h, float s, float v) {
    h = h - std::floor(h);
    float i = std::floor(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    switch (static_cast<int>(i) % 6) {
        case 0:  return {v, t, p};
        case 1:  return {q, v, p};
        case 2:  return {p, v, t};
        case 3:  return {p, q, v};
        case 4:  return {t, p, v};
        default: return {v, p, q};
    }
}

// An orthonormal pair spanning the plane perpendicular to `axis` (assumed non-zero).
void PerpBasis(const glm::vec3& axis, glm::vec3& u, glm::vec3& v) {
    glm::vec3 a = glm::normalize(axis);
    glm::vec3 ref = (std::abs(a.y) < 0.99f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    u = glm::normalize(glm::cross(a, ref));
    v = glm::cross(a, u);
}

} // namespace

void UpdateAnimators(World& world, float dt) {
    auto view = world.Registry.view<AnimatorComponent, TransformComponent>(entt::exclude<InactiveTag>); // #199 - an inactive object is paused, not just hidden
    for (auto entity : view) {
        auto& anim = view.get<AnimatorComponent>(entity);
        auto& transform = view.get<TransformComponent>(entity);
        auto* light = world.Registry.try_get<LightComponent>(entity);

        if (!anim.Initialized) {
            anim.BasePosition = transform.Position;
            anim.BaseRotation = transform.Rotation;
            anim.BaseColor = light ? light->Color : glm::vec3(1.0f);
            anim.Elapsed = 0.0f;
            anim.Initialized = true;
        }
        anim.Elapsed += dt;
        const float t = anim.Elapsed;

        // Spin: authored base turned by offset(Elapsed), same reversible form as orbit/bob (#109) —
        // no unbounded accumulation. SpinDegPerSec is an angular velocity, so
        // the turn is |w| * t about w's own direction (#123); adding it to the Euler components
        // would only match that for a single principal axis. The angle is wrapped to one turn so
        // a long-running scene doesn't lose float precision. A zero spin leaves the base as-is.
        const float spinRate = glm::length(anim.SpinDegPerSec);
        transform.SetRotationQuaternion(spinRate > 0.0f
            ? RotateAboutLocalAxis(anim.BaseRotation, anim.SpinDegPerSec, std::fmod(spinRate * t, 360.0f))
            : anim.BaseRotation);

        // Position = authored base + orbit + bob.
        glm::vec3 pos = anim.BasePosition;
        if (anim.OrbitRadius != 0.0f && glm::dot(anim.OrbitAxis, anim.OrbitAxis) > 1e-8f) {
            glm::vec3 u, v;
            PerpBasis(anim.OrbitAxis, u, v);
            float ang = glm::radians(anim.OrbitDegPerSec * t);
            pos += (std::cos(ang) * u + std::sin(ang) * v) * anim.OrbitRadius;
        }
        if (anim.BobAmplitude != 0.0f && anim.BobFreqHz != 0.0f) {
            pos.y += std::sin(glm::two_pi<float>() * anim.BobFreqHz * t) * anim.BobAmplitude;
        }
        transform.Position = pos;

        // Light hue cycle: keep the authored brightness, sweep the hue.
        if (light && anim.ColorCycleHzPerSec != 0.0f) {
            float brightness = glm::max(glm::max(anim.BaseColor.r, anim.BaseColor.g),
                                       glm::max(anim.BaseColor.b, 0.35f));
            light->Color = HsvToRgb(anim.ColorCycleHzPerSec * t, 0.85f, brightness);
        }
    }
}

void ApplyRootMotion(World& world, entt::entity entity, RootMotionOptions& opts, const RootMotionDelta& motion, float dt) {
    opts.DeltaPosition = glm::vec3(0.0f);
    opts.DeltaYaw = 0.0f;
    auto* transform = world.Registry.try_get<TransformComponent>(entity);
    if (!transform || opts.Mode == (int)RootMotionMode::Off || opts.ResolvedBone < 0) {
        opts.Speed = opts.TurnRate = 0.0f;
        return;
    }
    // The motion is in the model's space; the object's own rotation and scale take it to its
    // parent's space, and the parent's world matrix on to world space for the readout.
    const glm::vec3 parentDelta = transform->Rotation * (transform->Scale * motion.Translation);
    glm::mat3 parentWorld(1.0f);
    if (const auto* h = world.Registry.try_get<HierarchyComponent>(entity); h && world.Registry.valid(h->Parent))
        parentWorld = glm::mat3(world.GetCachedWorldTransform(h->Parent));
    const glm::vec3 worldDelta = parentWorld * parentDelta;
    opts.DeltaPosition = worldDelta;
    opts.DeltaYaw = glm::degrees(motion.Yaw);
    if (dt > 0.0f) {
        // Smoothed a little: per-frame travel steps with the clip's keys.
        const float k = 1.0f - std::exp(-dt * 12.0f);
        opts.Speed += (glm::length(glm::vec2(worldDelta.x, worldDelta.z)) / dt - opts.Speed) * k;
        opts.TurnRate += (opts.DeltaYaw / dt - opts.TurnRate) * k;
    }
    if (opts.Mode != (int)RootMotionMode::Apply || motion.IsZero()) return;
    // The player's controller owns its own movement.
    if (world.Registry.all_of<FirstPersonControllerComponent>(entity)) return;

    const glm::quat turned = transform->Rotation * glm::angleAxis(motion.Yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    const auto* body = world.Registry.try_get<RigidbodyComponent>(entity);
    BodyState state;
    const unsigned id = (unsigned)entt::to_integral(entity);
    if (body && !body->IsKinematic && dt > 0.0f && PhysicsWorld::GetBodyState(id, state) && state.Valid) {
        // A simulated body is steered, not teleported: the travel becomes its velocity (gravity
        // keeps the vertical unless Vertical is on), so walls and slopes still stop it.
        glm::vec3 v = worldDelta / dt;
        if (!opts.Vertical) v.y = state.Velocity[1];
        const float vv[3] = {v.x, v.y, v.z};
        PhysicsWorld::SetLinearVelocity(id, vv);
        if (motion.Yaw != 0.0f) {
            transform->SetRotationQuaternion(turned);
            const TransformComponent w = world.WorldSpaceTransform(entity);
            const float p[3] = {w.Position.x, w.Position.y, w.Position.z};
            const float q[4] = {w.Rotation.x, w.Rotation.y, w.Rotation.z, w.Rotation.w};
            PhysicsWorld::SetActorPose(id, p, q, false);
        }
        return;
    }
    // Everything else (a kinematic body follows its transform on the next physics step).
    transform->Position += parentDelta;
    if (motion.Yaw != 0.0f) transform->SetRotationQuaternion(turned);
}

void ApplyModelRootMotion(World& world, entt::entity entity, Model& model, RootMotionOptions& opts, float dt) {
    const bool on = opts.Mode != (int)RootMotionMode::Off;
    opts.ResolvedBone = on ? model.FindRootMotionNode(opts.Bone) : -1;
    RootMotionSettings s;
    s.Rotation = opts.Rotation;
    s.Vertical = opts.Vertical;
    model.SetRootMotion(opts.ResolvedBone, s);
    ApplyRootMotion(world, entity, opts, model.ConsumeRootMotion(), dt);
}

std::string AnimationClipRef(const Model& source, int sourceClip) {
    std::string ref = ProjectPaths::Relativize(source.Path());
    if (source.OwnAnimationCount() > 1) ref += "#" + source.AnimationName(sourceClip);
    return ref;
}

std::string AnimationClipLabel(const Model& source, int sourceClip) {
    std::string label = std::filesystem::u8path(source.Path()).stem().u8string();
    if (source.OwnAnimationCount() > 1) label += " / " + source.AnimationName(sourceClip);
    return label;
}

int ResolveAnimationClip(Model& model, const std::string& clipRef, AssetLibrary& assets) {
    if (clipRef.empty()) return model.AnimationCount() > 0 ? 0 : -1;
    if (int i = model.FindAnimation(clipRef); i >= 0) return i; // own clip name, or already attached
    // "path[#clip]" naming a model file.
    const size_t hash = clipRef.find('#');
    const std::string path = clipRef.substr(0, hash);
    const std::string clipName = hash == std::string::npos ? std::string() : clipRef.substr(hash + 1);
    std::string ext = std::filesystem::u8path(path).extension().u8string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext != ".fbx" && ext != ".gltf" && ext != ".glb" && ext != ".dae" && ext != ".obj") return -1;
    const std::string abs = std::filesystem::u8path(path).is_absolute() ? path : ProjectPaths::Resolve(path);
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::u8path(abs), ec)) return -1;
    std::shared_ptr<Model> src = assets.LoadModel(abs);
    if (!src || src.get() == &model || src->OwnAnimationCount() == 0) return -1;
    int srcClip = 0;
    if (!clipName.empty()) {
        srcClip = -1;
        for (int i = 0; i < src->OwnAnimationCount(); ++i)
            if (src->AnimationName(i) == clipName) { srcClip = i; break; }
        if (srcClip < 0) return -1;
    }
    return model.AttachClip(*src, srcClip, clipRef, AnimationClipLabel(*src, srcClip));
}

void UpdateSkeletalAnimations(World& world, AssetLibrary& assets, float dt) {
    // #175 Part B - an Animator Controller on the same entity owns the model's clip.
    auto view = world.Registry.view<SkeletalAnimationComponent, RenderableComponent>(
        entt::exclude<InactiveTag, AnimatorControllerComponent>);
    for (auto e : view) {
        auto& anim = view.get<SkeletalAnimationComponent>(e);
        Model* model = view.get<RenderableComponent>(e).ModelRef.get();
        if (!model) continue;

        const AnimationWrapMode wrap = (AnimationWrapMode)std::clamp(anim.WrapMode, 0, 3);
        if (!anim.Started) model->ConsumeRootMotion(); // nothing an edit-mode preview collected
        ApplyModelRootMotion(world, e, *model, anim.RootMotion, dt);
        if (!anim.Started) {
            anim.Started = true;
            anim.IsPlaying = anim.PlayAutomatically;
            anim.PlayingClip.clear();
            if (!anim.IsPlaying) model->StopAnimation();
        }
        int want = ResolveAnimationClip(*model, anim.Clip, assets);
        if (want < 0) want = model->OwnAnimationCount() > 0 ? 0 : -1; // missing clip -> the first one
        if (want < 0) continue; // nothing this model can play

        if (!anim.IsPlaying) {
            if (model->IsPlayingAnimation() && !anim.PlayingClip.empty()) model->PlayAnimation(-1, anim.CrossFade);
            anim.PlayingClip.clear();
            continue;
        }
        const std::string& wantName = model->AnimationName(want);
        if (anim.PlayingClip != wantName) {
            // First start snaps; a change while already playing crossfades.
            model->PlayAnimation(want, anim.PlayingClip.empty() ? 0.0f : anim.CrossFade, wrap, anim.Speed);
            anim.PlayingClip = wantName;
        } else if (!model->IsPlayingAnimation()) {
            anim.IsPlaying = false; // a Once clip ran out
            anim.PlayingClip.clear();
        } else {
            model->SetAnimationSpeed(anim.Speed); // live edits / game code
            model->SetAnimationWrapMode(wrap);
        }
    }
}
