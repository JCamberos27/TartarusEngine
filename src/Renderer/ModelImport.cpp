// Model import from a source file through Assimp: the node walk, mesh merge / simplify, imported materials,
// bone weights, the node hierarchy and the animation clips.

#include "Model.h"
#include "AnimRetarget.h"
#include "MaterialAsset.h"
#include "ShaderAsset.h"
#include "Log.h"
#include "AssetDatabase.h"
#include "Texture.h"
#include "DefaultTextures.h"
#include "meshoptimizer.h"
#include "gl.h"
#include "Core/Env.h"

#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <assimp/GltfMaterial.h>

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <json.hpp>
#include <cctype>
#include <fstream>
#include <set>
#include <cstddef>
#include <mutex>
#include <unordered_map>

namespace {

glm::mat4 AiToGlm(const aiMatrix4x4& from) {
    glm::mat4 to;
    to[0][0] = from.a1; to[1][0] = from.a2; to[2][0] = from.a3; to[3][0] = from.a4;
    to[0][1] = from.b1; to[1][1] = from.b2; to[2][1] = from.b3; to[3][1] = from.b4;
    to[0][2] = from.c1; to[1][2] = from.c2; to[2][2] = from.c3; to[3][2] = from.c4;
    to[0][3] = from.d1; to[1][3] = from.d2; to[2][3] = from.d3; to[3][3] = from.d4;
    return to;
}

// ModelImportSettings::UnrotateNodes: each named node keeps its position and scale and loses its own rotation.
void UnrotateNodes(aiNode* node, const std::vector<std::string>& names) {
    if (std::find(names.begin(), names.end(), node->mName.C_Str()) != names.end()) {
        aiVector3D scale, position;
        aiQuaternion rotation;
        node->mTransformation.Decompose(scale, rotation, position);
        node->mTransformation = aiMatrix4x4(scale, aiQuaternion(), position);
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) UnrotateNodes(node->mChildren[i], names);
}

// ModelImportSettings::MoveNodes: each named node shifted in model space, through its parents' frames.
void MoveNodes(aiNode* root, const std::vector<ModelImportSettings::NodeMove>& moves) {
    for (const auto& m : moves) {
        aiNode* node = root->FindNode(m.Node.c_str());
        if (!node) continue;
        aiMatrix4x4 parent;
        for (const aiNode* p = node->mParent; p; p = p->mParent) parent = p->mTransformation * parent;
        aiMatrix4x4 by;
        aiMatrix4x4::Translation(aiVector3D(m.By.x, m.By.y, m.By.z), by);
        node->mTransformation = aiMatrix4x4(parent).Inverse() * by * parent * node->mTransformation;
    }
}

// Element-wise, relative to the matrices' size (translations can be in centimetres).
bool NearlyEqual(const glm::mat4& a, const glm::mat4& b) {
    float scale = 1.0f;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) scale = std::max(scale, std::max(std::abs(a[c][r]), std::abs(b[c][r])));
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > 1e-3f * scale) return false;
    return true;
}

bool IsNearlyIdentity(const glm::mat4& m) { return NearlyEqual(m, glm::mat4(1.0f)); }

glm::vec3 AiToGlm(const aiVector3D& v) { return {v.x, v.y, v.z}; }
glm::quat AiToGlm(const aiQuaternion& q) { return {q.w, q.x, q.y, q.z}; }
} // namespace

bool Model::ImportFromFile(const ModelImportSettings& settings) {
    ImportScratch scratch;
    m_Import = &scratch;
    struct ClearScratch { Model* M; ~ClearScratch() { M->m_Import = nullptr; } } clearScratch{this};
    BumpAnimationEpoch(); // bones or clips may appear (a reimport reaches every instance sharing m_D)
    Assimp::Importer importer;
    // FBX files embed their own unit scale (commonly centimeters, sometimes meters or
    // inches) in the file's global settings. aiProcess_GlobalScale + this property tells
    // Assimp to read that and auto-convert to real-world meters, so a model built at 1
    // unit = 1cm no longer imports 100x too large without the artist doing anything special.
    // Files with no such metadata (most .obj) are unaffected — this can't invent scale that
    // was never recorded, so hand-authored/untagged assets may still need manual correction.
    // settings.GlobalScale multiplies on top of that derived correction (Import Settings'
    // "Import Scale" knob), rather than replacing it.
    importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, settings.GlobalScale);
    // #175 — don't split FBX bones into "$AssimpFbx$_Translation/_PreRotation/..." helper nodes.
    // Assimp writes each bone's animation channel as the bone's FULL local transform, so with
    // the helpers kept their bind offsets were applied a second time and animated limbs came
    // apart (every Mixamo rig). Folding the pivots into the bone node keeps bind pose identical.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    unsigned int flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale;
    if (settings.ImportNormals) flags |= aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;
    // The skin audit (--model-report with TARTARUS_SKIN_AUDIT set) reads each vertex's true influence count: Assimp's own
    // cap is left off and ExtractBoneWeights' (the same rule - the largest four, renormalized) counts what it drops.
    static const bool s_SkinAudit = Env::Flag(Env::Var::SkinAudit);
    if (settings.ImportSkeleton && !s_SkinAudit) flags |= aiProcess_LimitBoneWeights;
    importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, MAX_BONE_INFLUENCE); // Assimp's default is four
    if (settings.OptimizeGraph) flags |= aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes;
    // A clip source: the same scale and pivot handling (so its nodes and keys are exactly a full import's), none of the
    // mesh work - those flags only touch meshes, and the meshes aren't read.
    if (settings.AnimationOnly) flags = aiProcess_GlobalScale;

    const aiScene* scene = importer.ReadFile(m_Path, flags);

    // Up axis and pivot: folded into the root node, so static meshes bake them into their vertices (and UnrotateNodes /
    // MoveNodes into their own nodes). A clip source is left alone so its keys stay exactly the full import's.
    if (scene && scene->mRootNode && !settings.AnimationOnly) {
        if (!settings.UnrotateNodes.empty()) UnrotateNodes(scene->mRootNode, settings.UnrotateNodes);
        if (!settings.MoveNodes.empty()) MoveNodes(scene->mRootNode, settings.MoveNodes);
        glm::mat4 fix = glm::translate(glm::mat4(1.0f), settings.PivotOffset);
        if (settings.SourceUpAxis == ModelImportSettings::UpAxis::Z) fix = fix * AxisCorrection(2, 1, 1, -1);
        else if (settings.SourceUpAxis == ModelImportSettings::UpAxis::NegZ) fix = fix * AxisCorrection(2, -1, 1, 1);
        fix = fix * settings.RootTransform;
        if (fix != glm::mat4(1.0f)) {
            const glm::mat4 root = fix * AiToGlm(scene->mRootNode->mTransformation);
            aiMatrix4x4& m = scene->mRootNode->mTransformation;
            m.a1 = root[0][0]; m.a2 = root[1][0]; m.a3 = root[2][0]; m.a4 = root[3][0];
            m.b1 = root[0][1]; m.b2 = root[1][1]; m.b3 = root[2][1]; m.b4 = root[3][1];
            m.c1 = root[0][2]; m.c2 = root[1][2]; m.c3 = root[2][2]; m.c4 = root[3][2];
            m.d1 = root[0][3]; m.d2 = root[1][3]; m.d3 = root[2][3]; m.d4 = root[3][3];
        }
    }

    // #175 — an animation-only file (Mixamo "without skin", a clip library) has no meshes, which
    // Assimp flags INCOMPLETE; that's a valid clip asset, not a failed import.
    const bool animationOnly = scene && scene->mRootNode && scene->mNumMeshes == 0 && scene->mNumAnimations > 0;
    if (!scene || ((scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) && !animationOnly) || !scene->mRootNode) {
        if (!AssetDatabase::IsUnfetchedExternal(m_Path)) // a Drive file not fetched yet: the scan's warning counts it
            Log::Error("Model: import failed for '" + m_Path + "': " + importer.GetErrorString(), LogContext::Asset(m_Path));
        // A failed import leaves an empty model's bounds collapsed to a finite point, so anything
        // that folds this model into a wider AABB (scene framing, focus) can't inherit the
        // inverted 1e30 sentinel and blow the result up to inf/NaN. A failed REimport keeps the
        // previous import untouched.
        if (m_D->Meshes.empty()) {
            m_D->BoundsMin = glm::vec3(0.0f);
            m_D->BoundsMax = glm::vec3(0.0f);
        }
        if (m_FinalBoneMatrices.empty()) m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
        ++m_PoseVersion;
        return false;
    }

    // #96 — import into a fresh SharedData, then move it into the object every instance of this
    // asset shares, so a Reimport reaches placed instances too and starts from a clean slate.
    const std::shared_ptr<SharedData> target = m_D;
    m_D = std::make_shared<SharedData>();
    m_D->Directory = target->Directory;
    m_D->Settings = settings;

    m_D->GlobalInverseTransform = glm::inverse(AiToGlm(scene->mRootNode->mTransformation));
    m_D->BindPoseBones.assign(MAX_BONES, glm::mat4(1.0f));
    m_Import->NodeGlobals.clear();
    CollectNodeGlobals(scene->mRootNode, glm::mat4(1.0f));

    if (!settings.AnimationOnly) {
        m_Import->MergeBuckets.clear();
        ProcessNode(scene->mRootNode, scene, glm::mat4(1.0f));
        FlushMergeBuckets(scene);
        // RootNodeInModelSpace: the vertices were baked with the root node's frame (ProcessNode starts above it), so
        // model space keeps it too. Re-base every bone onto it: offset' = offset * GlobalInverse makes
        // GlobalInverse' (identity) * Global * offset' == R * (GlobalInverse * Global * offset) * R^-1 - the same
        // bind, and each bone's delta turned into the vertices' frame instead of the root-less one.
        if (settings.RootNodeInModelSpace && !IsNearlyIdentity(m_D->GlobalInverseTransform)) {
            const glm::mat4 gi = m_D->GlobalInverseTransform, root = glm::inverse(gi);
            for (auto& [name, info] : m_D->BoneInfoMap) {
                info.OffsetMatrix = info.OffsetMatrix * gi;
                m_D->BindPoseBones[info.ID] = root * m_D->BindPoseBones[info.ID] * gi;
            }
            m_D->GlobalInverseTransform = glm::mat4(1.0f);
        }
    } else m_D->BoundsMin = m_D->BoundsMax = glm::vec3(0.0f);
    ReadHierarchy(scene->mRootNode, -1);
    if (settings.ImportAnimations) ReadAnimations(scene);
    m_Import->NodeGlobals.clear();

    *target = std::move(*m_D);
    m_D = target;

    m_Anim = {};
    m_AnimFrom = {};
    m_FadeDuration = m_FadeElapsed = 0.0f;
    m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
    // The node list may have changed: an applied pose, hidden nodes and the auto root-motion pick were for the old one.
    m_ExternalPose = false;
    m_AppliedPose.clear();
    m_HiddenNodes.clear();
    m_PoseDirty = m_PaletteDirty = false;
    m_AutoRootMotionFor = nullptr;
    ++m_PoseVersion;
    return true;
}

