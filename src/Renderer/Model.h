#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <glm/glm.hpp>
#include "ModelMesh.h"
#include "Animation.h"
#include "Material.h"
#include "MaterialAsset.h"
#include "RootMotion.h"

class Texture;
class Shader;
struct aiScene;
struct aiNode;
struct aiMesh;
struct aiMaterial;
struct aiTexture;

struct BoneInfo {
    int ID;
    glm::mat4 OffsetMatrix;
};

// #113 — bones live in an SSBO (#104), so the old 100-entry uniform-array cap is gone; 512 covers
// production character rigs. Only the rig's own bone count is uploaded per draw.
constexpr int MAX_BONES = 512;

// Mirrors the knobs a Unity-style Model Importer would expose. Stored per-asset-path in
// AssetLibrary and applied whenever a Model is constructed or re-imported — see Model::Reimport.
struct ModelImportSettings {
    enum class MaterialMode {
        ImportEmbedded,   // read materials/textures from the source file (today's only behavior)
        CreateSynthetic,  // ignore the file's materials entirely; assign one neutral PBR material
        None              // leave every mesh at Material{}'s bare defaults
    };

    // Fed into Assimp's AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY alongside aiProcess_GlobalScale —
    // multiplies whatever unit-scale correction the importer already derives from the file's
    // own metadata (see the comment on that in Model.cpp), rather than replacing it.
    float GlobalScale = 1.0f;
    bool ImportNormals = true;   // generate smooth normals + tangent space if the file lacks them
    bool ImportAnimations = true;
    bool ImportSkeleton = true;  // false imports every mesh as static (no bone weights)
    bool OptimizeGraph = true;   // aiProcess_JoinIdenticalVertices + OptimizeMeshes
    MaterialMode MaterialImportMode = MaterialMode::ImportEmbedded;
    // Clips cut to a range of the source (seconds): the take is longer than the motion you want, or holds
    // two motions. Only the range plays, and it is the clip's length everywhere (looping, blend trees,
    // root motion, the Animator's analysis). End <= 0 = to the end of the clip.
    struct ClipTrim {
        std::string Clip;
        float StartSeconds = 0.0f;
        float EndSeconds = 0.0f;
    };
    std::vector<ClipTrim> ClipTrims;
};

// An imported 3D asset (FBX/glTF/OBJ via Assimp): one or more meshes, optional skeleton,
// optional animation clips, and a PBR material per mesh as read from the source file.
// Static (non-rigged) models simply have no bones/animations.
class Model {
public:
    explicit Model(const std::string& path);
    Model(const std::string& path, const ModelImportSettings& settings);
    ~Model();

    // Re-runs the Assimp import from disk with new settings, replacing the shared imported data
    // in place — every instance made by CreateInstance() (placed scene objects) sees the update,
    // since they all share it (#96). MaterialOverride (an editor-set look that
    // deliberately replaces whatever the file specifies) is left untouched. Returns false (and
    // leaves the previous import in place) if the file can't be re-read.
    bool Reimport(const ModelImportSettings& settings);
    const ModelImportSettings& ImportSettings() const { return m_D->Settings; }

    // #96 — a new placed instance of this model: shares the imported meshes, GPU buffers,
    // textures, skeleton and clips (no Assimp import, no extra VRAM), with its own animation
    // state. Imported mesh materials are shared too; per-object looks go through MaterialAsset
    // slots on the RenderableComponent.
    std::shared_ptr<Model> CreateInstance() const;

    // Builds a procedural primitive (kind: "cube"/"sphere"/"cylinder"/"cone"/"plane") instead
    // of importing a file. `path` is stored as this Model's Path() so the editor's usual
    // per-path asset cache and scene-save/load round-trip both work unmodified — the caller
    // (AssetLibrary) is responsible for making that path unique per placed instance.
    static std::shared_ptr<Model> CreatePrimitive(const std::string& kind, const std::string& path);

    // #124 — the files a model file needs next to it to import completely: glTF buffers and
    // images, OBJ .mtl libraries, and every external texture the materials reference (resolved
    // the same way an import resolves them). Each entry is {absolute source file, path to copy it
    // to relative to the model's new folder} — the original relative layout when the file sits
    // under the model's directory, else just its filename (where the import also looks).
    static std::vector<std::pair<std::string, std::string>> SourceDependencies(const std::string& modelPath);

