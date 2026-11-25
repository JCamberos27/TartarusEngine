#pragma once
// SceneRenderer::RenderScene's stages (SceneViewPass.cpp). Internal to the renderer: SceneRenderer.cpp and
// SceneViewPass.cpp include it (and GpuCull.cpp, for the candidate flag bits), nothing else.
#include "SceneRenderer.h"
#include "Frustum.h"
#include "InstanceBatch.h"
#include "HiZCuller.h"
#include "Model.h"
#include "GpuCull.h"

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class ParticleRenderer;
struct MaterialAsset;
class VisibleIndexBuffer;

// VisibleRenderable::CandFlags (GatherVisible's candidate cache, SceneRenderer.cpp).
enum CandidateFlag : std::uint16_t {
    kCandInactive = 1 << 0, kCandPoseSource = 1 << 1, kCandOwnerViewOnly = 1 << 2, kCandHiddenFromOwner = 1 << 3,
    kCandViewModel = 1 << 4, kCandPlayerBody = 1 << 5, kCandHiddenInScene = 1 << 6, kCandPosed = 1 << 7,
};

// #177 - deliberately leaked: a function-local static would run its GL deletes at exit, after the
// context is gone. The OS reclaims the handful of GL objects with the process.
ParticleRenderer* SceneParticles();

// One draw of the scene pass: a model's opaque or transparent sub-meshes with everything the per-draw hook sets.
// #192: the frustum-culled visible set is sorted by material before drawing, so entities sharing a material land
// adjacent - which is what makes the BindMaterial dedup (GLStateCache::MaterialAlreadyBound) actually hit.
struct SceneDrawItem {
    glm::mat4 Xform;
    Model* Ref;
    const std::vector<std::shared_ptr<MaterialAsset>>* Slots;
    std::uint64_t MatKey;
    const Shader* Prog;   // #112 - slot 0's program; the opaque pass groups by it first
    int Meshes, Tris, Verts;
    int  QueueIndex; // the material's render queue index: the transparent sort's first key
    float ViewDepth; // view-space -Z (more positive = farther) for back-to-front sort
    glm::vec3 Centre; // world-space bounds centre — picks this object's reflection probes (#108)
    Model::MeshPass Pass; // #112 — which of the model's submeshes this item draws
    bool ReceiveShadows;  // #163
    int  LayerBit;        // #203 - 1 << Layer, tested against each light's excluded layers
    float NearHide = 0.0f; // PlayerBodyTag: metres around the eye not drawn (the player's own body)
    const int* HideBones = nullptr; // PlayerBodyTag::CameraHideBones in the camera's view, else none
    bool NoSsao = false;  // left out of the SSAO depth pre-pass (ViewPipeline), so it must not sample the map
    unsigned HideVerts = 0; // OutfitHideTag: the piece's covered vertices (SkinHideBuffer), 0 = none
    float NearHideWidth = 0.0f; // PlayerBodyTag: clothing's Near Hide to either side (0 = a sphere)
    unsigned CollarVerts = 0;   // PlayerBodyTag::CollarVerts in the camera's world pass, 0 = none
    // PlayerBodyTag bone masks: 1 = drop the vertices weighted mostly to BoneMask's bones (the camera's
    // world pass: the head's surroundings, and the sleeves while they're in the view-model pass),
    // 2 = draw only those (the view-model pass's copy of the sleeves). 0 = the whole piece.
    int BoneMaskMode = 0;
    std::array<std::uint32_t, 16> BoneMask{};
    // Clothing (an outfit piece, not a body part): drawn double-sided, so a collar's or cuff's inside
    // isn't culled away to show straight through it.
    bool DoubleSided = false;
    // OutfitHideTag::Visible: the piece's triangles not covered entirely, drawn instead of its own.
    const VisibleIndexBuffer* Visible = nullptr;
    float LayerPull = 0.0f; // OutfitLayerTag: drawn this many metres nearer the camera (depth only)
    std::uint64_t GIMask = ~0ull; // the GI Volumes its bounds touch (GIVolumes::MaskFor), uGIMask per draw
    // Skinned or a rigidbody: drawn after the static world. Blood stains take only to the static world
    // (Affect Dynamic off), so a character walking through a pool or a crate on a stain covers it.
    bool Dynamic = false;
    unsigned Entity = 0xFFFFFFFFu; // for the blood splats on it (BloodRenderer::SplatRange)
    // The entity it draws, for every item (Entity above is opaque-only): SortItems breaks exact ties by it, so equal
    // keys (coplanar surfaces) draw in the same order whatever order GatherVisible handed them out in.
    std::uint32_t SortId = 0xFFFFFFFFu;
    // Which decals take to it (Affect Static / Dynamic): StaticTag, or nothing skinned or simulated.
    bool StaticReceiver = true;
    // World bounds (HiZCuller box test); Bounded false = none, never culled.
    glm::vec3 WorldMin{0.0f}, WorldMax{0.0f};
    bool Bounded = false;
    glm::mat3 Normal{1.0f}; // transpose(inverse(mat3(Xform))), from the candidate cache (opaque items)
    float LodFade = 0.0f;   // World::LodFadeOf: a LOD cross-fade under way (dithered, drawn on its own)
    bool GIFlatL1 = false; // an OpenableComponent leaf: its GI lookup damps the directional term (GIVolumes::SetMask)
    std::uint32_t Slot = ~0u; // VisibleRenderable::Slot (opaque items): where GpuScene holds Xform and Normal
};

