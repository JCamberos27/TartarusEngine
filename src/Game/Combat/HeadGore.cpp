#include "HeadGore.h"

#include "AssetLibrary.h"
#include "Components.h"
#include "FxSprites.h"
#include "GameModuleAPI.h"
#include "Log.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "ProjectPaths.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

bool HeadGore::MaskUnder(const Model& m, const char* root, std::uint32_t (&bits)[16]) {
    bool any = false;
    std::vector<char> under((size_t)m.NodeCount(), 0);
    for (int i = 0; i < m.NodeCount(); ++i) { // parents come before their children
        const int p = m.NodeParent(i);
        under[(size_t)i] = (p >= 0 && under[(size_t)p]) || m.NodeName(i) == root;
        if (!under[(size_t)i]) continue;
        const int id = m.BoneId(m.NodeName(i));
        if (id < 0 || id >= 16 * 32) continue;
        bits[id >> 5] |= 1u << (id & 31);
        any = true;
    }
    return any;
}

glm::quat HeadGore::StumpRotation(const glm::vec3& neck, const glm::vec3& head, const glm::vec3& forward) {
    glm::vec3 up = head - neck;
    up = glm::length(up) > 1e-4f ? glm::normalize(up) : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 fwd = forward - up * glm::dot(forward, up);
    if (glm::length(fwd) < 1e-3f) fwd = std::abs(up.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
    fwd = glm::normalize(fwd - up * glm::dot(fwd, up));
    const glm::vec3 right = glm::normalize(glm::cross(up, fwd));
    // The stump's own frame: +Y up, +Z its front (as baked: Blender -Y forward -> engine +Z).
    return glm::quat_cast(glm::mat3(right, up, fwd));
}

bool HeadGore::LoadMeshes(AssetLibrary& assets) {
    if (m_LoadTried) return m_Stump != nullptr;
    m_LoadTried = true;
    const std::string dir = ProjectPaths::Resolve("assets/Effects/Knife/Gore");
    if (!std::filesystem::exists(std::filesystem::u8path(dir + "/exploded_head.fbx"))) {
        Log::Warn("Head gore: no meshes in assets/Effects/Knife/Gore - run tools/knife_gore_bake.py (docs/BLOOD_FX.md)");
        return false;
    }
    m_Stump = assets.LoadModel(dir + "/exploded_head.fbx");
    for (int i = 1; i <= 4; ++i)
        if (auto m = assets.LoadModel(dir + "/brain_part_" + std::to_string(i) + ".fbx")) m_Brain.push_back(m);
    return m_Stump != nullptr;
}

bool HeadGore::Headless(unsigned entity) const {
    for (const Victim& v : m_Victims)
        if (v.Entity == entity) return true;
    return false;
}

bool HeadGore::Burst(World& world, AssetLibrary& assets, unsigned entity, const glm::vec3& point, const glm::vec3& dir, float energy) {
    if (Headless(entity) || !m_Body || !LoadMeshes(assets)) return false;
    Body body;
    if (!m_Body(entity, body) || body.Pieces.empty()) return false;
    // The head and all that hangs on it, off every piece of him.
    bool any = false;
    for (entt::entity p : body.Pieces) {
        const auto* r = world.Registry.valid(p) ? world.Registry.try_get<RenderableComponent>(p) : nullptr;
        if (!r || !r->ModelRef || !r->ModelRef->HasBones()) continue;
        GoreHideTag tag;
        // Clothing from the neck up (a hood standing empty round the stump reads wrong); the body itself from the
        // head up, so its neck stays under the stump.
        const auto* piece = world.Registry.try_get<OutfitPieceComponent>(p);
        const bool clothing = piece && !(piece->Flags & OutfitPieceBodyPart);
        if (!MaskUnder(*r->ModelRef, clothing ? "neck_01" : "head", tag.Bones)) continue;
        world.Registry.emplace_or_replace<GoreHideTag>(p, tag);
        any = true;
    }
    if (!any) return false;
    Victim v{entity};
    const glm::quat rot = StumpRotation(body.Neck, body.Head, body.Forward);
    v.Stump = world.CreateModelEntity(m_Stump->CreateInstance(), body.Head, glm::vec3(0.0f), glm::vec3(1.0f), "[Runtime] Head gore");
    world.SetWorldPose(v.Stump, body.Head, rot);
    m_Victims.push_back(v);
    ++m_Bursts;

    // Brain chunks, thrown on along the round's line and up.
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    const glm::vec3 d = glm::length(dir) > 1e-4f ? glm::normalize(dir) : glm::vec3(0, 0, 1);
    const int chunks = std::min((int)m_Brain.size(), 2 + (energy > 1.8f ? 1 : 0));
    for (int i = 0; i < chunks; ++i) {
        const auto& mesh = m_Brain[(size_t)((m_Bursts + i) % (int)m_Brain.size())];
        Chunk c;
        c.Position = point + d * 0.06f + glm::vec3(u(m_Rng), u(m_Rng) * 0.5f + 0.5f, u(m_Rng)) * 0.04f;
        c.Velocity = d * (2.0f + 1.5f * (u(m_Rng) * 0.5f + 0.5f)) * std::min(energy, 2.0f) * 0.7f +
                     glm::vec3(u(m_Rng), 1.2f + 0.8f * u(m_Rng), u(m_Rng)) * 1.2f;
        c.Spin = glm::vec3(u(m_Rng), u(m_Rng), u(m_Rng)) * 14.0f;
        c.Rotation = glm::normalize(glm::quat(1.0f, u(m_Rng), u(m_Rng), u(m_Rng)));
        const glm::vec3 size = glm::max(mesh->BoundsMax() - mesh->BoundsMin(), glm::vec3(0.01f));
        c.Radius = std::clamp(0.35f * std::min({size.x, size.y, size.z}), 0.008f, 0.03f);
        c.Entity = world.CreateModelEntity(mesh->CreateInstance(), c.Position, glm::vec3(0.0f), glm::vec3(1.0f), "[Runtime] Gore chunk");
        world.Registry.get<RenderableComponent>(c.Entity).CastShadows = RenderableComponent::ShadowCasting::Off;
        world.SetWorldPose(c.Entity, c.Position, c.Rotation);
        m_Chunks.push_back(c);
    }

    // The air: a heavier burst of the liquid and lumps of it flung out.
    if (m_Sprites) {
        FxSprites& fx = *m_Sprites;
        static const glm::vec3 kBlood(0.30f, 0.012f, 0.010f);
        const int blob = fx.Entry("blood_blob"), spurt = fx.Entry("blood_spurt"), drop = fx.Entry("blood_drop");
        for (int i = 0; i < 2 && spurt >= 0; ++i) {
            FxSprites::Emit e;
            e.Entry = spurt;
            e.Mode = FxSprites::Shade::Blood;
            e.Pos = point + d * 0.1f + glm::vec3(0.0f, 0.05f, 0.0f);
            e.Vel = d * 1.5f + glm::vec3(0.0f, 0.6f, 0.0f);
            e.Drag = 2.0f;
            e.Life = 0.5f;
            e.Size0 = 0.6f;
            e.Size1 = 0.9f;
            e.Rot = u(m_Rng) * 3.14f;
            e.Color = kBlood;
            e.Erosion1 = 0.6f;
            e.Softness = 0.2f;
            fx.Spawn(e);
        }
        for (int i = 0; i < 8 && blob >= 0; ++i) {
            FxSprites::Emit e;
            e.Entry = blob;
            e.Mode = FxSprites::Shade::Blood;
            e.Frame = -1;
            e.Animate = false;
            e.Pos = point;
            e.Vel = glm::normalize(d + glm::vec3(u(m_Rng), u(m_Rng) * 0.6f + 0.5f, u(m_Rng)) * 0.7f) * (2.5f + 2.0f * (u(m_Rng) * 0.5f + 0.5f));
            e.Gravity = 1.0f;
            e.Life = 0.6f + 0.3f * (u(m_Rng) * 0.5f + 0.5f);
            e.Size0 = 0.1f;
            e.Size1 = 0.16f;
            e.Rot = u(m_Rng) * 3.14f;
            e.Spin = u(m_Rng) * 6.0f;
            e.Color = kBlood;
            e.Erosion0 = 0.1f;
            e.Erosion1 = 0.7f;
            e.Softness = 0.2f;
            fx.Spawn(e);
        }
        for (int i = 0; i < 20 && drop >= 0; ++i) {
            FxSprites::Emit e;
            e.Entry = drop;
            e.Mode = FxSprites::Shade::Blood;
            e.Pos = point;
            e.Vel = glm::normalize(d + glm::vec3(u(m_Rng), u(m_Rng) + 0.6f, u(m_Rng)) * 0.9f) * (3.0f + 4.0f * (u(m_Rng) * 0.5f + 0.5f));
            e.Gravity = 1.0f;
            e.Drag = 0.5f;
            e.Life = 0.5f + 0.3f * (u(m_Rng) * 0.5f + 0.5f);
            e.Size0 = e.Size1 = 0.04f;
            e.Stretch = 0.09f;
            e.Color = kBlood;
            e.Erosion1 = 0.5f;
            e.Softness = 0.25f;
            fx.Spawn(e);
        }
    }
    return true;
}

void HeadGore::Step(Chunk& c, float dt) {
    c.Age += dt;
    c.Velocity.y -= 9.81f * dt;
    glm::vec3 move = c.Velocity * dt;
    for (int pass = 0; pass < 2; ++pass) {
        const float len = glm::length(move);
        if (len < 1e-7f) break;
        const glm::vec3 dir = move / len;
        const float o[3] = {c.Position.x, c.Position.y, c.Position.z}, d[3] = {dir.x, dir.y, dir.z};
        RaycastHit hit;
        QueryFilter filter;
        filter.HitTriggers = 0;
        if (!PhysicsWorld::SphereCastSolid(o, d, c.Radius, len, filter, hit) || !hit.Hit) { c.Position += move; break; }
        glm::vec3 n(hit.Normal[0], hit.Normal[1], hit.Normal[2]);
        if (glm::dot(n, n) < 1e-6f || glm::dot(c.Velocity, n) >= 0.0f) { c.Position += move; break; }
        n = glm::normalize(n);
        const float travel = std::max(hit.Distance - 1e-4f, 0.0f);
        c.Position += dir * travel;
        // Soft and wet: it barely bounces, slides a little and sticks.
        const glm::vec3 vn = glm::dot(c.Velocity, n) * n, vt = c.Velocity - vn;
        c.Velocity = vt * 0.45f - vn * 0.15f;
        c.Spin *= 0.4f;
        if (n.y > 0.5f && glm::length(c.Velocity) < 0.4f) c.Sleeping = true;
        move = c.Velocity * dt * std::max(0.0f, 1.0f - travel / len);
    }
    const float w = glm::length(c.Spin);
    if (w > 1e-5f) c.Rotation = glm::normalize(glm::angleAxis(w * dt, c.Spin / w) * c.Rotation);
    if (c.Age > 6.0f) c.Sleeping = true; // stuck somewhere odd: done moving
}

void HeadGore::Update(World& world, float dt) {
    // Each stump on its neck, as the body moves (and falls, and lies).
    for (Victim& v : m_Victims) {
        Body body;
        if (!world.Registry.valid(v.Stump) || !m_Body || !m_Body(v.Entity, body)) continue;
        world.SetWorldPose(v.Stump, body.Head, StumpRotation(body.Neck, body.Head, body.Forward));
    }
    if (dt <= 0.0f) return;
    const bool recording = PhysicsWorld::GetQueryRecording();
    PhysicsWorld::SetQueryRecording(false);
    for (Chunk& c : m_Chunks) {
        if (c.Sleeping || !world.Registry.valid(c.Entity)) continue;
        for (float left = std::min(dt, 0.05f); left > 0.0f; left -= 0.01f) Step(c, std::min(left, 0.01f));
        world.SetWorldPose(c.Entity, c.Position, c.Rotation);
    }
    PhysicsWorld::SetQueryRecording(recording);
}

void HeadGore::Clear(World& world) {
    for (const Victim& v : m_Victims)
        if (world.Registry.valid(v.Stump)) world.DestroyEntityAndChildren(v.Stump);
    for (const Chunk& c : m_Chunks)
        if (world.Registry.valid(c.Entity)) world.DestroyEntityAndChildren(c.Entity);
    m_Victims.clear();
    m_Chunks.clear();
}