ModelImportSettings::UpAxis Model::ParseUpAxis(const std::string& s) {
    return s == "Z" ? ModelImportSettings::UpAxis::Z : s == "-Z" ? ModelImportSettings::UpAxis::NegZ : ModelImportSettings::UpAxis::Y;
}

const char* Model::UpAxisName(ModelImportSettings::UpAxis a) {
    return a == ModelImportSettings::UpAxis::Z ? "Z" : a == ModelImportSettings::UpAxis::NegZ ? "-Z" : "Y";
}

glm::mat4 Model::AxisCorrection(int upAxis, int upSign, int frontAxis, int frontSign) {
    if (upAxis < 0 || upAxis > 2 || frontAxis < 0 || frontAxis > 2 || upAxis == frontAxis) return glm::mat4(1.0f);
    glm::vec3 u(0.0f), f(0.0f);
    u[upAxis] = upSign < 0 ? -1.0f : 1.0f;
    f[frontAxis] = frontSign < 0 ? -1.0f : 1.0f;
    const glm::vec3 r = glm::cross(u, f);
    // Rows r, u, f: a source vector's components along right / up / front become engine X / Y / Z.
    return glm::mat4(glm::transpose(glm::mat3(r, u, f)));
}

void Model::CollectNodeGlobals(const aiNode* node, const glm::mat4& parentTransform) {
    const glm::mat4 global = parentTransform * AiToGlm(node->mTransformation);
    m_Import->NodeGlobals.emplace(node->mName.C_Str(), global);
    for (unsigned int i = 0; i < node->mNumChildren; ++i) CollectNodeGlobals(node->mChildren[i], global);
}

bool Model::Reimport(const ModelImportSettings& settings) {
    std::error_code ec;
    // primitive:// paths aren't real files - regenerating one from ImportFromFile would try
    // (and fail) to open that synthetic path via Assimp, so reimporting a primitive is a no-op.
    if (m_Path.rfind("primitive://", 0) == 0) return false;
    if (!std::filesystem::exists(m_Path, ec) || ec) {
        Log::Error("Model: cannot reimport '" + m_Path + "' - file is missing.", LogContext::Asset(m_Path));
        return false;
    }
    return ImportFromFile(settings);
}

Model::~Model() { BumpAnimationEpoch(); }

void Model::CollisionGeometry(std::vector<glm::vec3>& outVertices,
                              std::vector<unsigned int>& outIndices) const {
    outVertices.clear();
    outIndices.clear();
    for (const auto& mesh : m_D->Meshes) {
        const auto& pos = mesh->LocalPositions();
        const auto& idx = mesh->LocalIndices();
        const unsigned int base = (unsigned int)outVertices.size();
        outVertices.insert(outVertices.end(), pos.begin(), pos.end());
        outIndices.reserve(outIndices.size() + idx.size());
        for (unsigned int i : idx) outIndices.push_back(base + i);
    }
}

