#include "Model.h"
#include "Log.h"
#include "Texture.h"
#include "Shader.h"
#include "PrimitiveMeshes.h"
#include "gl.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>

#include <iostream>
#include <cmath>
#include <algorithm>
#include <filesystem>

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
    : m_Path(path), m_Directory(DirectoryOf(path)) {
    ImportFromFile(settings);
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
        Log::Error("Model: import failed for '" + m_Path + "': " + importer.GetErrorString());
        // A failed import leaves m_Meshes empty; collapse the bounds to a finite point too, so
        // anything that folds this model into a wider AABB (scene framing, focus) can't inherit
        // the inverted 1e30 sentinel and blow the result up to inf/NaN.
        m_BoundsMin = glm::vec3(0.0f);
        m_BoundsMax = glm::vec3(0.0f);
        return;
    }

    // Reset every field an import populates, so re-running this on an already-imported Model
    // (Reimport) starts from a clean slate instead of appending to/leaking the previous import's
    // state. Deliberately NOT touched: m_Path, m_Directory, m_MaterialOverride (an independent
    // editor-set look, not part of what "importing" produces).
    m_Meshes.clear();
    m_TextureCache.clear();
    m_BoneInfoMap.clear();
    m_BoneCounter = 0;
    m_Animations.clear();
    m_CurrentAnimation = -1;
    m_CurrentTimeTicks = 0.0f;
    m_BoundsMin = glm::vec3(1e30f);
    m_BoundsMax = glm::vec3(-1e30f);
    m_Settings = settings;

    m_GlobalInverseTransform = glm::inverse(AiToGlm(scene->mRootNode->mTransformation));

    ProcessNode(scene->mRootNode, scene, glm::mat4(1.0f));
    ReadHierarchy(m_RootNode, scene->mRootNode);
    if (settings.ImportAnimations) ReadAnimations(scene);

    m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
}

bool Model::Reimport(const ModelImportSettings& settings) {
    std::error_code ec;
    // primitive:// paths aren't real files - regenerating one from ImportFromFile would try
    // (and fail) to open that synthetic path via Assimp, so reimporting a primitive is a no-op.
    if (m_Path.rfind("primitive://", 0) == 0) return false;
    if (!std::filesystem::exists(m_Path, ec) || ec) {
        Log::Error("Model: cannot reimport '" + m_Path + "' - file is missing.");
        return false;
    }
    ImportFromFile(settings);
    return !m_Meshes.empty();
}

Model::~Model() = default;

