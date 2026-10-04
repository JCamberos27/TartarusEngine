#include "ImpactFx.h"

#include "Audio/FoleyAudio.h"
#include "Components.h"
#include "FxSprites.h"
#include "Model.h"
#include "World.h"

#include <algorithm>
#include <cmath>

std::string ImpactFx::SurfaceFromName(const std::string& name) { return FoleyAudio::SurfaceFromName(kSurfaceTable, name, "concrete"); }

std::string ImpactFx::SurfaceOf(const World& world, std::uint32_t entity) const {
    const auto e = static_cast<entt::entity>(entity);
    if (entity == 0xFFFFFFFFu || !world.Registry.valid(e)) return "concrete";
    std::string name;
    if (const auto* c = world.Registry.try_get<ColliderComponent>(e)) name += c->Material + " ";
    if (const auto* t = world.Registry.try_get<TagComponent>(e)) name += t->Tag + " ";
    if (const auto* n = world.Registry.try_get<NameComponent>(e)) name += n->Name;
    return SurfaceFromName(name);
}

bool ImpactFx::IsStatic(const World& world, std::uint32_t entity) {
    const auto e = static_cast<entt::entity>(entity);
    if (entity == 0xFFFFFFFFu || !world.Registry.valid(e)) return true; // nothing there: world space
    if (world.Registry.all_of<RigidbodyComponent>(e)) return false;
    if (const auto* r = world.Registry.try_get<RenderableComponent>(e); r && r->ModelRef && r->ModelRef->HasBones()) return false;
    return true;
}

const char* ImpactFx::HoleEntry(const std::string& s) {
    if (s == "metal") return "hole_metal";
    if (s == "glass") return "hole_glass";
    if (s == "wood") return "hole_wood";
    if (s == "brick") return "hole_brick";
    if (s == "asphalt") return "hole_asphalt";
    if (s == "tile") return "hole_tile";
    if (s == "sand") return "hole_sand";
    if (s == "mud") return "hole_mud";
    if (s == "rock") return "hole_rock";
    return "hole_concrete";
}

float ImpactFx::HoleSize(const std::string& s, float radius) {
    // The procedural hole's radius is the round's (~4.5 mm rifle, more for buckshot clusters); the PRO decals hold the
    // crater and chipped rim around it, the cell ~1.4x the visible damage.
    const float k = s == "glass" ? 34.0f : s == "metal" ? 14.0f : s == "wood" ? 18.0f : (s == "mud" || s == "sand") ? 22.0f : 24.0f;
    return std::clamp(radius * k, 0.05f, 0.2f);
}

glm::vec3 ImpactFx::DustColor(const std::string& s) {
    if (s == "brick") return {0.42f, 0.17f, 0.11f};
    if (s == "wood") return {0.38f, 0.26f, 0.14f};
    if (s == "mud") return {0.2f, 0.15f, 0.09f};
    if (s == "sand") return {0.5f, 0.42f, 0.28f};
    if (s == "asphalt") return {0.18f, 0.18f, 0.18f};
    if (s == "metal") return {0.3f, 0.3f, 0.3f};
    if (s == "glass") return {0.6f, 0.62f, 0.65f};
    if (s == "rock") return {0.33f, 0.31f, 0.29f};
    return {0.45f, 0.44f, 0.42f}; // concrete, tile
}

