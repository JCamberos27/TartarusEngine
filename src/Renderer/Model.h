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

class Texture;
class Shader;
struct aiScene;
struct aiNode;
struct aiMesh;
struct aiMaterial;

struct BoneInfo {
    int ID;
    glm::mat4 OffsetMatrix;
};

constexpr int MAX_BONES = 100;

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

    // Backward-compat: no material slots → uses every submesh's imported Material.
    void Draw(Shader& shader) { Draw(shader, {}); }

    // Geometry only — no material binds, no texture units. For the shadow / depth pre-pass.
    // Still uploads bone matrices + uUseSkinning: the depth vertex shader skins too.
    // slots: per-submesh MaterialAsset overrides; empty/short → imported mesh material.
    void DrawDepthOnly(Shader& shader) { DrawDepthOnly(shader, {}); }

    bool HasAnimations() const { return !m_D->Animations.empty(); }
    int AnimationCount() const { return (int)m_D->Animations.size(); }
    const std::string& AnimationName(int index) const { return m_D->Animations[index].Name; }

    void PlayAnimation(int index);
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
    bool IsPlayingAnimation() const { return m_CurrentAnimation >= 0; }

    const std::string& Path() const { return m_Path; }
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
    void DrawSelected(Shader& fallback, const glm::mat4& xform,
                      const std::vector<std::shared_ptr<MaterialAsset>>& slots,
                      const ProgramSelector& selectProgram, float opacity = 1.0f);
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
        AssimpNodeData RootNode;
        std::vector<AnimationClip> Animations;
        std::vector<glm::mat4> BindPoseBones; // #98 — palette with no clip playing
        glm::vec3 BoundsMin{1e30f}, BoundsMax{-1e30f};
        ModelImportSettings Settings;
    };
    std::shared_ptr<SharedData> m_D = std::make_shared<SharedData>();

    std::string m_Path;

    // Per-instance animation state.
    int m_CurrentAnimation = -1;
    float m_CurrentTimeTicks = 0.0f;
    std::vector<glm::mat4> m_FinalBoneMatrices;

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
    // Turns whatever path string a model file baked in for a texture (bare filename, path
    // relative to the model, a "..\tex\x.png" with junk separators, or an absolute path from
    // the machine the asset was authored on) into a real file on THIS disk. Tries the sensible
    // interpretations in order and returns the first that exists; falls back to a clean model-dir
    // join (so a failure logs a sane path, never "modelDir + C:\someone-else\..."). An embedded
    // ("*0") reference is returned unchanged for the caller to handle. See Model.cpp for the
    // full resolution order.
    std::string ResolveTexturePath(const std::string& raw) const;
    void ExtractBoneWeights(std::vector<ModelVertex>& vertices, aiMesh* mesh);
    void ReadHierarchy(AssimpNodeData& out, const aiNode* node);
    void ReadAnimations(const aiScene* scene);
    void CalculateBoneTransform(const AssimpNodeData& node, const glm::mat4& parentTransform);
    void CollectNodeGlobals(const aiNode* node, const glm::mat4& parentTransform);
};
