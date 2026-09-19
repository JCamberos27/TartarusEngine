#include "Model.h"
#include "MaterialAsset.h"
#include "ShaderAsset.h"
#include "Log.h"
#include "Texture.h"
#include "Shader.h"
#include "GLStateCache.h"
#include "DefaultTextures.h"
#include "PrimitiveMeshes.h"
#include "gl.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <assimp/GltfMaterial.h>

#include <iostream>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <json.hpp>
#include <cctype>
#include <fstream>
#include <set>

namespace {

glm::mat4 AiToGlm(const aiMatrix4x4& from) {
    glm::mat4 to;
    to[0][0] = from.a1; to[1][0] = from.a2; to[2][0] = from.a3; to[3][0] = from.a4;
    to[0][1] = from.b1; to[1][1] = from.b2; to[2][1] = from.b3; to[3][1] = from.b4;
    to[0][2] = from.c1; to[1][2] = from.c2; to[2][2] = from.c3; to[3][2] = from.c4;
    to[0][3] = from.d1; to[1][3] = from.d2; to[2][3] = from.d3; to[3][3] = from.d4;
    return to;
}

glm::vec3 AiToGlm(const aiVector3D& v) { return {v.x, v.y, v.z}; }
glm::quat AiToGlm(const aiQuaternion& q) { return {q.w, q.x, q.y, q.z}; }

std::string DirectoryOf(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

} // namespace

Model::Model(const std::string& path) : Model(path, ModelImportSettings{}) {}

Model::Model(const std::string& path, const ModelImportSettings& settings)
    : m_Path(path) {
    m_D->Directory = DirectoryOf(path);
    ImportFromFile(settings);
}

std::shared_ptr<Model> Model::CreateInstance() const {
    std::shared_ptr<Model> inst(new Model());
    inst->m_D = m_D;       // #96 — shared import: no Assimp, no new GPU buffers or textures
    inst->m_Path = m_Path;
    inst->m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
    return inst;
}

void Model::ImportFromFile(const ModelImportSettings& settings) {
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

    unsigned int flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale;
    if (settings.ImportNormals) flags |= aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;
    if (settings.ImportSkeleton) flags |= aiProcess_LimitBoneWeights;
    if (settings.OptimizeGraph) flags |= aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes;

    const aiScene* scene = importer.ReadFile(m_Path, flags);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
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
        return;
    }

    // #96 — import into a fresh SharedData, then move it into the object every instance of this
    // asset shares, so a Reimport reaches placed instances too and starts from a clean slate.
    const std::shared_ptr<SharedData> target = m_D;
    m_D = std::make_shared<SharedData>();
    m_D->Directory = target->Directory;
    m_D->Settings = settings;

    m_D->GlobalInverseTransform = glm::inverse(AiToGlm(scene->mRootNode->mTransformation));
    m_D->BindPoseBones.assign(MAX_BONES, glm::mat4(1.0f));
    m_ImportNodeGlobals.clear();
    CollectNodeGlobals(scene->mRootNode, glm::mat4(1.0f));

    ProcessNode(scene->mRootNode, scene, glm::mat4(1.0f));
    ReadHierarchy(m_D->RootNode, scene->mRootNode);
    if (settings.ImportAnimations) ReadAnimations(scene);
    m_ImportNodeGlobals.clear();

    *target = std::move(*m_D);
    m_D = target;

    m_CurrentAnimation = -1;
    m_CurrentTimeTicks = 0.0f;
    m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
}

void Model::CollectNodeGlobals(const aiNode* node, const glm::mat4& parentTransform) {
    const glm::mat4 global = parentTransform * AiToGlm(node->mTransformation);
    m_ImportNodeGlobals.emplace(node->mName.C_Str(), global);
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
    ImportFromFile(settings);
    return !m_D->Meshes.empty();
}

Model::~Model() = default;

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
                             float& outT, glm::vec3* outNormal, float minT) const {
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
    for (const auto& mesh : m_D->Meshes) {
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
            hit = true;
        }
    }
    if (!hit) return false;
    outT = bestWorldT;
    if (outNormal) {
        glm::vec3 n = glm::normalize(glm::transpose(glm::inverse(glm::mat3(modelMatrix))) * bestLocalN);
        if (glm::dot(n, wd) > 0.0f) n = -n; // face the ray origin
        *outNormal = n;
    }
    return true;
}

std::shared_ptr<Model> Model::CreatePrimitive(const std::string& kind, const std::string& path) {
    std::shared_ptr<Model> model(new Model());
    model->m_Path = path;
    model->m_D->GlobalInverseTransform = glm::mat4(1.0f);

    std::vector<ModelVertex> verts;
    std::vector<unsigned int> indices;
    if (kind == "sphere") PrimitiveMeshes::GenerateSphere(verts, indices);
    else if (kind == "cylinder") PrimitiveMeshes::GenerateCylinder(verts, indices);
    else if (kind == "cone") PrimitiveMeshes::GenerateCone(verts, indices);
    else if (kind == "plane") PrimitiveMeshes::GeneratePlane(verts, indices);
    else if (kind == "pyramid") PrimitiveMeshes::GeneratePyramid(verts, indices);
    else if (kind == "donut") PrimitiveMeshes::GenerateDonut(verts, indices);
    else if (kind == "capsule") PrimitiveMeshes::GenerateCapsule(verts, indices);
    else PrimitiveMeshes::GenerateCube(verts, indices); // default/"cube"

    for (const auto& v : verts) {
        model->m_D->BoundsMin = glm::min(model->m_D->BoundsMin, v.Position);
        model->m_D->BoundsMax = glm::max(model->m_D->BoundsMax, v.Position);
    }

    auto mesh = std::make_unique<ModelMesh>(verts, indices);
    mesh->Mat.BaseColor = glm::vec3(0.75f); // neutral default; override via the Inspector's PBR Material section
    model->m_D->Meshes.push_back(std::move(mesh));

    model->m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
    return model;
}