void ImpactFx::Spawn(const std::string& surface, const glm::vec3& point, const glm::vec3& normal, const glm::vec3& dir) {
    if (!Enabled || !m_Sprites) return;
    FxSprites& fx = *m_Sprites;
    const glm::vec3 n = glm::length(normal) > 1e-4f ? glm::normalize(normal) : glm::vec3(0, 1, 0);
    const glm::vec3 d = glm::length(dir) > 1e-4f ? glm::normalize(dir) : -n;
    // Off the surface: the reflection of the round, mostly back out along the normal.
    const glm::vec3 refl = glm::normalize(glm::reflect(d, n) * 0.5f + n);
    auto cone = [&](const glm::vec3& axis, float spread) {
        glm::vec3 t = glm::cross(axis, std::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
        t = glm::normalize(t);
        const glm::vec3 b = glm::cross(axis, t);
        const float a = U() * 6.2832f, r = spread * std::sqrt(U());
        return glm::normalize(axis + (t * std::cos(a) + b * std::sin(a)) * r);
    };
    const glm::vec3 dust = DustColor(surface);
    const glm::vec4 plane(n, -glm::dot(n, point) + 0.002f);
    const bool soft = surface == "mud" || surface == "sand";
    const bool hard = surface == "metal" || surface == "rock" || surface == "concrete" || surface == "brick" || surface == "asphalt" || surface == "tile";
    ++m_Spawned;

    // The puff (PRO "Smoke": 3 of it, 0.2-0.4 m, out at 3-7 m/s and stopped fast, 1.5-2.5 s, faint).
    if (const int smoke = fx.Entry("smoke_impact"); smoke >= 0 && surface != "glass") {
        for (int i = 0; i < (soft ? 4 : 3); ++i) {
            FxSprites::Emit e;
            e.Entry = smoke;
            e.Mode = FxSprites::Shade::Lit;
            e.Pos = point + n * 0.03f;
            e.Vel = cone(refl, 0.6f) * (2.0f + 3.0f * U());
            e.Drag = 6.0f;
            e.Gravity = -0.01f;
            e.Life = 1.4f + 1.0f * U();
            e.Size0 = 0.12f + 0.08f * U();
            e.Size1 = (soft ? 0.7f : 0.5f) + 0.2f * U();
            e.Rot = U() * 6.2832f;
            e.Spin = (U() - 0.5f) * 1.0f;
            e.BlendFrames = true;
            e.Color = dust;
            e.Alpha = soft ? 0.55f : surface == "metal" ? 0.25f : 0.45f;
            e.FadeIn = 0.03f;
            e.FadeOut = 0.7f;
            fx.Spawn(e);
        }
        // The streak: dust shot straight out of the hole (PRO "Smoke Stretched": 7 of it at 7-8 m/s).
        for (int i = 0; i < (surface == "metal" ? 2 : 5); ++i) {
            FxSprites::Emit e;
            e.Entry = smoke;
            e.Mode = FxSprites::Shade::Lit;
            e.Pos = point + n * 0.02f;
            e.Vel = cone(refl, 0.35f) * (6.0f + 2.0f * U());
            e.Drag = 7.0f;
            e.Life = 0.5f + 0.4f * U();
            e.Size0 = 0.05f;
            e.Size1 = 0.12f;
            e.Stretch = 0.05f;
            e.Color = dust;
            e.Alpha = 0.4f;
            e.FadeOut = 0.8f;
            fx.Spawn(e);
        }
    }
    // Chips of it, falling and bouncing on the surface they came off (PRO "Rocks": 3, 1-3 m/s, gravity).
    const char* debris = surface == "wood" ? "debris_wood" : surface == "glass" ? "debris_glass"
                       : (surface == "rock" || surface == "mud" || surface == "sand") ? "debris_rock" : surface == "metal" ? nullptr : "debris_concrete";
    if (const int chips = debris ? fx.Entry(debris) : -1; chips >= 0) {
        const int count = surface == "glass" ? 8 : surface == "wood" ? 4 : 3 + (int)(U() * 3.0f);
        for (int i = 0; i < count; ++i) {
            FxSprites::Emit e;
            e.Entry = chips;
            e.Mode = FxSprites::Shade::Lit;
            e.Frame = -1;
            e.Animate = false;
            e.Pos = point + n * 0.02f;
            e.Vel = cone(refl, 0.9f) * (1.5f + 2.5f * U());
            e.Gravity = 1.0f;
            e.Life = 1.5f + 1.5f * U();
            e.Size0 = e.Size1 = (surface == "glass" ? 0.012f : 0.015f) + 0.025f * U();
            e.Rot = U() * 6.2832f;
            e.Spin = (U() - 0.5f) * 25.0f;
            e.Color = surface == "brick" ? glm::vec3(1.4f, 0.75f, 0.6f) : surface == "mud" ? glm::vec3(0.6f, 0.45f, 0.3f) : glm::vec3(1.0f);
            e.Alpha = surface == "glass" ? 0.6f : 1.0f;
            e.FadeOut = 0.2f;
            // A floor under a wall hit: the chips land on it; on a floor, they bounce on it.
            if (n.y > 0.6f) e.Plane = plane;
            fx.Spawn(e);
        }
    }
    // Sparks off metal (and now and then off stone): hot, fast, short.
    if (surface == "metal" || (hard && U() < 0.35f)) {
        const int count = surface == "metal" ? 10 + (int)(U() * 8.0f) : 3;
        for (int i = 0; i < count; ++i) {
            FxSprites::Emit e;
            e.Entry = -1; // a soft dot, stretched along its flight
            e.Mode = FxSprites::Shade::Additive;
            e.Pos = point + n * 0.01f;
            e.Vel = cone(refl, 0.8f) * (4.0f + 7.0f * U());
            e.Gravity = 1.0f;
            e.Drag = 1.5f;
            e.Life = 0.12f + 0.25f * U();
            e.Size0 = 0.008f;
            e.Size1 = 0.004f;
            e.Stretch = 0.02f;
            e.Color = glm::vec3(1.0f, 0.55f, 0.2f);
            e.Intensity = 18.0f;
            e.FadeOut = 0.5f;
            fx.Spawn(e);
        }
        if (const int glow = fx.Entry("glow"); glow >= 0 && surface == "metal") { // the flash at the hit
            FxSprites::Emit e;
            e.Entry = glow;
            e.Mode = FxSprites::Shade::Additive;
            e.Pos = point + n * 0.02f;
            e.Life = 0.06f;
            e.Size0 = 0.18f;
            e.Size1 = 0.1f;
            e.Color = glm::vec3(1.0f, 0.7f, 0.4f);
            e.Intensity = 6.0f;
            e.FadeOut = 1.0f;
            fx.Spawn(e);
        }
    }
}
