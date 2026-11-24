#pragma once
#include <cstdint>
#include <functional>
#include <utility>
#include <set>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "ShadowLimits.h"
#include "ShadowSlotScoring.h"
#include "SkyAtmosphere.h" // SkyLighting (FrameLights holds one)

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
    bool SpotLive[ShadowLimits::kMaxSpots] = {};
    glm::mat4 SpotVP[ShadowLimits::kMaxSpots] = {};
    glm::vec3 SpotPos[ShadowLimits::kMaxSpots] = {};
    float SpotFar[ShadowLimits::kMaxSpots] = {};
    float SpotHalfTan[ShadowLimits::kMaxSpots] = {}; // tan(half-FOV) of the map: sizes the shader's texel estimate (#134)
    // Per-light shadow multipliers (#140 phase 2): 1.0 unless the light's ShadowSettings override them.
    float SpotBias[ShadowLimits::kMaxSpots] = {};
    float SpotNormalBias[ShadowLimits::kMaxSpots] = {};
    float SpotSoftness[ShadowLimits::kMaxSpots] = {};
    // How much of each slot's shadow shows, 0..1 (GI Phase 1): a shadow fades in when its light takes a slot and out
    // before it gives the slot up (ShadowSlotScoring::FadingSlots), instead of popping.
    float SpotWeight[ShadowLimits::kMaxSpots] = {};

    int PointCount = 0;
    bool PointLive[ShadowLimits::kMaxPoints] = {};
    glm::vec3 PointPos[ShadowLimits::kMaxPoints] = {};
    float PointFar[ShadowLimits::kMaxPoints] = {};
    float PointNear[ShadowLimits::kMaxPoints] = {};
    float PointBias[ShadowLimits::kMaxPoints] = {};
    float PointNormalBias[ShadowLimits::kMaxPoints] = {};
    float PointWeight[ShadowLimits::kMaxPoints] = {};

    int ShadowRoom = -1; // the room (GI Volume group) the slots were scored for, -1 outside them all (traces)
    int SlotChanges = 0; // slots that changed hands this frame (traces)

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
    float dt = 1.0f / 60.0f;               // seconds since the last Gather: the shadow fades run on it
    bool snapShadowFades = false;          // no fades this frame: --bake-gi, a scene load, a camera cut
    const TorchLight* torch = nullptr;     // the lit flashlight, or null
    std::function<bool(entt::entity)> suppressed; // the Lights panel's solo/mute (editor only); empty in Play
};

// Builds the light SSBO once per frame (the Scene and Game views share it, #122) and hands out the shadow slots.
class LightGather {
public:
    void Gather(const LightGatherInputs& in, FrameLights& out);
    // The last Gather's shadow-slot holders (light -> slot; the flashlight's key is not an entity in the registry).
    // For traces (--bake-gi TARTARUS_GI_BAKE_TRACE).
    const std::unordered_map<entt::entity, int>& SpotSlotHolders() const { return m_SpotSlotOf; }
    const std::unordered_map<entt::entity, int>& PointSlotHolders() const { return m_PointSlotOf; }

    // The rooms the last Gather scored against (the scene's GI Volumes), for traces.
    const ShadowSlotScoring::Rooms& ShadowRooms() const { return m_Rooms; }
    // The next Gather is a camera cut: its shadows snap instead of fading (--gi-tour asks on every waypoint jump, since
    // it knows its cuts; short ones, a door's 0.6 m, are below what LightGather can tell from a step).
    void RequestSnap() { m_SnapRequested = true; }

private:
    void AssignShadowSlots(const LightGatherInputs& in, FrameLights& out);
    void GatherRooms(World& world, const glm::vec3& viewPos);

    std::vector<entt::entity> m_PrevShadowed;   // last frame's shadow holders (fading ones too), sorted
    // The shadow candidates' world transforms in registry order (what AssignShadowSlots composed), which Gather's pass
    // over the same view takes instead of composing them again.
    std::vector<std::pair<entt::entity, glm::mat4>> m_LightXforms;
    // AssignShadowSlots' per-frame scratch (kept for its storage).
    std::vector<std::uint32_t> m_ChosenSpots, m_ChosenPoints, m_SpotKeys, m_PointKeys;
    std::vector<std::pair<float, entt::entity>> m_SpotCandidates, m_PointCandidates;
    // Each light keeps its shadow slot - its layer, and with it the cached static depth - for as long as it holds a
    // shadow; a light taking a shadow gets the free slot unused longest, and fades in; one losing it fades out first.
    ShadowSlotScoring::FadingSlots<ShadowLimits::kMaxSpots> m_SpotSlots;
    ShadowSlotScoring::FadingSlots<ShadowLimits::kMaxPoints> m_PointSlots;
    std::unordered_map<entt::entity, int> m_SpotSlotOf, m_PointSlotOf;
    ShadowSlotScoring::Rooms m_Rooms;
    bool m_Gathered = false; // the first Gather snaps its fades
    // Snaps: a new scene (World::SceneLoads; the slots are reset, entity ids are reused) or a camera cut (another
    // view camera, or the view jumping kCutDistance in one Gather: a --gi-tour cut, a teleport).
    std::uint64_t m_SceneLoads = 0;
    const Camera* m_LastViewCam = nullptr;
    glm::vec3 m_LastViewPos{0.0f};
    bool m_SnapRequested = false;
    std::vector<std::uint32_t> m_SpotLit, m_PointLit; // this frame's lit shadowed lights (before the distance cull)
    static constexpr float kCutDistance = 1.0f; // metres in one Gather: below this a move is never a cut
    static constexpr float kCutSpeed = 12.0f;   // m/s: past this the view jumped (a teleport), not moved
};
