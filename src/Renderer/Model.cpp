#include "Model.h"
#include "Texture.h"
#include "Shader.h"
#include "PrimitiveMeshes.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>

#include <iostream>
#include <cmath>
#include <algorithm>

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

Model::Model(const std::string& path) : m_Path(path), m_Directory(DirectoryOf(path)) {
    Assimp::Importer importer;
    // FBX files embed their own unit scale (commonly centimeters, sometimes meters or
    // inches) in the file's global settings. aiProcess_GlobalScale + this property tells
    // Assimp to read that and auto-convert to real-world meters, so a model built at 1
    // unit = 1cm no longer imports 100x too large without the artist doing anything special.
    // Files with no such metadata (most .obj) are unaffected — this can't invent scale that
    // was never recorded, so hand-authored/untagged assets may still need manual correction.
    importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs |
        aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices |
        aiProcess_GlobalScale);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        std::cerr << "Assimp import failed for '" << path << "': " << importer.GetErrorString() << std::endl;
        return;
    }

    m_GlobalInverseTransform = glm::inverse(AiToGlm(scene->mRootNode->mTransformation));

    ProcessNode(scene->mRootNode, scene, glm::mat4(1.0f));
    ReadHierarchy(m_RootNode, scene->mRootNode);
    ReadAnimations(scene);

    m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
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
    bool skinned = mesh->mNumBones > 0;
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

    ExtractBoneWeights(vertices, mesh);

    auto gpuMesh = std::make_unique<ModelMesh>(vertices, indices);
    if (mesh->mMaterialIndex < scene->mNumMaterials) {
        gpuMesh->Mat = ExtractMaterial(scene, mesh->mMaterialIndex);
    }
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

Material Model::ExtractMaterial(const aiScene* scene, unsigned int materialIndex) {
    Material mat;
    aiMaterial* material = scene->mMaterials[materialIndex];

    auto loadSlot = [&](aiTextureType type) -> std::shared_ptr<Texture> {
        if (material->GetTextureCount(type) == 0) return nullptr;
        aiString str;
        material->GetTexture(type, 0, &str);
        return LoadCachedTexture(m_Directory + "/" + str.C_Str());
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
    for (unsigned int boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
        aiBone* bone = mesh->mBones[boneIdx];
        std::string boneName = bone->mName.C_Str();

        int boneID;
        auto it = m_BoneInfoMap.find(boneName);
        if (it == m_BoneInfoMap.end()) {
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

void Model::UploadBoneMatrices(Shader& shader) const {
    bool skinning = m_CurrentAnimation >= 0;
    shader.SetInt("uUseSkinning", skinning ? 1 : 0);
    if (!skinning) return;

    for (int i = 0; i < MAX_BONES; ++i) {
        shader.SetMat4("uBones[" + std::to_string(i) + "]", m_FinalBoneMatrices[i]);
    }
}

namespace {
void BindMaterial(Shader& shader, const Material& mat) {
    shader.SetVec3("uBaseColor", mat.BaseColor);
    shader.SetFloat("uMetallic", mat.Metallic);
    shader.SetFloat("uRoughness", mat.Roughness);
    shader.SetVec3("uEmissiveColor", mat.EmissiveColor * mat.EmissiveStrength);

    int unit = 1; // unit 0 reserved by caller for nothing; start textures at 1..5
    auto bindOptional = [&](const std::shared_ptr<Texture>& tex, const char* hasUniform, const char* samplerUniform) {
        if (tex) {
            tex->Bind(unit);
            shader.SetInt(samplerUniform, unit);
            shader.SetInt(hasUniform, 1);
            unit++;
        } else {
            shader.SetInt(hasUniform, 0);
        }
    };

    bindOptional(mat.AlbedoMap, "uHasAlbedoMap", "uAlbedoMap");
    bindOptional(mat.NormalMap, "uHasNormalMap", "uNormalMap");
    bindOptional(mat.MetallicRoughnessMap, "uHasMetallicRoughnessMap", "uMetallicRoughnessMap");
    bindOptional(mat.MetallicMap, "uHasMetallicMap", "uMetallicMap");
    bindOptional(mat.RoughnessMap, "uHasRoughnessMap", "uRoughnessMap");
    bindOptional(mat.AOMap, "uHasAOMap", "uAOMap");
    bindOptional(mat.EmissiveMap, "uHasEmissiveMap", "uEmissiveMap");
}
} // namespace

void Model::Draw(Shader& shader) {
    UploadBoneMatrices(shader);
    for (auto& mesh : m_Meshes) {
        const Material& mat = m_MaterialOverride ? *m_MaterialOverride : mesh->Mat;
        BindMaterial(shader, mat);
        mesh->Draw();
    }
}

bool Model::FindNearestVertexWorld(const glm::mat4& modelMatrix, const glm::vec3& worldQuery, float maxDist, glm::vec3& outWorldPos) const {
    float bestDistSq = maxDist * maxDist;
    bool found = false;
    for (const auto& mesh : m_Meshes) {
        for (const glm::vec3& local : mesh->LocalPositions()) {
            glm::vec3 world = glm::vec3(modelMatrix * glm::vec4(local, 1.0f));
            glm::vec3 diff = world - worldQuery;
            float distSq = glm::dot(diff, diff);
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                outWorldPos = world;
                found = true;
            }
        }
    }
    return found;
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
