#include "SpiderDamage.h"

#include "Combat/Damage.h"
#include "Components.h"
#include "FirstPersonWeaponGameplay.h"
#include "Log.h"
#include "Model.h"
#include "Profiler.h"
#include "ProjectPaths.h"
#include "Renderer/BloodPalette.h"
#include "SpiderSystem.h"
#include "World.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

bool ReadText(const std::string& projectPath, std::string& out) {
    std::ifstream in(std::filesystem::u8path(ProjectPaths::Resolve(projectPath)), std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// The child under `root` that holds the rig (as SpiderSystem finds it): the first with a model and an Animator Controller.
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

glm::vec3 P(const glm::mat4& m) { return glm::vec3(m[3]); }

// The closest distance from `p` to the segment a..b.
float SegmentDistance(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float t = glm::dot(ab, ab) > 1e-10f ? std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

glm::mat4 Pose(const glm::vec3& c, const glm::quat& r) { return glm::translate(glm::mat4(1.0f), c) * glm::mat4_cast(r); }

constexpr float kCurlRise = 0.1f, kCurlHold = 0.15f, kCurlFall = 0.5f; // a struck leg drawn up, held, let back down (s)
constexpr float kLimpTime = 1.6f;   // seconds it limps after a leg round
constexpr float kLimpSpeed = 0.35f; // x its speed at the worst of it
constexpr float kHitboxGrow = 1.15f; // a leg's hitbox is a little fatter than its ragdoll capsule (thin legs are hard to hit)

} // namespace

// ---- bookkeeping ----

SpiderDamage::Body* SpiderDamage::Find(unsigned entity) {
    for (Body& b : m_Bodies)
        if ((unsigned)entt::to_integral(b.Root) == entity) return &b;
    return nullptr;
}

const SpiderDamage::Body* SpiderDamage::Find(unsigned entity) const {
    for (const Body& b : m_Bodies)
        if ((unsigned)entt::to_integral(b.Root) == entity) return &b;
    return nullptr;
}

bool SpiderDamage::Owns(unsigned entity) const {
    const Body* b = Find(entity);
    return b && b->Ready;
}

int SpiderDamage::RootPart() const {
    for (size_t i = 0; i < m_Rig.Parts.size(); ++i)
        if (m_Rig.Parts[i].Parent < 0) return (int)i;
    return 0;
}

float SpiderDamage::ZoneMultiplier(const Spider::RigDef& rig, Spider::PartZone zone) {
    const int z = std::clamp((int)zone, 0, Spider::kZones - 1);
    return std::max(rig.ZoneMultiplier[z], 0.0f);
}

bool SpiderDamage::Resolve(World& world, Body& b) {
    auto& reg = world.Registry;
    const auto* sc = reg.try_get<SpiderComponent>(b.Root);
    if (!sc) return false;
    if (sc->Rig != m_RigPath) {
        m_RigPath = sc->Rig;
        std::string text, error;
        m_RigOk = ReadText(m_RigPath, text) && Spider::ParseRig(text, m_Rig, &error) && !m_Rig.Parts.empty();
        if (!m_RigOk) Log::Error("Spider damage: rig '" + m_RigPath + "' has no usable parts" + (error.empty() ? std::string() : ": " + error) + ".");
    }
    if (!m_RigOk) return false;
    if (b.Model == entt::null || !reg.valid(b.Model)) b.Model = FindModelEntity(world, b.Root);
    if (b.Model == entt::null) return false;
    const auto* rc = reg.try_get<RenderableComponent>(b.Model);
    if (!rc || !rc->ModelRef || rc->ModelRef->NodeCount() == 0) return false;
    const Model& m = *rc->ModelRef;
    b.Parents.resize((size_t)m.NodeCount());
    for (int i = 0; i < m.NodeCount(); ++i) b.Parents[(size_t)i] = m.NodeParent(i);
    b.Count = (int)m_Rig.Parts.size();
    for (int k = 0; k < b.Count; ++k) {
        const Spider::PartDef& d = m_Rig.Parts[(size_t)k];
        b.Node[(size_t)k] = m.NodeIndex(d.Bone);
        b.StartNode[(size_t)k] = d.Start.empty() ? b.Node[(size_t)k] : m.NodeIndex(d.Start);
        b.EndNode[(size_t)k] = d.End.empty() ? -1 : m.NodeIndex(d.End);
        if (b.Node[(size_t)k] < 0 || b.StartNode[(size_t)k] < 0 || (!d.End.empty() && b.EndNode[(size_t)k] < 0)) {
            Log::Error("Spider damage: the model has no bone for part '" + d.Name + "' (" + m_RigPath + ").");
            m_RigOk = false;
            return false;
        }
    }
    // Each leg's outward line (its hip from the body's middle, level), from the bind pose: where the legs fold in from.
    IK::Pose bind;
    m.BindLocalPose(bind);
    IK::ComputeGlobals(bind, b.Parents, b.Globals);
    const int root = RootPart();
    const glm::vec3 centre = P(b.Globals[(size_t)b.Node[(size_t)root]]);
    for (int k = 0; k < b.Count; ++k) {
        const Spider::PartDef& d = m_Rig.Parts[(size_t)k];
        if (d.Leg < 0 || d.Segment != 0) continue;
        glm::vec3 out = P(b.Globals[(size_t)b.EndNode[(size_t)k] >= 0 ? (size_t)b.EndNode[(size_t)k] : (size_t)b.Node[(size_t)k]]) - centre;
        out.y = 0.0f;
        b.LegOut[(size_t)d.Leg] = glm::length(out) > 1e-4f ? glm::normalize(out) : glm::vec3(1.0f, 0.0f, 0.0f);
    }
    auto& hp = reg.get_or_emplace<HealthComponent>(b.Root);
    if (hp.Current < 0.0f) hp.Current = hp.Max;
    b.CurlAge.fill(-1.0f);
    b.Ready = true;
    return true;
}

// ---- rounds ----

bool SpiderDamage::OnHit(World& world, SpiderSystem& spiders, unsigned entity, const glm::vec3& point, const glm::vec3& origin,
                         const glm::vec3& dir, const FirstPersonWeaponGameplay& weapon, int pellets, Result& out) {
    const float range = glm::length(point - origin);
    const float damage = std::max(0.0f, weapon.Damage * DamageFalloff(range, weapon.FalloffStart, weapon.FalloffEnd, weapon.FalloffMin));
    return OnHit(world, spiders, entity, point, origin, dir, damage, pellets, out);
}

bool SpiderDamage::OnHit(World& world, SpiderSystem& spiders, unsigned entity, const glm::vec3& point, const glm::vec3& origin,
                         const glm::vec3& dirIn, float damage, int pellets, Result& out) {
    Body* b = Find(entity);
    if (!b || !b->Ready) return false;
    out = Result{};
    const glm::vec3 dir = glm::length(dirIn) > 1e-6f ? glm::normalize(dirIn) : glm::vec3(0.0f, 0.0f, -1.0f);
    out.Entity = entity;
    out.Point = point;
    out.Origin = origin;
    out.Direction = dir;
    out.Pellets = std::max(pellets, 1);

    // Which part: the body-part ray along the round's line (the hitbox, or the corpse's capsule, it struck), else the
    // capsule nearest the point.
    int part = -1;
    {
        PhysicsWorld::BodyPartHit h;
        const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z};
        if (PhysicsWorld::RaycastBodyParts(o, d, glm::length(point - origin) + 0.3f, h) && h.Entity == entity && h.Part >= 0 &&
            h.Part < b->Count)
            part = h.Part;
    }
    if (part < 0) {
        float best = 1e9f;
        for (int k = 0; k < b->Count; ++k) {
            const glm::mat4& w = b->Dead ? b->PartWorld[(size_t)k] : Pose(glm::vec3(b->Caps[(size_t)k].Position[0], b->Caps[(size_t)k].Position[1], b->Caps[(size_t)k].Position[2]),
                                                                          glm::quat(b->Caps[(size_t)k].Rotation[3], b->Caps[(size_t)k].Rotation[0], b->Caps[(size_t)k].Rotation[1], b->Caps[(size_t)k].Rotation[2]));
            const glm::vec3 axis = glm::vec3(w[0]) * b->Caps[(size_t)k].HalfLength;
            const float dist = SegmentDistance(point, P(w) - axis, P(w) + axis) - m_Rig.Parts[(size_t)k].Radius;
            if (dist < best) { best = dist; part = k; }
        }
    }
    part = std::clamp(part, 0, b->Count - 1);
    const Spider::PartDef& def = m_Rig.Parts[(size_t)part];
    out.Part = part;
    out.Zone = def.Zone;
    out.Damage = damage * ZoneMultiplier(m_Rig, def.Zone);
    b->LastPart = part;
    b->LastDamage = out.Damage;
    ++b->Hits;

    if (b->Dead) { // a round into the corpse: it jolts, wakes, and settles again
        if (b->Ragdoll >= 0) {
            const glm::vec3 j = dir * std::clamp(0.25f + out.Damage * 0.01f, 0.25f, 1.2f) * std::min(def.Mass, 0.6f);
            const float ji[3] = {j.x, j.y, j.z}, at[3] = {point.x, point.y, point.z};
            PhysicsWorld::RagdollImpulse(b->Ragdoll, part, ji, at);
            b->Settle.Wake();
        }
        return true;
    }
    out.Living = true;
    auto& hp = world.Registry.get_or_emplace<HealthComponent>(b->Root);
    if (hp.Current < 0.0f) hp.Current = hp.Max;
    if (!hp.Invulnerable) hp.Current -= out.Damage;
    const glm::mat4 model = world.ComposeWorldTransform(b->Model);
    if (hp.Current <= 0.0f) {
        hp.Current = 0.0f;
        out.Killed = true;
        Kill(world, spiders, *b, part, dir, out.Damage, point);
        return true;
    }
    Flinch(*b, part, dir, out.Damage, model);
    SpiderSystem::Sound("hurt", point);
    SpiderSystem::Sound("splat", point, 0.8f);
    if (def.Zone == Spider::PartZone::Leg && def.Leg >= 0) { // it staggers: that leg up off its foothold, a limp after
        b->CurlAge[(size_t)def.Leg] = 0.0f;
        b->LimpAge = 0.0f;
    }
    // A heavy round into a spider hanging on a wall or the ceiling knocks it off.
    SpiderSystem::Stats st;
    if (Heavy(out.Damage, out.Pellets) && spiders.Inspect(b->Root, st) && st.Attached && st.Up.y < 0.6f) {
        // Off the surface (along its normal) and on the way the round went across it: never back into it.
        const glm::vec3 v = st.Up * 1.3f + (dir - st.Up * glm::dot(dir, st.Up)) * 1.2f;
        out.KnockedOff = spiders.Detach(b->Root, v);
    }
    return true;
}

BloodFx::Hit SpiderDamage::Blood(const Result& r) {
    BloodFx::Hit h;
    h.Point = r.Point;
    h.Direction = r.Direction;
    h.Entity = r.Entity;
    h.Part = r.Part;
    // The soft abdomen is a bag of it: it throws more than a round's damage alone would.
    h.Damage = r.Damage * (r.Zone == Spider::PartZone::Abdomen ? 1.8f : 1.0f);
    h.Killed = r.Killed;
    h.Head = r.Zone == Spider::PartZone::Head;
    h.Corpse = !r.Living;
    h.Pellets = r.Pellets;
    h.Origin = r.Origin;
    h.ByPlayer = true;
    h.Palette = BloodPalette::kIchor;
    return h;
}

void SpiderDamage::Flinch(Body& b, int part, const glm::vec3& dirWorld, float damage, const glm::mat4& model) {
    if (part < 0 || part >= b.Count || b.Globals.empty()) return;
    const Spider::PartDef& def = m_Rig.Parts[(size_t)part];
    const glm::vec3 dir = glm::normalize(glm::inverse(glm::mat3(model)) * dirWorld);
    const int end = b.EndNode[(size_t)part] >= 0 ? b.EndNode[(size_t)part] : b.Node[(size_t)part];
    const glm::vec3 bone = P(b.Globals[(size_t)end]) - P(b.Globals[(size_t)b.StartNode[(size_t)part]]);
    glm::vec3 axis = glm::cross(bone, dir); // its far end goes where the round went
    if (glm::length(axis) < 1e-5f) axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), dir);
    if (glm::length(axis) < 1e-5f) return;
    axis = glm::normalize(axis);
    const float time = std::max(m_Rig.FlinchTime, 0.05f), w = 5.0f / time;
    const float peak = glm::radians(m_Rig.FlinchDeg[(int)def.Zone]) * std::clamp(damage / 30.0f, 0.5f, 1.4f);
    // Critically damped: an initial angular speed of peak * w * e peaks at `peak`, a fifth of the flinch's time in.
    const float v = peak * w * 2.71828f;
    auto kick = [&](int k, float share) {
        if (k < 0 || k >= b.Count) return;
        glm::vec3& vel = b.FlVel[(size_t)k];
        vel += axis * v * share;
        const float cap = v * 1.6f;
        if (glm::length(vel) > cap) vel = glm::normalize(vel) * cap;
    };
    kick(part, 1.0f);
    const int parent = def.Parent;
    if (parent >= 0 && m_Rig.Parts[(size_t)parent].Parent >= 0) kick(parent, 0.4f);
    kick(RootPart(), def.Zone == Spider::PartZone::Leg ? 0.15f : 0.3f); // the body staggers with it
    b.Flinching = true;
}