std::shared_ptr<Model> Model::CreatePrimitive(const std::string& kind, const std::string& path) {
    std::shared_ptr<Model> model(new Model());
    model->m_Path = path;
    model->m_GlobalInverseTransform = glm::mat4(1.0f);

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
        model->m_BoundsMin = glm::min(model->m_BoundsMin, v.Position);
        model->m_BoundsMax = glm::max(model->m_BoundsMax, v.Position);
    }

    auto mesh = std::make_unique<ModelMesh>(verts, indices);
    mesh->Mat.BaseColor = glm::vec3(0.75f); // neutral default; override via the Inspector's PBR Material section
    model->m_Meshes.push_back(std::move(mesh));

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
        m_Meshes.push_back(ProcessMesh(mesh, scene, nodeTransform));
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
    // Gated on m_Settings.ImportSkeleton too: with skeleton import off, ExtractBoneWeights below
    // never runs, so treating this as "skinned" would leave every vertex's bone weights at their
    // default (unset) values instead of the identity-pose vertex position baked in here - the
    // mesh would render collapsed to the origin rather than as a static copy of its bind pose.
    bool skinned = m_Settings.ImportSkeleton && mesh->mNumBones > 0;
    glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(nodeTransform)));

    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        ModelVertex v;
        v.Position = AiToGlm(mesh->mVertices[i]);
        v.Normal = mesh->HasNormals() ? AiToGlm(mesh->mNormals[i]) : glm::vec3(0, 1, 0);
        if (mesh->mTextureCoords[0]) {
            v.UV = {mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y};
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

        m_BoundsMin = glm::min(m_BoundsMin, v.Position);
        m_BoundsMax = glm::max(m_BoundsMax, v.Position);
    }

    std::vector<unsigned int> indices;
    indices.reserve(mesh->mNumFaces * 3);
    for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
        const aiFace& face = mesh->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; ++j) {
            indices.push_back(face.mIndices[j]);
        }
    }

    if (m_Settings.ImportSkeleton) ExtractBoneWeights(vertices, mesh);

    auto gpuMesh = std::make_unique<ModelMesh>(vertices, indices);
    if (m_Settings.MaterialImportMode == ModelImportSettings::MaterialMode::ImportEmbedded) {
        if (mesh->mMaterialIndex < scene->mNumMaterials) {
            gpuMesh->Mat = ExtractMaterial(scene, mesh->mMaterialIndex);
        }
    } else if (m_Settings.MaterialImportMode == ModelImportSettings::MaterialMode::CreateSynthetic) {
        // Ignore the file's own materials/textures entirely - same neutral look CreatePrimitive
        // assigns, left for the Inspector's PBR Material section to author from scratch.
        gpuMesh->Mat.BaseColor = glm::vec3(0.75f);
    }
    // MaterialMode::None: leave gpuMesh->Mat at Material{}'s bare defaults, untouched.
    return gpuMesh;
}

std::shared_ptr<Texture> Model::LoadCachedTexture(const std::string& fullPath) {
    auto it = m_TextureCache.find(fullPath);
    if (it != m_TextureCache.end()) return it->second;

    auto tex = std::make_shared<Texture>(fullPath);
    if (!tex->IsValid()) return nullptr;
    m_TextureCache[fullPath] = tex;
    return tex;
}

