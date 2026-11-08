#include "LightGather.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"
#include "Components.h"
#include "LightBuffer.h"
#include "World.h"

namespace {
const entt::entity kTorchSlotKey = static_cast<entt::entity>(0xFFFFFFF0u); // the flashlight's own key (not an entity)
}

LightGather::StableSlots::StableSlots() { for (auto& o : Owner) o = entt::null; }

void LightGather::StableSlots::Assign(const std::vector<entt::entity>& want, int budget, std::uint64_t frame,
                                      std::unordered_map<entt::entity, int>& out) {
    out.clear();
    budget = std::clamp(budget, 0, SpotShadowMap::kMaxSpots);
    for (int i = budget; i < SpotShadowMap::kMaxSpots; ++i) Owner[i] = entt::null;
    bool taken[SpotShadowMap::kMaxSpots] = {};
    for (entt::entity e : want)
        for (int i = 0; i < budget; ++i)
            if (Owner[i] == e && !taken[i]) { taken[i] = true; Used[i] = frame; out[e] = i; break; }
    for (entt::entity e : want) {
        if (out.count(e)) continue;
        int pick = -1;
        for (int i = 0; i < budget; ++i)
            if (!taken[i] && (pick < 0 || Used[i] < Used[pick])) pick = i;
        if (pick < 0) break;
        taken[pick] = true; Owner[pick] = e; Used[pick] = frame; out[e] = pick;
    }
}

// #110 - shadow slots go to the most important shadowed spot / point lights, not the first ones in registry order
// (which let a far light keep its shadow while one right next to the camera lost it, and reshuffled slots whenever any
// entity was created). Score ~ brightness reaching the viewer: intensity * range^2 over the squared distance from the
// camera to the light's sphere of influence (inside it = full score). Last frame's holders get a 25% bonus, so
// near-ties don't flicker. Slots are then kept per light (StableSlots): handing them out in registry order every frame
// shifted every later light to another layer whenever one light gained or lost its shadow, and redrew all their caches.
void LightGather::AssignShadowSlots(const LightGatherInputs& in, FrameLights& out) {
    World& world = *in.world;
    std::set<entt::entity> frameShadowed;
    std::vector<entt::entity> chosenSpots, chosenPoints;
    // The player's flashlight takes the first spot shadow; the scene's spots share the rest.
    const int sceneSpotShadowBudget = std::max(out.SpotBudget - (in.torch ? 1 : 0), 0);
    const glm::vec3 viewPos = in.viewCam->Position;
    const float tanHalfFov = std::tan(glm::radians(in.viewCam->Fov) * 0.5f);
    std::vector<std::pair<float, entt::entity>> spots, points;
    for (auto e : world.Registry.view<TransformComponent, LightComponent>()) {
        const auto& lc = world.Registry.get<LightComponent>(e);
        // (Scored on the intensity now: holding a flickering light's shadow through its bright peaks - a TV
        // across the living room - cost the main pass ~1.3 ms in Floor3. Its slot, and so its cache, waits for it.)
        if (!lc.Shadow.Enabled || !world.ShadowsEnabled || lc.Kind == LightComponent::Type::Directional ||
            lc.Intensity <= 0.0f || glm::dot(lc.Color, lc.Color) <= 0.0f || world.Registry.all_of<InactiveTag>(e) || (in.suppressed && in.suppressed(e)))
            continue;
        const glm::vec3 lpos = glm::vec3(world.ComposeWorldTransform(e)[3]);
        const float range = std::max(lc.Range, 0.2f);
        const float lightDist = glm::length(lpos - viewPos);
        if (LightBuffer::DistanceFade(lc.Range, lightDist, tanHalfFov) <= 0.0f) continue;
        const float gap = std::max(lightDist - range, 0.0f);
        float score = lc.Intensity * range * range / (gap * gap + 1.0f);
        if (m_PrevShadowed.count(e)) score *= 1.25f;
        (lc.Kind == LightComponent::Type::Spot ? spots : points).emplace_back(score, e);
    }
    auto take = [&](std::vector<std::pair<float, entt::entity>>& v, size_t n, std::vector<entt::entity>& chosen) {
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < v.size() && i < n; ++i) { frameShadowed.insert(v[i].second); chosen.push_back(v[i].second); }
    };
    take(spots, (size_t)sceneSpotShadowBudget, chosenSpots);
    take(points, (size_t)out.PointBudget, chosenPoints);
    m_PrevShadowed = frameShadowed;
    if (in.torch && world.ShadowsEnabled && out.SpotBudget > 0) chosenSpots.insert(chosenSpots.begin(), kTorchSlotKey);
    m_SpotSlots.Assign(chosenSpots, out.SpotBudget, in.frame + 1, m_SpotSlotOf);
    m_PointSlots.Assign(chosenPoints, out.PointBudget, in.frame + 1, m_PointSlotOf);
    // Candidates that didn't get a slot, for the Inspector's over-budget warning.
    out.OverBudget.clear();
    for (const auto* v : {&spots, &points})
        for (const auto& [score, e] : *v)
            if (!frameShadowed.count(e)) out.OverBudget.insert(e);
}