// ---- every frame ----

void SpiderDamage::LateUpdate(World& world, SpiderSystem& spiders, float dt) {
    if (const std::uint64_t gen = PhysicsWorld::SimulationGeneration(); gen != m_Generation) {
        m_Bodies.clear(); // a new Play (the physics went with the last one)
        m_Generation = gen;
    }
    auto& reg = world.Registry;
    m_Bodies.erase(std::remove_if(m_Bodies.begin(), m_Bodies.end(),
                                  [&](const Body& b) {
                                      const bool gone = !reg.valid(b.Root) || !reg.all_of<SpiderComponent>(b.Root);
                                      if (gone && b.Hitboxes >= 0) PhysicsWorld::DestroyNpcHitboxes(b.Hitboxes);
                                      if (gone && b.Ragdoll >= 0) PhysicsWorld::DestroyRagdoll(b.Ragdoll);
                                      return gone;
                                  }),
                   m_Bodies.end());
    for (auto e : reg.view<SpiderComponent, TransformComponent>(entt::exclude<InactiveTag>))
        if (std::none_of(m_Bodies.begin(), m_Bodies.end(), [&](const Body& b) { return b.Root == e; })) {
            Body b;
            b.Root = e;
            m_Bodies.push_back(std::move(b));
        }
    if (m_Bodies.empty() || dt <= 0.0f) return;
    PROFILE_SCOPE("Spider Damage");
    for (Body& b : m_Bodies) {
        if (!b.Ready) {
            SpiderSystem::Stats st;
            if (!spiders.Inspect(b.Root, st) || !st.Ready || !Resolve(world, b)) continue;
        }
        if (b.Dead) PoseDead(world, b, dt);
        else PoseLiving(world, spiders, b, dt);
    }
}