    // Backward-compat: no material slots → uses every submesh's imported Material.
    void Draw(Shader& shader) { Draw(shader, {}); }

    // Geometry only — no material binds, no texture units. For the shadow / depth pre-pass.
    // Still uploads bone matrices + uUseSkinning: the depth vertex shader skins too.
    // slots: per-submesh MaterialAsset overrides; empty/short → imported mesh material.
    void DrawDepthOnly(Shader& shader) { DrawDepthOnly(shader, {}); }

    // Clips: this model's own (0..OwnAnimationCount-1), then any attached from other files
    // (AttachClip, #175 — e.g. Mixamo animation-only FBXs played on the character).
    bool HasAnimations() const { return AnimationCount() > 0; }
    int OwnAnimationCount() const { return (int)m_D->Animations.size(); }
    int AnimationCount() const { return (int)m_D->Animations.size() + (int)m_ExternalClips.size(); }
    const std::string& AnimationName(int index) const;
    // A model file that is nothing but animation (no meshes) - an animation clip asset.
    bool IsAnimationOnly() const { return m_D->Meshes.empty() && !m_D->Animations.empty(); }

    // #175 — plays `source`'s clip `sourceIndex` on this model, matching animated nodes by name
    // (a shared skeleton, e.g. every Mixamo export). `ref` is the stable reference it's found by
    // again (FindClipByRef), `displayName` what pickers show. Returns the clip's index here, or -1
    // when the source animates none of this model's nodes. Attaching the same ref twice returns
    // the existing index. Per instance: other instances of this model don't see it.
    int AttachClip(const Model& source, int sourceIndex, const std::string& ref, const std::string& displayName);
    int FindClipByRef(const std::string& ref) const;

    // #113 / #175 — playback. `fadeSeconds` > 0 crossfades from whatever is playing (or the bind
    // pose) instead of snapping. Index -1 stops. Speed may be negative (plays backwards).
    void PlayAnimation(int index, float fadeSeconds = 0.0f,
                       AnimationWrapMode wrap = AnimationWrapMode::Loop, float speed = 1.0f);
    void StopAnimation() { PlayAnimation(-1); }
    void SetAnimationSpeed(float speed) { m_Anim.Speed = speed; }
    void SetAnimationWrapMode(AnimationWrapMode wrap) { m_Anim.Wrap = wrap; }
    int  CurrentAnimation() const { return m_Anim.Clip; }
    int  FindAnimation(const std::string& name) const; // -1 if no clip has that name
    float AnimationLength(int index) const;            // seconds
    float AnimationTime() const { return m_Anim.Time; } // seconds since the current clip started
    void UpdateAnimation(float dt);

    // Advances this model's animation at most once per engine frame. Scene entities each get
    // their own Model instance (AssetLibrary::InstantiateModel), so this is normally a 1:1 call
    // anyway — but a future shared-Model path could otherwise tick one player N*dt in a single
    // frame if N entities reference the same Model (#106). frameIndex is a monotonically
    // increasing per-frame counter owned by the caller; comparing against it needs no
    // allocation or hashing, unlike the per-frame std::unordered_set<Model*> this replaced.
    void TickAnimationOnce(uint64_t frameIndex, float dt) {
        if (m_LastTickedFrame == frameIndex) return;
        m_LastTickedFrame = frameIndex;
        UpdateAnimation(dt);
    }

