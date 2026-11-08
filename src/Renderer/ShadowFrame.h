#pragma once
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "AABB.h"
#include "CascadedShadowMap.h"
#include "InstanceBatch.h"
#include "PointShadowMap.h"
#include "SceneRenderer.h"
#include "SpotShadowMap.h"

class World;
class Shader;
class Model;
struct MaterialAsset;
class VisibleIndexBuffer;
struct FrameLights;
struct Frustum;

struct ShadowFrameInputs {
    World* world = nullptr;
    const FrameLights* lights = nullptr;
    Shader* sunShader = nullptr;   // ShadowDepth: the cascades (and the SSAO pre-pass)
    Shader* localShader = nullptr; // ShadowDepthLocal: linear distance-to-light depth for spot/point maps
    CascadedShadowMap* sunMap = nullptr;
    SpotShadowMap* spotMap = nullptr;
    PointShadowMap* pointMap = nullptr;
    std::uint64_t frame = 0;
    int windowWidth = 0, windowHeight = 0; // the viewport the local passes restore
};

// The frame's shadow maps: the sun's cascades (per rendered view, #109) and the spot / point maps (once per frame, #119),
// with the static-depth caches that keep a still light's map from being redrawn.
class ShadowFrame {
public:
    // Fits and renders the cascades to this view; a second view with the same matrices in the same frame reuses them.
    // False when the sun casts no shadows this frame. Restores the caller's framebuffer and viewport.
    bool RenderSun(const ShadowFrameInputs& in, const glm::mat4& fitView, const glm::mat4& fitProj);
    // Spot and point maps. Call once per frame after the characters are pre-skinned.
    void RenderLocal(const ShadowFrameInputs& in);

private:
    struct SunCaster {
        Model* model;
        const std::vector<std::shared_ptr<MaterialAsset>>* materials;
        const VisibleIndexBuffer* visible; // an outfit piece's uncovered triangles (OutfitHideTag)
        glm::mat4 xform;
        AABB bounds;
        bool bounded, twoSided;
    };
    struct LocalCaster {
        Model* model;
        const std::vector<std::shared_ptr<MaterialAsset>>* materials;
        const VisibleIndexBuffer* visible; // an outfit piece's uncovered triangles (OutfitHideTag)
        glm::mat4 xform;
        AABB bounds;
        std::uint32_t id;
        bool bounded, twoSided, animated;
        bool moving;               // moved within the last kMovingFrames (a rolling ball, a door)
        bool settling;             // stopped moving / animating within the last kSettleFrames (see DynamicFor)
        std::uint64_t settleStart; // the frame it stopped counting as moving / animated
        bool player;               // the player's own body and gun (world copies): no shadow in their own flashlight
        std::uint64_t hash;        // id, model, materials, transform and twoSided folded once (see HashCaster)
    };
    // Per caster, its transform and when it last changed; drives LocalCaster::moving. A skinned caster counts as
    // animated only while its pose changes (PoseVersion): a corpse at rest is a static caster like any prop.
    struct CasterMotion { glm::mat4 xform; std::uint64_t moved; std::uint64_t pose = 0, posed = 0, seen = 0; };

    void GatherLocalCasters(const ShadowFrameInputs& in);
    void RenderSpots(const ShadowFrameInputs& in);
    void RenderPoints(const ShadowFrameInputs& in);
    bool DynamicFor(const LocalCaster& c, int turnSlot, std::uint64_t frame) const;
    // The collect + signature step RenderSpots and RenderPoints share: fills m_Visible / m_VisibleDynamic with the local
    // casters inside `frustum` (a spot) or `range` (a point; the other is null), skipping the player's own copies when
    // `skipPlayer`, and folds each static one's hash into `sig`. Returns whether any visible caster is dynamic.
    bool CollectLocalCasters(const Frustum* frustum, const AABB* range, bool skipPlayer, int turnSlot, std::uint64_t frame,
                             std::uint64_t& sig);

    // Sun
    struct SunFit { std::uint64_t frame = ~0ull; glm::mat4 view{0.0f}, proj{0.0f}; };
    SunFit m_LastSunFit;
    std::vector<SunCaster> m_SunCasters;
    std::vector<VisibleRenderable> m_Casting;
    std::vector<DepthCaster> m_Draws;

    // Spot / point
    std::vector<LocalCaster> m_LocalCasters;
    std::unordered_map<std::uint32_t, CasterMotion> m_CasterMotion;
    std::vector<const LocalCaster*> m_Visible;
    std::vector<std::uint8_t> m_VisibleDynamic; // per m_Visible entry: DynamicFor for the light being rendered
    std::uint64_t m_SpotShadowSig[SpotShadowMap::kMaxSpots] = {}; // the static casters'
    std::uint64_t m_SpotLiveSig[SpotShadowMap::kMaxSpots] = {};   // the static signature a moving light's live layer was drawn with
    bool m_SpotHadAnimated[SpotShadowMap::kMaxSpots] = {};
    glm::ivec4 m_SpotDynamicRect[SpotShadowMap::kMaxSpots] = {};  // the texels its animated casters covered when last drawn
    glm::mat4 m_SpotLastVP[SpotShadowMap::kMaxSpots] = {};
    std::uint64_t m_PointShadowSig[PointShadowMap::kMaxPoints] = {}; // the static casters'
    bool m_PointFaceHadDynamic[PointShadowMap::kMaxPoints][6] = {};  // moving casters were drawn into the live face last frame
};