void LightGather::Gather(const LightGatherInputs& in, FrameLights& out) {
    World& world = *in.world;
    LightBuffer& lightBuffer = *in.lights;
    SkyAtmosphere& skyAtmosphere = *in.atmosphere;
    out.SunDir = in.defaultSunDir;
    lightBuffer.Clear();

    // Per-scene budgets (Lighting > Shadows), clamped to the shader's array ceilings.
    out.SpotBudget = std::clamp(world.MaxSpotShadows, 0, SpotShadowMap::kMaxSpots);
    out.PointBudget = std::clamp(world.MaxPointShadows, 0, PointShadowMap::kMaxPoints);
    AssignShadowSlots(in, out);

    const Camera& lightViewCam = *in.viewCam;
    const float lightTanHalfFov = std::tan(glm::radians(lightViewCam.Fov) * 0.5f);

    if (const TorchLight* torch = in.torch; torch && lightBuffer.Count() + 2 <= LightBuffer::kMaxLights) {
        const glm::vec3 pos = torch->Position, aim = torch->Aim;
        int slot = -1;
        if (const auto it = m_SpotSlotOf.find(kTorchSlotKey); it != m_SpotSlotOf.end()) {
            slot = out.TorchShadowSlot = it->second;
            out.SpotLive[slot] = true;
            out.SpotCount = std::max(out.SpotCount, slot + 1);
            const glm::vec3 up = std::abs(aim.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const float fov = glm::radians(std::min(torch->SpillOuterDeg * 2.0f + 4.0f, 175.0f));
            out.SpotFar[slot] = torch->HotRange;
            out.SpotPos[slot] = pos;
            out.SpotHalfTan[slot] = std::tan(0.5f * fov);
            out.SpotBias[slot] = 1.0f;
            out.SpotNormalBias[slot] = 1.0f;
            out.SpotSoftness[slot] = 1.5f;
            out.SpotVP[slot] = MakePerspective(glm::degrees(fov), 1.0f, torch->ShadowNear, out.SpotFar[slot]) *
                               glm::lookAt(pos, pos + aim, up);
        }
        // Both cones share the one shadow map (it spans the spill), so the spill's shadows match the hotspot's.
        lightBuffer.AddSpot(pos, aim, torch->Color, torch->HotIntensity, torch->HotRange,
                            std::cos(glm::radians(torch->HotOuterDeg)), std::cos(glm::radians(torch->HotInnerDeg)), slot, 0u);
        lightBuffer.AddSpot(pos, aim, torch->Color, torch->SpillIntensity, torch->SpillRange,
                            std::cos(glm::radians(torch->SpillOuterDeg)), std::cos(glm::radians(torch->SpillInnerDeg)), slot, 0u);
    }
    for (auto e : world.Registry.view<TransformComponent, LightComponent>()) {
        if (lightBuffer.Count() >= LightBuffer::kMaxLights) { lightBuffer.MarkOverflowed(); break; }
        if (world.Registry.all_of<InactiveTag>(e)) continue;
        // Lights panel solo/mute is an editing aid only - Play renders every light (#140).
        if (in.suppressed && in.suppressed(e)) continue;
        const auto& lc = world.Registry.get<LightComponent>(e);
        // A dark point or spot light (a scripted flash between flashes) adds nothing, but every fragment in its
        // range would still shade it. (A directional keeps its slot: it decides the cascade pass.)
        if (lc.Kind != LightComponent::Type::Directional &&
            (lc.Intensity <= 0.0f || glm::dot(lc.Color, lc.Color) <= 0.0f)) continue;
        glm::mat4 m = world.ComposeWorldTransform(e);
        glm::vec3 pos = glm::vec3(m[3]);
        glm::vec3 aim = glm::normalize(glm::vec3(m * glm::vec4(0, 0, -1, 0)));
        // A point/spot light too small on screen to show fades out, then drops out (DistanceFade).
        float localIntensity = lc.Intensity;
        if (lc.Kind != LightComponent::Type::Directional) {
            const float fade = LightBuffer::DistanceFade(lc.Range, glm::length(pos - lightViewCam.Position), lightTanHalfFov);
            if (fade <= 0.0f) continue;
            localIntensity *= fade;
        }
        if (lc.Kind == LightComponent::Type::Directional) {
            glm::vec3 dirColor = lc.Color;
            float dirIntensity = lc.Intensity;
            // Physical sky: the first lit directional takes the resolved direction and the atmosphere-filtered colour.
            if (in.physicalSky && !out.SkyResolved && lc.Intensity > 0.0f) {
                out.Sky = skyAtmosphere.ResolveLighting(world.Sky, aim, lc.Color * lc.Intensity,
                                                        lc.AngularSizeDegrees, in.skyCameraPos);
                out.SkyResolved = true;
                aim = -out.Sky.LightDir;
                dirColor = out.Sky.LightRadiance;
                dirIntensity = 1.0f;
            }
            // Only the first lit directional drives the cascade pass - it alone samples it.
            lightBuffer.AddDirectional(aim, dirColor, dirIntensity, !out.HaveDirectional && lc.Intensity > 0.0f,
                                       ~(std::uint32_t)lc.CullingMask, lc.VolumetricIntensity); // #203
            // A zero-intensity sun contributes no light, so it must not drive the cascaded shadow pass either - it's
            // the way a scene opts out of having a directional at all while still suppressing SceneSerializer's
            // synthesised fallback sun (which keys off a Directional existing, not its intensity).
            if (lc.Intensity > 0.0f) {
                if (!out.HaveDirectional) {
                    out.SunDir = aim;
                    out.SunAngularDeg = lc.AngularSizeDegrees;
                    out.SunCastShadows = lc.Shadow.Enabled;
                    out.SunShadowBias = lc.Shadow.Bias;
                    out.SunShadowNormalBias = lc.Shadow.NormalBias;
                    out.SunShadowSoftness = lc.Shadow.Softness;
                }
                out.HaveDirectional = true;
            }
        } else if (lc.Kind == LightComponent::Type::Spot) {
            float cosOuter = cosf(glm::radians(lc.SpotAngleDegrees));
            float cosInner = cosf(glm::radians(lc.SpotAngleDegrees * 0.9f));
            int slot = -1;
            if (const auto it = m_SpotSlotOf.find(e); it != m_SpotSlotOf.end()) {
                slot = it->second;
                out.SpotLive[slot] = true;
                out.SpotCount = std::max(out.SpotCount, slot + 1);
                glm::vec3 up = std::abs(aim.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
                float fov = glm::radians(std::min(lc.SpotAngleDegrees * 2.0f + 4.0f, 175.0f));
                float nearP = std::max(lc.Shadow.NearPlane, 1e-3f);
                out.SpotFar[slot] = std::max(lc.Range, 0.2f);
                out.SpotPos[slot] = pos;
                out.SpotHalfTan[slot] = std::tan(0.5f * fov);
                out.SpotBias[slot] = lc.Shadow.Bias;
                out.SpotNormalBias[slot] = lc.Shadow.NormalBias;
                out.SpotSoftness[slot] = lc.Shadow.Softness;
                // #202 - MakePerspective takes degrees; `fov` above is already radians.
                out.SpotVP[slot] = MakePerspective(glm::degrees(fov), 1.0f, nearP, out.SpotFar[slot]) *
                                   glm::lookAt(pos, pos + aim, up);
            }
            lightBuffer.AddSpot(pos, aim, lc.Color, localIntensity, lc.Range, cosOuter, cosInner, slot,
                                ~(std::uint32_t)lc.CullingMask, lc.VolumetricIntensity); // #203
        } else {
            int slot = -1;
            if (const auto it = m_PointSlotOf.find(e); it != m_PointSlotOf.end()) {
                slot = it->second;
                out.PointLive[slot] = true;
                out.PointCount = std::max(out.PointCount, slot + 1);
                out.PointPos[slot] = pos;
                out.PointFar[slot] = std::max(lc.Range, 0.2f);
                out.PointNear[slot] = std::max(lc.Shadow.NearPlane, 1e-3f);
                out.PointBias[slot] = lc.Shadow.Bias;
                out.PointNormalBias[slot] = lc.Shadow.NormalBias;
            }
            lightBuffer.AddPoint(pos, lc.Color, localIntensity, lc.Range, slot, ~(std::uint32_t)lc.CullingMask,
                                 lc.VolumetricIntensity); // #203
        }
    }
    // Prefab Mode has a render-only sun when the prefab owns no directional light. There is no helper entity to
    // select, parent or accidentally save into the asset. An authored directional (even disabled) takes precedence.
    if (in.prefabMode) {
        bool prefabOwnsSun = false;
        for (auto [e, light] : world.Registry.view<const LightComponent>().each())
            if (light.Kind == LightComponent::Type::Directional) { prefabOwnsSun = true; break; }
        if (!prefabOwnsSun && lightBuffer.Count() < LightBuffer::kMaxLights) {
            const glm::vec3 previewSunColor(1.0f, 0.96f, 0.9f);
            glm::vec3 radiance = previewSunColor * 5.0f;
            out.SunDir = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
            if (in.physicalSky) {
                out.Sky = skyAtmosphere.ResolveLighting(world.Sky, out.SunDir, radiance, out.SunAngularDeg, in.skyCameraPos);
                out.SkyResolved = true;
                out.SunDir = -out.Sky.LightDir;
                radiance = out.Sky.LightRadiance;
            }
            lightBuffer.AddDirectional(out.SunDir, radiance, 1.0f, true);
            out.HaveDirectional = true;
            out.SunCastShadows = true;
        }
    }
    // Ordinary scenes without lights remain intentionally unlit.
    lightBuffer.SortLocalNearestFirst(lightViewCam.Position); // the per-cluster cap keeps the near ones
    lightBuffer.Upload();
    out.LightCount = lightBuffer.Count();
}