void SpiderDamage::PoseLiving(World& world, SpiderSystem& spiders, Body& b, float dt) {
    auto& reg = world.Registry;
    if (!reg.valid(b.Model)) return;
    auto* rc = reg.try_get<RenderableComponent>(b.Model);
    if (!rc || !rc->ModelRef) return;
    Model& m = *rc->ModelRef;

    // The hurt legs and the limp, for SpiderSystem's next frame.
    if (SpiderSystem::Hurt* hurt = spiders.HurtOf(b.Root)) {
        for (int i = 0; i < Spider::kLegs; ++i) {
            float& age = b.CurlAge[(size_t)i];
            float curl = 0.0f;
            if (age >= 0.0f) {
                age += dt;
                if (age < kCurlRise) curl = age / kCurlRise;
                else if (age < kCurlRise + kCurlHold) curl = 1.0f;
                else curl = std::max(0.0f, 1.0f - (age - kCurlRise - kCurlHold) / kCurlFall);
                if (age > kCurlRise + kCurlHold + kCurlFall) age = -1.0f;
            }
            hurt->LegCurl[(size_t)i] = curl * 0.9f;
        }
        hurt->SpeedScale = 1.0f;
        if (b.LimpAge >= 0.0f) {
            b.LimpAge += dt;
            const float t = std::clamp((b.LimpAge - 0.25f) / (kLimpTime - 0.25f), 0.0f, 1.0f);
            hurt->SpeedScale = kLimpSpeed + (1.0f - kLimpSpeed) * t * t;
            if (b.LimpAge > kLimpTime) b.LimpAge = -1.0f;
        }
    }

    // The pose as SpiderSystem left it (the animator's, the legs on their feet), the flinch added.
    const IK::Pose& applied = m.AppliedLocalPose();
    if (applied.size() == (size_t)m.NodeCount()) b.Pose = applied;
    else m.BindLocalPose(b.Pose);
    IK::ComputeGlobals(b.Pose, b.Parents, b.Globals);
    if (b.Flinching) {
        const float w = 5.0f / std::max(m_Rig.FlinchTime, 0.05f);
        bool any = false;
        for (int k = 0; k < b.Count; ++k) {
            glm::vec3& a = b.FlAngle[(size_t)k];
            glm::vec3& v = b.FlVel[(size_t)k];
            if (glm::dot(a, a) + glm::dot(v, v) < 1e-10f) { a = v = glm::vec3(0.0f); continue; }
            v += (-w * w * a - 2.0f * w * v) * dt; // semi-implicit: stable at w dt ~ 0.25
            a += v * dt;
            any = true;
            const float angle = glm::length(a);
            if (angle < 1e-4f) continue;
            const int node = b.Node[(size_t)k];
            IK::OffsetBone(b.Pose, b.Parents, b.Globals, node, glm::vec3(0.0f), glm::angleAxis(angle, a / angle), P(b.Globals[(size_t)node]));
        }
        b.Flinching = any;
        if (any) m.ApplyLocalPose(b.Pose);
    }

    // The hitboxes onto the bones as drawn.
    const glm::mat4 model = world.ComposeWorldTransform(b.Model);
    const glm::vec3 up = glm::normalize(glm::vec3(model[1]));
    for (int k = 0; k < b.Count; ++k) {
        const Spider::PartDef& d = m_Rig.Parts[(size_t)k];
        const glm::vec3 a = glm::vec3(model * glm::vec4(P(b.Globals[(size_t)b.StartNode[(size_t)k]]), 1.0f));
        glm::vec3 e;
        if (b.EndNode[(size_t)k] >= 0) e = glm::vec3(model * glm::vec4(P(b.Globals[(size_t)b.EndNode[(size_t)k]]), 1.0f));
        else {
            const int pn = b.Parents[(size_t)b.Node[(size_t)k]];
            const glm::vec3 from = pn >= 0 ? glm::vec3(model * glm::vec4(P(b.Globals[(size_t)pn]), 1.0f)) : a - up;
            e = a + (glm::length(a - from) > 1e-5f ? glm::normalize(a - from) : up) * d.Length;
        }
        const float radius = d.Radius * (d.Leg >= 0 ? kHitboxGrow : 1.0f);
        glm::vec3 c;
        glm::quat r;
        float half;
        SpiderRagdoll::CapsuleOver(a, e, radius, up, c, r, half);
        PhysicsWorld::HitCapsule& cap = b.Caps[(size_t)k];
        cap.Position[0] = c.x; cap.Position[1] = c.y; cap.Position[2] = c.z;
        cap.Rotation[0] = r.x; cap.Rotation[1] = r.y; cap.Rotation[2] = r.z; cap.Rotation[3] = r.w;
        cap.HalfLength = half;
        cap.Radius = radius;
        b.PrevCentre[(size_t)k] = b.Centre[(size_t)k];
        b.Centre[(size_t)k] = c;
    }
    b.HavePrev = b.Hitboxes >= 0; // (the first frame has nothing before it)
    b.PrevDt = dt;
    if (b.Hitboxes < 0) b.Hitboxes = PhysicsWorld::CreateBodyHitboxes((unsigned)entt::to_integral(b.Root), b.Caps.data(), b.Count);
    else PhysicsWorld::SetNpcHitboxPoses(b.Hitboxes, b.Caps.data(), b.Count);
}