void Model::ProcessNode(aiNode* node, const aiScene* scene, const glm::mat4& parentTransform) {
    // Assimp's mesh vertex data is expressed in the local space of whatever node the mesh
    // is attached to, NOT world/model space — each node in the FBX/glTF hierarchy can carry
    // its own offset/rotation (e.g. a shotgun's barrel, stock, and trigger guard are commonly
    // separate nodes). Skipping this accumulation collapses every part to the model origin.
    glm::mat4 nodeTransform = parentTransform * AiToGlm(node->mTransformation);

    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        m_D->Meshes.push_back(ProcessMesh(mesh, scene, nodeTransform));
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        ProcessNode(node->mChildren[i], scene, nodeTransform);
    }
}

std::unique_ptr<ModelMesh> Model::ProcessMesh(aiMesh* mesh, const aiScene* scene, const glm::mat4& nodeTransform) {
    std::vector<ModelVertex> vertices(mesh->mNumVertices);

    // Skinned meshes are positioned entirely by their bone matrices (computed by walking
    // the full node hierarchy in CalculateBoneTransform), so baking the mesh's own node
    // transform into the raw vertex data here would double-apply it once skinning runs.
    // Gated on m_D->Settings.ImportSkeleton too: with skeleton import off, ExtractBoneWeights below
    // never runs, so treating this as "skinned" would leave every vertex's bone weights at their
    // default (unset) values instead of the identity-pose vertex position baked in here - the
    // mesh would render collapsed to the origin rather than as a static copy of its bind pose.
    bool skinned = m_D->Settings.ImportSkeleton && mesh->mNumBones > 0;
    glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(nodeTransform)));

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

        if (!skinned) {
            v.Position = glm::vec3(nodeTransform * glm::vec4(v.Position, 1.0f));
            v.Normal = glm::normalize(normalMatrix * v.Normal);
            v.Tangent = glm::normalize(glm::mat3(nodeTransform) * v.Tangent);
        }

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
    const bool flipWinding = !skinned && glm::determinant(glm::mat3(nodeTransform)) < 0.0f;
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

    if (m_D->Settings.ImportSkeleton) ExtractBoneWeights(vertices, mesh);

    auto gpuMesh = std::make_unique<ModelMesh>(vertices, indices);
    if (skinned) {
        // #98 — the GPU copy stays in mesh space for the skinning shader; bounds, picking,
        // snapping and collider cooking use the bind pose (same blend as the vertex shader).
        std::vector<glm::vec3> posed;
        posed.reserve(vertices.size());
        for (const ModelVertex& v : vertices) {
            glm::mat4 skin(0.0f);
            float total = 0.0f;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                if (v.BoneIDs[k] < 0) continue;
                skin += m_D->BindPoseBones[std::clamp(v.BoneIDs[k], 0, MAX_BONES - 1)] * v.Weights[k];
                total += v.Weights[k];
            }
            if (total <= 0.0001f) skin = glm::mat4(1.0f);
            const glm::vec3 p = glm::vec3(skin * glm::vec4(v.Position, 1.0f));
            posed.push_back(p);
            m_D->BoundsMin = glm::min(m_D->BoundsMin, p);
            m_D->BoundsMax = glm::max(m_D->BoundsMax, p);
        }
        gpuMesh->SetLocalPositions(std::move(posed));
    }
    if (m_D->Settings.MaterialImportMode == ModelImportSettings::MaterialMode::ImportEmbedded) {
        if (mesh->mMaterialIndex < scene->mNumMaterials) {
            gpuMesh->Mat = ExtractMaterial(scene, mesh->mMaterialIndex);
            // #113 — a mesh that ships vertex colours gets them (glTF COLOR_0 always tints).
            if (mesh->HasVertexColors(0)) gpuMesh->Mat.UseVertexColor = true;
        }
    } else if (m_D->Settings.MaterialImportMode == ModelImportSettings::MaterialMode::CreateSynthetic) {
        // Ignore the file's own materials/textures entirely - same neutral look CreatePrimitive
        // assigns, left for the Inspector's PBR Material section to author from scratch.
        gpuMesh->Mat.BaseColor = glm::vec3(0.75f);
    }
    // MaterialMode::None: leave gpuMesh->Mat at Material{}'s bare defaults, untouched.
    return gpuMesh;
}

std::shared_ptr<Texture> Model::LoadCachedTexture(const std::string& fullPath, TextureRole role) {
    // #95 — the role decides the colour space. Every map used to be loaded with the default
    // (sRGB) settings, so normal / metallic / roughness / AO maps were gamma-decoded on sample:
    // bent normals and wrong roughness on essentially every imported model. Only albedo and
    // emissive are colour data. Keyed by path + role, in case one file feeds both kinds of slot.
    const std::string key = fullPath + (role == TextureRole::Color ? "|srgb" : role == TextureRole::Normal ? "|normal" : "|linear");
    auto it = m_D->TextureCache.find(key);
    if (it != m_D->TextureCache.end()) return it->second;

    TextureImportSettings settings;
    settings.IsSRGB = role == TextureRole::Color;
    if (role == TextureRole::Normal) settings.TextureType = TextureImportSettings::Type::NormalMap;
    auto tex = std::make_shared<Texture>(fullPath, settings);
    if (!tex->IsValid()) return nullptr;
    m_D->TextureCache[key] = tex;
    return tex;
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
        const fs::path rel = f.lexically_relative(dir);
        const bool inside = !rel.empty() && !rel.is_absolute() && *rel.begin() != fs::path("..");
        out.emplace_back(f.string(), (inside ? rel : f.filename()).generic_string());
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
        }
    }
    return out;
}

std::string Model::ResolveTexturePath(const std::string& raw) const {
    return ResolveTexturePathIn(m_D->Directory, raw);
}

