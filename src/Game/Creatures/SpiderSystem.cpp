#include "SpiderSystem.h"

#include "AssetLibrary.h"
#include "Audio/WeaponAudio.h"
#include "Components.h"
#include "GameModuleAPI.h" // RaycastHit, QueryFilter
#include "Log.h"
#include "Model.h"
#include "Openable.h"
#include "PhysicsWorld.h"
#include "Profiler.h"
#include "ProjectPaths.h"
#include "SceneSerializer.h"
#include "World.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

// Every CreatureBarrierComponent's box: its entity's render bounds (a unit box without a model) through its world
// transform, grown by its padding.
void CollectBarriers(const World& world, std::vector<Spider::Barrier>& out) {
    out.clear();
    const auto& reg = world.Registry;
    for (entt::entity e : reg.view<const CreatureBarrierComponent, const TransformComponent>(entt::exclude<InactiveTag>)) {
        glm::vec3 lo(-0.5f), hi(0.5f);
        if (const auto* r = reg.try_get<RenderableComponent>(e); r && r->ModelRef) {
            lo = r->ModelRef->BoundsMin();
            hi = r->ModelRef->BoundsMax();
        }
        const glm::mat4 m = world.ComposeWorldTransform(e);
        glm::mat3 axes(m);
        glm::vec3 scale;
        for (int a = 0; a < 3; ++a) {
            scale[a] = glm::length(axes[a]);
            if (scale[a] > 1e-6f) axes[a] /= scale[a];
        }
        Spider::Barrier b;
        b.Centre = glm::vec3(m * glm::vec4(0.5f * (lo + hi), 1.0f));
        b.Rotation = glm::normalize(glm::quat_cast(axes));
        b.Half = 0.5f * glm::abs(hi - lo) * scale + glm::vec3(std::max(0.0f, reg.get<CreatureBarrierComponent>(e).Padding));
        out.push_back(b);
    }
}

// The openable a hit belongs to: the entity hit, or a parent of it (a door's collider on a child of its leaf).
entt::entity OpenableOf(const World& world, entt::entity e) {
    for (int i = 0; i < 4 && e != entt::null && world.Registry.valid(e); ++i) {
        if (world.Registry.all_of<OpenableComponent>(e)) return e;
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        e = h ? h->Parent : entt::null;
    }
    return entt::null;
}

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

double UsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

// The jank counters' bar for turning: the body's up never swings faster than the mover lets it (a 90-degree roll over an
// edge takes 1/6 s or more), and its whole rotation no faster than that plus its turn rate.
const float kMaxUpRateDeg = Spider::MoverSettings{}.MaxUpRate;

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

bool Spider::WorldQuery::Ray(const glm::vec3& from, const glm::vec3& dir, float maxDistance, Spider::SurfaceHit& out) const {
    ++Rays;
    RaycastHit h;
    QueryFilter f;
    f.HitTriggers = 0;
    if (!(StaticOnly ? PhysicsWorld::RaycastStatic(&from.x, &dir.x, maxDistance, f, h)
                     : PhysicsWorld::RaycastSolid(&from.x, &dir.x, maxDistance, f, h, /*fromOutside=*/true)) ||
        !h.Hit)
        return false;
    out.Hit = true;
    out.Distance = h.Distance;
    out.Point = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
    out.Normal = glm::vec3(h.Normal[0], h.Normal[1], h.Normal[2]);
    return true;
}

bool Spider::WorldQuery::Occupied(const glm::vec3& centre, float radius) const {
    QueryFilter f;
    f.HitTriggers = 0;
    if (StaticOnly) return PhysicsWorld::OverlapSphereStatic(&centre.x, radius, f);
    unsigned hit = 0;
    return PhysicsWorld::OverlapSphereFiltered(&centre.x, radius, f, &hit, 1) > 0;
}