bool Model::RaycastTriangles(const glm::mat4& modelMatrix, const glm::vec3& worldOrigin, const glm::vec3& worldDir,
                             float& outT, glm::vec3* outNormal, float minT, int* outMeshIndex) const {
    const float dirLen = glm::length(worldDir);
    if (dirLen < 1e-12f) return false;
    const glm::vec3 wd = worldDir / dirLen;
    // Test in LOCAL space (no per-vertex transform), then measure the hit back in world space so
    // a non-uniformly scaled model still reports a correct world distance.
    const glm::mat4 inv = glm::inverse(modelMatrix);
    const glm::vec3 lo = glm::vec3(inv * glm::vec4(worldOrigin, 1.0f));
    const glm::vec3 ld = glm::vec3(inv * glm::vec4(wd, 0.0f));
    float bestWorldT = 1e30f;
    glm::vec3 bestLocalN(0.0f);
    bool hit = false;
    int bestMesh = -1, meshIndex = -1;
    for (const auto& mesh : m_D->Meshes) {
        ++meshIndex;
        const auto& pos = mesh->LocalPositions();
        const auto& idx = mesh->LocalIndices();
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            if (idx[i] >= pos.size() || idx[i + 1] >= pos.size() || idx[i + 2] >= pos.size()) continue;
            const glm::vec3& a = pos[idx[i]];
            const glm::vec3& b = pos[idx[i + 1]];
            const glm::vec3& c = pos[idx[i + 2]];
            // Moller-Trumbore, two-sided.
            const glm::vec3 e1 = b - a, e2 = c - a;
            const glm::vec3 p = glm::cross(ld, e2);
            const float det = glm::dot(e1, p);
            if (std::fabs(det) < 1e-12f) continue;
            const float invDet = 1.0f / det;
            const glm::vec3 tv = lo - a;
            const float u = glm::dot(tv, p) * invDet;
            if (u < 0.0f || u > 1.0f) continue;
            const glm::vec3 q = glm::cross(tv, e1);
            const float v = glm::dot(ld, q) * invDet;
            if (v < 0.0f || u + v > 1.0f) continue;
            const float tl = glm::dot(e2, q) * invDet;
            if (tl <= 0.0f) continue;
            const glm::vec3 worldHit = glm::vec3(modelMatrix * glm::vec4(lo + ld * tl, 1.0f));
            const float tw = glm::dot(worldHit - worldOrigin, wd);
            if (tw < minT || tw >= bestWorldT) continue;
            bestWorldT = tw;
            bestLocalN = glm::cross(e1, e2);
            bestMesh = meshIndex;
            hit = true;
        }
    }
    if (!hit) return false;
    outT = bestWorldT;
    if (outMeshIndex) *outMeshIndex = bestMesh;
    if (outNormal) {
        glm::vec3 n = glm::normalize(NormalMatrix(modelMatrix) * bestLocalN);
        if (glm::dot(n, wd) > 0.0f) n = -n; // face the ray origin
        *outNormal = n;
    }
    return true;
}

void Model::ProcessNode(aiNode* node, const aiScene* scene, const glm::mat4& parentTransform) {
    const auto& excluded = m_D->Settings.ExcludeNodes;
    if (!excluded.empty() && std::find(excluded.begin(), excluded.end(), node->mName.C_Str()) != excluded.end()) return;
    // Assimp's mesh vertex data is expressed in the local space of whatever node the mesh
    // is attached to, NOT world/model space — each node in the FBX/glTF hierarchy can carry
    // its own offset/rotation (e.g. a shotgun's barrel, stock, and trigger guard are commonly
    // separate nodes). Skipping this accumulation collapses every part to the model origin.
    glm::mat4 nodeTransform = parentTransform * AiToGlm(node->mTransformation);

    const auto& only = m_D->Settings.MeshNodes;
    const bool keep = only.empty() || std::find(only.begin(), only.end(), node->mName.C_Str()) != only.end();
    for (unsigned int i = 0; keep && i < node->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        if (auto m = ProcessMesh(mesh, scene, nodeTransform, node)) m_D->Meshes.push_back(std::move(m));
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        ProcessNode(node->mChildren[i], scene, nodeTransform);
    }
}

// Import Settings > Simplify: the mesh cut to about `ratio` of its triangles (meshoptimizer, the error capped at 1% of the
// mesh's size, so a flat or already lean mesh keeps what it needs), then its vertices packed to the ones still used.
// Skinned meshes too: the simplifier only drops vertices, so each one kept keeps its own bone weights. What addresses a
// skinned mesh's vertices by number (the outfit hiding, the first-person collar) works them out from the simplified mesh.
static void SimplifyMesh(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices, float ratio) {
    if (!(ratio > 0.0f && ratio < 1.0f) || indices.size() < 3 * 512 || indices.size() % 3 != 0 || vertices.empty()) return;
    const size_t target = std::max<size_t>(3 * 64, (size_t)((double)indices.size() * ratio) / 3 * 3);
    std::vector<unsigned int> out(indices.size());
    size_t n = meshopt_simplify(out.data(), indices.data(), indices.size(), &vertices[0].Position.x, vertices.size(),
                                sizeof(ModelVertex), target, 0.01f, 0, nullptr);
    if (n > 2 * target) {
        // Barely reduced: the mesh is split at every face (per-face normals or UVs, a scan), and the simplifier keeps every
        // seam. Collapsed on the shape alone instead - each corner then takes one of its coincident vertices' attributes.
        std::vector<unsigned int> shape(indices.size());
        meshopt_generateShadowIndexBuffer(shape.data(), indices.data(), indices.size(), &vertices[0].Position.x, vertices.size(),
                                          sizeof(float) * 3, sizeof(ModelVertex));
        const size_t m = meshopt_simplify(out.data(), shape.data(), shape.size(), &vertices[0].Position.x, vertices.size(),
                                          sizeof(ModelVertex), target, 0.01f, 0, nullptr);
        if (m < n) n = m;
        else n = meshopt_simplify(out.data(), indices.data(), indices.size(), &vertices[0].Position.x, vertices.size(),
                                  sizeof(ModelVertex), target, 0.01f, 0, nullptr);
    }
    if (n > 2 * target) {
        // Still barely reduced (triangle soup, or borders everywhere): clustered, without keeping the topology.
        const size_t m = meshopt_simplifySloppy(out.data(), indices.data(), indices.size(), &vertices[0].Position.x, vertices.size(),
                                                sizeof(ModelVertex), nullptr, target, 0.01f, nullptr);
        if (m >= 3 && m < n) n = m;
        else n = meshopt_simplify(out.data(), indices.data(), indices.size(), &vertices[0].Position.x, vertices.size(),
                                  sizeof(ModelVertex), target, 0.01f, 0, nullptr);
    }
    if (n < 3 || n >= indices.size()) return;
    out.resize(n);
    std::vector<ModelVertex> packed(vertices.size());
    const size_t used = meshopt_optimizeVertexFetch(packed.data(), out.data(), out.size(), vertices.data(), vertices.size(), sizeof(ModelVertex));
    packed.resize(used);
    char line[160];
    std::snprintf(line, sizeof line, "Model import: a mesh simplified from %zu to %zu triangles (Simplify %.2f).", indices.size() / 3, out.size() / 3, ratio);
    Log::Info(line);
    vertices = std::move(packed);
    indices = std::move(out);
}