void SpiderDamage::Kill(World& world, SpiderSystem& spiders, Body& b, int part, const glm::vec3& dir, float damage, const glm::vec3& point) {
    b.Dead = true;
    b.DeadFor = 0.0f;
    SpiderSystem::Sound("death", point);
    SpiderSystem::Sound("splat", point);
    if (SpiderSystem::Hurt* hurt = spiders.HurtOf(b.Root)) {
        hurt->Dead = true;
        hurt->LegCurl.fill(0.0f);
    }
    if (b.Hitboxes >= 0) PhysicsWorld::DestroyNpcHitboxes(b.Hitboxes);
    b.Hitboxes = -1;
    auto& reg = world.Registry;
    if (b.Globals.size() != b.Parents.size()) return; // never posed: nothing to build from
    const glm::mat4 model = world.ComposeWorldTransform(b.Model);
    const glm::vec3 up = glm::normalize(glm::vec3(model[1]));
    auto world_ = [&](int node) { return glm::vec3(model * glm::vec4(P(b.Globals[(size_t)node]), 1.0f)); };

    std::array<SpiderRagdoll::PartPoint, Spider::kMaxParts> points{};
    for (int k = 0; k < b.Count; ++k) {
        SpiderRagdoll::PartPoint& p = points[(size_t)k];
        p.Anchor = world_(b.Node[(size_t)k]);
        p.A = world_(b.StartNode[(size_t)k]);
        p.B = b.EndNode[(size_t)k] >= 0 ? world_(b.EndNode[(size_t)k]) : p.A + up * m_Rig.Parts[(size_t)k].Length;
    }
    std::array<PhysicsWorld::RagdollPart, Spider::kMaxParts> parts{};
    if (!SpiderRagdoll::BuildParts(m_Rig.Parts.data(), b.Count, points.data(), up, parts.data(), b.PartWorld.data())) return;
    // Each part carries its bones' motion (the body crawling, a leg mid-step), clamped against a bad frame.
    if (b.HavePrev && b.PrevDt > 0.002f)
        for (int k = 0; k < b.Count; ++k) {
            glm::vec3 v = (b.Centre[(size_t)k] - b.PrevCentre[(size_t)k]) / b.PrevDt;
            if (glm::length(v) > 4.0f) v = glm::normalize(v) * 4.0f;
            parts[(size_t)k].Velocity[0] = v.x; parts[(size_t)k].Velocity[1] = v.y; parts[(size_t)k].Velocity[2] = v.z;
        }
    const PhysicsWorld::RagdollParams prm = SpiderRagdoll::BodyParams();
    b.Ragdoll = PhysicsWorld::CreateRagdoll((unsigned)entt::to_integral(b.Root), parts.data(), b.Count, &prm);
    if (b.Ragdoll < 0) {
        Log::Error("Spider damage: the ragdoll wouldn't build; the body stays where it died.");
        return;
    }
    // The killing round's shove, at the part it struck.
    const glm::vec3 j = dir * std::clamp(0.6f + damage * 0.012f, 0.6f, 1.8f);
    const float ji[3] = {j.x, j.y, j.z}, at[3] = {point.x, point.y, point.z};
    PhysicsWorld::RagdollImpulse(b.Ragdoll, part, ji, at);

    // The death curl: every leg folded in under the body, then slack.
    std::array<glm::quat, Spider::kMaxParts> targets{};
    std::array<float, Spider::kMaxParts> weight{};
    for (int k = 0; k < b.Count; ++k) {
        const Spider::PartDef& d = m_Rig.Parts[(size_t)k];
        targets[(size_t)k] = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        weight[(size_t)k] = d.Parent < 0 ? 0.0f : d.Leg >= 0 ? 1.0f : 0.5f;
        if (d.Leg < 0) continue;
        glm::vec3 out = glm::mat3(model) * b.LegOut[(size_t)d.Leg];
        out -= up * glm::dot(out, up);
        const glm::quat rot = glm::quat_cast(glm::mat3(b.PartWorld[(size_t)k]));
        targets[(size_t)k] = SpiderRagdoll::CurlTarget(d, rot, up, glm::length(out) > 1e-4f ? glm::normalize(out) : glm::vec3(1, 0, 0));
    }
    b.Motor.Begin(b.Ragdoll, b.Count, targets.data(), weight.data(), SpiderRagdoll::LimpMotor::Settings());
    b.Settle.Begin(b.Ragdoll);

    // The skin: every bone under the body rides the part that carries it (the nearest part bone at or above it).
    const int nodes = (int)b.Parents.size();
    std::vector<int> partOf((size_t)nodes, -1);
    for (int k = 0; k < b.Count; ++k) partOf[(size_t)b.Node[(size_t)k]] = k;
    b.Owner.assign((size_t)nodes, -1);
    b.Offset.assign((size_t)nodes, glm::mat4(1.0f));
    for (int n = 0; n < nodes; ++n) {
        int owner = -1;
        for (int a = n, guard = 0; a >= 0 && guard < nodes; a = b.Parents[(size_t)a], ++guard)
            if (partOf[(size_t)a] >= 0) { owner = partOf[(size_t)a]; break; }
        if (owner < 0) continue;
        b.Owner[(size_t)n] = owner;
        b.Offset[(size_t)n] = glm::inverse(b.PartWorld[(size_t)owner]) * model * b.Globals[(size_t)n];
    }
    const int root = RootPart();
    const glm::mat4 rootInv = glm::inverse(b.PartWorld[(size_t)root]);
    for (int k = 0; k < b.Count; ++k) {
        const Spider::PartDef& d = m_Rig.Parts[(size_t)k];
        if (d.Leg >= 0 && d.Segment == 2) b.TipAtDeath[(size_t)d.Leg] = glm::vec3(rootInv * glm::vec4(points[(size_t)k].B, 1.0f));
    }
    if (auto* t = reg.try_get<TransformComponent>(b.Root)) b.RootOffset = t->Position - P(b.PartWorld[(size_t)root]);
    b.PoseWritten = false;
}