glm::vec3 SpiderSystem::Shove(const glm::vec3& velocity, const glm::vec3& push) {
    const glm::vec2 p(push.x, push.z), v(velocity.x, velocity.z);
    const float strength = glm::length(p);
    if (strength <= 1e-3f) return velocity;
    const glm::vec2 dir = p / strength;
    const float along = glm::dot(v, dir);
    glm::vec2 out = along < strength ? v + dir * (strength - along) : v;
    const float cap = std::max(strength, glm::length(v)), speed = glm::length(out);
    if (speed > cap) out *= cap / speed;
    return glm::vec3(out.x, velocity.y, out.y);
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

void SpiderSystem::BuildNav(const World& world) {
    m_NavTried = true;
    CollectBarriers(world, m_Barriers);
    PROFILE_SCOPE("Spider Nav");
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<float> verts;
    std::vector<int> tris;
    PhysicsWorld::CollectStaticGeometry(verts, tris, /*includeKinematic=*/false);
    const Spider::NavSettings s;
    const std::uint64_t hash = Spider::SurfaceGraph::HashBarriers(Spider::SurfaceGraph::HashInput(verts, tris, s), m_Barriers);
    char name[64];
    std::snprintf(name, sizeof name, "Library/SpiderNavCache/%016llx.snav", (unsigned long long)hash);
    const std::string cache = ProjectPaths::Resolve(name);
    bool loaded = m_Nav.Load(cache, hash);
    if (!loaded) {
        Spider::WorldQuery solid(/*staticOnly=*/true);
        const Spider::BarrierQuery q(solid, m_Barriers);
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

bool SpiderSystem::NavMatchesFreshBuild(std::string& why) const {
    std::vector<float> verts;
    std::vector<int> tris;
    PhysicsWorld::CollectStaticGeometry(verts, tris, /*includeKinematic=*/false);
    Spider::SurfaceGraph fresh;
    Spider::WorldQuery solid(/*staticOnly=*/true);
    const Spider::BarrierQuery q(solid, m_Barriers);
    if (!fresh.Build(verts, tris, q, Spider::NavSettings{})) {
        why = "the fresh build failed";
        return false;
    }
    const Spider::SurfaceGraph& g = m_Nav;
    if (g.NodeCount() != fresh.NodeCount() || g.LinkCount() != fresh.LinkCount()) {
        why = "in use " + std::to_string(g.NodeCount()) + " nodes / " + std::to_string(g.LinkCount()) + " links, fresh " +
              std::to_string(fresh.NodeCount()) + " / " + std::to_string(fresh.LinkCount());
        return false;
    }
    for (int i = 0; i < g.NodeCount(); ++i) {
        bool same = glm::length(g.Pos(i) - fresh.Pos(i)) < 1e-4f && glm::length(g.Normal(i) - fresh.Normal(i)) < 1e-4f &&
                    g.FirstLink(i + 1) - g.FirstLink(i) == fresh.FirstLink(i + 1) - fresh.FirstLink(i);
        for (int k = 0; same && k < g.FirstLink(i + 1) - g.FirstLink(i); ++k)
            same = g.Neighbour(g.FirstLink(i) + k) == fresh.Neighbour(fresh.FirstLink(i) + k);
        if (!same) {
            why = "node " + std::to_string(i) + " differs";
            return false;
        }
    }
    return true;
}

glm::vec3 SpiderSystem::Steer(Body& b, const glm::vec3& goal, float dt) {
    if (m_Nav.Empty()) return goal;
    const glm::vec3 pos = b.Mover.Position(), up = b.Mover.Up();
    b.RouteAge += dt;
    // A new route when the goal has moved, when the old one is used up or stale, or (once a second) after a failure.
    const bool moved = glm::length(goal - b.RouteGoal) > 0.4f;
    bool want = b.Route.empty() ? (moved || !b.RouteFailed || b.RouteAge > 1.0f) : (moved || b.RouteAge > 3.0f);
    // One search a frame between all the spiders (a search can walk much of the graph): another that wants one this frame
    // keeps its route a frame or two longer. One with no route at all waits no more than a couple of frames.
    if (want && m_SearchedThisFrame && (!b.Route.empty() || b.PlanWait < 2)) {
        ++b.PlanWait;
        want = false;
    }
    if (want) {
        const int from = m_Nav.Nearest(pos, 0.8f, &up), to = m_Nav.Nearest(goal, 1.0f, nullptr, from >= 0 ? m_Nav.Component(from) : -1);
        // The same search as one that failed, with nothing blocked or freed since, fails the same way: it isn't run again.
        const bool same = b.RouteFailed && from == b.FailFrom && to == b.FailTo && m_Nav.BlockGeneration() == b.FailGen;
        b.RouteGoal = goal;
        b.RouteAge = 0.0f;
        b.PlanWait = 0;
        b.Route.clear();
        b.RouteNormal.clear();
        b.RouteAt = 0;
        if (!same) {
            const auto t0 = std::chrono::steady_clock::now();
            b.RouteFailed = from < 0 || to < 0 || !m_Nav.FindPath(from, to, b.Costs, b.RouteScratch);
            if (from >= 0 && to >= 0) {
                const double us = UsSince(t0);
                m_SearchedThisFrame = true;
                ++m_Times.Searches;
                m_Times.SearchUs += us;
                m_Times.WorstSearchUs = std::max(m_Times.WorstSearchUs, us);
                m_Times.WorstExpanded = std::max(m_Times.WorstExpanded, m_Nav.LastExpanded());
            }
            b.FailFrom = from;
            b.FailTo = to;
            b.FailGen = m_Nav.BlockGeneration();
        }
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

glm::vec3 SpiderSystem::Doors(World& world, Body& b, const glm::vec3& aim, float dt, bool& hold) {
    hold = false;
    auto& reg = world.Registry;
    const glm::vec3 pos = b.Mover.Position(), up = b.Mover.Up();
    if (b.Door == entt::null) {
        RaycastHit h;
        QueryFilter f;
        f.HitTriggers = 0;
        const glm::vec3 from = pos + up * 0.15f;
        // The door it is standing on (the mover climbed a shut leaf as it would a wall), else the one ahead on its way,
        // within a body's length or so.
        const glm::vec3 down = -up;
        bool onLeaf = PhysicsWorld::RaycastSolid(&from.x, &down.x, 0.4f, f, h, /*fromOutside=*/true) && h.Hit;
        entt::entity door = onLeaf ? OpenableOf(world, static_cast<entt::entity>(h.Entity)) : entt::null;
        onLeaf = door != entt::null;
        if (!onLeaf) {
            glm::vec3 d = aim - pos;
            d -= up * glm::dot(d, up);
            const float len = glm::length(d);
            if (len < 1e-3f) return aim;
            d /= len;
            if (!PhysicsWorld::RaycastSolid(&from.x, &d.x, 1.0f, f, h, /*fromOutside=*/true) || !h.Hit) return aim;
            door = OpenableOf(world, static_cast<entt::entity>(h.Entity));
        }
        const auto* o = door != entt::null ? reg.try_get<OpenableComponent>(door) : nullptr;
        if (!o || !o->Started || o->Target > 0.5f || o->Progress > 0.5f) return aim;
        // A door: a hinged leaf a person walks through, standing on the floor - never a cabinet's, a drawer or a lid.
        glm::vec3 hinge, toLatch, swing;
        float width = 0.0f, height = 0.0f;
        if (!Openable::Leaf(world, door, hinge, toLatch, width, swing, &height) || height < 1.6f || width < 0.6f) return aim;
        const glm::vec3 mid = hinge + toLatch * (0.5f * width);
        // Its side of the door: the face it stands on, or where it stands.
        const float side = (onLeaf ? glm::dot(up, swing) : glm::dot(glm::vec3(pos.x - mid.x, 0.0f, pos.z - mid.z), swing)) > 0.0f ? 1.0f : -1.0f;
        // Only a door its way goes through: where it heads next, or the route's next few nodes, past the door's plane and
        // through its opening - not one it merely brushes walking along the wall beside it.
        auto through = [&](const glm::vec3& p) {
            const glm::vec3 d(p.x - mid.x, 0.0f, p.z - mid.z);
            return glm::dot(d, swing) * side < 0.0f && std::fabs(glm::dot(d, toLatch)) < 0.5f * width + 0.3f;
        };
        bool crosses = through(aim);
        for (int k = b.RouteAt; !crosses && k < (int)b.Route.size() && k < b.RouteAt + 6; ++k) crosses = through(b.Route[(size_t)k]);
        if (!crosses) return aim;
        if (b.Ai.State == Mode::Perch) {
            // Only settling, unaware: it doesn't go pushing doors - it perches on this side of this one.
            const glm::vec3 n = swing * side;
            b.PerchAvoid.push_back(glm::vec4(n, -glm::dot(n, mid) + 0.1f));
            b.Ai.Perch = -1;
            b.RouteAge = 1e9f;
            if (onLeaf) b.Mover.Detach(up * 1.2f); // off the door it had started up
            return aim;
        }
        b.Door = door;
        b.DoorFor = 0.0f;
        b.DoorPushed = false;
#pragma warning(suppress : 4996)
        static const bool trace = std::getenv("TARTARUS_SPIDER_TRACE") != nullptr;
        if (trace)
            std::printf("[Spider] %u: a shut door on its way at %.2f %.2f %.2f\n", (unsigned)entt::to_integral(b.Root), hinge.x,
                        hinge.y, hinge.z);
        b.DoorPress = pos;
        b.DoorFace = glm::vec3(mid.x, pos.y, mid.z);
        if (onLeaf) {
            // On the leaf itself: it pushes it from there, and springs off as it swings.
            if (Openable::Open(world, door)) ++b.Last.DoorsOpened;
            Sound("thud", pos, 0.6f);
            b.Mover.Detach(up * 1.2f);
            b.Door = entt::null;
            return aim;
        }
        if (up.y > 0.7f) {
            // On the floor: it presses where the leaf won't sweep it - its middle when it swings away; when it swings
            // toward the spider, past its latch edge (out of the swing's reach) or, if that spot is in a wall or a
            // corner's furniture, past its hinge edge (behind where the leaf stops).
            const Spider::WorldQuery solid;
            auto standable = [&](const glm::vec3& p) {
                Spider::SurfaceHit under;
                const glm::vec3 dn(0.0f, -1.0f, 0.0f);
                return !solid.Occupied(p + glm::vec3(0.0f, 0.25f, 0.0f), 0.22f) &&
                       solid.Ray(p + glm::vec3(0.0f, 0.3f, 0.0f), dn, 0.6f, under) && under.Normal.y > 0.8f;
            };
            const glm::vec3 spots[2] = {hinge + toLatch * (width + 0.55f) + swing * 0.4f, hinge - toLatch * 0.6f + swing * 0.25f};
            if (side < 0.0f) {
                b.DoorPress = mid - swing * 0.45f;
            } else {
                b.DoorPress = spots[0];
                for (const glm::vec3& s : spots)
                    if (standable(glm::vec3(s.x, pos.y, s.z))) {
                        b.DoorPress = s;
                        break;
                    }
            }
            b.DoorPress.y = pos.y;
        } else { // on a wall or the ceiling over it: it pushes from where it is
            b.DoorPress = pos;
            b.DoorFace = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
        }
    }
    auto* o = reg.valid(b.Door) ? reg.try_get<OpenableComponent>(b.Door) : nullptr;
    if (!o) {
        b.Door = entt::null;
        return aim;
    }
    b.DoorFor += dt;
    if (!b.DoorPushed) {
        // At its spot - or, if it can't get there, from wherever it got to.
        if (glm::length(b.DoorPress - pos) < 0.3f || b.DoorFor > 4.0f) {
            const bool opened = Openable::Open(world, b.Door);
            if (opened) {
                ++b.Last.DoorsOpened;
                Sound("thud", pos, 0.6f); // its weight against the leaf
            }
            b.DoorPushed = opened || o->Target > 0.5f;
            b.DoorFor = 0.0f;
        }
        if (!b.DoorPushed) return b.DoorPress;
    }
    // Pushed: it holds, facing the door, while the door swings; then on along its way. It stays open.
    if (o->Progress >= 0.85f || o->Target < 0.5f || b.DoorFor > 3.0f) {
        b.Door = entt::null;
        return aim;
    }
    hold = true;
    return b.DoorFace;
}

void SpiderSystem::UpdateDoorBlocks(World& world) {
    if (m_Nav.Empty()) return;
    auto& reg = world.Registry;
    bool changed = false;
    // A few doors a frame, taken in turn: the first frame of a Play found every door in the building new at once (a 1+ ms
    // hitch); a door left for a frame or two is blocked a frame or two late.
    constexpr int kDoorsPerFrame = 3;
    auto doors = reg.view<OpenableComponent>(entt::exclude<InactiveTag>);
    unsigned index = 0, done = 0, first = m_DoorTurn;
    for (auto e : doors) {
        const unsigned at = index++;
        if (at < first || done >= (unsigned)kDoorsPerFrame) continue;
        const auto& o = reg.get<OpenableComponent>(e);
        if (!o.Started) continue;
        auto it = std::find_if(m_DoorBlocks.begin(), m_DoorBlocks.end(), [&](const DoorBlock& d) { return d.Door == e; });
        const float was = it != m_DoorBlocks.end() ? it->Progress : -1.0f;
        const bool settled = o.Progress == 0.0f || o.Progress == 1.0f;
        if (o.Progress == was || (!settled && std::fabs(o.Progress - was) < 0.05f)) continue;
        // Doors only - a hinged leaf a person walks through (a cabinet's swings over furniture, where no route runs).
        glm::vec3 hinge, toLatch, swing;
        float width = 0.0f, height = 0.0f;
        if (!Openable::Leaf(world, e, hinge, toLatch, width, swing, &height) || height < 1.6f || width < 0.6f) continue;
        if (it == m_DoorBlocks.end()) {
            m_DoorBlocks.push_back(DoorBlock{e});
            it = m_DoorBlocks.end() - 1;
        }
        // Metres of route per doorway node: a route crosses about four, so a door to push weighs some 4 m (stopping,
        // pushing, the swing).
        constexpr float kPushCost = 1.0f;
        for (int n : it->Nodes) {
            if (it->Shut) m_Nav.AddPenalty(n, -kPushCost);
            else m_Nav.Block(n, -1);
        }
        it->Nodes.clear();
        ++done;
        m_DoorTurn = at + 1;
        it->Progress = o.Progress;
        it->Shut = o.Progress <= 0.0f;
        changed = true;
        glm::vec3 centre, half;
        glm::quat rot;
        if (!Openable::LeafBox(world, e, centre, half, rot)) continue;
        // Open: no room within a body's half-width of the leaf. Shut: the doorway, both sides of the leaf.
        const glm::vec3 reach = half + glm::vec3(it->Shut ? 0.35f : 0.25f);
        const float r2 = glm::dot(reach, reach);
        const glm::quat inv = glm::conjugate(rot);
        m_Near.clear();
        m_Nav.NodesNear(centre, std::sqrt(r2), m_Near); // (the leaf's neighbourhood, not all the graph's nodes)
        for (int i : m_Near) {
            const glm::vec3 d = m_Nav.Pos(i) - centre;
            const glm::vec3 l = glm::abs(inv * d);
            if (l.x <= reach.x && l.y <= reach.y && l.z <= reach.z) {
                if (it->Shut) m_Nav.AddPenalty(i, kPushCost);
                else m_Nav.Block(i, 1);
                it->Nodes.push_back(i);
            }
        }
    }
    if (done < (unsigned)kDoorsPerFrame) m_DoorTurn = 0; // (round to the first door again)
    if (changed)
        for (Body& b : m_Bodies) b.RouteAge = 1e9f; // routes afresh round where the leaves are now
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
    const auto thinkStart = std::chrono::steady_clock::now();
    double navUs = 0.0;
    if (!m_NavTried) {
        BuildNav(world);
        navUs = m_Times.NavUs = UsSince(thinkStart);
    }
    const auto doorsStart = std::chrono::steady_clock::now();
    UpdateDoorBlocks(world);
    const double doorsUs = UsSince(doorsStart);
    m_SearchedThisFrame = false;
    m_Times.DoorBlocksUs += doorsUs;
    m_Times.WorstDoorBlocksUs = std::max(m_Times.WorstDoorBlocksUs, doorsUs);
    Spider::WorldQuery wq;
    const Spider::BarrierQuery q(wq, m_Barriers);
    for (Body& b : m_Bodies) {
        if (!b.Ready && !Resolve(world, assets, b)) continue;
        if (b.Pain.Dead) continue; // its ragdoll has it
        const auto bodyStart = std::chrono::steady_clock::now();
        auto& sc = reg.get<SpiderComponent>(b.Root);
        if (sc.Brain == 0 && target) Hunt(world, b, sc, *target, dt);
        else b.Ai.State = Mode::Scripted;
        Spider::MoveInput in;
        in.HasGoal = sc.HasGoal;
        in.Goal = sc.HasGoal ? Steer(b, sc.Goal, dt) : sc.Goal;
        if (!sc.HasGoal) b.Route.clear();
        bool doorHold = false;
        if (sc.HasGoal && b.Mover.Attached() && !b.Ai.Hold) in.Goal = Doors(world, b, in.Goal, dt, doorHold);
        else if (!b.Mover.Attached()) b.Door = entt::null;
        in.SpeedScale = b.Ai.Hold || doorHold ? 0.0f : (sc.Run && sc.WalkSpeed > 1e-3f ? sc.RunSpeed / sc.WalkSpeed : 1.0f) * b.Pain.SpeedScale;
        in.LegUp = b.Gait.MeanNormal();
        wq.Rays = 0;
        b.Mover.Step(q, MoverFrom(sc), in, dt);
        if (b.Mover.Attached() && !b.WasAttached) Sound("thud", b.Mover.Position()); // down from a leap, a drop, a fall
        // For tests: stuck (meaning to move, not moving) and sunk (an end of the body inside a surface).
        {
            Stats& st = b.Last;
            st.Goal = sc.Goal;
            st.HasGoal = sc.HasGoal;
            const bool meansToMove = sc.HasGoal && !b.Mover.Arrived() && !b.Ai.Hold && !doorHold && b.Mover.Attached();
            st.StuckFor = meansToMove && glm::length(b.Mover.Velocity()) < 0.05f ? st.StuckFor + dt : 0.0f;
            if (st.StuckFor > st.WorstStuck) {
                st.WorstStuck = st.StuckFor;
                st.WorstStuckAt = b.Mover.Position();
                st.WorstStuckGoal = in.Goal;
                st.WorstStuckMode = (int)b.Ai.State;
            }
            if (b.Mover.Attached()) {
                const Spider::MoverSettings ms = MoverFrom(sc);
                const glm::vec3 u = b.Mover.Up(), f = b.Mover.Forward(), mid = b.Mover.Position() + u * ms.BodyLift;
                Spider::SurfaceHit h;
                const bool sunkAhead = q.Ray(mid, f, ms.BodyFront - 0.03f, h);
                if (sunkAhead || (q.Ray(mid, -f, ms.BodyBack - 0.03f, h))) {
#pragma warning(suppress : 4996)
                    static const bool trace = std::getenv("TARTARUS_SPIDER_TRACE") != nullptr;
                    if (trace && st.SunkFrames < 15)
                        std::printf("[SpiderJank] %u sunk at %.2f %.2f %.2f up %.2f %.2f %.2f hit %.2f %s n %.2f %.2f %.2f fwd %.2f %.2f %.2f speed %.2f mode %d corner %d\n",
                                    (unsigned)entt::to_integral(b.Root), b.Mover.Position().x, b.Mover.Position().y, b.Mover.Position().z, u.x, u.y,
                                    u.z, h.Distance, sunkAhead ? "ahead" : "behind", h.Normal.x, h.Normal.y, h.Normal.z, f.x, f.y, f.z,
                                    b.Mover.Speed(), (int)b.Ai.State, b.Mover.Cornering());
                    ++st.SunkFrames;
                }
            }
        }
        b.WasAttached = b.Mover.Attached();
        b.Last.Rays = wq.Rays;
        {
            const bool meansToMove = sc.HasGoal && !b.Mover.Arrived() && !b.Ai.Hold && !doorHold && b.Mover.Attached();
            b.Track.ThinkUs = UsSince(bodyStart);
            const unsigned moves = b.Mover.TakeMoves();
            MeasureBody(b, q, sc, in.Goal, meansToMove, moves, dt);
        }

        auto& t = reg.get<TransformComponent>(b.Root);
        t.Position = b.Mover.Position();
        t.Rotation = b.Mover.Rotation();
        if (auto* ac = reg.try_get<AnimatorControllerComponent>(b.ModelEntity)) {
            ac->SetFloat("Speed", b.Mover.Speed());
            ac->SetBool("Airborne", !b.Mover.Attached());
        }
    }
    m_ThinkUs = UsSince(thinkStart) - navUs; // (the graph's bake, once a Play, is timed on its own)
}

void SpiderSystem::LatePose(World& world, float dt) {
    if (m_Bodies.empty() || dt <= 0.0f) return;
    PROFILE_SCOPE("Spider Legs");
    auto& reg = world.Registry;
    Spider::WorldQuery wq;
    const Spider::BarrierQuery q(wq, m_Barriers);
    double legsUs = 0.0;
    for (Body& b : m_Bodies) {
        if (!b.Ready || b.Pain.Dead || !reg.valid(b.ModelEntity)) continue;
        // Its cost this frame, think and legs, however the loop leaves (the jank counters' per-spider budget).
        struct Cost {
            Body& B;
            double& Sum;
            std::chrono::steady_clock::time_point T0 = std::chrono::steady_clock::now();
            ~Cost() {
                const double legs = UsSince(T0), us = legs + B.Track.ThinkUs;
                Sum += legs;
                Stats::JankStats& j = B.Last.Jank;
                j.CostUs += us;
                j.WorstCostUs = std::max(j.WorstCostUs, us);
                ++j.CostFrames;
            }
        } cost{b, legsUs};
        const auto& sc = reg.get<SpiderComponent>(b.Root);
        auto* rc = reg.try_get<RenderableComponent>(b.ModelEntity);
        if (!rc || !rc->ModelRef) continue;
        Model& m = *rc->ModelRef;
        const glm::mat4 model = world.ComposeWorldTransform(b.ModelEntity);

        wq.Rays = 0;
        b.Gait.Step(q, GaitFrom(sc), model, b.Mover.Position(), b.Mover.Up(), b.Mover.Velocity(), b.Mover.Attached(), dt);

        Stats& st = b.Last;
        st.Rays += wq.Rays;
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
        MeasureLegs(b, q, GaitFrom(sc), model, dt);
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
    const double frameUs = m_ThinkUs + legsUs;
    m_ThinkUs = 0.0;
    m_Times.SumUs += frameUs;
    m_Times.WorstUs = std::max(m_Times.WorstUs, frameUs);
    ++m_Times.Frames;
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

void SpiderSystem::Alarm(entt::entity root) {
    for (Body& b : m_Bodies)
        if (b.Root == root) b.Ai.Alarmed = true;
}

void SpiderSystem::HearShot(const glm::vec3& origin, float radius) {
    for (Body& b : m_Bodies)
        if (glm::length(b.Mover.Position() - origin) < radius) b.Ai.Alarmed = true;
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

void SpiderSystem::MeasureBody(Body& b, const Spider::SurfaceQuery& q, const SpiderComponent& sc, const glm::vec3& aim,
                               bool meansToMove, unsigned moves, float dt) {
    Stats::JankStats& j = b.Last.Jank;
    Body::JankTrack& t = b.Track;
    const Spider::Mover& m = b.Mover;
    const glm::vec3 pos = m.Position(), up = m.Up(), fwd = m.Forward();
    const float moved = glm::length(pos - t.Pos);
    // Pops are measured at the body's centre (its belly, what the eye follows): over a corner the body turns about it, and
    // the origin between its feet swings round it.
    const float lift = Spider::MoverSettings{}.BodyLift;
    const float centreMoved = glm::length((pos + up * lift) - (t.Pos + t.Rot * glm::vec3(0.0f, lift, 0.0f)));
    if (t.Valid) {
        // Pops: further in a frame than its speed takes it (on a surface its own speed, in the air its fall's).
        const float v = m.Attached() && t.Attached ? std::max(t.Speed, m.Speed()) : std::max(glm::length(t.Vel), glm::length(m.Velocity()));
#pragma warning(suppress : 4996)
        static const bool trace = std::getenv("TARTARUS_SPIDER_TRACE") != nullptr;
        if (trace && (moves & (Spider::Mover::kMoveLetGo | Spider::Mover::kMoveDetach)))
            std::printf("[SpiderJank] %u lets go at %.2f %.2f %.2f up %.2f %.2f %.2f moves 0x%x mode %d\n", (unsigned)entt::to_integral(b.Root),
                        t.Pos.x, t.Pos.y, t.Pos.z, up.x, up.y, up.z, moves, (int)b.Ai.State);
        if (centreMoved > 1.5f * v * dt + 0.01f) {
            if (trace && j.Pops < 40)
                std::printf("[SpiderJank] %u pop %.3f m %.2f %.2f %.2f -> %.2f %.2f %.2f moves 0x%x mode %d attached %d\n",
                            (unsigned)entt::to_integral(b.Root), centreMoved, t.Pos.x, t.Pos.y, t.Pos.z, pos.x, pos.y, pos.z, moves,
                            (int)b.Ai.State, m.Attached());
            ++j.Pops;
            if (centreMoved > j.WorstPop) {
                j.WorstPop = centreMoved;
                j.WorstPopAt = pos;
                j.WorstPopMode = (int)b.Ai.State;
                j.WorstPopMoves = moves;
            }
        }
        {
            static int lowLines = 0;
            if (trace && pos.y < 0.0f && pos.y > -1.0f && t.Pos.y >= -0.004f && lowLines < 12) {
                ++lowLines;
                std::printf("[SpiderJank] %u below the floor top: %.3f %.3f %.3f -> %.3f %.3f %.3f up %.2f %.2f %.2f moves 0x%x corner %d mode %d\n",
                            (unsigned)entt::to_integral(b.Root), t.Pos.x, t.Pos.y, t.Pos.z, pos.x, pos.y, pos.z, up.x, up.y, up.z, moves,
                            m.Cornering(), (int)b.Ai.State);
            }
        }
        // Snaps: its up, or its whole rotation, turned faster than it may.
        const glm::vec3 upWas = t.Rot * glm::vec3(0.0f, 1.0f, 0.0f);
        const float upTurn = glm::degrees(std::acos(std::clamp(glm::dot(upWas, up), -1.0f, 1.0f)));
        const glm::quat d = m.Rotation() * glm::conjugate(t.Rot);
        const float total = glm::degrees(2.0f * std::acos(std::clamp(std::fabs(d.w), 0.0f, 1.0f)));
        if (upTurn > kMaxUpRateDeg * dt * 1.25f + 0.5f || total > (sc.TurnRate + kMaxUpRateDeg) * dt * 1.25f + 0.5f) {
            ++j.Snaps;
            if (std::max(upTurn, total) > j.WorstSnap) {
                j.WorstSnap = std::max(upTurn, total);
                j.WorstSnapAt = pos;
                j.WorstSnapMode = (int)b.Ai.State;
                j.WorstSnapMoves = moves;
            }
        }
        // Stutter: stopped dead for a moment and off again while it meant to go on; or given far less than its speed.
        const float vel = moved / dt;
        if (meansToMove) {
            if (vel < 0.05f) {
                t.StoppedFor += dt;
            } else {
                if (t.WasMoving && t.StoppedFor > 0.0f && t.StoppedFor < 0.4f) ++j.Stutters;
                t.StoppedFor = 0.0f;
                t.WasMoving = t.WasMoving || vel > 0.2f;
            }
            if (m.Speed() > 0.2f && moved < 0.25f * m.Speed() * dt) {
                ++j.Refused;
                static int refusedLines = 0;
#pragma warning(suppress : 4996)
                static const bool traceRefused = std::getenv("TARTARUS_SPIDER_TRACE") != nullptr;
                if (traceRefused && refusedLines < 40 && (j.Refused % 15) == 1) {
                    ++refusedLines;
                    const glm::vec3 u = m.Up(), f = m.Forward();
                    Spider::SurfaceHit lid;
                    const bool hasLid = q.Ray(pos + u * 0.001f, u, 0.05f, lid);
                    Spider::SurfaceHit ah, lo;
                    const bool hasAhead = q.Ray(pos + u * 0.1f, f, 0.8f, ah), hasLow = q.Ray(pos + u * 0.08f, f, 0.8f, lo);
                    std::printf("[SpiderJank]   ahead belly %.3f n %.2f %.2f %.2f at %.2f %.2f %.2f | low %.3f n %.2f %.2f %.2f\n", hasAhead ? ah.Distance : -1.0f,
                                ah.Normal.x, ah.Normal.y, ah.Normal.z, ah.Point.x, ah.Point.y, ah.Point.z, hasLow ? lo.Distance : -1.0f, lo.Normal.x,
                                lo.Normal.y, lo.Normal.z);
                    std::printf("[SpiderJank] %u refused at %.3f %.3f %.3f up %.2f %.2f %.2f fwd %.2f %.2f %.2f speed %.2f moved %.4f lid %.3f moves 0x%x corner %d reject %d mode %d\n",
                                (unsigned)entt::to_integral(b.Root), pos.x, pos.y, pos.z, u.x, u.y, u.z, f.x, f.y, f.z, m.Speed(), moved,
                                hasLid ? lid.Distance : -1.0f, moves, m.Cornering(), m.CornerReject(), (int)b.Ai.State);
                }
            }
        } else {
            t.WasMoving = false;
            t.StoppedFor = 0.0f;
        }
        // A stopped body that moves anyway.
        if (m.Attached() && t.Attached && t.Speed < 0.01f && m.Speed() < 0.01f && moved > 0.002f) {
            if (trace && j.StopSlides < 15)
                std::printf("[SpiderJank] %u stop slide %.3f at %.2f %.2f %.2f moves 0x%x mode %d corner %d\n", (unsigned)entt::to_integral(b.Root),
                            moved, pos.x, pos.y, pos.z, moves, (int)b.Ai.State, m.Cornering());
            ++j.StopSlides;
        }
        // Its aim turning back on itself twice within a second (a route flip-flopping).
        glm::vec3 a = aim - pos;
        a -= up * glm::dot(a, up);
        t.FlipAgo += dt;
        if (meansToMove && glm::length(a) > 0.05f) {
            a = glm::normalize(a);
            if (glm::length(t.Aim) > 0.5f && glm::dot(a, t.Aim) < 0.0f) {
                if (t.FlipAgo < 1.0f) ++j.Flips;
                t.FlipAgo = 0.0f;
            }
            t.Aim = a;
        } else {
            t.Aim = glm::vec3(0.0f);
        }
        // A full turn on the spot or near it (its yaw, decaying over about a second and a half).
        glm::vec3 f0 = t.Rot * glm::vec3(0.0f, 0.0f, -1.0f);
        f0 -= up * glm::dot(f0, up);
        if (glm::length(f0) > 1e-3f) {
            f0 = glm::normalize(f0);
            t.Yaw = t.Yaw * std::exp(-dt / 1.5f) + std::atan2(glm::dot(glm::cross(f0, fwd), up), glm::dot(f0, fwd));
            if (std::fabs(t.Yaw) > 6.2832f) {
                ++j.Spins;
                t.Yaw = 0.0f;
            }
        }
    }
    // Dithering: meaning to move for 2.5 s and never more than 0.3 m from where it was.
    t.MeantFor = meansToMove ? t.MeantFor + dt : 0.0f;
    if (!meansToMove) t.RingN = 0;
    t.RingClock += dt;
    if (t.RingClock >= 0.25f) {
        t.RingClock = 0.0f;
        for (size_t k = t.Ring.size() - 1; k > 0; --k) t.Ring[k] = t.Ring[k - 1];
        t.Ring[0] = pos;
        t.RingN = std::min(t.RingN + 1, (int)t.Ring.size());
    }
    if (t.RingN == (int)t.Ring.size() && t.MeantFor > 2.5f) {
        float far = 0.0f;
        for (const glm::vec3& r : t.Ring) far = std::max(far, glm::length(r - pos));
        if (far < 0.3f) {
#pragma warning(suppress : 4996)
            static const bool trace = std::getenv("TARTARUS_SPIDER_TRACE") != nullptr;
            if (trace && std::fmod(j.Dither, 1.0f) < dt)
                std::printf("[SpiderJank] %u dither at %.2f %.2f %.2f up %.2f %.2f %.2f aim %.2f %.2f %.2f speed %.2f corner %d mode %d\n",
                            (unsigned)entt::to_integral(b.Root), pos.x, pos.y, pos.z, up.x, up.y, up.z, aim.x, aim.y, aim.z, m.Speed(),
                            m.Cornering(), (int)b.Ai.State);
            j.Dither += dt;
        }
    }
    // Landing on nothing; standing off its surface.
    if (MeasureJank && m.Attached() && !m.Cornering()) { // (mid-corner the body bridges the two faces on its legs)
        Spider::SurfaceHit h;
        float gap = 1.0f;
        if (q.Ray(pos + up * 0.1f, -up, 0.25f, h)) gap = h.Distance - 0.1f;
        const glm::vec3 n = m.SurfaceNormal();
        if (gap > 0.04f && q.Ray(pos + n * 0.1f, -n, 0.25f, h)) gap = std::min(gap, h.Distance - 0.1f);
        if (gap > 0.04f) {
            if (!t.Attached) ++j.AirLandings;
            ++j.Floating;
        }
    }
    t.Valid = true;
    t.Pos = pos;
    t.Rot = m.Rotation();
    t.Speed = m.Speed();
    t.Vel = m.Velocity();
    t.Attached = m.Attached();
}

void SpiderSystem::MeasureLegs(Body& b, const Spider::SurfaceQuery& q, const Spider::GaitSettings& gs, const glm::mat4& model,
                               float dt) {
    Stats::JankStats& j = b.Last.Jank;
    Body::JankTrack& t = b.Track;
    const glm::vec3 body = b.Mover.Position();
    const float bodyMove = glm::length(body - t.LegBody);
    // The body's turn this frame sweeps a foot that goes with it (hanging, or swinging free while it falls).
    const glm::quat dq = b.Mover.Rotation() * glm::conjugate(t.LegRot);
    const float turned = 2.0f * std::acos(std::clamp(std::fabs(dq.w), 0.0f, 1.0f));
    bool stepping = false;
    for (int i = 0; i < Spider::kLegs; ++i) {
        const Spider::LegState& l = b.Gait.Leg(i);
        const Spider::LegRest& r = b.Gait.Rest(i);
        if (t.FeetValid) {
            // A foot further in a frame than its step takes it (a smoothstep's peak is 1.5x its mean) or the body carries it.
            float allowed = bodyMove + turned * glm::length(l.Foot - body) + 0.01f;
            if (!(l.Planted && l.Grounded) && !l.Stepping) allowed += Spider::kLegSwing * dt; // (a free foot swings to where it hangs)
            if (l.Stepping || t.Stepping[(size_t)i]) { // (a step's landing frame is still the step)
                const float dur = std::max(1e-3f, l.Duration);
                allowed += 1.6f * glm::length(l.To - l.From) * dt / dur + gs.StepHeight * 3.1416f * dt / dur;
            }
            const bool standing = l.Planted && l.Grounded && b.PrevPlanted[(size_t)i]; // (its travel is the slide)
            if (!standing && glm::length(l.Foot - t.Feet[(size_t)i]) > allowed) {
#pragma warning(suppress : 4996)
                static const bool trace = std::getenv("TARTARUS_SPIDER_TRACE") != nullptr;
                if (trace && j.LegPops < 25)
                    std::printf("[SpiderJank] %u leg %d pop %.3f allowed %.3f planted %d grounded %d stepping %d wasStepping %d T %.2f attached %d mode %d\n",
                                (unsigned)entt::to_integral(b.Root), i, glm::length(l.Foot - t.Feet[(size_t)i]), allowed, l.Planted, l.Grounded,
                                l.Stepping, (int)t.Stepping[(size_t)i], l.T, b.Mover.Attached(), (int)b.Ai.State);
                ++j.LegPops;
            }
        }
        t.Feet[(size_t)i] = l.Foot;
        t.Stepping[(size_t)i] = l.Stepping;
        stepping = stepping || l.Stepping;
        if (!(l.Planted && l.Grounded) || !b.Mover.Attached()) continue;
        const glm::vec3 hip = glm::vec3(model * glm::vec4(r.Hip, 1.0f));
        if (glm::length(l.Foot - hip) > r.Reach * 1.03f) ++j.Overreach;
        if (!MeasureJank) continue;
        // Standing on nothing, or under (inside) a surface.
        Spider::SurfaceHit h;
        const glm::vec3 n = l.Normal;
        if (!q.Ray(l.Foot + n * 0.06f, -n, 0.06f + std::max(0.0f, r.Lift) + 0.04f, h)) {
#pragma warning(suppress : 4996)
            static const bool trace = std::getenv("TARTARUS_SPIDER_TRACE") != nullptr;
            if (trace && j.FeetInAir < 15)
                std::printf("[SpiderJank] %u leg %d in air at %.2f %.2f %.2f n %.2f %.2f %.2f reach %.2f/%.2f body %.2f %.2f %.2f corner %d mode %d\n",
                            (unsigned)entt::to_integral(b.Root), i, l.Foot.x, l.Foot.y, l.Foot.z, n.x, n.y, n.z, glm::length(l.Foot - hip), r.Reach,
                            body.x, body.y, body.z, b.Mover.Cornering(), (int)b.Ai.State);
            ++j.FeetInAir;
        }
        else if (q.Ray(l.Foot + n * 0.08f, -n, 0.08f - 0.002f, h)) ++j.FeetInside;
    }
    t.FeetValid = true;
    t.LegBody = body;
    t.LegRot = b.Mover.Rotation();
    j.WorstSlide = std::max(j.WorstSlide, b.Last.PlantedSlide);
    // The gait out of step with the body: moving at pace with no leg stepping.
    const glm::vec3 up = b.Mover.Up(), v = b.Mover.Velocity();
    const float travel = glm::length(v - up * glm::dot(v, up));
    t.IdleLegs = b.Mover.Attached() && travel > 0.3f && !stepping ? t.IdleLegs + dt : 0.0f;
    if (t.IdleLegs > 0.4f) ++j.Skating;
}

int SpiderSystem::SumJank(Stats::JankStats& out) const {
    out = Stats::JankStats{};
    int n = 0;
    for (const Body& b : m_Bodies) {
        if (!b.Ready) continue;
        const Stats::JankStats& j = b.Last.Jank;
        ++n;
        out.Pops += j.Pops;
        if (j.WorstPop > out.WorstPop) {
            out.WorstPop = j.WorstPop;
            out.WorstPopAt = j.WorstPopAt;
            out.WorstPopMode = j.WorstPopMode;
            out.WorstPopMoves = j.WorstPopMoves;
        }
        out.Snaps += j.Snaps;
        if (j.WorstSnap > out.WorstSnap) {
            out.WorstSnap = j.WorstSnap;
            out.WorstSnapAt = j.WorstSnapAt;
            out.WorstSnapMode = j.WorstSnapMode;
            out.WorstSnapMoves = j.WorstSnapMoves;
        }
        out.Stutters += j.Stutters;
        out.Refused += j.Refused;
        out.FeetInAir += j.FeetInAir;
        out.FeetInside += j.FeetInside;
        out.WorstSlide = std::max(out.WorstSlide, j.WorstSlide);
        out.LegPops += j.LegPops;
        out.Skating += j.Skating;
        out.Overreach += j.Overreach;
        out.Flips += j.Flips;
        out.Spins += j.Spins;
        out.Dither += j.Dither;
        out.AirLandings += j.AirLandings;
        out.Floating += j.Floating;
        out.StopSlides += j.StopSlides;
        out.CostUs += j.CostUs;
        out.CostFrames += j.CostFrames;
        out.WorstCostUs = std::max(out.WorstCostUs, j.WorstCostUs);
    }
    return n;
}
