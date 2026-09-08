#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <glm/glm.hpp>
#include "ModelMesh.h"
#include "Animation.h"
#include "Material.h"

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

    // Re-runs the Assimp import from disk with new settings, replacing this Model's meshes/
    // bones/animations in place — every existing shared_ptr<Model> (placed instances included)
    // sees the update, since it's the same object. MaterialOverride (an editor-set look that
    // deliberately replaces whatever the file specifies) is left untouched. Returns false (and
    // leaves the previous import in place) if the file can't be re-read.
    bool Reimport(const ModelImportSettings& settings);
    const ModelImportSettings& ImportSettings() const { return m_Settings; }

    // Builds a procedural primitive (kind: "cube"/"sphere"/"cylinder"/"cone"/"plane") instead
    // of importing a file. `path` is stored as this Model's Path() so the editor's usual
    // per-path asset cache and scene-save/load round-trip both work unmodified — the caller
    // (AssetLibrary) is responsible for making that path unique per placed instance.
    static std::shared_ptr<Model> CreatePrimitive(const std::string& kind, const std::string& path);

    void Draw(Shader& shader);

    // Geometry only — no material binds, no texture units. For the shadow / depth pre-pass,
    // whose fragment shader has none of the PBR uniforms, so the ~23 SetX + 7 texture binds
    // BindMaterial does per mesh are all wasted (and it runs per cascade). Still uploads bone
    // matrices + uUseSkinning: the depth vertex shader skins too, so skinned casters animate.
    void DrawDepthOnly(Shader& shader);

    bool HasAnimations() const { return !m_Animations.empty(); }
    int AnimationCount() const { return (int)m_Animations.size(); }
    const std::string& AnimationName(int index) const { return m_Animations[index].Name; }

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

    void UploadBoneMatrices(Shader& shader) const;
    bool IsPlayingAnimation() const { return m_CurrentAnimation >= 0; }

    const std::string& Path() const { return m_Path; }
    glm::vec3 BoundsMin() const { return m_BoundsMin; }
    glm::vec3 BoundsMax() const { return m_BoundsMax; }

    // When set, every mesh in this model renders with this material instead of the
    // one extracted from the source file — this is how the editor lets you override
    // an imported model's look with a custom PBR material.
    void SetMaterialOverride(std::shared_ptr<Material> mat) { m_MaterialOverride = std::move(mat); }
    std::shared_ptr<Material> MaterialOverride() const { return m_MaterialOverride; }
    int MeshCount() const { return (int)m_Meshes.size(); }
    Material& MeshMaterial(int index) { return m_Meshes[index]->Mat; }
    const Material& MeshMaterial(int index) const { return m_Meshes[index]->Mat; }
    // Editor sub-asset list (#236 G): per-mesh geometry counts.
    unsigned int MeshTriangleCount(int index) const { return m_Meshes[index]->IndexCount() / 3u; }
    unsigned int MeshVertexCount(int index) const { return m_Meshes[index]->VertexCount(); }

    // #192: the material value-hash this model draws with, so the scene draw loop can sort
    // entities to put value-identical materials adjacent (which is what makes the BindMaterial
    // dedup in GLStateCache actually hit). The override, or the first sub-mesh's material —
    // enough to cluster the primitives that share a handful of material values; multi-mesh
    // imports still group by this plus the model pointer.
    std::uint64_t MaterialSortKey() const {
        if (m_MaterialOverride) return m_MaterialOverride->Hash();
        return m_Meshes.empty() ? 0 : m_Meshes[0]->Mat.Hash();
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

private:
    Model() = default; // used only by CreatePrimitive; file-based loading always goes through the path constructor

    std::string m_Path;
    std::string m_Directory;
    std::vector<std::unique_ptr<ModelMesh>> m_Meshes;
    std::map<std::string, std::shared_ptr<Texture>> m_TextureCache;
    std::shared_ptr<Material> m_MaterialOverride;

    std::map<std::string, BoneInfo> m_BoneInfoMap;
    int m_BoneCounter = 0;
    glm::mat4 m_GlobalInverseTransform{1.0f};
    AssimpNodeData m_RootNode;

    std::vector<AnimationClip> m_Animations;
    int m_CurrentAnimation = -1;
    float m_CurrentTimeTicks = 0.0f;
    std::vector<glm::mat4> m_FinalBoneMatrices;

    // Last engine frame index on which TickAnimationOnce() actually advanced this model; an
    // impossible sentinel (uint64_t max) so frame index 0 doesn't look "already ticked".
    uint64_t m_LastTickedFrame = ~0ull;

    glm::vec3 m_BoundsMin{1e30f}, m_BoundsMax{-1e30f};
    ModelImportSettings m_Settings;

    void ImportFromFile(const ModelImportSettings& settings);
    void ProcessNode(aiNode* node, const aiScene* scene, const glm::mat4& parentTransform);
    std::unique_ptr<ModelMesh> ProcessMesh(aiMesh* mesh, const aiScene* scene, const glm::mat4& nodeTransform);
    Material ExtractMaterial(const aiScene* scene, unsigned int materialIndex);
    std::shared_ptr<Texture> LoadCachedTexture(const std::string& fullPath);
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
};