void SpiderDamage::PoseDead(World& world, Body& b, float dt) {
    auto& reg = world.Registry;
    if (!reg.valid(b.Model)) return;
    auto* rc = reg.try_get<RenderableComponent>(b.Model);
    if (!rc || !rc->ModelRef || b.Pose.size() != (size_t)rc->ModelRef->NodeCount()) return;
    Model& m = *rc->ModelRef;
    b.DeadFor += dt;
    if (b.Ragdoll < 0) { // never built: it keeps the pose it died in
        m.ApplyLocalPose(b.Pose);
        return;
    }
    b.Motor.Update(dt);
    b.Settle.Update(dt);
    // Asleep and already shown so: the same pose again (the animator still writes its own each frame).
    if (b.PoseWritten && PhysicsWorld::RagdollAsleep(b.Ragdoll)) {
        m.ApplyLocalPose(b.Pose);
        return;
    }
    for (int k = 0; k < b.Count; ++k) {
        float p[3], q[4];
        if (!PhysicsWorld::GetRagdollPart(b.Ragdoll, k, p, q)) continue;
        b.PartWorld[(size_t)k] = Pose(glm::vec3(p[0], p[1], p[2]), glm::normalize(glm::quat(q[3], q[0], q[1], q[2])));
    }
    // The root entity follows the body (the mesh's bounds and whatever hangs off the root go with it).
    if (auto* t = reg.try_get<TransformComponent>(b.Root)) t->Position = P(b.PartWorld[(size_t)RootPart()]) + b.RootOffset;
    const glm::mat4 toModel = glm::inverse(world.ComposeWorldTransform(b.Model));
    const int nodes = (int)b.Parents.size();
    for (int n = 0; n < nodes; ++n) {
        const int k = b.Owner[(size_t)n];
        if (k < 0) continue; // (the rig's own root and control bones: as they were)
        b.Globals[(size_t)n] = toModel * b.PartWorld[(size_t)k] * b.Offset[(size_t)n];
        const int parent = b.Parents[(size_t)n];
        const glm::mat4 local = parent >= 0 ? glm::inverse(b.Globals[(size_t)parent]) * b.Globals[(size_t)n] : b.Globals[(size_t)n];
        LocalTRS& l = b.Pose[(size_t)n];
        l.T = glm::vec3(local[3]);
        glm::mat3 r(local);
        r[0] = glm::normalize(r[0]);
        r[1] = glm::normalize(r[1]);
        r[2] = glm::normalize(r[2]);
        l.R = glm::normalize(glm::quat_cast(r));
    }
    m.ApplyLocalPose(b.Pose);
    b.PoseWritten = true;
}