std::unique_ptr<ModelMesh> Model::ProcessMesh(aiMesh* mesh, const aiScene* scene, const glm::mat4& nodeTransform,
                                              const aiNode* node) {
    std::vector<ModelVertex> vertices(mesh->mNumVertices);

    // Gated on m_D->Settings.ImportSkeleton too: with skeleton import off, ExtractBoneWeights below
    // never runs, so treating this as "skinned" would leave every vertex's bone weights at their
    // default (unset) values instead of the identity-pose vertex position baked in here - the
    // mesh would render collapsed to the origin rather than as a static copy of its bind pose.
    bool skinned = m_D->Settings.ImportSkeleton && mesh->mNumBones > 0;

    // Which transform belongs in the vertex data?  Exactly the same one as for a non-skinned
    // mesh: the owning node's world transform.
    //
    // The shader multiplies every vertex by `GlobalInverse * PosedGlobal * BoneOffset`, which
    // lands in ROOT space. BoneOffset (Assimp: inverse(TransformLink) * absolute_transform)
    // only accounts for the *bone* hierarchy - FBX passes the ROOT node's transform as
    // absolute_transform, i.e. identity here - so nothing in that palette ever mentions the mesh
    // node's own frame. Folding nodeTransform in here is what puts the vertex into root space
    // before skinning, exactly as it does for static meshes.
    //
    // Measuring the residual C = RestGlobal * BoneOffset does NOT let you cancel anything out:
    // C is not a constant error term, it is the node tree's pose-versus-bind delta (bind pose
    // => C == I; a posed import => C == that pose). Inverting it erased the pose Assimp had
    // already baked into the node tree - it dropped the FPS weapon from the hands down to the
    // model's origin, where the FBX's un-posed mesh data sits.
    //
    // Why this reads as "no bug at bind, tears the moment a clip plays": at bind the whole
    // palette collapses to GlobalInverse * C, a single matrix shared by every bone, so a missing
    // node transform cannot tear anything - it just uniformly rotates the mesh. Only once a clip
    // plays does each bone apply its own delta to that un-rotated vertex, blowing a 0.02 m edge
    // apart into 0.5 m blades.
    //
    // The exception: a skin whose offsets already carry a node's frame. Some FBX exports write a
    // node's global bind matrix as the cluster's Transform, so at bind C is that frame and baking
    // on top applied it twice. The Quantum pack does it two ways: the female parts use the mesh
    // node's own frame (C == nodeTransform: the body lay on its back, the head floated off), and
    // several jackets use their "Cloth" group's (a -90 X group, with the mesh node undoing it:
    // nodeTransform == I, C == the group's frame - the jacket lay on the floor). So: when C is the
    // global frame of the mesh node or any node above it, and not the identity, it is folded back
    // out; every other file (C == I at bind, or a posed import's per-bone delta, which is no node's
    // frame) bakes as before.
    //
    // The correction goes on the offsets, not the bake: those offsets are stored once per bone
    // NAME for the whole model (first mesh wins, ExtractBoneWeights), and one file can mix both
    // kinds - the Vivian heads' eyes and teeth have C == I, the skin and lashes C == N - so a
    // per-mesh "skip the bake" would skin a later mesh with an earlier mesh's offsets. Folding
    // inverse(C) into this mesh's offsets instead makes C == I for it too, so every mesh bakes
    // nodeTransform and every bone's stored offset means the same thing.
    // SkinIgnoresMeshNode: the node's parent frame only (the root's up axis and pivot still apply).
    const glm::mat4 bake = skinned && m_D->Settings.SkinIgnoresMeshNode && node
                               ? nodeTransform * glm::inverse(AiToGlm(node->mTransformation))
                               : nodeTransform;
    glm::mat4 offsetFix(1.0f);
    if (skinned) {
        const aiBone* first = mesh->mBones[0];
        if (auto g = m_Import->NodeGlobals.find(first->mName.C_Str()); g != m_Import->NodeGlobals.end()) {
            const glm::mat4 c = m_D->GlobalInverseTransform * g->second * AiToGlm(first->mOffsetMatrix);
            if (!IsNearlyIdentity(c)) {
                bool nodeFrame = NearlyEqual(c, nodeTransform);
                for (const aiNode* a = node ? node->mParent : nullptr; a && !nodeFrame; a = a->mParent)
                    if (auto ag = m_Import->NodeGlobals.find(a->mName.C_Str()); ag != m_Import->NodeGlobals.end())
                        nodeFrame = NearlyEqual(c, ag->second);
                if (nodeFrame) offsetFix = glm::inverse(c);
            }
        }
    }
    glm::mat3 normalMatrix = NormalMatrix(bake);

    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        ModelVertex v;
        v.Position = AiToGlm(mesh->mVertices[i]);
        v.Normal = mesh->HasNormals() ? AiToGlm(mesh->mNormals[i]) : glm::vec3(0, 1, 0);
        if (mesh->mTextureCoords[0]) {
            v.UV = {mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y};
        }
        if (mesh->HasVertexColors(0)) { // #113
            const aiColor4D& c = mesh->mColors[0][i];
            v.Color = {c.r, c.g, c.b, c.a};
        }
        if (mesh->HasTangentsAndBitangents()) {
            v.Tangent = AiToGlm(mesh->mTangents[i]);
            glm::vec3 bitangent = AiToGlm(mesh->mBitangents[i]);
            // Assimp gives tangent and bitangent independently; on a mirrored UV island
            // (very common on symmetric characters/props) the "natural" cross(N,T) points
            // the wrong way. Recover the correct handedness so normal mapping isn't inverted there.
            v.TangentSign = (glm::dot(glm::cross(v.Normal, v.Tangent), bitangent) < 0.0f) ? -1.0f : 1.0f;
        } else {
            v.Tangent = glm::vec3(1, 0, 0);
            v.TangentSign = 1.0f;
        }

        v.Position = glm::vec3(bake * glm::vec4(v.Position, 1.0f));
        v.Normal = glm::normalize(normalMatrix * v.Normal);
        v.Tangent = glm::normalize(glm::mat3(bake) * v.Tangent);

        vertices[i] = v;

        if (!skinned) {
            m_D->BoundsMin = glm::min(m_D->BoundsMin, v.Position);
            m_D->BoundsMax = glm::max(m_D->BoundsMax, v.Position);
        }
    }

    std::vector<unsigned int> indices;
    indices.reserve(mesh->mNumFaces * 3);
    // #113 — a mirrored node transform (negative determinant, e.g. a -1 scale for the other
    // side of a symmetric prop) baked into the vertices flips every triangle's winding, so the
    // mesh renders inside-out under back-face culling. Swap two indices per triangle to undo it.
    const bool flipWinding = glm::determinant(glm::mat3(bake)) < 0.0f;
    for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
        const aiFace& face = mesh->mFaces[i];
        if (flipWinding && face.mNumIndices == 3) {
            indices.push_back(face.mIndices[0]);
            indices.push_back(face.mIndices[2]);
            indices.push_back(face.mIndices[1]);
            continue;
        }
        for (unsigned int j = 0; j < face.mNumIndices; ++j) {
            indices.push_back(face.mIndices[j]);
        }
    }

    if (m_D->Settings.ImportSkeleton) ExtractBoneWeights(vertices, mesh, offsetFix);
    SimplifyMesh(vertices, indices, m_D->Settings.SimplifyRatio);

    // #98 — bounds, picking, snapping and collider cooking are rebuilt from this CPU copy at
    // the bind pose (same blend as the vertex shader), so they track whatever `bake` folded
    // into the GPU vertices above and never drift out of step with what's on screen.
    std::vector<glm::vec3> posed;
    if (skinned) {
        posed.reserve(vertices.size());
        auto& boxMin = m_D->BoneBoxMin;
        auto& boxMax = m_D->BoneBoxMax;
        if (boxMin.empty()) { boxMin.assign(MAX_BONES, glm::vec3(1e30f)); boxMax.assign(MAX_BONES, glm::vec3(-1e30f)); }
        for (const ModelVertex& v : vertices) {
            glm::mat4 skin(0.0f);
            float total = 0.0f;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                if (v.BoneIDs[k] < 0) continue;
                const int b = std::clamp(v.BoneIDs[k], 0, MAX_BONES - 1);
                skin += m_D->BindPoseBones[b] * v.Weights[k];
                total += v.Weights[k];
                // PosedBounds: a skinned vertex is a weighted mean of its bones' moves, so it stays inside the
                // box of each bone's moved box.
                boxMin[(size_t)b] = glm::min(boxMin[(size_t)b], v.Position);
                boxMax[(size_t)b] = glm::max(boxMax[(size_t)b], v.Position);
            }
            if (total <= 0.0001f) {
                skin = glm::mat4(1.0f);
                m_D->UnweightedMin = glm::min(m_D->UnweightedMin, v.Position);
                m_D->UnweightedMax = glm::max(m_D->UnweightedMax, v.Position);
            }
            const glm::vec3 p = glm::vec3(skin * glm::vec4(v.Position, 1.0f));
            posed.push_back(p);
            m_D->BoundsMin = glm::min(m_D->BoundsMin, p);
            m_D->BoundsMax = glm::max(m_D->BoundsMax, p);
        }
    }
    // MergeByMaterial: a static mesh joins its material's bucket (in file order), made into one mesh after the walk.
    if (m_D->Settings.MergeByMaterial && !skinned) {
        auto it = std::find_if(m_Import->MergeBuckets.begin(), m_Import->MergeBuckets.end(),
                               [&](const MergeBucket& b) { return b.MaterialIndex == mesh->mMaterialIndex; });
        if (it == m_Import->MergeBuckets.end()) {
            m_Import->MergeBuckets.push_back({mesh->mMaterialIndex, false, {}, {}});
            it = m_Import->MergeBuckets.end() - 1;
        }
        const unsigned int base = (unsigned int)it->Vertices.size();
        it->VertexColors = it->VertexColors || mesh->HasVertexColors(0);
        it->Vertices.insert(it->Vertices.end(), vertices.begin(), vertices.end());
        it->Indices.reserve(it->Indices.size() + indices.size());
        for (unsigned int i : indices) it->Indices.push_back(base + i);
        return nullptr;
    }
    // Deferred (ImportDeferred on a worker): keep the vertices for FinishGpuUploadStep, no GL here.
    auto gpuMesh = m_DeferGpu ? std::make_unique<ModelMesh>(std::move(vertices), indices, true)
                              : std::make_unique<ModelMesh>(vertices, indices);
    if (skinned) gpuMesh->SetLocalPositions(std::move(posed));
    AssignImportedMaterial(*gpuMesh, scene, mesh->mMaterialIndex, mesh->HasVertexColors(0));
    return gpuMesh;
}