    // Skins whenever the model has bones: the current clip's pose while one plays, otherwise the
    // bind pose (#98 — a rig with no clip playing used to render in raw mesh space).
    void UploadBoneMatrices(Shader& shader) const;
    bool IsPlayingAnimation() const { return m_Anim.Clip >= 0; }
    // Has the current clip run out? True when nothing is set to play, false while a wrapping clip
    // runs, and - unlike IsPlayingAnimation(), which a ClampForever clip never clears - true for a
    // ClampForever clip that has reached its last frame and is now only holding it. This is the
    // question a "did that one-shot finish?" gate actually wants: IsPlayingAnimation() answers
    // "is a clip still driving the pose", and a held pose still is.
    bool AnimationFinished() const;
    int  BoneCount() const { return m_D->BoneCounter; }
    // The current skinning matrix of bone `i` (bind pose when nothing plays). For tests / tools.
    glm::mat4 FinalBoneMatrix(int i) const {
        if (i < 0 || i >= m_D->BoneCounter) return glm::mat4(1.0f);
        const bool posed = m_ExternalPose || m_Anim.Clip >= 0 || m_FadeDuration > 0.0f;
        return posed && i < (int)m_FinalBoneMatrices.size() ? m_FinalBoneMatrices[i] : m_D->BindPoseBones[i];
    }

    // Model-root-space transform of a named node (a bone, for a rig) as it currently stands:
    // the animated pose while a clip plays, the bind pose otherwise. This is the NODE's world,
    // not a skinning matrix - it is what you multiply by the entity's world transform to get a
    // bone's world position, e.g. to hang a camera off a head bone. Returns false when the model
    // has no node of that name, in which case `out` is left untouched.
    bool NodeTransform(const std::string& name, glm::mat4& out) const;

    // --- Animator pose API ------------------------------------------------------------------
    // The Animator Controller (AnimatorController.cpp) blends poses itself - crossfade stacks,
    // blend trees, layers with bone masks - so it needs "sample a clip" and "apply a pose" as
    // separate steps rather than PlayAnimation's single built-in crossfade. A pose is one
    // LocalTRS per node, in the flattened parents-first order of NodeName()/NodeParent().
    int NodeCount() const { return (int)m_D->Nodes.size(); }
    int NodeIndex(const std::string& name) const; // -1 when absent
    const std::string& NodeName(int i) const { return m_D->Nodes[i].Name; }
    int NodeParent(int i) const { return m_D->Nodes[i].Parent; }
    // Every node at its authored bind-local transform.
    void BindLocalPose(std::vector<LocalTRS>& out) const;
    // Clip `clip` at `seconds` of playback under `wrap`; nodes the clip doesn't animate keep
    // their bind value. `driven`, when given, is set to 1 per node the clip has a channel for.
    // Returns false (and writes the bind pose) when the clip index is invalid.
    bool SampleLocalPose(int clip, float seconds, AnimationWrapMode wrap, std::vector<LocalTRS>& out,
                         std::vector<unsigned char>* driven = nullptr) const;
    // Makes `pose` this model's current pose: fills the node globals and skinning palette, and
    // from then on NodeTransform, FinalBoneMatrix and UploadBoneMatrices report it while
    // UpdateAnimation leaves it alone. The next PlayAnimation/StopAnimation hands control back
    // to the built-in playback. A pose of the wrong size is ignored.
    void ApplyLocalPose(const std::vector<LocalTRS>& pose);
    bool HasExternalPose() const { return m_ExternalPose; }
    // The last pose ApplyLocalPose was given (empty before the first): for code that edits a
    // finished pose - IK on top of the animator's - and applies it again.
    const std::vector<LocalTRS>& AppliedLocalPose() const { return m_AppliedPose; }
    // Seconds into the current clip as a 0..1 fraction of its length (0 when nothing plays).
    float NormalizedTime() const;

