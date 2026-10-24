#include "SpiderSystem.h"

#include "AssetLibrary.h"
#include "Audio/WeaponAudio.h"
#include "Components.h"
#include "GameModuleAPI.h" // RaycastHit, QueryFilter
#include "Log.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "Profiler.h"
#include "ProjectPaths.h"
#include "SceneSerializer.h"
#include "World.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

// The solid world (statics and kinematic bodies, not props rolling about, not triggers), counted.
class WorldQuery : public Spider::SurfaceQuery {
public:
    bool Ray(const glm::vec3& from, const glm::vec3& dir, float maxDistance, Spider::SurfaceHit& out) const override {
        ++Rays;
        RaycastHit h;
        QueryFilter f;
        f.HitTriggers = 0;
        if (!PhysicsWorld::RaycastSolid(&from.x, &dir.x, maxDistance, f, h) || !h.Hit) return false;
        out.Hit = true;
        out.Distance = h.Distance;
        out.Point = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
        out.Normal = glm::vec3(h.Normal[0], h.Normal[1], h.Normal[2]);
        return true;
    }
    bool Occupied(const glm::vec3& centre, float radius) const override {
        QueryFilter f;
        f.HitTriggers = 0;
        unsigned hit = 0;
        return PhysicsWorld::OverlapSphereFiltered(&centre.x, radius, f, &hit, 1) > 0;
    }
    mutable int Rays = 0;
};