void Model::FlushMergeBuckets(const aiScene* scene) {
    for (MergeBucket& b : m_Import->MergeBuckets) {
        auto gpuMesh = m_DeferGpu ? std::make_unique<ModelMesh>(std::move(b.Vertices), b.Indices, true)
                                  : std::make_unique<ModelMesh>(b.Vertices, b.Indices);
        AssignImportedMaterial(*gpuMesh, scene, b.MaterialIndex, b.VertexColors);
        m_D->Meshes.push_back(std::move(gpuMesh));
    }
    m_Import->MergeBuckets.clear();
}

void Model::AssignImportedMaterial(ModelMesh& gpuMesh, const aiScene* scene, unsigned int materialIndex, bool vertexColors) {
    if (m_D->Settings.MaterialImportMode == ModelImportSettings::MaterialMode::ImportEmbedded) {
        if (materialIndex < scene->mNumMaterials) {
            gpuMesh.Mat = ExtractMaterial(scene, materialIndex);
            // #113 — a mesh that ships vertex colours gets them (glTF COLOR_0 always tints).
            if (vertexColors) gpuMesh.Mat.UseVertexColor = true;
        }
    } else if (m_D->Settings.MaterialImportMode == ModelImportSettings::MaterialMode::CreateSynthetic) {
        // Ignore the file's own materials/textures entirely - same neutral look CreatePrimitive
        // assigns, left for the Inspector's PBR Material section to author from scratch.
        gpuMesh.Mat.BaseColor = glm::vec3(0.75f);
    }
    // MaterialMode::None: leave gpuMesh.Mat at Material{}'s bare defaults, untouched.
    // Every mode keeps the file's material name: the model's materialRemap matches on it, so a model whose .mat files
    // replace its own materials can skip loading the file's textures.
    if (m_D->Settings.MaterialImportMode != ModelImportSettings::MaterialMode::ImportEmbedded &&
        materialIndex < scene->mNumMaterials)
        gpuMesh.Mat.Name = scene->mMaterials[materialIndex]->GetName().C_Str();
}

std::vector<std::pair<std::string, std::string>> Model::SourceDependencies(const std::string& modelPath) {
    namespace fs = std::filesystem;
    std::vector<std::pair<std::string, std::string>> out;
    std::error_code ec;
    const fs::path model = fs::absolute(modelPath, ec).lexically_normal();
    const fs::path dir = model.parent_path();
    std::set<std::string> seen;
    auto add = [&](const fs::path& file) {
        const fs::path f = fs::absolute(file, ec).lexically_normal();
        if (ec || f == model || !fs::is_regular_file(f, ec)) return;
        if (!seen.insert(f.generic_string()).second) return;
        auto under = [&](const fs::path& base, fs::path& rel) {
            rel = f.lexically_relative(base);
            return !base.empty() && !rel.empty() && !rel.is_absolute() && *rel.begin() != fs::path("..");
        };
        // Beside/below the model: keep the layout. Found through the folder above (a pack's
        // Models/ + Textures/ siblings): keep the layout relative to that folder, so the copy
        // lands in a subfolder of the model's new folder, where the resolver finds it again and
        // two same-named maps from different subfolders can't overwrite each other. Anything
        // else goes beside the model by name.
        fs::path rel;
        if (!under(dir, rel) && !under(dir.parent_path(), rel)) rel = f.filename();
        out.emplace_back(f.string(), rel.generic_string());
    };
    auto uriDecode = [](const std::string& u) {
        std::string r;
        for (size_t i = 0; i < u.size(); ++i) {
            if (u[i] == '%' && i + 2 < u.size() && std::isxdigit((unsigned char)u[i + 1]) && std::isxdigit((unsigned char)u[i + 2])) {
                r += (char)std::stoi(u.substr(i + 1, 2), nullptr, 16);
                i += 2;
            } else r += u[i];
        }
        return r;
    };

    std::string ext = model.extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);

    if (ext == ".gltf") {
        // Buffers (.bin) and images: without the buffers Assimp can't read the file at all.
        std::ifstream in(model);
        try {
            const nlohmann::json j = nlohmann::json::parse(in);
            for (const char* key : {"buffers", "images"}) {
                if (!j.contains(key) || !j[key].is_array()) continue;
                for (const auto& b : j[key]) {
                    if (!b.is_object() || !b.contains("uri") || !b["uri"].is_string()) continue;
                    const std::string uri = b["uri"].get<std::string>();
                    if (uri.rfind("data:", 0) == 0) continue; // embedded base64
                    add(dir / fs::path(uriDecode(uri)));
                }
            }
        } catch (const std::exception&) {}
    } else if (ext == ".obj") {
        // mtllib lines; the .mtl's own map_* entries come back through Assimp's materials below.
        std::ifstream in(model);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("mtllib", 0) != 0) continue;
            std::string name = line.substr(6);
            name.erase(0, name.find_first_not_of(" \t"));
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ' || name.back() == '\t')) name.pop_back();
            if (!name.empty()) add(dir / fs::path(name));
        }
    }

    // Every external texture any material references (FBX / OBJ / glTF alike).
    Assimp::Importer importer;
    if (const aiScene* scene = importer.ReadFile(model.string(), 0)) {
        for (unsigned int m = 0; m < scene->mNumMaterials; ++m) {
            const aiMaterial* mat = scene->mMaterials[m];
            for (int t = aiTextureType_NONE; t <= AI_TEXTURE_TYPE_MAX; ++t) {
                const aiTextureType type = (aiTextureType)t;
                for (unsigned int i = 0; i < mat->GetTextureCount(type); ++i) {
                    aiString str;
                    if (mat->GetTexture(type, i, &str) != AI_SUCCESS || str.length == 0) continue;
                    if (str.C_Str()[0] == '*' || scene->GetEmbeddedTexture(str.C_Str())) continue;
                    const std::string resolved = ResolveTexturePathIn(dir.string(), str.C_Str());
                    if (!resolved.empty()) add(fs::path(resolved));
                }
            }
            // The maps an import fills from the material's texture set (see ExtractMaterial).
            if (ext != ".gltf" && ext != ".glb") {
                const std::vector<std::string> setNames{mat->GetName().C_Str(), model.stem().string()};
                for (int k = 0; k <= (int)TextureSetMap::Emissive; ++k) {
                    const std::string found = FindTextureSetMap(dir.string(), setNames, (TextureSetMap)k);
                    if (!found.empty()) add(fs::path(found));
                }
            }
        }
    }
    return out;
}

