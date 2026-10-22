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
    uint64_t SortKey = 0;       // (SortOrder, entity) packed (SortKeyFor); then SortSeed; ties broken by insertion
    uint32_t SortSeed = 0;      // the seed (a component's Seed, a runtime decal's Id), all 32 bits
};

// A decal a runtime system hands in each game frame (DecalSystem::SubmitRuntime): blood stains
// (BloodRenderer), bullet holes (BulletHoleList), anything not authored as a DecalComponent. Its maps
// are files like any decal's (often generated ones, GeneratedDecalMaps); the response comes from the
// caller, usually read from a .decal (DA_Blood, DA_BulletHole).
struct RuntimeDecal {
    glm::mat4 BoxToWorld{1.0f};       // unit box -> world: +X the image's right, +Y its top, +Z out of the surface
    // The maps (absolute paths; Albedo required), held by whoever made them - GeneratedDecalMaps keeps its
    // sets for the process - and alive at least until the next BeginRuntimeFrame. Sealed (Source::Key) so
    // the per-frame lookup hashes nothing.
    const DecalTextureArrays::Source* Maps = nullptr;
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
    bool Liquid = false;              // FLAG_LIQUID: a film (blood) that fills the receiver's relief
    float CoverGain = 1.0f;           // liquids: opacity = albedo alpha x this, after filtering (H1.x; no glow)
    bool AffectStatic = true, AffectDynamic = true;
    int SortOrder = 0;
    uint32_t Id = 0;                  // ties in the sort: a stable per-instance number (submission order otherwise)
};

// CPU cost counters for the decal path (the before / after of the optimisation pass): Prepare's time and the
// runtime systems' submit time (blood, bullet holes), summed per frame, and dropouts - a component decal drawn
// in one of the last two Prepares that is in view again but has no resident slot (it would flash off).
// "[DecalPerf]" lines go to stdout every 600 frames and when the DecalSystem goes.
namespace DecalPerf {
void AddPrepare(double ms);
void AddSubmit(double ms);
void AddDropout();
void Report();
// Times its scope into AddSubmit.
struct SubmitScope {
    SubmitScope();
    ~SubmitScope();
    long long Start;
};
} // namespace DecalPerf

// Gathers decals, culls them against the view frustum, sorts deterministically and uploads SSBOs 11/12.
// ClusterGrid::Cull then bins them into clusters (13/14) and the model fragment shader applies them
// before lighting (ModelDecals.glsl).
//
// The work is split by how often its inputs change: each .decal's resolved maps (and their slot key) once
// per load; each DecalComponent's box, its inverse, bounds and packed record once per change of the
// component, its transform, its asset, its slot or the grading; the frame's bookkeeping (texture uploads,
// the LRU, substrate pins, hot reload) once per frame (Time::FrameCount), whatever the number of views;
// and per view only the frustum and distance test, the fade, and the upload into a ring of SSBOs.
class DecalSystem {
public:
    DecalSystem();
    ~DecalSystem();

    // Load-time: owns the texture arrays and the .decal cache (hot-reloaded on file change in the
    // editor and development builds, LiveReload). Takes a project-relative or absolute path; cached by
    // the absolute path, case- and slash-insensitive.
    std::shared_ptr<DecalAsset> GetAsset(const std::string& projectRelPath);
    // Bumped whenever a cached .decal is re-read (Refresh, hot reload): a caller holding one of
    // GetAsset's pointers re-fetches it when this moves.
    std::uint32_t AssetGeneration() const { return m_AssetGen; }
    DecalTextureArrays& Textures() { return *m_Textures; }
    // A .decal whose maps were just rewritten (a graffiti bake): re-reads it and re-decodes only its
    // textures now, instead of waiting for the hot-reload poll.
    // Resident: its textures are in and it draws (the Designer keeps its live overlay until then).
    void Refresh(const std::string& path);
    bool Resident(const std::string& path);
    // The same for one entity's decal, with its reveal order in place of the aux map while it has one (SetReveal).
    bool Resident(const std::string& path, uint32_t entity);
    // The one the renderer uses (main.cpp owns it): the editor reaches it for Refresh / Resident.
    // Null before it exists or after it's gone.
    static DecalSystem* Current();