// An opaque multi-draw batch's sub-mesh. Prog/State/Mat/Object: how the batch binds, for drawing the pre-passed
// batches grouped by program and material.
struct SceneOpaqueTag {
    std::uint32_t Group;
    std::uint16_t Mesh;
    bool Prepassed;
    const Shader* Prog;
    std::uint64_t State, Mat, Object;
};

// A DrawItem is ~350 bytes: sorting the lists in place moved each one O(log n) times. The sort runs on a small key
// per item (its sort fields and its index), then every item moves once into its place.
struct SceneSortKey {
    std::uint64_t A, B; // A: Dynamic + program (opaque) or the queue index (transparent); B: material key
    float Depth;
    std::uint32_t Id;    // DrawItem::SortId: exact ties go by entity, then by Index
    std::uint32_t Index;
};

// G2-b: the batched static world the camera views draw from the GPU cull (SceneViewPassGpu.cpp). Built from the GPU
// scene's slots when they stand for other renderables or settings (SceneSlotTableGeneration), the frame's programs, or
// the probes' and GI volumes' layout change; every view of every frame culls and draws from it.
struct SceneGpuTable {
    bool Built = false;
    std::uint64_t Gen = 0, ProbeHash = 0, GIHash = 0;
    const void* Probes = nullptr;
    const void* GI = nullptr;
    std::uint32_t Variant = 0;
    bool AnyProbes = false;
    std::uint32_t SceneCount = 0;      // the GPU scene's slots at the build
    std::vector<SceneDrawItem> Leads;  // per group: what its batches bind (the per-draw hook's item)
    std::vector<std::uint32_t> Owned;  // 1 bit per scene slot: drawn from the table (GatherVisible skips them)
    std::vector<std::uint32_t> Moved;  // 1 bit per scene slot: a member that moved since the build (CPU path now)
    std::vector<std::uint32_t> MemberEntity; // per scene slot: the member's entity (integral), ~0u when not a member
    std::vector<std::uint32_t> SlotOfEntity; // entity index -> scene slot of a member, ~0u none
    struct DepthTag { std::uint32_t Group; std::uint16_t Mesh; };
    std::vector<SceneOpaqueTag> OpaqueTags;
    std::vector<IndirectBatch> OpaqueBatches;
    std::vector<DepthTag> DepthTags;
    std::vector<IndirectBatch> DepthBatches;   // First indexes Commands (after the opaque ones)
    std::vector<IndirectCommand> Commands;     // the opaque batches' commands, then the depth batches'
    std::uint32_t OpaqueCommands = 0;
    // Groups are split by ground cell (kGpuTableCell), so each has a box the view tests before anything is sent: only
    // groups that can be seen are culled on the GPU, and only batches holding one are drawn.
    std::vector<GpuCull::Group> StageGroups;   // per group: run base, count, command list at, every command
    std::vector<glm::uvec4> HizGroups;         // per group: run base, foliage sway bits, command list at, opaque commands
    std::vector<glm::vec3> GroupMin, GroupMax; // the members' world bounds, joined
    std::vector<std::uint8_t> GroupAlways;     // a member without bounds: always sent
    std::vector<float> GroupRadius;            // its largest member's bounding sphere (TooSmallOnScreen's radius)
    std::vector<std::uint32_t> RunSlotsCpu;    // the runs' scene slots (verify mode reads skipped groups' members)
    std::vector<std::uint32_t> OpaqueBatchGroups, DepthBatchGroups; // each batch's groups, back to back
    std::vector<std::uint32_t> OpaqueBatchAt, DepthBatchAt;         // where batch i's start (size + 1)
    std::uint32_t Members = 0;
    GpuCull::DrawBuffers Gl;
    // Per frame / view scratch.
    std::vector<std::uint32_t> Cleared;  // Owned bits cleared for this frame (fading, blood): set again next frame
    std::vector<std::uint32_t> Reject;   // the view's stage-A reject bits
    std::vector<std::uint32_t> MovedNow; // TakeMovedSceneSlots
    std::vector<std::uint8_t> GroupSeen;  // the view's
    std::vector<GpuCull::Group> ViewStage;
    std::vector<glm::uvec4> ViewHiz;
    std::vector<std::uint8_t> OpaqueBatchOn, DepthBatchOn;
};