namespace {
bool IsGltfPath(const std::string& p) {
    std::string ext = std::filesystem::path(p).extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".gltf" || ext == ".glb";
}
} // namespace

Material Model::ExtractMaterial(const aiScene* scene, unsigned int materialIndex) {
    Material mat;
    aiMaterial* material = scene->mMaterials[materialIndex];
    aiString str_unused;
    mat.Name = material->GetName().C_Str();

    // Textures the file names that aren't on disk: {what it names, the path it resolved to}.
    // Reported after the texture-set fallback below, and only if that couldn't fill the slot.
    std::vector<std::pair<std::string, std::string>> missing;
    const std::shared_ptr<Texture>* slotTarget = nullptr;
    std::vector<const std::shared_ptr<Texture>*> missingSlot;
    auto loadSlot = [&](aiTextureType type, TextureRole role) -> std::shared_ptr<Texture> {
        if (material->GetTextureCount(type) == 0) return nullptr;
        aiString str;
        material->GetTexture(type, 0, &str);
        if (str.length == 0) return nullptr;

        // #113 — embedded media (.glb, FBX with embedded textures): "*N" or a name matching an
        // aiTexture's filename. Decoded from memory instead of leaving the slot blank.
        if (const aiTexture* emb = scene->GetEmbeddedTexture(str.C_Str()))
            return LoadEmbeddedTexture(emb, str.C_Str(), role);
        std::string resolved = ResolveTexturePath(str.C_Str());
        if (!resolved.empty() && resolved[0] == '*') {
            Log::Warn("Model: '" + m_Path + "' references embedded texture " + resolved +
                      " which the file doesn't contain - that map slot will be blank.", LogContext::Asset(m_Path));
            return nullptr;
        }
        std::error_code ec;
        if (!std::filesystem::exists(resolved, ec)) {
            missing.emplace_back(str.C_Str(), resolved);
            missingSlot.push_back(slotTarget);
            return nullptr;
        }
        return LoadCachedTexture(resolved, role);
    };

    slotTarget = &mat.AlbedoMap;
    mat.AlbedoMap = loadSlot(aiTextureType_DIFFUSE, TextureRole::Color);
    if (!mat.AlbedoMap) mat.AlbedoMap = loadSlot(aiTextureType_BASE_COLOR, TextureRole::Color); // glTF2 alt slot
    slotTarget = &mat.NormalMap;
    mat.NormalMap = loadSlot(aiTextureType_NORMALS, TextureRole::Normal);
    // assimp puts glTF2's packed metal-rough map in UNKNOWN (and, in newer versions, also in
    // GLTF_METALLIC_ROUGHNESS). FBX uses UNKNOWN for arbitrary unmapped slots, which must not be
    // read as metal-rough (#113) — only trust it for glTF materials.
    const bool isGltf = material->Get(AI_MATKEY_GLTF_ALPHAMODE, str_unused) == AI_SUCCESS ||
                        IsGltfPath(m_Path);
    slotTarget = &mat.MetallicRoughnessMap;
    if (isGltf) mat.MetallicRoughnessMap = loadSlot(aiTextureType_UNKNOWN, TextureRole::Data);
    // Standalone maps — NOT the packed slot above, which is a different (G=rough, B=metal)
    // texture layout that a plain grayscale roughness/metalness map would be misread against.
    slotTarget = &mat.RoughnessMap;
    mat.RoughnessMap = loadSlot(aiTextureType_DIFFUSE_ROUGHNESS, TextureRole::Data);
    slotTarget = &mat.MetallicMap;
    mat.MetallicMap = loadSlot(aiTextureType_METALNESS, TextureRole::Data);
    // #113 — the real AO slot first; glTF occlusion arrives as LIGHTMAP in assimp, so that stays
    // a fallback (a true FBX lightmap is baked lighting, not occlusion, but is rarely shipped).
    slotTarget = &mat.AOMap;
    mat.AOMap = loadSlot(aiTextureType_AMBIENT_OCCLUSION, TextureRole::Data);
    if (!mat.AOMap) mat.AOMap = loadSlot(aiTextureType_LIGHTMAP, TextureRole::Data);
    slotTarget = &mat.EmissiveMap;
    mat.EmissiveMap = loadSlot(aiTextureType_EMISSIVE, TextureRole::Color);
    slotTarget = nullptr;

    // Slots still empty are filled from the texture set named after the material (or, failing
    // that, the model): a pack whose textures were renamed to <Set>_<MapType> after export
    // otherwise imports untextured. Also fills maps the file never named but the pack ships
    // (roughness / metallic / AO beside an FBX that only knew its diffuse). Not for glTF, whose
    // materials are explicit and whose packed metal-rough map these would conflict with.
    int fromSet = 0;
    if (!isGltf) {
        const std::vector<std::string> setNames{mat.Name, std::filesystem::path(m_Path).stem().string()};
        auto fill = [&](std::shared_ptr<Texture>& slot, TextureSetMap kind, TextureRole role) {
            if (slot) return;
            const std::string found = FindTextureSetMap(m_D->Directory, setNames, kind);
            if (found.empty()) return;
            slot = LoadCachedTexture(found, role);
            if (slot) ++fromSet;
        };
        fill(mat.AlbedoMap, TextureSetMap::Albedo, TextureRole::Color);
        fill(mat.NormalMap, TextureSetMap::Normal, TextureRole::Normal);
        // Factors multiply their maps (#102). The file's own factor was chosen with no map in
        // mind (an FBX's roughness comes from its shininess), so a map found here shows as-is.
        const bool hadMetallicMap = (bool)mat.MetallicMap, hadRoughnessMap = (bool)mat.RoughnessMap;
        fill(mat.MetallicMap, TextureSetMap::Metallic, TextureRole::Data);
        fill(mat.RoughnessMap, TextureSetMap::Roughness, TextureRole::Data);
        if (!hadMetallicMap && mat.MetallicMap) mat.Metallic = 1.0f;
        if (!hadRoughnessMap && mat.RoughnessMap) mat.Roughness = 1.0f;
        fill(mat.AOMap, TextureSetMap::Occlusion, TextureRole::Data);
        fill(mat.EmissiveMap, TextureSetMap::Emissive, TextureRole::Color);
    }

    // A texture that isn't on disk is an asset-pack problem, not a load failure: say so once
    // per texture, as a warning, instead of an error from every model and every reload that
    // references it (one animation pack's FBX files used to log the same two lines ~20 times
    // per launch). Nothing to say when the texture-set fallback filled the slot anyway.
    {
        static std::mutex s_ReportedMutex;
        static std::set<std::string> s_Reported;
        std::lock_guard<std::mutex> lock(s_ReportedMutex);
        for (size_t i = 0; i < missing.size(); ++i) {
            if (missingSlot[i] && *missingSlot[i]) continue;
            if (!s_Reported.insert(missing[i].second).second) continue;
            Log::Warn("Model: '" + m_Path + "' uses texture '" + missing[i].first + "', which isn't in the "
                      "model's folder, the folder above it, or their subfolders - that map slot will be "
                      "blank. Put the texture beside the model or assign a material.", LogContext::Asset(m_Path));
        }
        // Once per folder and material: an animation pack's clips all share one material.
        if (fromSet > 0 && s_Reported.insert(m_D->Directory + "|" + mat.Name + "|set").second)
            Log::Info("Model: material '" + mat.Name + "' in '" + std::filesystem::path(m_Path).filename().string() +
                      "' - " + std::to_string(fromSet) + " texture map(s) matched by name from its texture set.",
                      LogContext::Asset(m_Path));
    }

    aiColor4D color;
    if (material->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
        mat.BaseColor = {color.r, color.g, color.b};
    }
    if (material->Get(AI_MATKEY_COLOR_EMISSIVE, color) == AI_SUCCESS) {
        mat.EmissiveColor = {color.r, color.g, color.b};
    }
    // #102 — the emissive map is now tinted by EmissiveColor; files that ship an emissive map
    // with no (or a black) emissive factor mean "the map as-is".
    if (mat.EmissiveMap && mat.EmissiveColor == glm::vec3(0.0f)) mat.EmissiveColor = glm::vec3(1.0f);
    // #101 — alpha cutout. glTF says so explicitly (alphaMode MASK + alphaCutoff); formats with
    // no alpha mode (FBX/OBJ) keep the old behaviour of treating an albedo map that carries an
    // alpha channel as cutout, which is what foliage/fence assets in those formats rely on.
    aiString alphaMode;
    if (material->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS) {
        mat.AlphaClip = std::string(alphaMode.C_Str()) == "MASK";
        float cutoff = 0.5f;
        if (material->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff) == AI_SUCCESS) mat.AlphaCutoff = cutoff;
    } else if (mat.AlbedoMap && (mat.AlbedoMap->SourceChannels() == 4 || mat.AlbedoMap->SourceChannels() == 2)) {
        mat.AlphaClip = true;
    }

    // #102 — factors now scale their maps, so a file that has a map but no factor (FBX, OBJ)
    // means factor 1 (the glTF default), not the engine's scalar-only defaults of 0 / 0.5.
    // #113 — double-sided materials (glTF doubleSided, FBX/OBJ two-sided flag).
    int twoSided = 0;
    if (material->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS && twoSided) mat.DoubleSided = true;

    // #113 - KHR_texture_transform (glTF). Assimp converts the extension into an aiUVTransform
    // already expressed in ITS uv space (it flips V on the mesh at import and rotates about the
    // image centre), and for an unrotated transform that space is exactly what the shader does:
    // `uv * uUVTiling + uUVOffset`. So scale/translation map straight across.
    //
    // The engine's UV transform is per-material and applies to every map, while the glTF extension
    // is per-texture-slot. In practice an asset transforms all of its slots the same way, so the
    // base-colour slot is read and any slot that disagrees is reported rather than silently
    // dropped. Rotation has no material field to land in and is reported the same way.
    {
        const std::pair<aiTextureType, const char*> slots[] = {
            {aiTextureType_BASE_COLOR, "base colour"}, {aiTextureType_DIFFUSE, "diffuse"},
            {aiTextureType_NORMALS, "normal"},         {aiTextureType_EMISSIVE, "emissive"},
            {aiTextureType_METALNESS, "metallic"},     {aiTextureType_DIFFUSE_ROUGHNESS, "roughness"},
            {aiTextureType_AMBIENT_OCCLUSION, "occlusion"},
        };
        bool have = false, rotated = false, disagreed = false;
        aiUVTransform chosen;
        for (const auto& [type, label] : slots) {
            aiUVTransform x;
            if (material->Get(AI_MATKEY_UVTRANSFORM(type, 0), x) != AI_SUCCESS) continue;
            if (x.mRotation != 0.0f) rotated = true;
            if (!have) { chosen = x; have = true; continue; }
            if (x.mScaling.x != chosen.mScaling.x || x.mScaling.y != chosen.mScaling.y ||
                x.mTranslation.x != chosen.mTranslation.x || x.mTranslation.y != chosen.mTranslation.y)
                disagreed = true;
        }
        if (have) {
            // assimp's translation carries a V compensation for ITS glTF importer flipping V on
            // the mesh. This engine also passes aiProcess_FlipUVs, which flips it back, so the
            // imported UVs come out exactly as authored (verified: the fixture quad's UVs import
            // as (0,0),(1,0),(1,1),(0,1), its authored values). That compensation therefore has
            // to be undone, or every transformed texture is offset in V by scale-1.
            //   offset.y = t_y - (s_y - 1)   ->  identity (t=0,s=1) stays 0,
            //                                    scale 3 / offset 0.5 -> 2.5 - 3 + 1 = 0.5.
            mat.UVTiling = {chosen.mScaling.x, chosen.mScaling.y};
            mat.UVOffset = {chosen.mTranslation.x, chosen.mTranslation.y - chosen.mScaling.y + 1.0f};
            if (rotated)
                Log::Warn("Model: '" + m_Path + "' uses KHR_texture_transform with a rotation, which "
                          "this material has no field for - the offset and scale were applied, the "
                          "rotation was not.");
            if (disagreed)
                Log::Warn("Model: '" + m_Path + "' gives different texture slots different "
                          "KHR_texture_transform values; the engine's UV tiling/offset is per-material, "
                          "so the base-colour slot's transform was applied to every map.");
        }
    }

    float scalar;
    const bool hasMetalFactor = material->Get(AI_MATKEY_METALLIC_FACTOR, scalar) == AI_SUCCESS;
    if (hasMetalFactor) mat.Metallic = scalar;
    else if (mat.MetallicMap || mat.MetallicRoughnessMap) mat.Metallic = 1.0f;
    const bool hasRoughFactor = material->Get(AI_MATKEY_ROUGHNESS_FACTOR, scalar) == AI_SUCCESS;
    if (hasRoughFactor) mat.Roughness = scalar;
    else if (mat.RoughnessMap || mat.MetallicRoughnessMap) mat.Roughness = 1.0f;

    return mat;
}