std::string Model::ResolveTexturePathIn(const std::string& modelDirStr, const std::string& raw) {
    namespace fs = std::filesystem;

    // Embedded texture ("*0", "*1", ...): the pixels live inside the model file, not on disk.
    // The Texture class is disk-only, so there's nothing to resolve here — hand the marker back
    // and let the caller log one clear line instead of a mangled path.
    if (!raw.empty() && raw[0] == '*') return raw;
    if (raw.empty()) return raw;

    std::error_code ec;

    // Unify separators so a Windows-authored "\" path (or a mixed "/"+"\" one) parses the same
    // way regardless of the platform doing the import.
    std::string norm = raw;
    std::replace(norm.begin(), norm.end(), '\\', '/');

    const fs::path p(norm);
    const fs::path modelDir(modelDirStr);

    // 1. Exactly as given, when it's an absolute path that actually exists on THIS machine.
    if (p.is_absolute()) {
        if (fs::exists(p, ec)) return p.lexically_normal().string();
    } else {
        // 2. Relative to the model's own directory (the common, correct case), ".." collapsed.
        const fs::path joined = (modelDir / p).lexically_normal();
        if (fs::exists(joined, ec)) return joined.string();
    }

    const fs::path filename = p.filename();
    if (!filename.empty()) {
        // 3. Just the filename, sitting next to the model. Covers texture folders that got
        //    flattened, and absolute paths from another machine that shipped a same-named file
        //    with the model (e.g. the Chesterfield Sofa pack, whose materials point at the
        //    original author's Dropbox).
        const fs::path byName = modelDir / filename;
        if (fs::exists(byName, ec)) return byName.string();

        // 4. The filename one level down, through the model directory's immediate subfolders
        //    ("textures/", "maps/", ...). Non-recursive and cheap; resolves most "textures are
        //    in a sibling subfolder the baked path didn't name" cases.
        if (fs::is_directory(modelDir, ec)) {
            for (fs::directory_iterator it(modelDir, ec), end; it != end && !ec; it.increment(ec)) {
                if (!it->is_directory(ec)) continue;
                const fs::path candidate = it->path() / filename;
                if (fs::exists(candidate, ec)) return candidate.string();
            }
        }
    }

    // Nothing matched — return a clean best-effort path so the load failure names something
    // sensible rather than "modelDir + someone-else's-absolute-path".
    if (p.is_absolute()) return p.lexically_normal().string();
    return (modelDir / p).lexically_normal().string();
}


namespace {
bool IsGltfPath(const std::string& p) {
    std::string ext = std::filesystem::path(p).extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".gltf" || ext == ".glb";
}
} // namespace

std::shared_ptr<Texture> Model::LoadEmbeddedTexture(const aiTexture* tex, const std::string& ref, TextureRole role) {
    const std::string key = "*embedded:" + ref + (role == TextureRole::Color ? "|srgb" : role == TextureRole::Normal ? "|normal" : "|linear");
    auto it = m_D->TextureCache.find(key);
    if (it != m_D->TextureCache.end()) return it->second;

    std::vector<unsigned char> bytes;
    int rawW = 0, rawH = 0;
    if (tex->mHeight == 0) {
        // Compressed (PNG/JPG/...): mWidth is the byte count.
        const auto* b = reinterpret_cast<const unsigned char*>(tex->pcData);
        bytes.assign(b, b + tex->mWidth);
    } else {
        // Raw aiTexel (BGRA) -> RGBA8.
        rawW = (int)tex->mWidth; rawH = (int)tex->mHeight;
        bytes.resize((size_t)rawW * rawH * 4);
        for (size_t i = 0; i < (size_t)rawW * rawH; ++i) {
            const aiTexel& t = tex->pcData[i];
            bytes[i * 4 + 0] = t.r; bytes[i * 4 + 1] = t.g; bytes[i * 4 + 2] = t.b; bytes[i * 4 + 3] = t.a;
        }
    }
    TextureImportSettings settings;
    settings.IsSRGB = role == TextureRole::Color;
    if (role == TextureRole::Normal) settings.TextureType = TextureImportSettings::Type::NormalMap;
    auto out = std::make_shared<Texture>(m_Path + "#" + ref, std::move(bytes), rawW, rawH, settings);
    if (!out->IsValid()) return nullptr;
    m_D->TextureCache[key] = out;
    return out;
}

Material Model::ExtractMaterial(const aiScene* scene, unsigned int materialIndex) {
    Material mat;
    aiMaterial* material = scene->mMaterials[materialIndex];
    aiString str_unused;

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
        return LoadCachedTexture(resolved, role);
    };

    mat.AlbedoMap = loadSlot(aiTextureType_DIFFUSE, TextureRole::Color);
    if (!mat.AlbedoMap) mat.AlbedoMap = loadSlot(aiTextureType_BASE_COLOR, TextureRole::Color); // glTF2 alt slot
    mat.NormalMap = loadSlot(aiTextureType_NORMALS, TextureRole::Normal);
    // assimp puts glTF2's packed metal-rough map in UNKNOWN (and, in newer versions, also in
    // GLTF_METALLIC_ROUGHNESS). FBX uses UNKNOWN for arbitrary unmapped slots, which must not be
    // read as metal-rough (#113) — only trust it for glTF materials.
    const bool isGltf = material->Get(AI_MATKEY_GLTF_ALPHAMODE, str_unused) == AI_SUCCESS ||
                        IsGltfPath(m_Path);
    if (isGltf) mat.MetallicRoughnessMap = loadSlot(aiTextureType_UNKNOWN, TextureRole::Data);
    // Standalone maps — NOT the packed slot above, which is a different (G=rough, B=metal)
    // texture layout that a plain grayscale roughness/metalness map would be misread against.
    mat.RoughnessMap = loadSlot(aiTextureType_DIFFUSE_ROUGHNESS, TextureRole::Data);
    mat.MetallicMap = loadSlot(aiTextureType_METALNESS, TextureRole::Data);
    // #113 — the real AO slot first; glTF occlusion arrives as LIGHTMAP in assimp, so that stays
    // a fallback (a true FBX lightmap is baked lighting, not occlusion, but is rarely shipped).
    mat.AOMap = loadSlot(aiTextureType_AMBIENT_OCCLUSION, TextureRole::Data);
    if (!mat.AOMap) mat.AOMap = loadSlot(aiTextureType_LIGHTMAP, TextureRole::Data);
    mat.EmissiveMap = loadSlot(aiTextureType_EMISSIVE, TextureRole::Color);

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

    float scalar;
    const bool hasMetalFactor = material->Get(AI_MATKEY_METALLIC_FACTOR, scalar) == AI_SUCCESS;
    if (hasMetalFactor) mat.Metallic = scalar;
    else if (mat.MetallicMap || mat.MetallicRoughnessMap) mat.Metallic = 1.0f;
    const bool hasRoughFactor = material->Get(AI_MATKEY_ROUGHNESS_FACTOR, scalar) == AI_SUCCESS;
    if (hasRoughFactor) mat.Roughness = scalar;
    else if (mat.RoughnessMap || mat.MetallicRoughnessMap) mat.Roughness = 1.0f;

    return mat;
}