    // --- Root motion (RootMotion.h) -------------------------------------------------------------
    // The node whose travel is the character's: `name` when this model has it, else (empty name)
    // the first of "root" (any namespace), then the hips/pelvis. -1 when none is found.
    int FindRootMotionNode(const std::string& name = {}) const;
    // Model-space transform (what the entity's transform multiplies) of `node` in clip `clip` at
    // `seconds` under `wrap`. Ancestors the clip doesn't animate hold their bind pose.
    glm::mat4 SampleNodeModelSpace(int clip, float seconds, AnimationWrapMode wrap, int node) const;
    // The same for a node of a local pose, and its inverse: rewrite `node`'s local transform so
    // that its model-space transform becomes `modelSpace` (its own scale is kept).
    glm::mat4 PoseNodeModelSpace(const std::vector<LocalTRS>& pose, int node) const;
    void SetPoseNodeModelSpace(std::vector<LocalTRS>& pose, int node, const glm::mat4& modelSpace) const;
    // Takes `clip`'s root motion out of `pose` (a sample of that clip): see RootMotionInPlace.
    void StripRootMotion(std::vector<LocalTRS>& pose, int clip, int node, const RootMotionSettings& s) const;
    // The root's motion in `clip` between playback times t0 and t1 (seconds, unwrapped).
    RootMotionDelta ClipRootMotion(int clip, float t0, float t1, AnimationWrapMode wrap, int node,
                                   const RootMotionSettings& s) const;
    // Built-in playback (PlayAnimation / an Animation component): with a node set (>= 0) the
    // playing clips are posed in place and their motion collects until ConsumeRootMotion takes it.
    // -1 turns it off.
    void SetRootMotion(int node, const RootMotionSettings& s);
    int RootMotionNode() const { return m_RootMotionNode; }

    // Nodes drawn collapsed: each one's subtree folds into its pivot (scale ~0), so skinned
    // geometry it drives disappears - e.g. a first-person body's arms while separate view-model
    // arms are shown. Empty = none. The pose itself is unchanged; only the node globals are.
    void SetHiddenNodes(const std::vector<int>& nodes);
    RootMotionDelta ConsumeRootMotion();

    const std::string& Path() const { return m_Path; }
    // #132 - the file was renamed or moved outside the editor; the loaded data stays valid.
    void SetPath(const std::string& path) { m_Path = path; }
    glm::vec3 BoundsMin() const { return m_D->BoundsMin; }
    glm::vec3 BoundsMax() const { return m_D->BoundsMax; }

    // Draw with per-submesh MaterialAsset slots (PR5). slot[i] non-null overrides submesh i's
    // imported material. Empty or short slots fall back to the imported mesh material.
    void Draw(Shader& shader, const std::vector<std::shared_ptr<MaterialAsset>>& slots);
    void DrawDepthOnly(Shader& shader, const std::vector<std::shared_ptr<MaterialAsset>>& slots);

    // Scene-path draw (audit #354): per submesh, `selectProgram(slot)` picks the program (a
    // ShaderAsset variant, or `fallback` when it returns null / there's no linked shader). This
    // owns the per-program uModel / uNormalMatrix / bone upload so a mesh can draw through a
    // different program than its neighbour. `xform` is the entity's world transform.
    using ProgramSelector = std::function<Shader*(const MaterialAsset*)>;
    // #112 — which submeshes a DrawSelected call draws: the render queue is per material slot,
    // so a model can have opaque and transparent parts that belong in different passes.
    enum class MeshPass { All, Opaque, Transparent };
    void DrawSelected(Shader& fallback, const glm::mat4& xform,
                      const std::vector<std::shared_ptr<MaterialAsset>>& slots,
                      const ProgramSelector& selectProgram, float opacity = 1.0f,
                      const std::function<void(Shader&)>& onProgramBound = {},
                      MeshPass pass = MeshPass::All);
    int MeshCount() const { return (int)m_D->Meshes.size(); }
    Material& MeshMaterial(int index) { return m_D->Meshes[index]->Mat; }
    const Material& MeshMaterial(int index) const { return m_D->Meshes[index]->Mat; }
    // Editor sub-asset list (#236 G): per-mesh geometry counts.
    unsigned int MeshTriangleCount(int index) const { return m_D->Meshes[index]->IndexCount() / 3u; }
    unsigned int MeshVertexCount(int index) const { return m_D->Meshes[index]->VertexCount(); }

    // Flatten every sub-mesh's bind-pose geometry into one vertex list + one triangle-index
    // list (indices rebased per sub-mesh) for PhysX mesh / convex collider cooking (#185 PR 6).
    // Local space — the caller applies the entity transform via PxMeshScale. Clears the outputs
    // first; leaves them empty for a model with no meshes.
    void CollisionGeometry(std::vector<glm::vec3>& outVertices,
                           std::vector<unsigned int>& outIndices) const;