void Model::ExtractBoneWeights(std::vector<ModelVertex>& vertices, aiMesh* mesh, const glm::mat4& offsetFix) {
    bool overflowWarned = false;
    // Every influence each vertex has, before the cap (the skin stats).
    std::vector<unsigned char> count(vertices.size(), 0);
    std::vector<float> total(vertices.size(), 0.0f), dropped(vertices.size(), 0.0f);
    for (unsigned int boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
        aiBone* bone = mesh->mBones[boneIdx];
        std::string boneName = bone->mName.C_Str();

        int boneID;
        auto it = m_D->BoneInfoMap.find(boneName);
        if (it == m_D->BoneInfoMap.end()) {
            // uBones[] is a fixed mat4[MAX_BONES] in the shader; a rig with more unique bones
            // would index it out of bounds (undefined in GLSL, TDR/black on many drivers).
            // Drop the extra bone's influences rather than let that reach the GPU (#98).
            if (m_D->BoneCounter >= MAX_BONES) {
                if (!overflowWarned) {
                    Log::Warn("Model '" + m_Path + "' has more than " + std::to_string(MAX_BONES) +
                              " bones - influences past that are dropped (skinning will be wrong).", LogContext::Asset(m_Path));
                    overflowWarned = true;
                }
                continue;
            }
            BoneInfo info{m_D->BoneCounter, AiToGlm(bone->mOffsetMatrix) * offsetFix};
            m_D->BoneInfoMap[boneName] = info;
            if (auto g = m_Import->NodeGlobals.find(boneName); g != m_Import->NodeGlobals.end())
                m_D->BindPoseBones[info.ID] = m_D->GlobalInverseTransform * g->second * info.OffsetMatrix; // #98
            boneID = m_D->BoneCounter++;
        } else {
            boneID = it->second.ID;
        }

        for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
            unsigned int vertexId = bone->mWeights[w].mVertexId;
            float weight = bone->mWeights[w].mWeight;
            if (vertexId >= vertices.size()) continue;

            ModelVertex& v = vertices[vertexId];
            if (count[vertexId] < 255) ++count[vertexId];
            total[vertexId] += weight;
            int slot = 0;
            for (; slot < MAX_BONE_INFLUENCE; ++slot)
                if (v.BoneIDs[slot] < 0) break;
            if (slot == MAX_BONE_INFLUENCE) {
                // Already at capacity - a 5th+ influence (common right where a twist bone blends
                // in) must replace the current SMALLEST kept weight if it outweighs it, not get
                // silently dropped. Dropping arbitrarily by arrival order can evict the vertex's
                // dominant bone entirely, skinning it from the wrong bones altogether.
                int smallest = 0;
                for (int s = 1; s < MAX_BONE_INFLUENCE; ++s)
                    if (v.Weights[s] < v.Weights[smallest]) smallest = s;
                if (weight <= v.Weights[smallest]) { dropped[vertexId] += weight; continue; }
                dropped[vertexId] += v.Weights[smallest];
                slot = smallest;
            }
            v.BoneIDs[slot] = boneID;
            v.Weights[slot] = weight;
        }
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
        if (!count[i]) continue;
        ++m_D->SkinVertices;
        m_D->SkinMaxInfluences = std::max(m_D->SkinMaxInfluences, (int)count[i]);
        if (count[i] > MAX_BONE_INFLUENCE) {
            ++m_D->SkinOverflow;
            if (total[i] > 1e-6f) m_D->SkinMaxDropped = std::max(m_D->SkinMaxDropped, dropped[i] / total[i]);
        }
    }

    // A vertex whose true influence count exceeds MAX_BONE_INFLUENCE only keeps its largest
    // weights above, leaving them summing below 1 - the vertex shader would skin it as a
    // shrunken blend toward the origin. Renormalizing keeps it a proper convex combination.
    for (ModelVertex& v : vertices) {
        float sum = 0.0f;
        for (int slot = 0; slot < MAX_BONE_INFLUENCE; ++slot)
            if (v.BoneIDs[slot] >= 0) sum += v.Weights[slot];
        if (sum > 0.0001f && std::fabs(sum - 1.0f) > 0.0001f) {
            for (int slot = 0; slot < MAX_BONE_INFLUENCE; ++slot)
                if (v.BoneIDs[slot] >= 0) v.Weights[slot] /= sum;
        }
    }
}