void Model::ExtractBoneWeights(std::vector<ModelVertex>& vertices, aiMesh* mesh) {
    bool overflowWarned = false;
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
            BoneInfo info{m_D->BoneCounter, AiToGlm(bone->mOffsetMatrix)};
            m_D->BoneInfoMap[boneName] = info;
            if (auto g = m_ImportNodeGlobals.find(boneName); g != m_ImportNodeGlobals.end())
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
            for (int slot = 0; slot < MAX_BONE_INFLUENCE; ++slot) {
                if (v.BoneIDs[slot] < 0) {
                    v.BoneIDs[slot] = boneID;
                    v.Weights[slot] = weight;
                    break;
                }
            }
        }
    }
}

void Model::ReadHierarchy(AssimpNodeData& out, const aiNode* node) {
    out.Name = node->mName.C_Str();
    out.Transform = AiToGlm(node->mTransformation);
    out.Children.resize(node->mNumChildren);
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        ReadHierarchy(out.Children[i], node->mChildren[i]);
    }
}

void Model::ReadAnimations(const aiScene* scene) {
    for (unsigned int i = 0; i < scene->mNumAnimations; ++i) {
        aiAnimation* anim = scene->mAnimations[i];
        AnimationClip clip;
        clip.Name = anim->mName.length ? anim->mName.C_Str() : ("Animation_" + std::to_string(i));
        clip.DurationTicks = (float)anim->mDuration;
        clip.TicksPerSecond = anim->mTicksPerSecond != 0 ? (float)anim->mTicksPerSecond : 25.0f;

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
            clip.Channels[bac.BoneName] = bac;
        }
        m_D->Animations.push_back(std::move(clip));
    }
}

void Model::PlayAnimation(int index) {
    if (index < 0 || index >= (int)m_D->Animations.size()) {
        m_CurrentAnimation = -1;
        return;
    }
    m_CurrentAnimation = index;
    m_CurrentTimeTicks = 0.0f;
}

void Model::UpdateAnimation(float dt) {
    if (m_CurrentAnimation >= (int)m_D->Animations.size()) m_CurrentAnimation = -1; // #96 — reimported
    if (m_CurrentAnimation < 0) return;
    const AnimationClip& clip = m_D->Animations[m_CurrentAnimation];
    m_CurrentTimeTicks += dt * clip.TicksPerSecond;
    if (clip.DurationTicks > 0.0f) {
        m_CurrentTimeTicks = std::fmod(m_CurrentTimeTicks, clip.DurationTicks);
    }
    CalculateBoneTransform(m_D->RootNode, glm::mat4(1.0f));
}

void Model::CalculateBoneTransform(const AssimpNodeData& node, const glm::mat4& parentTransform) {
    glm::mat4 nodeTransform = node.Transform;

    if (m_CurrentAnimation >= 0) {
        const AnimationClip& clip = m_D->Animations[m_CurrentAnimation];
        auto it = clip.Channels.find(node.Name);
        if (it != clip.Channels.end()) {
            nodeTransform = it->second.Interpolate(m_CurrentTimeTicks);
        }
    }

    glm::mat4 globalTransform = parentTransform * nodeTransform;

    auto boneIt = m_D->BoneInfoMap.find(node.Name);
    if (boneIt != m_D->BoneInfoMap.end()) {
        int idx = boneIt->second.ID;
        if (idx < MAX_BONES) {
            m_FinalBoneMatrices[idx] = m_D->GlobalInverseTransform * globalTransform * boneIt->second.OffsetMatrix;
        }
    }

    for (const auto& child : node.Children) {
        CalculateBoneTransform(child, globalTransform);
    }
}

namespace {
// One shared bone-palette SSBO (binding 1), re-uploaded before each skinned draw. Replaces the
// 100-element glUniformMatrix4fv array (#104). Single-threaded, sequential draws, so one buffer
// is enough; process-lifetime, never freed (like the other engine-lifetime GL objects).
unsigned int g_BoneSsbo = 0;
void EnsureBoneSsbo() {
    if (g_BoneSsbo) return;
    glCreateBuffers(1, &g_BoneSsbo);
    glNamedBufferStorage(g_BoneSsbo, MAX_BONES * (GLsizeiptr)sizeof(glm::mat4), nullptr, GL_DYNAMIC_STORAGE_BIT);
}
} // namespace

void Model::UploadBoneMatrices(Shader& shader) const {
    // #98 — skin whenever there are bones: the clip's pose while playing, else the bind pose.
    const bool skinning = m_D->BoneCounter > 0;
    shader.SetInt("uUseSkinning", skinning ? 1 : 0);

    EnsureBoneSsbo();
    if (skinning) {
        const bool playing = m_CurrentAnimation >= 0 && m_CurrentAnimation < (int)m_D->Animations.size() &&
                             m_FinalBoneMatrices.size() >= (size_t)MAX_BONES;
        const std::vector<glm::mat4>& palette = playing ? m_FinalBoneMatrices : m_D->BindPoseBones;
        if (palette.size() >= (size_t)MAX_BONES)
            glNamedBufferSubData(g_BoneSsbo, 0, MAX_BONES * (GLsizeiptr)sizeof(glm::mat4), palette.data());
    }
    // Bind even when not skinning: the vertex shader still declares the block, and leaving
    // binding 1 dangling from a previous model is asking for trouble on stricter drivers.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g_BoneSsbo);
}

