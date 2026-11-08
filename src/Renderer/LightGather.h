#pragma once
#include <cstdint>
#include <functional>
#include <utility>
#include <set>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "PointShadowMap.h"
#include "SkyAtmosphere.h"
#include "SpotShadowMap.h"

class World;
class Camera;
class LightBuffer;

// The player's flashlight, as the frame's lights see it: two cones sharing one shadow map (see Game/Flashlight.h).
struct TorchLight {
    glm::vec3 Position{0.0f}, Aim{0.0f, 0.0f, -1.0f}, Color{1.0f};
    float HotIntensity = 0.0f, HotRange = 0.0f, HotOuterDeg = 0.0f, HotInnerDeg = 0.0f;
    float SpillIntensity = 0.0f, SpillRange = 0.0f, SpillOuterDeg = 0.0f, SpillInnerDeg = 0.0f;
    float ShadowNear = 0.25f;
};

// One frame's lights: the sun, the shadow slots handed to spot / point lights, and the light count in the SSBO.
// The slot arrays are what SceneInputs points at, so this lives for the whole frame.
struct FrameLights {
    glm::vec3 SunDir{0.0f, -1.0f, 0.0f};
    float SunAngularDeg = 0.53f; // Earth's sun; drives the penumbra width
    bool HaveDirectional = false;
    bool SunCastShadows = false; // the active directional's per-light Shadow.Enabled
    float SunShadowBias = 1.0f, SunShadowNormalBias = 1.0f, SunShadowSoftness = 1.0f;
    SkyLighting Sky;
    bool SkyResolved = false;
    int LightCount = 0;

    int SpotBudget = 0, PointBudget = 0;
    int TorchShadowSlot = -1; // the flashlight's slot (its casters leave out the player)

    // One past the highest slot in use this frame (#119); *Live says which are.
    int SpotCount = 0;
    bool SpotLive[SpotShadowMap::kMaxSpots] = {};
    glm::mat4 SpotVP[SpotShadowMap::kMaxSpots] = {};
    glm::vec3 SpotPos[SpotShadowMap::kMaxSpots] = {};
    float SpotFar[SpotShadowMap::kMaxSpots] = {};
    float SpotHalfTan[SpotShadowMap::kMaxSpots] = {}; // tan(half-FOV) of the map: sizes the shader's texel estimate (#134)
    // Per-light shadow multipliers (#140 phase 2): 1.0 unless the light's ShadowSettings override them.
    float SpotBias[SpotShadowMap::kMaxSpots] = {};
    float SpotNormalBias[SpotShadowMap::kMaxSpots] = {};
    float SpotSoftness[SpotShadowMap::kMaxSpots] = {};

    int PointCount = 0;
    bool PointLive[PointShadowMap::kMaxPoints] = {};
    glm::vec3 PointPos[PointShadowMap::kMaxPoints] = {};
    float PointFar[PointShadowMap::kMaxPoints] = {};
    float PointNear[PointShadowMap::kMaxPoints] = {};
    float PointBias[PointShadowMap::kMaxPoints] = {};
    float PointNormalBias[PointShadowMap::kMaxPoints] = {};

    std::set<entt::entity> OverBudget; // shadow candidates that got no slot (the Inspector's warning)
};

struct LightGatherInputs {
    World* world = nullptr;
    LightBuffer* lights = nullptr;
    SkyAtmosphere* atmosphere = nullptr;
    const Camera* viewCam = nullptr;       // the view lights fade by (LightBuffer::DistanceFade) and the shadow slots score from
    glm::vec3 skyCameraPos{0.0f};
    glm::vec3 defaultSunDir{-0.4f, -1.0f, -0.3f};
    bool physicalSky = false;
    bool prefabMode = false;               // a render-only preview sun when the prefab owns no directional
    std::uint64_t frame = 0;
    const TorchLight* torch = nullptr;     // the lit flashlight, or null
    std::function<bool(entt::entity)> suppressed; // the Lights panel's solo/mute (editor only); empty in Play
};

// Builds the light SSBO once per frame (the Scene and Game views share it, #122) and hands out the shadow slots.
class LightGather {
public:
    void Gather(const LightGatherInputs& in, FrameLights& out);

private:
    // Each light keeps its shadow slot - its layer, and with it the cached static depth - for as long as it holds a
    // shadow; a light taking a shadow gets the free slot unused longest.
    struct StableSlots {
        entt::entity Owner[SpotShadowMap::kMaxSpots];
        std::uint64_t Used[SpotShadowMap::kMaxSpots] = {};
        StableSlots();
        void Assign(const std::vector<entt::entity>& want, int budget, std::uint64_t frame, std::unordered_map<entt::entity, int>& out);
    };
    void AssignShadowSlots(const LightGatherInputs& in, FrameLights& out);

    std::vector<entt::entity> m_PrevShadowed;   // last frame's shadow holders, sorted
    std::vector<entt::entity> m_ShadowedScratch; // this frame's, built here and swapped in (keeps its storage)
    // The shadow candidates' world transforms in registry order (what AssignShadowSlots composed), which Gather's pass
    // over the same view takes instead of composing them again.
    std::vector<std::pair<entt::entity, glm::mat4>> m_LightXforms;
    StableSlots m_SpotSlots, m_PointSlots;
    std::unordered_map<entt::entity, int> m_SpotSlotOf, m_PointSlotOf;
};