bool ReadText(const std::string& projectPath, std::string& out) {
    std::ifstream in(std::filesystem::u8path(ProjectPaths::Resolve(projectPath)), std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// The child under `root` that holds the rig: the first with both a model and an Animator Controller.
entt::entity FindModelEntity(const World& world, entt::entity root) {
    const auto& reg = world.Registry;
    std::vector<entt::entity> open{root};
    while (!open.empty()) {
        const entt::entity e = open.back();
        open.pop_back();
        if (reg.all_of<RenderableComponent, AnimatorControllerComponent>(e)) return e;
        if (const auto* h = reg.try_get<HierarchyComponent>(e)) open.insert(open.end(), h->Children.begin(), h->Children.end());
    }
    return entt::null;
}

Spider::MoverSettings MoverFrom(const SpiderComponent& c) {
    Spider::MoverSettings s;
    s.Speed = c.WalkSpeed;
    s.TurnRate = c.TurnRate;
    return s;
}

Spider::GaitSettings GaitFrom(const SpiderComponent& c) {
    Spider::GaitSettings s;
    s.StepHeight = c.StepHeight;
    s.StepTime = c.StepTime;
    return s;
}

} // namespace

bool SpiderSystem::Resolve(World& world, AssetLibrary&, Body& b) {
    auto& reg = world.Registry;
    const auto& sc = reg.get<SpiderComponent>(b.Root);
    if (sc.Rig != m_RigPath) {
        m_RigPath = sc.Rig;
        std::string error;
        m_RigOk = ReadText(m_RigPath, m_RigText) && Spider::ParseRig(m_RigText, m_Rig, &error);
        if (!m_RigOk) Log::Error("Spider: rig '" + m_RigPath + "' won't load" + (error.empty() ? std::string() : ": " + error) + ".");
    }
    if (!m_RigOk) return false;
    if (b.ModelEntity == entt::null || !reg.valid(b.ModelEntity)) b.ModelEntity = FindModelEntity(world, b.Root);
    if (b.ModelEntity == entt::null) return false;
    const auto& rc = reg.get<RenderableComponent>(b.ModelEntity);
    if (!rc.ModelRef || rc.ModelRef->NodeCount() == 0) return false; // still loading
    const Model& m = *rc.ModelRef;

    b.Parents.resize((size_t)m.NodeCount());
    for (int i = 0; i < m.NodeCount(); ++i) b.Parents[(size_t)i] = m.NodeParent(i);
    // Each leg's tip from the bind pose, where the legs lie straight; the legs' rest spots from the standing stance (a
    // rig's bind pose may lie splayed flat).
    IK::Pose bind, stance;
    m.BindLocalPose(bind);
    const int stanceClip = m_Rig.StanceClip.empty() ? -1 : m.FindAnimation(m_Rig.StanceClip);
    if (stanceClip < 0 || !m.SampleLocalPose(stanceClip, 0.0f, AnimationWrapMode::ClampForever, stance)) stance = bind;
    std::vector<glm::mat4> bindGlobals, globals;
    IK::ComputeGlobals(bind, b.Parents, bindGlobals);
    IK::ComputeGlobals(stance, b.Parents, globals);

    std::array<Spider::LegRest, Spider::kLegs> rest{};
    for (int leg = 0; leg < Spider::kLegs; ++leg) {
        auto& chain = b.Chains[(size_t)leg];
        chain.clear();
        for (const std::string& name : m_Rig.LegBones[(size_t)leg]) {
            const int n = m.NodeIndex(name);
            if (n < 0) {
                Log::Error("Spider: the model has no bone '" + name + "' (" + m_RigPath + ").");
                return false;
            }
            chain.push_back(n);
        }
        if (chain.size() < 2) return false;
        // A leaf bone carries no tip: the last segment is taken to run on, straight, as far as the one before it.
        const glm::vec3 last = IK::Position(bindGlobals[(size_t)chain.back()]);
        const glm::vec3 prev = IK::Position(bindGlobals[(size_t)chain[chain.size() - 2]]);
        b.TipOffset[(size_t)leg] = glm::vec3(glm::inverse(bindGlobals[(size_t)chain.back()]) * glm::vec4(last + (last - prev), 1.0f));
        Spider::LegRest& r = rest[(size_t)leg];
        r.Hip = IK::Position(globals[(size_t)chain.front()]);
        r.Foot = glm::vec3(globals[(size_t)chain.back()] * glm::vec4(b.TipOffset[(size_t)leg], 1.0f));
        // The stance stands on the model's y = 0: what the tip stands above it is the claw below the bone's end.
        r.Lift = std::clamp(r.Foot.y, 0.0f, 0.1f);
        r.Group = m_Rig.LegGroup[(size_t)leg];
        r.Reach = 0.0f;
        for (size_t k = 0; k + 1 < chain.size(); ++k)
            r.Reach += glm::length(IK::Position(globals[(size_t)chain[k + 1]]) - IK::Position(globals[(size_t)chain[k]]));
        r.Reach += glm::length(last - prev);
    }
    b.Thorax = m.NodeIndex(m_Rig.Thorax);
    const auto& t = reg.get<TransformComponent>(b.Root);
    b.Mover.Reset(t.Position, t.Rotation);
    b.Gait.Init(rest, world.ComposeWorldTransform(b.ModelEntity));
    b.Ready = true;
    return true;
}

void SpiderSystem::Sound(const char* element, const glm::vec3& pos, float gain) {
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return;
    SoundSet* set = wa.FoleySet("spider", element);
    if (!set || set->Files.empty()) {
        ++s_SoundsMissing;
        return;
    }
    if (wa.PlayKeyed(*set, pos, false, gain).Started) ++s_SoundsStarted;
}

void SpiderSystem::BuildNav() {
    m_NavTried = true;
    PROFILE_SCOPE("Spider Nav");
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<float> verts;
    std::vector<int> tris;
    PhysicsWorld::CollectStaticGeometry(verts, tris, /*includeKinematic=*/false);
    const Spider::NavSettings s;
    const std::uint64_t hash = Spider::SurfaceGraph::HashInput(verts, tris, s);
    char name[64];
    std::snprintf(name, sizeof name, "Library/SpiderNavCache/%016llx.snav", (unsigned long long)hash);
    const std::string cache = ProjectPaths::Resolve(name);
    bool loaded = m_Nav.Load(cache, hash);
    if (!loaded) {
        WorldQuery q;
        if (!m_Nav.Build(verts, tris, q, s)) {
            Log::Error("Spider: the surface graph didn't build - spiders head straight for their goals.");
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(cache).parent_path(), ec);
        m_Nav.Save(cache, hash);
    }
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Log::Info("Spider: surface graph " + std::to_string(m_Nav.NodeCount()) + " nodes, " + std::to_string(m_Nav.LinkCount()) +
              " links (" + (loaded ? "cached" : "baked") + ", " + std::to_string((int)ms) + " ms).");
}

glm::vec3 SpiderSystem::Steer(Body& b, const glm::vec3& goal, float dt) {
    if (m_Nav.Empty()) return goal;
    const glm::vec3 pos = b.Mover.Position(), up = b.Mover.Up();
    b.RouteAge += dt;
    // A new route when the goal has moved, when the old one is used up or stale, or (once a second) after a failure.
    const bool moved = glm::length(goal - b.RouteGoal) > 0.4f;
    if (b.Route.empty() ? (moved || !b.RouteFailed || b.RouteAge > 1.0f) : (moved || b.RouteAge > 3.0f)) {
        b.RouteGoal = goal;
        b.RouteAge = 0.0f;
        b.Route.clear();
        b.RouteNormal.clear();
        b.RouteAt = 0;
        const int from = m_Nav.Nearest(pos, 0.8f, &up), to = m_Nav.Nearest(goal, 1.0f);
        b.RouteFailed = from < 0 || to < 0 || !m_Nav.FindPath(from, to, b.Costs, b.RouteScratch);
        if (!b.RouteFailed) {
            for (int n : b.RouteScratch) {
                b.Route.push_back(m_Nav.Pos(n));
                b.RouteNormal.push_back(m_Nav.Normal(n));
            }
            ++b.Last.Replans;
        }
    }
    b.Last.RouteNodes = (int)b.Route.size();
    return Spider::RouteAim(b.Route, b.RouteNormal, b.RouteAt, pos, up, goal);
}

void SpiderSystem::Think(World& world, AssetLibrary& assets, float dt, const Target* target) {
    if (const std::uint64_t gen = PhysicsWorld::SimulationGeneration(); gen != m_Generation) {
        Clear(); // a new Play, maybe of another scene
        m_Generation = gen;
    }
    auto& reg = world.Registry;
    m_Bodies.erase(std::remove_if(m_Bodies.begin(), m_Bodies.end(),
                                  [&](const Body& b) { return !reg.valid(b.Root) || !reg.all_of<SpiderComponent>(b.Root) ||
                                                              reg.all_of<InactiveTag>(b.Root); }),
                   m_Bodies.end());
    for (auto e : reg.view<SpiderComponent, TransformComponent>(entt::exclude<InactiveTag>))
        if (std::none_of(m_Bodies.begin(), m_Bodies.end(), [&](const Body& b) { return b.Root == e; })) {
            Body b;
            b.Root = e;
            b.Ai.Dropper = (m_SpawnCount++ % 2) == 0; // every other one waits overhead
            b.Ai.LookIn = 0.05f * (float)(m_SpawnCount % 3); // their looks spread over frames
            m_Bodies.push_back(std::move(b));
        }
    if (m_Bodies.empty() || dt <= 0.0f) return;

    PROFILE_SCOPE("Spiders");
    if (!m_NavTried) BuildNav();
    WorldQuery q;
    for (Body& b : m_Bodies) {
        if (!b.Ready && !Resolve(world, assets, b)) continue;
        if (b.Pain.Dead) continue; // its ragdoll has it
        auto& sc = reg.get<SpiderComponent>(b.Root);
        if (sc.Brain == 0 && target) Hunt(world, b, sc, *target, dt);
        else b.Ai.State = Mode::Scripted;
        Spider::MoveInput in;
        in.HasGoal = sc.HasGoal;
        in.Goal = sc.HasGoal ? Steer(b, sc.Goal, dt) : sc.Goal;
        if (!sc.HasGoal) b.Route.clear();
        in.SpeedScale = b.Ai.Hold ? 0.0f : (sc.Run && sc.WalkSpeed > 1e-3f ? sc.RunSpeed / sc.WalkSpeed : 1.0f) * b.Pain.SpeedScale;
        in.LegUp = b.Gait.MeanNormal();
        q.Rays = 0;
        b.Mover.Step(q, MoverFrom(sc), in, dt);
        if (b.Mover.Attached() && !b.WasAttached) Sound("thud", b.Mover.Position()); // down from a leap, a drop, a fall
        b.WasAttached = b.Mover.Attached();
        b.Last.Rays = q.Rays;

        auto& t = reg.get<TransformComponent>(b.Root);
        t.Position = b.Mover.Position();
        t.Rotation = b.Mover.Rotation();
        if (auto* ac = reg.try_get<AnimatorControllerComponent>(b.ModelEntity)) {
            ac->SetFloat("Speed", b.Mover.Speed());
            ac->SetBool("Airborne", !b.Mover.Attached());
        }
    }
}

void SpiderSystem::LatePose(World& world, float dt) {
    if (m_Bodies.empty() || dt <= 0.0f) return;
    PROFILE_SCOPE("Spider Legs");
    auto& reg = world.Registry;
    WorldQuery q;
    for (Body& b : m_Bodies) {
        if (!b.Ready || b.Pain.Dead || !reg.valid(b.ModelEntity)) continue;
        const auto& sc = reg.get<SpiderComponent>(b.Root);
        auto* rc = reg.try_get<RenderableComponent>(b.ModelEntity);
        if (!rc || !rc->ModelRef) continue;
        Model& m = *rc->ModelRef;
        const glm::mat4 model = world.ComposeWorldTransform(b.ModelEntity);

        q.Rays = 0;
        b.Gait.Step(q, GaitFrom(sc), model, b.Mover.Position(), b.Mover.Up(), b.Mover.Velocity(), b.Mover.Attached(), dt);

        Stats& st = b.Last;
        st.Rays += q.Rays;
        st.PlantedSlide = 0.0f;
        st.Grounded = 0;
        for (int i = 0; i < Spider::kLegs; ++i) {
            const Spider::LegState& l = b.Gait.Leg(i);
            if (l.Planted && l.Grounded && b.PrevPlanted[(size_t)i])
                st.PlantedSlide = std::max(st.PlantedSlide, glm::length(l.Foot - b.PrevFoot[(size_t)i]));
            b.PrevFoot[(size_t)i] = l.Foot;
            b.PrevPlanted[(size_t)i] = l.Planted && l.Grounded;
            if (l.Planted && l.Grounded) ++st.Grounded;
        }
        const int landed = b.Gait.TakeLanded();
        st.Steps += landed;
        // Its feet: a chitin tap as legs land, no more than one every few frames (a run would chatter otherwise).
        b.TapIn -= dt;
        if (landed > 0 && b.TapIn <= 0.0f) {
            Sound("tap", b.Mover.Position(), 0.7f + 0.1f * (float)std::min(landed, 3));
            b.TapIn = 0.045f;
        }
        st.TipError = 0.0f;
        if (!sc.LegIK) continue;

        // The animator's pose, the legs re-solved onto the gait's feet (in the model's space).
        const IK::Pose& applied = m.AppliedLocalPose();
        if (applied.size() == (size_t)m.NodeCount()) b.Pose = applied;
        else m.BindLocalPose(b.Pose);
        IK::ComputeGlobals(b.Pose, b.Parents, b.Globals);
        const glm::mat4 toModel = glm::inverse(model);
        const glm::vec3 centre = b.Thorax >= 0 ? IK::Position(b.Globals[(size_t)b.Thorax]) : glm::vec3(0.0f);
        for (int i = 0; i < Spider::kLegs; ++i) {
            const auto& chain = b.Chains[(size_t)i];
            glm::vec3 foot = glm::vec3(toModel * glm::vec4(b.Gait.Leg(i).Foot, 1.0f));
            const glm::vec3 hip = IK::Position(b.Globals[(size_t)chain.front()]);
            if (const float curl = b.Pain.LegCurl[(size_t)i]; curl > 0.0f) // a struck leg drawn up off its foothold
                foot = glm::mix(foot, glm::mix(hip, foot, 0.45f) + glm::vec3(0.0f, 0.16f, 0.0f), std::min(curl, 1.0f));
            // The knees bend up and out from the body: the arch a spider's leg stands in.
            glm::vec3 out = hip - centre;
            out.y = 0.0f;
            out = glm::length(out) > 1e-4f ? glm::normalize(out) : glm::vec3(0.0f);
            const float reach = glm::length(foot - hip);
            const glm::vec3 pole = hip + glm::vec3(0.0f, 1.0f, 0.0f) * std::max(0.2f, reach) + out * 0.25f * reach;
            IK::SolveChain(b.Pose, b.Parents, b.Globals, chain.data(), (int)chain.size(), b.TipOffset[(size_t)i], foot, pole, 1.0f);
            const glm::vec3 tip = glm::vec3(b.Globals[(size_t)chain.back()] * glm::vec4(b.TipOffset[(size_t)i], 1.0f));
            st.TipError = std::max(st.TipError, glm::length(tip - foot));
        }
        m.ApplyLocalPose(b.Pose);
    }
}

entt::entity SpiderSystem::Spawn(World& world, AssetLibrary& assets, const std::string& prefab, const glm::vec3& pos,
                                 const glm::quat& rot) {
    if (prefab != m_PrefabPath) {
        m_PrefabPath = prefab;
        if (!ReadText(prefab, m_PrefabText)) m_PrefabText.clear();
    }
    std::vector<entt::entity> created;
    if (m_PrefabText.empty() || !SceneSerializer::AppendEntitiesFromString(world, assets, m_PrefabText, created)) {
        Log::Error("Spider: couldn't build a spider from '" + prefab + "'.");
        return entt::null;
    }
    auto& reg = world.Registry;
    entt::entity root = entt::null;
    for (entt::entity e : created) {
        const auto* h = reg.try_get<HierarchyComponent>(e);
        if (root == entt::null && (!h || h->Parent == entt::null)) root = e;
    }
    if (root == entt::null || !reg.all_of<SpiderComponent>(root)) {
        Log::Error("Spider: '" + prefab + "' has no Spider component on its root.");
        for (entt::entity e : created)
            if (reg.valid(e)) world.DestroyEntityAndChildren(e);
        return entt::null;
    }
    auto& t = reg.get_or_emplace<TransformComponent>(root);
    t.Position = pos;
    t.Rotation = glm::normalize(rot);
    return root;
}

SpiderSystem::Hurt* SpiderSystem::HurtOf(entt::entity root) {
    for (Body& b : m_Bodies)
        if (b.Root == root) return &b.Pain;
    return nullptr;
}

bool SpiderSystem::Detach(entt::entity root, const glm::vec3& velocity) {
    for (Body& b : m_Bodies)
        if (b.Root == root && b.Ready && !b.Pain.Dead) {
            b.Mover.Detach(velocity);
            return true;
        }
    return false;
}

bool SpiderSystem::Inspect(entt::entity root, Stats& out) const {
    for (const Body& b : m_Bodies) {
        if (b.Root != root) continue;
        out = b.Last;
        out.Ready = b.Ready;
        out.Pos = b.Mover.Position();
        out.Up = b.Mover.Up();
        out.Forward = b.Mover.Forward();
        out.Speed = b.Mover.Speed();
        out.SurfaceGap = b.Mover.SurfaceGap();
        out.SurfaceNormal = b.Mover.SurfaceNormal();
        out.LegNormal = b.Gait.MeanNormal();
        out.AiMode = b.Ai.State;
        out.Aware = b.Ai.Aware;
        out.HasToken = b.Ai.Token;
        out.Attached = b.Mover.Attached();
        out.Arrived = b.Mover.Arrived();
        out.Lifted[0] = b.Gait.Stepping(0);
        out.Lifted[1] = b.Gait.Stepping(1);
        out.BothLifted = b.Gait.BothGroupsLifted();
        return true;
    }
    return false;
}
