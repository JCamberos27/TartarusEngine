// Model: construction, deferred import and GPU upload, instances, primitives and mesh counts.
// Import: ModelImport.cpp. Textures: ModelTextures.cpp. Animation: ModelAnimation.cpp.
// Skinning: ModelSkinning.cpp. Drawing: ModelDraw.cpp.

#include "Model.h"
#include "Log.h"
#include "PrimitiveMeshes.h"
#include "TerrainGrid.h"
#include "Texture.h"
#include "gl.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

namespace {
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

std::shared_ptr<Model> Model::ImportDeferred(const std::string& path, const ModelImportSettings& settings) {
    std::shared_ptr<Model> model(new Model());
    model->m_Path = path;
    model->m_D->Directory = DirectoryOf(path);
    model->m_DeferGpu = true;
    model->ImportFromFile(settings);
    model->m_DeferGpu = false;
    model->m_GpuPending = true;
    return model;
}

UploadStaging::Step Model::GpuUploadStep() {
    using UploadStaging::Step;
    if (!m_GpuPending) return Step::Done;
    for (auto& mesh : m_D->Meshes)
        if (mesh->NeedsUpload()) {
            const Step s = mesh->UploadStep(true);
            return s == Step::Done ? Step::Progress : s;
        }
    for (auto& [key, tex] : m_D->TextureCache)
        if (tex && tex->IsPendingUpload()) {
            const Step s = tex->UploadStep(true);
            return s == Step::Done ? Step::Progress : s;
        }
    m_GpuPending = false;
    return Step::Done;
}

bool Model::FinishGpuUploadStep() {
    if (!m_GpuPending) return true;
    for (auto& mesh : m_D->Meshes)
        if (mesh->NeedsUpload()) { mesh->FinishUpload(); return false; }
    for (auto& [key, tex] : m_D->TextureCache)
        if (tex && tex->IsPendingUpload()) { tex->FinishUpload(); return false; }
    m_GpuPending = false;
    return true;
}

std::shared_ptr<Model> Model::CreateInstance() const {
    std::shared_ptr<Model> inst(new Model());
    inst->m_D = m_D;       // #96 — shared import: no Assimp, no new GPU buffers or textures
    inst->m_Path = m_Path;
    // Only a rig needs its own palette: MAX_BONES matrices are 32 KB, and a forest places ~150k unskinned copies
    // (~5 GB). EvaluatePose / ResolvePalette grow it on first use, and the readers fall back to the bind pose.
    if (m_D->BoneCounter > 0) inst->m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
    return inst;
}

std::shared_ptr<Model> Model::CreatePrimitive(const std::string& kind, const std::string& path) {
    if (kind == "terrain") { // a terrain's flat stand-in until TerrainSystem builds its mesh
        TerrainGrid flat;
        flat.ResX = flat.ResZ = 2;
        flat.SizeX = flat.SizeZ = 1.0f;
        flat.Heights.assign(4, 0.0f);
        return CreateTerrain(flat, 1.0f, path);
    }
    std::shared_ptr<Model> model(new Model());
    model->m_Path = path;
    model->m_D->GlobalInverseTransform = glm::mat4(1.0f);

    // One mesh per primitive kind: every placed sphere, cube... draws from the same GPU buffers (it used to
    // generate and upload its own, 139 spheres of 1152 triangles in the Sandbox) and has its own material.
    // Process-lifetime like the bone ring, so no GL delete runs after the context is gone.
    static std::map<std::string, std::unique_ptr<ModelMesh>>* const s_Templates = new std::map<std::string, std::unique_ptr<ModelMesh>>();
    const std::string key = kind == "sphere" || kind == "cylinder" || kind == "cone" || kind == "plane" ||
                            kind == "pyramid" || kind == "donut" || kind == "capsule" ? kind : std::string("cube");
    std::unique_ptr<ModelMesh>& tmpl = (*s_Templates)[key];
    if (!tmpl) {
        std::vector<ModelVertex> verts;
        std::vector<unsigned int> indices;
        if (key == "sphere") PrimitiveMeshes::GenerateSphere(verts, indices);
        else if (key == "cylinder") PrimitiveMeshes::GenerateCylinder(verts, indices);
        else if (key == "cone") PrimitiveMeshes::GenerateCone(verts, indices);
        else if (key == "plane") PrimitiveMeshes::GeneratePlane(verts, indices);
        else if (key == "pyramid") PrimitiveMeshes::GeneratePyramid(verts, indices);
        else if (key == "donut") PrimitiveMeshes::GenerateDonut(verts, indices);
        else if (key == "capsule") PrimitiveMeshes::GenerateCapsule(verts, indices);
        else PrimitiveMeshes::GenerateCube(verts, indices);
        tmpl = std::make_unique<ModelMesh>(verts, indices);
        tmpl->Mat.BaseColor = glm::vec3(0.75f); // neutral default; override via the Inspector's PBR Material section
    }

    for (const glm::vec3& p : tmpl->LocalPositions()) {
        model->m_D->BoundsMin = glm::min(model->m_D->BoundsMin, p);
        model->m_D->BoundsMax = glm::max(model->m_D->BoundsMax, p);
    }
    model->m_D->Meshes.push_back(std::make_unique<ModelMesh>(*tmpl, ModelMesh::ShareGeometry{}));

    model->m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
    return model;
}

std::shared_ptr<Model> Model::CreateTerrain(const TerrainGrid& grid, float uvTile, const std::string& path) {
    std::vector<ModelVertex> verts;
    std::vector<unsigned int> indices;
    grid.BuildMesh(uvTile, verts, indices);
    if (verts.empty()) return CreatePrimitive("cube", path);
    std::shared_ptr<Model> model(new Model());
    model->m_Path = path;
    model->m_D->GlobalInverseTransform = glm::mat4(1.0f);
    for (const ModelVertex& v : verts) {
        model->m_D->BoundsMin = glm::min(model->m_D->BoundsMin, v.Position);
        model->m_D->BoundsMax = glm::max(model->m_D->BoundsMax, v.Position);
    }
    // Built here and uploaded at once where there is a GL context; the unit tests (no context) only need its
    // collision geometry.
    auto mesh = std::make_unique<ModelMesh>(std::move(verts), indices, /*deferUpload=*/true);
    if (glCreateBuffers) mesh->FinishUpload();
    mesh->Mat.BaseColor = glm::vec3(0.75f);
    model->m_D->Meshes.push_back(std::move(mesh));
    model->m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
    return model;
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

std::atomic<uint64_t> Model::s_AnimationEpoch{0};
std::atomic<std::uint64_t> Model::s_ImportStamps{0};