    // #192: the material value-hash this model draws with, so the scene draw loop can sort
    // entities to put value-identical materials adjacent (which is what makes the BindMaterial
    // Per-model fallback sort key (first mesh). Callers that have a RenderableComponent
    // should prefer RenderableMaterialSortKey() which accounts for Materials slots.
    std::uint64_t MaterialSortKey() const {
        return m_D->Meshes.empty() ? 0 : m_D->Meshes[0]->Mat.Hash();
    }

    // Summed across every sub-mesh, for the editor's statistics overlay.
    unsigned int TriangleCount() const;
    unsigned int VertexCount() const;

    // Nearest bind-pose vertex to a screen-space point (e.g. the mouse cursor), within
    // maxPixelDist pixels. Used for the editor's "press V to grab a vertex" workflow — picks
    // which vertex on THIS model becomes the drag anchor. Returns the vertex in LOCAL space
    // so the caller can keep tracking it as the model's own transform changes during the drag.
    bool FindNearestVertexToScreenPoint(const glm::mat4& modelMatrix, const glm::mat4& viewProj,
        const glm::vec2& screenPoint, float viewportW, float viewportH, float maxPixelDist, glm::vec3& outLocalPos) const;

    // Lowest Y among every actual bind-pose vertex (across all meshes), after being placed by
    // modelMatrix. Used for "Snap to Ground" — unlike BoundsMin()/BoundsMax() transformed as a
    // box, this is exact for a rotated or non-box-shaped mesh: a transformed AABB is a loose
    // fit around a rotated shape and sits below every real vertex, so snapping to ITS bottom
    // leaves the actual geometry floating above the ground instead of resting on it.
    float LowestVertexWorldY(const glm::mat4& modelMatrix) const;

    // #116/#117 — exact ray vs. the model's bind-pose triangles, placed by modelMatrix. Ray in
    // world space (dir need not be normalised); on a hit returns the world-space distance along
    // normalize(dir) and the world-space face normal (facing the ray origin). Hits behind the
    // origin, and within minT of it, are ignored.
    bool RaycastTriangles(const glm::mat4& modelMatrix, const glm::vec3& worldOrigin, const glm::vec3& worldDir,
                          float& outT, glm::vec3* outNormal = nullptr, float minT = 1e-4f) const;

private:
    Model() = default; // used only by CreatePrimitive; file-based loading always goes through the path constructor

    // #96 — everything an import produces, shared by every instance of the same asset.
    struct SharedData {
        std::string Directory;
        std::vector<std::unique_ptr<ModelMesh>> Meshes;
        std::map<std::string, std::shared_ptr<Texture>> TextureCache;
        std::map<std::string, BoneInfo> BoneInfoMap;
        int BoneCounter = 0;
        glm::mat4 GlobalInverseTransform{1.0f};
        std::vector<AnimNode> Nodes; // flattened hierarchy, parents first (#113)
        std::vector<AnimationClip> Animations;
        std::vector<glm::mat4> BindPoseBones; // #98 — palette with no clip playing
        glm::vec3 BoundsMin{1e30f}, BoundsMax{-1e30f};
        ModelImportSettings Settings;
    };
    std::shared_ptr<SharedData> m_D = std::make_shared<SharedData>();

    std::string m_Path;

    // Per-instance animation state.
    struct PlaybackState {
        int Clip = -1;                                   // index into Animations, -1 = none
        float Time = 0.0f;                               // seconds since it started
        float Speed = 1.0f;
        AnimationWrapMode Wrap = AnimationWrapMode::Loop;
    };
    PlaybackState m_Anim;          // what's playing
    PlaybackState m_AnimFrom;      // what's being faded out (Clip -1 = the bind pose)
    float m_FadeElapsed = 0.0f, m_FadeDuration = 0.0f; // crossfade progress; duration 0 = none
    bool m_PosePending = false;    // a fade to "stopped" still needs final matrices this frame
    bool m_ExternalPose = false;   // ApplyLocalPose owns the pose until the next PlayAnimation
    std::vector<LocalTRS> m_AppliedPose; // ... and what it was given (AppliedLocalPose)
    std::vector<glm::mat4> m_FinalBoneMatrices;
    std::vector<glm::mat4> m_NodeGlobals; // scratch, one per AnimNode