    // Per view. Walks DecalComponents (+ the runtime decals), frustum/distance culls, sorts and
    // uploads. Returns the visible count (also uDecalCount). The first call in a frame also does the
    // frame's bookkeeping.
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
    // (a reveal order), FLAG_REVEAL with this threshold / band, glow off. Kept until cleared. While it
    // has one, the piece's own maps are asked for too, so they're in when the reveal is cleared.
    struct RevealOverride {
        std::string Aux; // absolute
        float Threshold = 0.0f, Band = 0.04f;
    };
    void SetReveal(uint32_t entity, const RevealOverride& r) { SetReveal(entity, r.Aux, r.Threshold, r.Band); }
    // The same, updating the one in place (no string copy once its order is set).
    void SetReveal(uint32_t entity, const std::string& aux, float threshold, float band);
    void ClearReveal(uint32_t entity) { m_Reveals.erase(entity); }
    bool HasReveal(uint32_t entity) const { return m_Reveals.count(entity) != 0; }
    // The editor's Place Decal / Decal Brush ghost: one decal, built like a component's, that stays
    // until ClearGhost(). `boxWorld` is the entity's world transform (the box's Size is applied on top).
    void SetGhost(const DecalComponent& d, const glm::mat4& boxWorld);
    void ClearGhost() { m_GhostActive = false; }

    // Substrates (Substrate.h): the table index + 1 for these maps and values (0 = none, the table is
    // full, or its maps are still decoding), pinning its slot for this frame. An entry not asked for in
    // ~10 s makes way for a new one once the table is full. Callable while drawing: a record that changes
    // outside Prepare reads 0 until the next Prepare uploads it.
    uint32_t SubstrateIndex(const SubstrateDesc& desc);
    // The same for a material's substrate, through the DecalSystem alive (main.cpp owns one); 0 when
    // there's none or the material has no substrate. Model.cpp sets it as uMaterialSubstrate. Cached by
    // the desc's address (the material's own), re-resolved only when the desc changes.
    static int MaterialSubstrate(const SubstrateDesc& desc);
    int SubstrateCount() const { return (int)m_Subs.size(); }
    // Textures still on their way in for what this frame asked for - a substrate not ready yet, or decal
    // maps decoding / turned away for now by the decode limit (unless the view is over the texture
    // budget, where they never settle): a frame now would show them missing.
    bool Streaming() const;

    // Bind SSBOs 11 (decals), 12 (bounds) and the texture arrays on units 22-25 (+ SSBO 16, substrates).
    // The SSBOs are the last Prepare's copy of a small ring (a write never lands on one still in flight).
    void BindForCull() const;
    void BindForShading() const;
    int VisibleCount() const { return m_Visible; }
    const DecalGrading& Grading() const { return m_Grading; }
    // The last Prepare's nearest visible decal that projects onto dynamic receivers (FLAG_DYNAMIC_RECEIVERS),
    // metres from the camera to its box (a huge number when there's none): the view-model pass skips its
    // decal cull when nothing is within the arms' reach.
    float NearestDynamicReceiverDecal() const { return m_NearestDynamic; }

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
    // A runtime decal packed against its resident slot (WorldToBox is left to Prepare). `fade` (the
    // distance fade) scales every channel's opacity, as a component decal's does.
    static void PackRuntime(const RuntimeDecal& r, const DecalTextureArrays::Slot& slot, Decals::GpuDecal& out,
                            float fade = 1.0f);
    // (SortOrder, entity) -> the sort key, ascending = drawn first; the seed breaks ties after it
    // (DecalInstance::SortSeed). Runtime decals use entity 0xFFFFFFFF and their Id as the seed.
    static uint64_t SortKeyFor(int sortOrder, uint32_t entity);
    // Whether a packed record changes anything: a channel with opacity, a substrate, or a liquid film.
    static bool Contributes(const Decals::GpuDecal& g);

private:
    struct AssetEntry;
    struct StaticDecal;
    struct RuntimeExtra;
    struct VisItem;