void Model::ReadHierarchy(const aiNode* node, int parent) {
    AnimNode n;
    n.Name = node->mName.C_Str();
    n.Parent = parent;
    n.BindLocal = AiToGlm(node->mTransformation);
    glm::vec3 skew; glm::vec4 persp;
    if (!glm::decompose(n.BindLocal, n.BindTRS.S, n.BindTRS.R, n.BindTRS.T, skew, persp)) n.BindTRS = LocalTRS{};
    n.BindTRS.R = glm::normalize(n.BindTRS.R);
    if (auto bone = m_D->BoneInfoMap.find(n.Name); bone != m_D->BoneInfoMap.end() && bone->second.ID < MAX_BONES) {
        n.BoneId = bone->second.ID;
        n.BoneOffset = bone->second.OffsetMatrix;
    }
    const int self = (int)m_D->Nodes.size();
    m_D->NodeLookup.emplace(n.Name, self);
    m_D->Nodes.push_back(std::move(n));
    for (unsigned int i = 0; i < node->mNumChildren; ++i) ReadHierarchy(node->mChildren[i], self);
}

void Model::ReadAnimations(const aiScene* scene) {
    for (unsigned int i = 0; i < scene->mNumAnimations; ++i) {
        aiAnimation* anim = scene->mAnimations[i];
        AnimationClip clip;
        clip.Name = anim->mName.length ? anim->mName.C_Str() : ("Animation_" + std::to_string(i));
        clip.DurationTicks = (float)anim->mDuration;
        clip.TicksPerSecond = anim->mTicksPerSecond != 0 ? (float)anim->mTicksPerSecond : 25.0f;
        for (const auto& trim : m_D->Settings.ClipTrims)
            if (trim.Clip == clip.Name) {
                clip.StartTicks = std::max(0.0f, trim.StartSeconds * clip.TicksPerSecond);
                clip.EndTicks = trim.EndSeconds > 0.0f ? trim.EndSeconds * clip.TicksPerSecond : 0.0f;
            }

        for (unsigned int c = 0; c < anim->mNumChannels; ++c) {
            aiNodeAnim* channel = anim->mChannels[c];
            BoneAnimChannel bac;
            bac.BoneName = channel->mNodeName.C_Str();

            for (unsigned int k = 0; k < channel->mNumPositionKeys; ++k) {
                bac.Positions.push_back({AiToGlm(channel->mPositionKeys[k].mValue), (float)channel->mPositionKeys[k].mTime});
            }
            for (unsigned int k = 0; k < channel->mNumRotationKeys; ++k) {
                bac.Rotations.push_back({AiToGlm(channel->mRotationKeys[k].mValue), (float)channel->mRotationKeys[k].mTime});
            }
            for (unsigned int k = 0; k < channel->mNumScalingKeys; ++k) {
                bac.Scales.push_back({AiToGlm(channel->mScalingKeys[k].mValue), (float)channel->mScalingKeys[k].mTime});
            }
            bac.Finalize();
            clip.Channels.push_back(std::move(bac));
        }
        // #113 — resolve node -> channel once here, not by name per node per frame.
        // (NodeLookup holds each name's first node, as the old nested scan matched.)
        clip.NodeChannel.assign(m_D->Nodes.size(), -1);
        for (int c = 0; c < (int)clip.Channels.size(); ++c)
            if (const auto it = m_D->NodeLookup.find(clip.Channels[c].BoneName); it != m_D->NodeLookup.end())
                clip.NodeChannel[it->second] = c;
        m_D->Animations.push_back(std::move(clip));
    }
}
