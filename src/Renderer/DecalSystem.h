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
class DecalTextureArrays;
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

    // Per frame. Walks DecalComponents (+ any PushRuntime() decals), frustum/distance culls,
    // sorts and uploads. Returns the visible count (also uDecalCount).
    // includeGhost: also gather the editor's placement ghost (SetGhost), for the editor's own views.
    int Prepare(World& world, const glm::mat4& viewProj, const glm::vec3& cameraPos,
                float maxDistance, const DecalGrading& grading, bool includeGhost = false);
    void PushRuntime(const DecalInstance& d); // cleared by every Prepare()
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

private:
    std::unique_ptr<DecalTextureArrays> m_Textures;
    std::unordered_map<std::string, std::shared_ptr<DecalAsset>> m_Assets;
    std::vector<DecalInstance> m_Runtime, m_Frame;
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