namespace {
// #194: per-material uniform locations for one Draw() call, resolved ONCE before the per-mesh
// loop (below) instead of re-hashing all these literal names for every mesh of every model.
struct MaterialLocs {
    int baseColor, metallic, roughness, emissiveColor, triplanar, triplanarScale;
    int hasAlbedo, albedoMap;
    int hasNormal, normalMap;
    int hasMetallicRoughness, metallicRoughnessMap;
    int hasMetallic, metallicMap;
    int hasRoughness, roughnessMap;
    int hasAO, aoMap;
    int hasEmissive, emissiveMap;
    int alphaClip, alphaCutoff; // #101
    int hasHeight, heightMap, hasDetailAlbedo, detailAlbedoMap, hasDetailNormal, detailNormalMap; // #102
};

// #102 / #113 — the surface-option uniforms, set for BOTH the built-in and the data-driven
// (Standard.shader) paths so a shader that declares them gets them either way.
void SetSurfaceOptions(Shader& shader, const Material& mat) {
    shader.SetVec2("uUVTiling", mat.UVTiling);
    shader.SetVec2("uUVOffset", mat.UVOffset);
    shader.SetFloat("uNormalStrength", mat.NormalStrength);
    shader.SetInt("uNormalFlipY", mat.NormalFlipY ? 1 : 0);
    shader.SetInt("uDoubleSided", mat.DoubleSided ? 1 : 0);
    shader.SetInt("uUseVertexColor", mat.UseVertexColor ? 1 : 0);
    shader.SetFloat("uParallaxScale", mat.ParallaxScale);
    shader.SetVec2("uDetailTiling", mat.DetailTiling);
}

MaterialLocs ResolveMaterialLocs(Shader& shader) {
    MaterialLocs L;
    L.baseColor = shader.Loc("uBaseColor");
    L.alphaClip = shader.Loc("uAlphaClip");
    L.alphaCutoff = shader.Loc("uAlphaCutoff");
    L.metallic = shader.Loc("uMetallic");
    L.roughness = shader.Loc("uRoughness");
    L.emissiveColor = shader.Loc("uEmissiveColor");
    L.triplanar = shader.Loc("uTriplanar");
    L.triplanarScale = shader.Loc("uTriplanarScale");
    L.hasAlbedo = shader.Loc("uHasAlbedoMap");
    L.albedoMap = shader.Loc("uAlbedoMap");
    L.hasNormal = shader.Loc("uHasNormalMap");
    L.normalMap = shader.Loc("uNormalMap");
    L.hasMetallicRoughness = shader.Loc("uHasMetallicRoughnessMap");
    L.metallicRoughnessMap = shader.Loc("uMetallicRoughnessMap");
    L.hasMetallic = shader.Loc("uHasMetallicMap");
    L.metallicMap = shader.Loc("uMetallicMap");
    L.hasRoughness = shader.Loc("uHasRoughnessMap");
    L.roughnessMap = shader.Loc("uRoughnessMap");
    L.hasAO = shader.Loc("uHasAOMap");
    L.aoMap = shader.Loc("uAOMap");
    L.hasEmissive = shader.Loc("uHasEmissiveMap");
    L.emissiveMap = shader.Loc("uEmissiveMap");
    L.hasHeight = shader.Loc("uHasHeightMap");
    L.heightMap = shader.Loc("uHeightMap");
    L.hasDetailAlbedo = shader.Loc("uHasDetailAlbedoMap");
    L.detailAlbedoMap = shader.Loc("uDetailAlbedoMap");
    L.hasDetailNormal = shader.Loc("uHasDetailNormalMap");
    L.detailNormalMap = shader.Loc("uDetailNormalMap");
    return L;
}

void BindMaterial(Shader& shader, const Material& mat, const MaterialLocs& locs) {
    // #192: ~12 uniform uploads + up to 7 texture binds below, almost all redundant when the
    // previous mesh drew with this same material under this same program. The draw loop sorts
    // by material so consecutive meshes usually match; GLStateCache clears this on Invalidate()
    // (end of frame + after every raw-GL pass), so an edited material can't be wrongly skipped.
    if (GLStateCache::MaterialAlreadyBound(mat.Hash(), shader.Program())) return;

    shader.SetVec3(locs.baseColor, mat.BaseColor);
    shader.SetFloat(locs.metallic, mat.Metallic);
    shader.SetFloat(locs.roughness, mat.Roughness);
    shader.SetVec3(locs.emissiveColor, mat.EmissiveColor * mat.EmissiveStrength);
    shader.SetInt(locs.triplanar, mat.Triplanar ? 1 : 0);
    shader.SetFloat(locs.triplanarScale, mat.TriplanarScale);
    shader.SetInt(locs.alphaClip, mat.AlphaClip ? 1 : 0);       // #101
    shader.SetFloat(locs.alphaCutoff, mat.AlphaCutoff);

    // Each map gets a FIXED unit (1..7). An absent map still binds a 1x1 default there, so the
    // driver never sees texture 0 on a sampler unit the program declares (audit GL-101 / #366:
    // was ~7 KHR 131204 warnings per draw). uHas*Map still tells the shader whether to use it.
    auto bindSlot = [&](int unit, const std::shared_ptr<Texture>& tex, int hasLoc, int samplerLoc,
                        unsigned int fallback) {
        // A Texture whose file failed to decode is still a live non-null object with m_ID == 0
        // (Texture.cpp logs and returns from the ctor). Treat it as an empty slot: bind the
        // fallback and leave uHas*Map = 0 so the shader never samples a 0-texture as a real map.
        if (tex && tex->IsValid()) {
            tex->Bind(unit);
            shader.SetInt(hasLoc, 1);
        } else {
            GLStateCache::BindTexture2D(unit, fallback);
            shader.SetInt(hasLoc, 0);
        }
        shader.SetInt(samplerLoc, unit);
    };

    bindSlot(1, mat.AlbedoMap, locs.hasAlbedo, locs.albedoMap, DefaultTextures::White());
    bindSlot(2, mat.NormalMap, locs.hasNormal, locs.normalMap, DefaultTextures::FlatNormal());
    bindSlot(3, mat.MetallicRoughnessMap, locs.hasMetallicRoughness, locs.metallicRoughnessMap, DefaultTextures::White());
    bindSlot(4, mat.MetallicMap, locs.hasMetallic, locs.metallicMap, DefaultTextures::White());
    bindSlot(5, mat.RoughnessMap, locs.hasRoughness, locs.roughnessMap, DefaultTextures::White());
    bindSlot(6, mat.AOMap, locs.hasAO, locs.aoMap, DefaultTextures::White());
    bindSlot(7, mat.EmissiveMap, locs.hasEmissive, locs.emissiveMap, DefaultTextures::Black());
    // #102 — units 16+ (8..15 are the engine's shadow / IBL / SSAO units).
    bindSlot(16, mat.HeightMap, locs.hasHeight, locs.heightMap, DefaultTextures::White());
    bindSlot(17, mat.DetailAlbedoMap, locs.hasDetailAlbedo, locs.detailAlbedoMap, DefaultTextures::White());
    bindSlot(18, mat.DetailNormalMap, locs.hasDetailNormal, locs.detailNormalMap, DefaultTextures::FlatNormal());
    SetSurfaceOptions(shader, mat);
}
// Data-driven BindMaterial using ShaderAsset::Bindings() + MaterialAsset property accessors.
// Activates when the MaterialAsset has a linked ShaderAsset (v2 .mat files referencing a .shader).
// Built-in PBR properties read from `ma.Mat`; every other declared property reads from
// `ma.Mat.ExtraProps` (typed store filled by MaterialAsset::Load, #354).
void BindMaterialDataDriven(Shader& shader, const MaterialAsset& ma, const ShaderAsset& sa) {
    const Material& mat = ma.Mat;
    // #99 — the redundant-bind skip must also see custom (non-builtin) shader properties;
    // Material::Hash() only covers the built-in fields, so two materials differing only in an
    // ExtraProp used to render with whichever was bound first.
    size_t hash = mat.Hash();
    auto mix = [&hash](size_t v) { hash ^= v + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2); };
    for (const auto& [name, p] : mat.ExtraProps) {
        mix(std::hash<std::string>{}(name));
        mix(std::hash<float>{}(p.F));
        for (int c = 0; c < 4; ++c) mix(std::hash<float>{}(p.V[c]));
        mix((size_t)p.B); mix((size_t)p.I);
        mix(std::hash<const void*>{}(p.Tex.get()));
    }
    mix((size_t)ma.RenderQueue);
    if (GLStateCache::MaterialAlreadyBound(hash, shader.Program())) return;
    // #101 — cutout for the AlphaTest queue (Standard.shader includes ModelFragment's uAlphaClip).
    shader.SetInt("uAlphaClip", (mat.AlphaClip || ma.RenderQueue == MaterialAsset::Queue::AlphaTest) ? 1 : 0);
    SetSurfaceOptions(shader, mat); // #102
    shader.SetFloat("uAlphaCutoff", mat.AlphaCutoff);

