#pragma once
#include "DecalTypes.h"
#include "DecalTextureArrays.h"
#include "Substrate.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class World;
struct DecalAsset;
struct DecalComponent;

// Scene-wide decal grading (age and wear), applied on top of every decal's own weathering.
struct DecalGrading {
    float Age = 0.0f;  // 0..1: fade + bleach
    float Wear = 0.0f; // 0..1: peel + buff + chipping
};

// One decal ready for the GPU, before culling/sorting. Built from a DecalComponent + its asset,
// or pushed directly by runtime systems (bullet holes, blood, spray-can) in Phase 4.
struct DecalInstance {
    glm::mat4 BoxToWorld{1.0f}; // unit box [-0.5,0.5]^3 -> world, +Z out of the surface
    Decals::GpuDecal Gpu{};     // filled except WorldToBox (derived from BoxToWorld)
    uint64_t SortKey = 0;       // (SortOrder, entity, seed) packed; ties broken by insertion
};

// A decal a runtime system hands in each game frame (DecalSystem::SubmitRuntime): blood stains
// (BloodRenderer), bullet holes (BulletHoleList), anything not authored as a DecalComponent. Its maps
// are files like any decal's (often generated ones, GeneratedDecalMaps); the response comes from the
// caller, usually read from a .decal (DA_Blood, DA_BulletHole).
struct RuntimeDecal {
    glm::mat4 BoxToWorld{1.0f};       // unit box -> world: +X the image's right, +Y its top, +Z out of the surface
    DecalTextureArrays::Source Maps;  // absolute paths; Albedo required
    int Cols = 1, Rows = 1, Frame = 0; // atlas grid in the maps and the cell drawn
    glm::vec3 Tint{1.0f};
    float AlbedoOp = 1.0f, NormalOp = 0.0f, RoughnessOp = 0.0f, MetalOp = 0.0f, AOOp = 0.0f;
    float Roughness = 0.5f, Metal = 0.0f;
    float AngleFadeStart = 0.25f, AngleFadeEnd = 0.05f; // cosines
    float EdgeFade = 0.05f;
    float Fade = 0.0f;                // weathering fade (overall opacity loss)
    float Wear = 0.0f;                // chips (dried blood flaking)
    // Reveal (FLAG_REVEAL, needs Maps.Aux): shows where aux >= RevealThreshold, fully from + RevealBand.
    bool Reveal = false;
    float RevealThreshold = 0.0f, RevealBand = 0.05f;
    bool AffectStatic = true, AffectDynamic = true;
    int SortOrder = 0;
    uint32_t Id = 0;                  // ties in the sort: a stable per-instance number (submission order otherwise)
};

// Gathers decals each frame, culls them against the view frustum, sorts deterministically and
// uploads SSBOs 11/12. ClusterGrid::Cull then bins them into clusters (13/14) and the model
// fragment shader applies them before lighting (ModelDecals.glsl).
class DecalSystem {
public:
    DecalSystem();
    ~DecalSystem();

    // Load-time: owns the texture arrays and the .decal cache (hot-reloaded on file change). Takes a
    // project-relative or absolute path; cached by the absolute path, case- and slash-insensitive.
    std::shared_ptr<DecalAsset> GetAsset(const std::string& projectRelPath);
    DecalTextureArrays& Textures() { return *m_Textures; }
    // A .decal whose maps were just rewritten (a graffiti bake): re-reads it and re-decodes only its
    // textures now, instead of the hot-reload poll's clear of every decal a second later.
    // Resident: its textures are in and it draws (the Designer keeps its live overlay until then).
    void Refresh(const std::string& path);
    bool Resident(const std::string& path);
    // The same for one entity's decal, with its reveal order in place of the aux map while it has one (SetReveal).
    bool Resident(const std::string& path, uint32_t entity);
    // The one the renderer uses (main.cpp owns it): the editor reaches it for Refresh / Resident.
    // Null before it exists or after it's gone.
    static DecalSystem* Current();

    // Per frame. Walks DecalComponents (+ any PushRuntime() decals), frustum/distance culls,
    // sorts and uploads. Returns the visible count (also uDecalCount).
    // includeGhost: also gather the editor's placement ghost (SetGhost), for the editor's own views.
    int Prepare(World& world, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                float maxDistance, const DecalGrading& grading, bool includeGhost = false);
    // Runtime decals, by game frame: BeginRuntimeFrame drops the last frame's (BloodRenderer::BeginFrame
    // calls it, so leaving Play clears them too), then each system submits what it has. Every view's
    // Prepare draws the same list until the next BeginRuntimeFrame.
    void BeginRuntimeFrame();
    void SubmitRuntime(const RuntimeDecal& d);
    void PushRuntime(const DecalInstance& d); // a ready-packed one, same lifetime
    int RuntimeCount() const { return (int)(m_RuntimeDecals.size() + m_Runtime.size()); }
    // Per-entity reveal on a DecalComponent (a spray-can piece going on): the aux map replaced by `Aux`
    // (a reveal order), FLAG_REVEAL with this threshold / band, glow off. Kept until cleared.
    struct RevealOverride {
        std::string Aux; // absolute
        float Threshold = 0.0f, Band = 0.04f;
    };
    void SetReveal(uint32_t entity, const RevealOverride& r) { m_Reveals[entity] = r; }
    void ClearReveal(uint32_t entity) { m_Reveals.erase(entity); }
    bool HasReveal(uint32_t entity) const { return m_Reveals.count(entity) != 0; }
    // The editor's Place Decal / Decal Brush ghost: one decal, built like a component's, that stays
    // until ClearGhost(). `boxWorld` is the entity's world transform (the box's Size is applied on top).
    void SetGhost(const DecalComponent& d, const glm::mat4& boxWorld);
    void ClearGhost() { m_GhostActive = false; }