// The scene pass's per-frame lists, reused across views and frames rather than reallocated each call. Owned by the
// SceneRenderer; one view renders at a time.
struct SceneViewScratch {
    std::vector<VisibleRenderable> Visible;
    std::vector<SceneDrawItem> DrawList, TransparentList;
    // Held out of the world pass while a view-model sub-pass will run.
    std::vector<SceneDrawItem> ViewModelList, ViewModelTransparentList;
    std::vector<Shader*> AppliedPrograms;
    std::vector<SceneSortKey> SortKeys;
    std::vector<SceneDrawItem> SortScratch;
    std::vector<DepthCaster> Prepass, ViewModelPrepass;
    std::vector<std::uint32_t> ProbeKeys;
    InstanceGroups OpaqueGroups;
    std::vector<int> GroupBase;
    std::vector<std::uint8_t> GroupIndirect; // 1: drawn by the multi-draw batches
    std::vector<SceneOpaqueTag> OpaqueTags;
    std::vector<IndirectItem> OpaqueItems;
    std::vector<IndirectCommand> OpaqueCommands;
    std::vector<IndirectBatch> OpaqueBatches;
    std::vector<HiZCuller::Group> HizGroups;
    std::vector<std::uint32_t> HizCmdList, HizSlotGroup;
    std::vector<std::uint32_t> PrepassedBatches;
    std::vector<std::uint32_t> SlotStaging; // the multi-draw copies' GPU scene slots, in instance order (BuildBatches)
    SceneGpuTable Gpu;                      // G2-b
};

// One RenderScene call: this view's state, and its stages in the order Run() calls them.
class SceneRenderer::ViewPass {
public:
    ViewPass(SceneRenderer& renderer, World& world, const RenderFrameContext& ctx, const SceneRenderInputs& in);
    void Run();
    const RenderStats& Stats() const { return m_Stats; }

private:
    using DrawItem = SceneDrawItem;

    void PrepareView();       // aerial perspective, decals, SSR prepare, cluster cull, volumetric fog, frame state
    void SetUpViewModel();    // the view-model sub-pass's projection and frustum
    void SetUpPrograms();     // program selection and the per-draw hook
    void BuildDrawLists();    // the visible set -> opaque / transparent / view-model lists
    void ReportTris() const;  // TARTARUS_TRIS_REPORT
    void DepthPrepass();      // static world depth, SSAO from it, grass depth, Hi-Z source, SSR, clouds
    void BuildBatches();      // instance groups, multi-draw batches, occlusion cull
    void DrawOpaque();        // batches + one-by-one draws, impostors, grass
    void DrawFurAndBlood();
    void DrawSky();
    void DrawEffects();       // particles, FX sprites, world text, the caller's overlay
    void DrawViewModel();

    // Program selection (audit #354): the program a material draws through, without touching GL state (also the
    // opaque sort key), and the selector that brings each distinct program up to the pass's frame state once.
    Shader* ProgramFor(const MaterialAsset* ma, std::uint32_t& key) const;
    Shader* SelectProgram(const MaterialAsset* ma);
    std::uint32_t FrameVariantBits() const;
    void PerDraw(Shader& prog);
    bool Prepassed(const DrawItem& it) const;
    bool Batchable(const DrawItem& it) const;
    std::uint32_t ProbeKey(std::uint32_t i) const;
    void SetAlphaBlend(int on);
    void BindTransmission(const FrameState& fs);
    void DrawTransparentList(std::vector<DrawItem>& list);
    void BeginDepthDraw(Shader& depth, const glm::mat4& proj) const;
    void SortItems(std::vector<DrawItem>& list, bool transparent);
    void CountDraw(const DrawItem& it);