    const auto& props    = sa.Properties();
    const auto& bindings = sa.Bindings();
    for (const PropertyBinding& b : bindings) {
        const ShaderProperty& prop = props[b.PropIndex];
        const std::string& pname   = prop.Name;
        // Derive uniform name: "_AlbedoMap" → "uAlbedoMap"
        std::string uname = "u" + pname.substr(1);
        const bool builtin = MaterialAsset::IsBuiltinProp(pname);
        const MaterialProp* extra = nullptr;
        if (!builtin) {
            auto it = mat.ExtraProps.find(pname);
            if (it != mat.ExtraProps.end()) extra = &it->second;
        }

        switch (prop.Type) {
        case ShaderPropType::Texture2D: {
            std::string hasName = "uHas" + pname.substr(1);
            const std::shared_ptr<Texture>& tex =
                builtin ? MaterialAsset::GetTexture(mat, pname)
                        : (extra ? extra->Tex : MaterialAsset::GetTexture(mat, pname) /*null*/);
            if (tex && tex->IsValid()) {
                tex->Bind(b.TextureUnit);
                shader.SetInt(hasName, 1);
            } else {
                // #99 — an absent map still binds its declared default ("white"/"black"/
                // "normal"), like the built-in path does, so the sampler never sees texture 0
                // (KHR 131204) or a stale texture left on that unit by a previous draw.
                const unsigned int fallback = prop.DefaultTex == "normal" ? DefaultTextures::FlatNormal()
                                            : prop.DefaultTex == "black"  ? DefaultTextures::Black()
                                                                          : DefaultTextures::White();
                GLStateCache::BindTexture2D(b.TextureUnit, fallback);
                shader.SetInt(hasName, 0);
            }
            shader.SetInt(uname, b.TextureUnit);
            break;
        }
        case ShaderPropType::Color:
        case ShaderPropType::Vec3:
            shader.SetVec3(uname, builtin ? MaterialAsset::GetColor(mat, pname)
                                          : (extra ? glm::vec3(extra->V)
                                                   : glm::vec3(prop.DefaultVec)));
            break;
        // #99 — vec2/vec4 uniforms need the matching setter; glUniform3f on them is
        // GL_INVALID_OPERATION and the value was silently never set.
        case ShaderPropType::Vec2:
            shader.SetVec2(uname, builtin ? glm::vec2(MaterialAsset::GetVec(mat, pname))
                                          : glm::vec2(extra ? extra->V : prop.DefaultVec));
            break;
        case ShaderPropType::Vec4:
            shader.SetVec4(uname, extra ? extra->V : prop.DefaultVec);
            break;
        case ShaderPropType::Float:
            shader.SetFloat(uname, builtin ? MaterialAsset::GetFloat(mat, pname)
                                           : (extra ? extra->F : prop.DefaultFloat));
            break;
        case ShaderPropType::Bool:
        case ShaderPropType::Int:
            shader.SetInt(uname, builtin ? (MaterialAsset::GetBool(mat, pname) ? 1 : 0)
                                         : (extra ? (prop.Type == ShaderPropType::Bool
                                                         ? (extra->B ? 1 : 0) : extra->I)
                                                  : (prop.DefaultBool ? 1 : 0)));
            break;
        default:
            break;
        }
    }
}
} // namespace