// ---- the blood's hooks, tests ----

bool SpiderDamage::BodyCentre(const World& world, unsigned entity, glm::vec3& centre) const {
    const Body* b = Find(entity);
    if (!b || !b->Ready) return false;
    const int root = RootPart();
    if (b->Dead && b->Ragdoll >= 0) {
        // Between the thorax and the abdomen: where a dead spider's ichor pools from.
        centre = P(b->PartWorld[(size_t)root]);
        for (int k = 0; k < b->Count; ++k)
            if (m_Rig.Parts[(size_t)k].Zone == Spider::PartZone::Abdomen) centre = glm::mix(centre, P(b->PartWorld[(size_t)k]), 0.5f);
        return true;
    }
    if (b->Hitboxes >= 0) {
        centre = b->Centre[(size_t)root];
        return true;
    }
    const auto* t = world.Registry.try_get<TransformComponent>(b->Root);
    if (!t) return false;
    centre = t->Position;
    return true;
}

bool SpiderDamage::SplatSpace(const World& world, unsigned entity, int part, const glm::vec3& point, BloodFx::SplatSpace& out) const {
    const Body* b = Find(entity);
    if (!b || !b->Ready || !world.Registry.valid(b->Model)) return false;
    const auto* rc = world.Registry.try_get<RenderableComponent>(b->Model);
    if (!rc || !rc->ModelRef) return false;
    const Model& m = *rc->ModelRef;
    if (part < 0 || part >= b->Count) { // the part nearest the point
        float best = 1e9f;
        for (int k = 0; k < b->Count; ++k) {
            const glm::vec3 c = b->Dead ? P(b->PartWorld[(size_t)k]) : b->Centre[(size_t)k];
            if (const float d = glm::length(c - point); d < best) { best = d; part = k; }
        }
    }
    if (part < 0) return false;
    // The splat rides the part's bone (or the nearest skinning bone above it).
    int bone = -1;
    for (int n = b->Node[(size_t)part], guard = 0; n >= 0 && bone < 0 && guard < (int)b->Parents.size(); n = b->Parents[(size_t)n], ++guard)
        bone = m.BoneId(m.NodeName(n));
    if (bone < 0) return false;
    out.Group = entity;
    out.Members.emplace_back((unsigned)entt::to_integral(b->Model),
                             glm::inverse(world.GetCachedWorldTransform(b->Model) * m.FinalBoneMatrix(bone)));
    return true;
}