    // #175 — clips borrowed from other model files, retargeted by node name.
    struct ExternalClip {
        std::shared_ptr<SharedData> Source; // the source model's (shared) import data
        int SourceIndex = -1;           // its clip index there (an index, not a pointer: a reimport
        size_t SourceChannels = 0;      // of the source rebuilds its clip list; a changed channel
                                        // count then drops the clip instead of reading stale data)
        std::vector<int> NodeChannel;   // per node of THIS model -> channel in Clip, or -1
        // Per node of this model: a rotation applied on top of the clip's sampled local, for the
        // top animated bones whose rest orientation differs between the two files - e.g. a Z-up
        // animation pack on a Y-up rig whose root carries the axis conversion as a pre-rotation.
        // Empty when no bone needs one.
        std::vector<glm::quat> Correction;
        std::string Ref, DisplayName;
    };
    std::vector<ExternalClip> m_ExternalClips;
    // The clip at combined index `i` and its node->channel map for this model (nullptr if none).
    const AnimationClip* ClipAt(int i, const std::vector<int>** nodeChannel) const;
    // Clip `clipIndex`'s channel `channel` sampled for node `node` at `ticks`, with the attached
    // clip's retarget correction applied.
    LocalTRS SampleClipNode(int clipIndex, const AnimationClip& clip, int channel, int node, float ticks) const;

    // Built-in playback root motion (SetRootMotion).
    int m_RootMotionNode = -1;
    RootMotionSettings m_RootMotionSettings;
    RootMotionDelta m_RootMotionPending;
    // FindRootMotionNode's auto pick, for the import it was found on.
    mutable const SharedData* m_AutoRootMotionFor = nullptr;
    mutable int m_AutoRootMotionNode = -1;
    std::vector<unsigned char> m_HiddenNodes; // per node, SetHiddenNodes (empty = none hidden)

    // Last engine frame index on which TickAnimationOnce() actually advanced this model; an
    // impossible sentinel (uint64_t max) so frame index 0 doesn't look "already ticked".
    uint64_t m_LastTickedFrame = ~0ull;

    // Import-time only: each node's model-space bind transform, for the bind-pose palette (#98).
    std::map<std::string, glm::mat4> m_ImportNodeGlobals;

    void ImportFromFile(const ModelImportSettings& settings);
    void ProcessNode(aiNode* node, const aiScene* scene, const glm::mat4& parentTransform);
    std::unique_ptr<ModelMesh> ProcessMesh(aiMesh* mesh, const aiScene* scene, const glm::mat4& nodeTransform);
    Material ExtractMaterial(const aiScene* scene, unsigned int materialIndex);
    // #95 — what a material slot's texture holds, which decides its colour space.
    enum class TextureRole { Color, Normal, Data };
    std::shared_ptr<Texture> LoadCachedTexture(const std::string& fullPath, TextureRole role);
    std::shared_ptr<Texture> LoadEmbeddedTexture(const aiTexture* tex, const std::string& ref, TextureRole role); // #113
    // Turns whatever path string a model file baked in for a texture (bare filename, path
    // relative to the model, a "..\tex\x.png" with junk separators, or an absolute path from
    // the machine the asset was authored on) into a real file on THIS disk. Tries the sensible
    // interpretations in order and returns the first that exists; falls back to a clean model-dir
    // join (so a failure logs a sane path, never "modelDir + C:\someone-else\..."). An embedded
    // ("*0") reference is returned unchanged for the caller to handle. See Model.cpp for the
    // full resolution order.
    std::string ResolveTexturePath(const std::string& raw) const;
    static std::string ResolveTexturePathIn(const std::string& modelDir, const std::string& raw);
    void ExtractBoneWeights(std::vector<ModelVertex>& vertices, aiMesh* mesh);
    void ReadHierarchy(const aiNode* node, int parent);
    void ReadAnimations(const aiScene* scene);
    void EvaluatePose(); // current (and fading-out) clip -> m_FinalBoneMatrices
    void CollectNodeGlobals(const aiNode* node, const glm::mat4& parentTransform);
};