    // Substrates (Substrate.h): the table index + 1 for these maps and values (0 = none, the table is
    // full, or its maps are still decoding), pinning its slot for this frame. Entries are kept until the
    // textures are cleared, so an index is stable from frame to frame. Callable while drawing: a change
    // is uploaded to SSBO 16 at once.
    uint32_t SubstrateIndex(const SubstrateDesc& desc);
    // The same for a material's substrate, through the DecalSystem alive (main.cpp owns one); 0 when
    // there's none or the material has no substrate. Model.cpp sets it as uMaterialSubstrate.
    static int MaterialSubstrate(const SubstrateDesc& desc);
    int SubstrateCount() const { return (int)m_Subs.size(); }
    // Textures still on their way in for what this frame asked for - a substrate not ready yet, or decal
    // maps decoding / turned away for now by the decode limit (unless the view is over the texture
    // budget, where they never settle): a frame now would show them missing.
    bool Streaming() const;

    // Bind SSBOs 11 (decals), 12 (bounds) and the texture arrays on units 22-25 (+ SSBO 16, substrates).
    void BindForCull() const;
    void BindForShading() const;
    int VisibleCount() const { return m_Visible; }
    const DecalGrading& Grading() const { return m_Grading; }

    // Pure helpers (unit-tested).
    static Decals::GpuDecalBounds BoundsFor(const glm::mat4& boxToWorld);
    static glm::mat4 VariationFor(int seed, float rotJitterDeg, float scaleJitter, bool flip);
    static void SortDeterministic(std::vector<DecalInstance>& v);
    // The cull compute's test (ClusterDecalCull.comp.glsl), CPU side: the box's AABB against `mn`/`mx`,
    // then the box's own three axes. Conservative (no edge-cross axes); both in the same space.
    static bool OverlapsAabb(const Decals::GpuDecalBounds& b, const glm::vec3& mn, const glm::vec3& mx);
    // What the compute writes for one cluster: the indices of `bounds` that overlap it, in order, and
    // past `cap` hits only the last `cap` (the top-most).
    static void BinCluster(const std::vector<Decals::GpuDecalBounds>& bounds, const glm::vec3& mn,
                           const glm::vec3& mx, int cap, std::vector<uint32_t>& out);
    // The atlas frame a seed picks when DecalComponent::Frame is -1.
    static int FrameFor(int seed, int frames);
    // A runtime decal packed against its resident slot (WorldToBox is left to Prepare).
    static void PackRuntime(const RuntimeDecal& r, const DecalTextureArrays::Slot& slot, Decals::GpuDecal& out);
    // (SortOrder, entity, seed) -> the sort key; runtime decals use entity 0xFFFFFFFF, seed = Id.
    static uint64_t SortKeyFor(int sortOrder, uint32_t entity, int seed);

private:
    std::unique_ptr<DecalTextureArrays> m_Textures;
    std::unordered_map<std::string, std::shared_ptr<DecalAsset>> m_Assets;
    std::vector<DecalInstance> m_Runtime, m_Frame;
    std::vector<RuntimeDecal> m_RuntimeDecals;
    std::unordered_map<uint32_t, RevealOverride> m_Reveals;
    std::unique_ptr<DecalComponent> m_Ghost;
    glm::mat4 m_GhostWorld{1.0f};
    bool m_GhostActive = false;
    unsigned int m_DecalBuf = 0, m_BoundsBuf = 0;
    int m_Visible = 0;
    DecalGrading m_Grading;
    // Hot reload: each cached .decal's write time, polled every so many Prepare()s.
    std::unordered_map<std::string, long long> m_AssetTimes;
    int m_ReloadTick = 0;
    std::vector<Decals::GpuDecal> m_UploadDecals;
    std::vector<Decals::GpuDecalBounds> m_UploadBounds;

    struct SubstrateEntry {
        std::string Key;
        DecalTextureArrays::Source Src;
        Decals::GpuSubstrate Gpu{};
        std::uint64_t Frame = 0; // the Prepare() it was last requested in
        std::uint64_t Used = 0;  // ...and last asked for by a decal or a material
        bool Ready = false;
        bool Logged = false;
    };
    std::vector<SubstrateEntry> m_Subs;
    std::unordered_map<std::string, uint32_t> m_SubIndex;
    unsigned int m_SubBuf = 0;
    bool m_SubDirty = false;
    std::uint64_t m_FrameNo = 1;
    bool m_InPrepare = false;
    void RefreshSubstrate(uint32_t idx);
    void FlushSubstrates();
    void ClearSubstrates();
};