    std::unique_ptr<DecalTextureArrays> m_Textures;
    std::unordered_map<std::string, std::unique_ptr<AssetEntry>> m_Assets; // by NormalPath(absolute)
    std::vector<AssetEntry*> m_AssetList;                                 // hot reload's round robin
    std::size_t m_ReloadCursor = 0;
    std::uint32_t m_AssetGen = 1;
    AssetEntry* Asset(const std::string& path);
    void Reload(AssetEntry& a);

    // DecalComponents, cached by entity (StaticDecal), and their draw order (rebuilt when the set or a
    // key changes).
    std::unordered_map<uint32_t, std::unique_ptr<StaticDecal>> m_Static;
    std::vector<StaticDecal*> m_StaticSorted;
    bool m_StaticOrderDirty = true;
    const World* m_StaticWorld = nullptr;
    std::uint64_t m_GradingGen = 1;
    void RefreshStatic(World& world);
    void PackStatic(StaticDecal& s, const DecalTextureArrays::Slot& slot);

    std::vector<DecalInstance> m_Runtime;
    std::vector<RuntimeDecal> m_RuntimeDecals;
    std::vector<RuntimeExtra> m_RuntimeExtra;  // per runtime decal: bounds, inverse, key (derived once)
    std::vector<uint32_t> m_RuntimeOrder;      // m_Runtime then m_RuntimeDecals, in draw order
    bool m_RuntimeDirty = true;
    void DeriveRuntime();

    std::unordered_map<uint32_t, RevealOverride> m_Reveals;
    std::unique_ptr<DecalComponent> m_Ghost;
    glm::mat4 m_GhostWorld{1.0f};
    bool m_GhostActive = false;

    static constexpr int kRing = 3;
    unsigned int m_DecalBuf[kRing] = {}, m_BoundsBuf[kRing] = {};
    int m_Ring = 0;
    int m_Visible = 0;
    float m_NearestDynamic = 1e30f;
    DecalGrading m_Grading;
    std::uint64_t m_LastFrame = ~0ull; // Time::FrameCount() of the frame's bookkeeping
    void BeginFrame(World& world);

    // Per view scratch.
    std::vector<VisItem> m_Vis;
    std::vector<Decals::GpuDecal> m_ViewGpu;
    std::vector<Decals::GpuDecalBounds> m_ViewBounds;
    std::vector<uint32_t> m_Cut;
    std::vector<Decals::GpuDecal> m_UploadDecals;
    std::vector<Decals::GpuDecalBounds> m_UploadBounds;

    struct SubstrateEntry {
        std::vector<std::uint64_t> Keys; // its hashes in m_SubIndex (the desc as asked for, and resolved)
        DecalTextureArrays::Source Src;
        Decals::GpuSubstrate Gpu{};
        std::uint64_t Frame = 0;  // the frame it was last requested in
        std::uint64_t Used = 0;   // ...and last asked for by a decal or a material
        std::uint32_t Gen = 0;    // bumped when the entry is handed to another desc
        bool Ready = false;
        bool Logged = false;
        bool Uploaded = false;    // Gpu is in SSBO 16
    };
    // A cached table index: valid while the entry's Gen matches.
    struct SubRef {
        std::uint32_t Idx = ~0u, Gen = 0;
    };
    std::vector<SubstrateEntry> m_Subs;
    std::unordered_map<std::uint64_t, uint32_t> m_SubIndex;
    struct MaterialSub {
        SubstrateDesc Desc;
        SubRef Ref;
    };
    std::unordered_map<const SubstrateDesc*, MaterialSub> m_MaterialSubs;
    unsigned int m_SubBuf = 0;
    bool m_SubDirty = false;
    std::uint64_t m_FrameNo = 1;
    bool m_InPrepare = false;
    std::uint64_t m_PrepareNo = 0;
    // The table entry for a desc (found, or made / recycled), or ~0u when the table is full.
    SubRef ResolveSubstrate(const SubstrateDesc& desc);
    // Pins a resolved entry for this frame: index + 1 when it's ready to draw, else 0.
    uint32_t TouchSubstrate(SubRef& ref, const SubstrateDesc& desc);
    void RefreshSubstrate(uint32_t idx);
    void FlushSubstrates();
};