int SpiderDamage::PartIndex(const std::string& name) const {
    for (size_t i = 0; i < m_Rig.Parts.size(); ++i)
        if (m_Rig.Parts[i].Name == name) return (int)i;
    return -1;
}

bool SpiderDamage::PartCentre(entt::entity root, int part, glm::vec3& out) const {
    const Body* b = Find((unsigned)entt::to_integral(root));
    if (!b || !b->Ready || part < 0 || part >= b->Count) return false;
    out = b->Dead ? P(b->PartWorld[(size_t)part]) : b->Centre[(size_t)part];
    return b->Dead || b->Hitboxes >= 0;
}

bool SpiderDamage::Inspect(entt::entity root, View& out) const {
    const Body* b = Find((unsigned)entt::to_integral(root));
    if (!b) return false;
    out = View{};
    out.Ready = b->Ready;
    out.Dead = b->Dead;
    out.Parts = b->Count;
    out.Hitboxes = b->Hitboxes;
    out.Ragdoll = b->Ragdoll;
    out.Hits = b->Hits;
    out.LastDamage = b->LastDamage;
    out.LastPart = b->LastPart;
    for (int k = 0; k < b->Count; ++k) out.Flinch = std::max(out.Flinch, glm::length(b->FlAngle[(size_t)k]));
    out.DeadFor = b->DeadFor;
    if (!b->Dead) {
        for (int i = 0; i < Spider::kLegs; ++i) {
            const float age = b->CurlAge[(size_t)i];
            if (age >= 0.0f) out.LegCurlMax = std::max(out.LegCurlMax, age < kCurlRise + kCurlHold + kCurlFall ? 1.0f - std::max(0.0f, age - kCurlRise - kCurlHold) / kCurlFall : 0.0f);
        }
        if (b->LimpAge >= 0.0f) {
            const float t = std::clamp((b->LimpAge - 0.25f) / (kLimpTime - 0.25f), 0.0f, 1.0f);
            out.SpeedScale = kLimpSpeed + (1.0f - kLimpSpeed) * t * t;
        }
        return true;
    }
    if (b->Ragdoll < 0) return true;
    out.Asleep = PhysicsWorld::RagdollAsleep(b->Ragdoll);
    out.AsleepAt = b->Settle.AsleepAt();
    out.MotorStrength = b->Motor.Strength();
    float lin = 0.0f, ang = 0.0f;
    if (PhysicsWorld::RagdollMotion(b->Ragdoll, &lin, &ang)) out.MaxSpeed = lin;
    const int rp = RootPart();
    out.Centre = P(b->PartWorld[(size_t)rp]);
    out.LowestPart = 1e9f;
    const glm::mat4 rootInv = glm::inverse(b->PartWorld[(size_t)rp]);
    float fold = 0.0f;
    int legs = 0;
    for (int k = 0; k < b->Count; ++k) {
        const glm::mat4& w = b->PartWorld[(size_t)k];
        float r = 0.0f, half = 0.0f;
        PhysicsWorld::GetRagdollPartShape(b->Ragdoll, k, &r, &half);
        const glm::vec3 c = P(w), x = glm::vec3(w[0]);
        out.Finite = out.Finite && std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z);
        out.LowestPart = std::min(out.LowestPart, c.y - std::fabs(x.y) * half - r);
        const Spider::PartDef& d = m_Rig.Parts[(size_t)k];
        if (d.Leg >= 0 && d.Segment == 2) { // how far the tip has come in toward the body, as an angle about the root part
            const glm::vec3 tip = glm::vec3(rootInv * glm::vec4(c + x * (half + r), 1.0f));
            const glm::vec3 was = b->TipAtDeath[(size_t)d.Leg];
            if (glm::length(tip) > 1e-4f && glm::length(was) > 1e-4f) {
                fold += glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(tip), glm::normalize(was)), -1.0f, 1.0f)));
                ++legs;
            }
        }
    }
    out.LegFold = legs ? fold / (float)legs : 0.0f;
    return true;
}