std::string Model::ResolveTexturePath(const std::string& raw) const {
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
    const fs::path modelDir(m_Directory);

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

Material Model::ExtractMaterial(const aiScene* scene, unsigned int materialIndex) {
    Material mat;
    aiMaterial* material = scene->mMaterials[materialIndex];

    auto loadSlot = [&](aiTextureType type) -> std::shared_ptr<Texture> {
        if (material->GetTextureCount(type) == 0) return nullptr;
        aiString str;
        material->GetTexture(type, 0, &str);
        if (str.length == 0) return nullptr;

        std::string resolved = ResolveTexturePath(str.C_Str());
        if (!resolved.empty() && resolved[0] == '*') {
            // Embedded texture — not supported by the disk-only Texture loader yet. Log once,
            // clearly, instead of failing on a bogus "*0" filename.
            Log::Warn("Model: '" + m_Path + "' uses an embedded texture (" + resolved +
                      ") which isn't supported yet - that map slot will be blank.");
            return nullptr;
        }
        return LoadCachedTexture(resolved);
    };

    mat.AlbedoMap = loadSlot(aiTextureType_DIFFUSE);
    if (!mat.AlbedoMap) mat.AlbedoMap = loadSlot(aiTextureType_BASE_COLOR); // glTF2 alt slot
    mat.NormalMap = loadSlot(aiTextureType_NORMALS);
    mat.MetallicRoughnessMap = loadSlot(aiTextureType_UNKNOWN); // assimp puts glTF2 packed metal-rough here
    // Standalone maps — NOT the packed slot above, which is a different (G=rough, B=metal)
    // texture layout that a plain grayscale roughness/metalness map would be misread against.
    mat.RoughnessMap = loadSlot(aiTextureType_DIFFUSE_ROUGHNESS);
    mat.MetallicMap = loadSlot(aiTextureType_METALNESS);
    mat.AOMap = loadSlot(aiTextureType_LIGHTMAP);
    if (!mat.AOMap) mat.AOMap = loadSlot(aiTextureType_AMBIENT_OCCLUSION);
    mat.EmissiveMap = loadSlot(aiTextureType_EMISSIVE);

    aiColor4D color;
    if (material->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
        mat.BaseColor = {color.r, color.g, color.b};
    }
    if (material->Get(AI_MATKEY_COLOR_EMISSIVE, color) == AI_SUCCESS) {
        mat.EmissiveColor = {color.r, color.g, color.b};
    }
    float scalar;
    if (material->Get(AI_MATKEY_METALLIC_FACTOR, scalar) == AI_SUCCESS) mat.Metallic = scalar;
    if (material->Get(AI_MATKEY_ROUGHNESS_FACTOR, scalar) == AI_SUCCESS) mat.Roughness = scalar;

    return mat;
}

void Model::ExtractBoneWeights(std::vector<ModelVertex>& vertices, aiMesh* mesh) {
    bool overflowWarned = false;
    for (unsigned int boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
        aiBone* bone = mesh->mBones[boneIdx];
        std::string boneName = bone->mName.C_Str();

        int boneID;
        auto it = m_BoneInfoMap.find(boneName);
        if (it == m_BoneInfoMap.end()) {
            // uBones[] is a fixed mat4[MAX_BONES] in the shader; a rig with more unique bones
            // would index it out of bounds (undefined in GLSL, TDR/black on many drivers).
            // Drop the extra bone's influences rather than let that reach the GPU (#98).
            if (m_BoneCounter >= MAX_BONES) {
                if (!overflowWarned) {
                    Log::Warn("Model '" + m_Path + "' has more than " + std::to_string(MAX_BONES) +
                              " bones - influences past that are dropped (skinning will be wrong).");
                    overflowWarned = true;
                }
                continue;
            }
            BoneInfo info{m_BoneCounter, AiToGlm(bone->mOffsetMatrix)};
            m_BoneInfoMap[boneName] = info;
            boneID = m_BoneCounter++;
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
        m_Animations.push_back(std::move(clip));
    }
}

void Model::PlayAnimation(int index) {
    if (index < 0 || index >= (int)m_Animations.size()) {
        m_CurrentAnimation = -1;
        return;
    }
    m_CurrentAnimation = index;
    m_CurrentTimeTicks = 0.0f;
}

void Model::UpdateAnimation(float dt) {
    if (m_CurrentAnimation < 0) return;
    const AnimationClip& clip = m_Animations[m_CurrentAnimation];
    m_CurrentTimeTicks += dt * clip.TicksPerSecond;
    if (clip.DurationTicks > 0.0f) {
        m_CurrentTimeTicks = std::fmod(m_CurrentTimeTicks, clip.DurationTicks);
    }
    CalculateBoneTransform(m_RootNode, glm::mat4(1.0f));
}

void Model::CalculateBoneTransform(const AssimpNodeData& node, const glm::mat4& parentTransform) {
    glm::mat4 nodeTransform = node.Transform;

    if (m_CurrentAnimation >= 0) {
        const AnimationClip& clip = m_Animations[m_CurrentAnimation];
        auto it = clip.Channels.find(node.Name);
        if (it != clip.Channels.end()) {
            nodeTransform = it->second.Interpolate(m_CurrentTimeTicks);
        }
    }

    glm::mat4 globalTransform = parentTransform * nodeTransform;

    auto boneIt = m_BoneInfoMap.find(node.Name);
    if (boneIt != m_BoneInfoMap.end()) {
        int idx = boneIt->second.ID;
        if (idx < MAX_BONES) {
            m_FinalBoneMatrices[idx] = m_GlobalInverseTransform * globalTransform * boneIt->second.OffsetMatrix;
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
    bool skinning = m_CurrentAnimation >= 0;
    shader.SetInt("uUseSkinning", skinning ? 1 : 0);

    EnsureBoneSsbo();
    if (skinning)
        glNamedBufferSubData(g_BoneSsbo, 0, MAX_BONES * (GLsizeiptr)sizeof(glm::mat4),
                             m_FinalBoneMatrices.data());
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
};

MaterialLocs ResolveMaterialLocs(Shader& shader) {
    MaterialLocs L;
    L.baseColor = shader.Loc("uBaseColor");
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
    return L;
}

void BindMaterial(Shader& shader, const Material& mat, const MaterialLocs& locs) {
    shader.SetVec3(locs.baseColor, mat.BaseColor);
    shader.SetFloat(locs.metallic, mat.Metallic);
    shader.SetFloat(locs.roughness, mat.Roughness);
    shader.SetVec3(locs.emissiveColor, mat.EmissiveColor * mat.EmissiveStrength);
    shader.SetInt(locs.triplanar, mat.Triplanar ? 1 : 0);
    shader.SetFloat(locs.triplanarScale, mat.TriplanarScale);

    int unit = 1; // unit 0 reserved by caller for nothing; start textures at 1..5
    auto bindOptional = [&](const std::shared_ptr<Texture>& tex, int hasLoc, int samplerLoc) {
        if (tex) {
            tex->Bind(unit);
            shader.SetInt(samplerLoc, unit);
            shader.SetInt(hasLoc, 1);
            unit++;
        } else {
            shader.SetInt(hasLoc, 0);
        }
    };

    bindOptional(mat.AlbedoMap, locs.hasAlbedo, locs.albedoMap);
    bindOptional(mat.NormalMap, locs.hasNormal, locs.normalMap);
    bindOptional(mat.MetallicRoughnessMap, locs.hasMetallicRoughness, locs.metallicRoughnessMap);
    bindOptional(mat.MetallicMap, locs.hasMetallic, locs.metallicMap);
    bindOptional(mat.RoughnessMap, locs.hasRoughness, locs.roughnessMap);
    bindOptional(mat.AOMap, locs.hasAO, locs.aoMap);
    bindOptional(mat.EmissiveMap, locs.hasEmissive, locs.emissiveMap);
}
} // namespace

void Model::Draw(Shader& shader) {
    UploadBoneMatrices(shader);
    MaterialLocs locs = ResolveMaterialLocs(shader);
    for (auto& mesh : m_Meshes) {
        const Material& mat = m_MaterialOverride ? *m_MaterialOverride : mesh->Mat;
        BindMaterial(shader, mat, locs);
        mesh->Draw();
    }
}

void Model::DrawDepthOnly(Shader& shader) {
    UploadBoneMatrices(shader);
    int albedoLoc = shader.Loc("uAlbedo");
    int alphaTestLoc = shader.Loc("uAlphaTest");
    for (auto& mesh : m_Meshes) {
        const Material& mat = m_MaterialOverride ? *m_MaterialOverride : mesh->Mat;
        // Only cost paid over a pure depth draw: one texture bind + two uniforms, and only for
        // meshes that actually have an albedo map (cutout foliage/fences) — the shadow then
        // follows the cutout instead of a solid silhouette (#116).
        if (mat.AlbedoMap) {
            mat.AlbedoMap->Bind(0);
            shader.SetInt(albedoLoc, 0);
            shader.SetInt(alphaTestLoc, 1);
        } else {
            shader.SetInt(alphaTestLoc, 0);
        }
        mesh->Draw();
    }
}

unsigned int Model::TriangleCount() const {
    unsigned int total = 0;
    for (const auto& mesh : m_Meshes) total += mesh->IndexCount() / 3;
    return total;
}

unsigned int Model::VertexCount() const {
    unsigned int total = 0;
    for (const auto& mesh : m_Meshes) total += mesh->VertexCount();
    return total;
}

float Model::LowestVertexWorldY(const glm::mat4& modelMatrix) const {
    float lowest = 1e30f;
    for (const auto& mesh : m_Meshes) {
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

    for (const auto& mesh : m_Meshes) {
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