void Model::Draw(Shader& shader, const std::vector<std::shared_ptr<MaterialAsset>>& slots) {
    UploadBoneMatrices(shader);
    MaterialLocs locs = ResolveMaterialLocs(shader);
    for (int i = 0; i < (int)m_D->Meshes.size(); ++i) {
        bool hasSlot = i < (int)slots.size() && slots[i];
        const Material& mat = hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat;
        if (hasSlot && slots[i]->Shader)
            BindMaterialDataDriven(shader, *slots[i], *slots[i]->Shader);
        else
            BindMaterial(shader, mat, locs);
        m_D->Meshes[i]->Draw();
    }
}

namespace {
// #104 — per-mesh ShaderLab render state around a draw. Captures the pass's own state the first
// time a mesh overrides something, applies the override, and puts the pass state back for the
// next mesh without one (and at the end), so passes and meshes with no declared state never pay
// for a glGet or a state change.
class ShaderStateScope {
public:
    void Apply(const ShaderRenderState* st) {
        const bool want = st && st->AffectsDraw();
        if (!want) { Restore(); return; }
        if (!m_Saved) Save();
        Restore(); // start from the pass state, so fields this shader leaves unset aren't stale
        if (st->Cull == ShaderRenderState::CullMode::Off) glDisable(GL_CULL_FACE);
        else if (st->Cull != ShaderRenderState::CullMode::Unset) {
            glEnable(GL_CULL_FACE);
            glCullFace(st->Cull == ShaderRenderState::CullMode::Front ? GL_FRONT : GL_BACK);
        }
        if (st->ZWrite >= 0) glDepthMask(st->ZWrite ? GL_TRUE : GL_FALSE);
        if (st->ZTest) glDepthFunc(st->ZTest);
        if (st->Blend == 0) glDisable(GL_BLEND);
        else if (st->Blend == 1) { glEnable(GL_BLEND); glBlendFunc(st->BlendSrc, st->BlendDst); }
        m_Dirty = true;
    }
    // Only culling matters to a depth-only (shadow / prepass) draw.
    void ApplyCullOnly(const ShaderRenderState* st) {
        if (!st || st->Cull == ShaderRenderState::CullMode::Unset) { Restore(); return; }
        ShaderRenderState cull;
        cull.Cull = st->Cull;
        Apply(&cull);
    }
    void Restore() {
        if (!m_Dirty) return;
        m_CullEnabled ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
        glCullFace((GLenum)m_CullFace);
        glDepthMask(m_DepthMask);
        glDepthFunc((GLenum)m_DepthFunc);
        m_BlendEnabled ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
        glBlendFuncSeparate((GLenum)m_BlendSrcRgb, (GLenum)m_BlendDstRgb, (GLenum)m_BlendSrcA, (GLenum)m_BlendDstA);
        m_Dirty = false;
    }
    ~ShaderStateScope() { Restore(); }
private:
    void Save() {
        m_CullEnabled = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
        glGetIntegerv(GL_CULL_FACE_MODE, &m_CullFace);
        GLint depthMask = GL_TRUE;
        glGetIntegerv(GL_DEPTH_WRITEMASK, &depthMask);
        m_DepthMask = depthMask ? GL_TRUE : GL_FALSE;
        glGetIntegerv(GL_DEPTH_FUNC, &m_DepthFunc);
        m_BlendEnabled = glIsEnabled(GL_BLEND) == GL_TRUE;
        glGetIntegerv(GL_BLEND_SRC_RGB, &m_BlendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &m_BlendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_BlendSrcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &m_BlendDstA);
        m_Saved = true;
    }
    bool m_Saved = false, m_Dirty = false;
    bool m_CullEnabled = true, m_BlendEnabled = false;
    GLint m_CullFace = GL_BACK, m_DepthFunc = GL_LESS;
    GLboolean m_DepthMask = GL_TRUE;
    GLint m_BlendSrcRgb = GL_ONE, m_BlendDstRgb = GL_ZERO, m_BlendSrcA = GL_ONE, m_BlendDstA = GL_ZERO;
};

const ShaderRenderState* SlotRenderState(const std::vector<std::shared_ptr<MaterialAsset>>& slots, int i) {
    if (i >= (int)slots.size() || !slots[i] || !slots[i]->Shader) return nullptr;
    return &slots[i]->Shader->RenderState();
}

// The shader's state plus the material's Double Sided (#113): culling off unless the shader
// itself says otherwise. Returns a pointer to a per-call scratch copy when it needs one.
const ShaderRenderState* EffectiveRenderState(const std::vector<std::shared_ptr<MaterialAsset>>& slots, int i,
                                              const Material& mat) {
    const ShaderRenderState* st = SlotRenderState(slots, i);
    if (!mat.DoubleSided || (st && st->Cull != ShaderRenderState::CullMode::Unset)) return st;
    static thread_local ShaderRenderState scratch;
    scratch = st ? *st : ShaderRenderState{};
    scratch.Cull = ShaderRenderState::CullMode::Off;
    return &scratch;
}
} // namespace