    SceneRenderer& m_R;
    World& m_World;
    const RenderFrameContext& m_Ctx;
    const SceneRenderInputs& m_In;
    SceneViewScratch& m_S;
    Shader& m_ModelShader;

    FrameState m_Fs{};
    // Which frame state the program selector pushes. The view-model sub-pass swaps this for its own.
    const FrameState* m_ActiveFs = nullptr;
    RenderStats m_Stats;

    bool m_CloudsAfterDepth = false;
    bool m_GrassOn = false, m_GrassCulled = false, m_GrassInPrepass = false;
    bool m_SsrWanted = false;
    Frustum m_CamFrustum{}, m_VmFrustum{};
    bool m_ViewModelPass = false;
    glm::mat4 m_VmProj{1.0f};
    bool m_AnyTransmission = false; // #112 — refraction capture only when something samples it

    // Program selection: passAlphaBlend is the opaque/transparent flag for whichever pass is running.
    int m_PassAlphaBlend = 0;
    bool m_NoLocalLights = false;
    Model::ProgramSelector m_Select;

    // The per-draw hook (#108 probes per object, #163 Receive Shadows per object, ...).
    std::function<void(Shader&)> m_PerDraw;
    const DrawItem* m_ProbeItem = nullptr;
    bool m_AnyProbes = false;
    glm::vec3 m_NearHideRight{1.0f, 0.0f, 0.0f};
    int m_InstanceBase = -1; // the opaque pass's current group in the instance buffer, -1 = drawn with uModel
    int m_InstanceMode = 1;  // uInstanced while m_InstanceBase >= 0: 2 = through HiZCuller's remap
    // An opaque multi-draw batch sets m_PerDrawKey (what the hook reads for it): the hook then skips its ~20 uploads
    // when the previous call was the same program with the same key. Any other call resets it.
    std::uint64_t m_PerDrawKey = 0, m_LastPerDrawKey = 0;
    bool m_PerDrawKeyed = false;
    const Shader* m_LastPerDrawProg = nullptr;

    bool m_DepthPrepass = false;
    bool m_ImpostorsOn = false;
    unsigned m_HizDepth = 0; // the view's depth after the pre-pass, single sample: HiZCuller's occluders
    bool m_Instancing = false;
    bool m_HizCulled = false;
    // The multi-draw batches read their transforms from the GPU scene (GpuScene.h): binding 27 holds its Xforms while
    // they draw and 28 each copy's slot (HiZCuller's remap, or SlotStaging as it is); the instance ring otherwise.
    bool m_SceneSlots = false;
    bool m_XformsOnScene = false; // binding 27 holds the GPU scene now, not this view's instance ring span
    void BindInstanceXforms(bool scene);
    bool m_VelocityOut = false;

    // G2-b, GPU-driven draws (SceneViewPassGpu.cpp): this view culls and draws the table's batches on the GPU, and its
    // gather skips them. Off: everything goes the CPU path as before.
    bool GpuDrawsWanted() const;
    void PrepareGpuFrame(VisibilityQuery& inView); // table up to date, this frame's CPU-path members, inView.SkipSlots
    void BuildGpuTable();
    void CullGpuView(const VisibilityQuery& framed); // stage A (after the gather)
    void DrawGpuDepth(Shader& depth);                // the table's batches into the depth pre-pass
    void CullGpuHiz();                               // Hi-Z over stage A's survivors (BuildBatches, after the CPU path's)
    void DrawGpuOpaque(Model::DrawPassState& passState, unsigned char& depthWrites); // GLboolean: the pass's depth writes
    bool m_GpuDraws = false;   // the table is in use this view
    bool m_GpuCulled = false;  // stage A ran: the table's commands hold this view's counts
    unsigned m_GpuRemap = 0;   // what the table's draws read at 28: stage A's survivors, or Hi-Z's
    unsigned m_GpuCmdBuffer = 0;
    std::size_t m_GpuCmdBase = 0; // the table's commands for this view: their indirect buffer and offset
};