void Model::DrawSelected(Shader& fallback, const glm::mat4& xform,
                         const std::vector<std::shared_ptr<MaterialAsset>>& slots,
                         const ProgramSelector& selectProgram, float opacity) {
    const glm::mat4 nrm = glm::mat4(glm::transpose(glm::inverse(glm::mat3(xform))));

    Shader*      lastProg = nullptr;
    MaterialLocs locs{};
    ShaderStateScope stateScope; // #104

    for (int i = 0; i < (int)m_D->Meshes.size(); ++i) {
        const bool hasSlot = i < (int)slots.size() && slots[i];
        stateScope.Apply(EffectiveRenderState(slots, i, hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat));
        Shader* prog = &fallback;
        if (Shader* p = selectProgram(hasSlot ? slots[i].get() : nullptr)) prog = p;

        if (prog != lastProg) {
            // Per-program state the single-program Model::Draw sets once up front. Bind first —
            // the selector only binds a program the first time it applies frame state to it, and
            // glUniform* always targets the currently bound program.
            prog->Bind();
            UploadBoneMatrices(*prog);           // uUseSkinning + bone SSBO
            locs = ResolveMaterialLocs(*prog);
            prog->SetMat4(prog->Loc("uModel"), xform);
            prog->SetMat4(prog->Loc("uNormalMatrix"), nrm);
            lastProg = prog;
        }
        // Per-draw: the transparent pass varies opacity per entity. No-op on opaque programs
        // (uOpacity is only read when uAlphaBlend == 1; an absent uniform is loc -1).
        prog->SetFloat("uOpacity", opacity);

        const Material& mat = hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat;
        if (hasSlot && slots[i]->Shader)
            BindMaterialDataDriven(*prog, *slots[i], *slots[i]->Shader);
        else
            BindMaterial(*prog, mat, locs);
        m_D->Meshes[i]->Draw();
    }
}

void Model::DrawDepthOnly(Shader& shader, const std::vector<std::shared_ptr<MaterialAsset>>& slots) {
    UploadBoneMatrices(shader);
    int albedoLoc = shader.Loc("uAlbedo");
    int alphaTestLoc = shader.Loc("uAlphaTest");
    int alphaCutoffLoc = shader.Loc("uAlphaCutoff");
    ShaderStateScope stateScope; // #104 — a Cull Off (double-sided) shader casts from both sides
    for (int i = 0; i < (int)m_D->Meshes.size(); ++i) {
        bool hasSlot = i < (int)slots.size() && slots[i];
        // Transparent materials don't cast shadows — skip them in the depth-only pass.
        if (hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::Transparent) continue;
        stateScope.ApplyCullOnly(EffectiveRenderState(slots, i, hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat));
        const Material& mat = hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat;
        // Only cost paid over a pure depth draw: one texture bind + two uniforms, and only for
        // CUTOUT materials with an albedo map (foliage/fences) — the shadow then follows the
        // cutout instead of a solid silhouette (#116). #101: it used to do this for every
        // albedo-mapped mesh, so an opaque material whose albedo alpha means something else
        // (smoothness, a mask) cast holey shadows. #192: skip even that when the previous mesh
        // in this pass drew with the same material.
        const bool clip = mat.AlphaClip || (hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::AlphaTest);
        if (!GLStateCache::MaterialAlreadyBound(mat.Hash() ^ (clip ? 0x5bd1e995ull : 0ull), shader.Program())) {
            shader.SetVec2("uUVTiling", mat.UVTiling); // #102 — the cutout follows the material's tiling
            shader.SetVec2("uUVOffset", mat.UVOffset);
            if (clip && mat.AlbedoMap && mat.AlbedoMap->IsValid()) {
                mat.AlbedoMap->Bind(0);
                shader.SetInt(alphaTestLoc, 1);
                shader.SetFloat(alphaCutoffLoc, mat.AlphaCutoff);
            } else {
                // uAlphaTest = 0 means the sampler result is never read, but the ShadowDepth
                // program still declares `sampler2D uAlbedo`, so unit 0 must hold a real texture
                // or the driver reports KHR 131204 ("texture 0 ... cannot be used") every draw —
                // the residual load-time warnings after PR #370 (audit GL-101 / #366 on unit 0).
                GLStateCache::BindTexture2D(0, DefaultTextures::White());
                shader.SetInt(alphaTestLoc, 0);
            }
            shader.SetInt(albedoLoc, 0);
        }
        m_D->Meshes[i]->Draw();
    }
}

unsigned int Model::TriangleCount() const {
    unsigned int total = 0;
    for (const auto& mesh : m_D->Meshes) total += mesh->IndexCount() / 3;
    return total;
}

unsigned int Model::VertexCount() const {
    unsigned int total = 0;
    for (const auto& mesh : m_D->Meshes) total += mesh->VertexCount();
    return total;
}

float Model::LowestVertexWorldY(const glm::mat4& modelMatrix) const {
    float lowest = 1e30f;
    for (const auto& mesh : m_D->Meshes) {
        for (const glm::vec3& local : mesh->LocalPositions()) {
            float worldY = (modelMatrix * glm::vec4(local, 1.0f)).y;
            lowest = std::min(lowest, worldY);
        }
    }
    return lowest;
}

bool Model::FindNearestVertexToScreenPoint(const glm::mat4& modelMatrix, const glm::mat4& viewProj,
    const glm::vec2& screenPoint, float viewportW, float viewportH, float maxPixelDist, glm::vec3& outLocalPos) const {
    float bestDistSq = maxPixelDist * maxPixelDist;
    bool found = false;
    glm::mat4 mvp = viewProj * modelMatrix;

    for (const auto& mesh : m_D->Meshes) {
        for (const glm::vec3& local : mesh->LocalPositions()) {
            glm::vec4 clip = mvp * glm::vec4(local, 1.0f);
            if (clip.w <= 0.0001f) continue; // behind the camera
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f) continue;

            glm::vec2 screen((ndc.x * 0.5f + 0.5f) * viewportW, (1.0f - (ndc.y * 0.5f + 0.5f)) * viewportH);
            glm::vec2 diff = screen - screenPoint;
            float distSq = glm::dot(diff, diff);
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                outLocalPos = local;
                found = true;
            }
        }
    }
    return found;
}
