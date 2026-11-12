#include "UnitTestSupport.h"
#include "../Renderer/GrassRenderer.h"
#include "../Game/Components.h"
#include "../Physics/Frustum.h"
#include "../Renderer/TerrainGrid.h"
#include "../Renderer/ModelVertex.h"
#include <memory>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace {
// CellsPerChunk: about kChunkSize metres of whole cells, one blade a cell.
void GrassCells() {
    CHECK(GrassRenderer::CellsPerChunk(1.0f) == 4);    // 1 m cells
    CHECK(GrassRenderer::CellsPerChunk(100.0f) == 40); // 10 cm cells
    CHECK(GrassRenderer::CellsPerChunk(0.0f) == 1);
    CHECK(GrassRenderer::CellsPerChunk(1e9f) == 256);  // clamped
}

// KeepFraction: all within the near range, smoothly down to kMinKeep at the draw distance, none past it.
void GrassKeep() {
    CHECK(GrassRenderer::KeepFraction(5.0f, 10.0f, 60.0f) == 1.0f);
    CHECK(GrassRenderer::KeepFraction(10.0f, 10.0f, 60.0f) == 1.0f);
    CHECK(GrassRenderer::KeepFraction(60.0f, 10.0f, 60.0f) == 0.0f);
    CHECK(GrassRenderer::KeepFraction(80.0f, 10.0f, 60.0f) == 0.0f);
    float prev = 1.0f;
    for (float d = 10.0f; d < 60.0f; d += 0.5f) {
        const float k = GrassRenderer::KeepFraction(d, 10.0f, 60.0f);
        CHECK(k <= prev + 1e-6f);
        CHECK(k >= GrassRenderer::kMinKeep - 1e-6f);
        prev = k;
    }
    CHECK(std::abs(GrassRenderer::KeepFraction(59.999f, 10.0f, 60.0f) - GrassRenderer::kMinKeep) < 1e-3f);
}

// SelectChunks: the area's chunks in front of the camera and within its draw distance; none behind it.
void GrassChunks() {
    GrassAreaComponent g;
    g.SizeX = 40.0f;
    g.SizeZ = 40.0f;
    g.Density = 100.0f; // 10 cm cells, 40 a chunk: a 10 x 10 grid of 4 m chunks
    g.DrawDistance = 1000.0f;
    std::vector<GrassRenderer::AreaDesc> areas{GrassRenderer::Describe(g, glm::mat4(1.0f))};
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 2000.0f);
    std::vector<GrassRenderer::ChunkGpu> out;

    // From high above, looking straight down at the whole field: every chunk.
    {
        const glm::vec3 eye(0.0f, 200.0f, 0.0f);
        const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
        GrassRenderer::SelectChunks(areas, Frustum::FromViewProj(proj * view), eye, out);
        CHECK(out.size() == 100);
        for (const auto& c : out) CHECK(c.Area == 0 && c.Cells == 40 && c.CellX % 40 == 0 && c.CellZ % 40 == 0);
    }
    // Standing at the field's +Z edge facing away from it: nothing.
    {
        const glm::vec3 eye(0.0f, 1.7f, 30.0f);
        const glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        GrassRenderer::SelectChunks(areas, Frustum::FromViewProj(proj * view), eye, out);
        CHECK(out.empty());
    }
    // Facing the field from inside, with a short draw distance: only chunks near the eye.
    {
        areas[0] = GrassRenderer::Describe([&] { GrassAreaComponent s = g; s.DrawDistance = 6.0f; s.NearDistance = 3.0f; return s; }(),
                                           glm::mat4(1.0f));
        const glm::vec3 eye(0.0f, 1.7f, 0.0f);
        const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 1.7f, -10.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        GrassRenderer::SelectChunks(areas, Frustum::FromViewProj(proj * view), eye, out);
        CHECK(!out.empty());
        CHECK(out.size() < 20);
    }
    // A field far beyond its draw distance: skipped whole.
    {
        areas[0] = GrassRenderer::Describe(g, glm::translate(glm::mat4(1.0f), glm::vec3(5000.0f, 0.0f, 0.0f)));
        const glm::vec3 eye(0.0f, 1.7f, 0.0f);
        const glm::mat4 view = glm::lookAt(eye, glm::vec3(10.0f, 1.7f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        GrassRenderer::SelectChunks(areas, Frustum::FromViewProj(proj * view), eye, out);
        CHECK(out.empty());
    }
}

// Describe: the area's axes follow the entity's yaw; scale doesn't change the blades.
void GrassDescribe() {
    GrassAreaComponent g;
    const glm::mat4 turned = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const auto d = GrassRenderer::Describe(g, glm::scale(turned, glm::vec3(3.0f)));
    // local X -> world -Z after a +90 degree yaw
    CHECK(std::abs(d.Gpu.Axes.x) < 1e-5f && std::abs(d.Gpu.Axes.y + 1.0f) < 1e-5f);
    CHECK(std::abs(d.Gpu.Axes.z - 1.0f) < 1e-5f && std::abs(d.Gpu.Axes.w) < 1e-5f);
    CHECK(d.Gpu.Size.x == g.SizeX && d.Gpu.Size.y == g.SizeZ);
    CHECK(std::abs(d.Gpu.Origin.w - 1.0f / std::sqrt(g.Density)) < 1e-6f);
    CHECK(d.MaxHeight > g.Height);
}

// A 5 x 4 grid over 8 x 6 m with a few bumps, through the scene encoding.
TerrainGrid MakeTerrain() {
    std::vector<float> h(5 * 4);
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 5; ++i) h[j * 5 + i] = 0.3f * (float)i - 0.2f * (float)j + ((i * 7 + j * 3) % 4) * 0.15f;
    TerrainGrid g;
    CHECK(TerrainGrid::Decode(5, 4, 8.0f, 6.0f, -2.0f, 3.0f, TerrainGrid::Encode(h, -2.0f, 3.0f), g));
    for (size_t k = 0; k < h.size(); ++k) CHECK(std::abs(g.Heights[k] - h[k]) < 1e-4f); // 16-bit steps of 5/65535 m
    return g;
}

// Decode refuses a sample count that doesn't match the resolution, and bad base64.
void TerrainDecode() {
    TerrainGrid g;
    const std::string enc = TerrainGrid::Encode(std::vector<float>(6, 1.0f), 0.0f, 2.0f);
    CHECK(TerrainGrid::Decode(3, 2, 4.0f, 4.0f, 0.0f, 2.0f, enc, g) && g.Valid());
    CHECK(std::abs(g.Height(0.3f, -0.7f) - 1.0f) < 1e-4f);
    CHECK(!TerrainGrid::Decode(3, 3, 4.0f, 4.0f, 0.0f, 2.0f, enc, g));
    CHECK(!TerrainGrid::Decode(3, 2, 4.0f, 4.0f, 0.0f, 2.0f, "@@@@", g));
}

// Height sits exactly on the mesh's triangles (and so on its collider), at the samples and between them.
void TerrainMatchesMesh() {
    const TerrainGrid g = MakeTerrain();
    std::vector<ModelVertex> v;
    std::vector<unsigned int> idx;
    g.BuildMesh(2.0f, v, idx);
    CHECK(v.size() == 20 && idx.size() == 4 * 3 * 6);
    for (const ModelVertex& p : v) CHECK(std::abs(g.Height(p.Position.x, p.Position.z) - p.Position.y) < 1e-5f);
    // Points inside every triangle (barycentric mixes) land on Height().
    const glm::vec3 mixes[3] = {{0.2f, 0.3f, 0.5f}, {0.6f, 0.2f, 0.2f}, {0.1f, 0.8f, 0.1f}};
    for (size_t t = 0; t < idx.size(); t += 3) {
        const glm::vec3 a = v[idx[t]].Position, b = v[idx[t + 1]].Position, c = v[idx[t + 2]].Position;
        CHECK(glm::cross(c - a, b - a).y < 0.0f); // counter-clockwise from above: the face points up
        for (const glm::vec3& m : mixes) {
            const glm::vec3 p = a * m.x + b * m.y + c * m.z;
            CHECK(std::abs(g.Height(p.x, p.z) - p.y) < 1e-4f);
        }
    }
    // Outside the grid: the edge's height.
    CHECK(std::abs(g.Height(-100.0f, -100.0f) - g.At(0, 0)) < 1e-5f);
    CHECK(std::abs(g.Height(100.0f, 100.0f) - g.At(4, 3)) < 1e-5f);
}

// Normal: up on flat ground, tipped against the slope on a ramp; RangeIn covers the samples inside.
void TerrainNormalRange() {
    std::vector<float> ramp(3 * 3);
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) ramp[j * 3 + i] = (float)i; // rises 1 m per 1 m along +X
    TerrainGrid g;
    CHECK(TerrainGrid::Decode(3, 3, 2.0f, 2.0f, 0.0f, 2.0f, TerrainGrid::Encode(ramp, 0.0f, 2.0f), g));
    const glm::vec3 n = g.Normal(0.2f, 0.3f);
    CHECK(std::abs(n.x + 0.70710678f) < 1e-3f && std::abs(n.y - 0.70710678f) < 1e-3f && std::abs(n.z) < 1e-3f);
    float lo, hi;
    g.RangeIn(-0.9f, -1.0f, 0.5f, 1.0f, lo, hi);
    CHECK(std::abs(lo - 0.1f) < 1e-3f && std::abs(hi - 1.5f) < 1e-3f);
}

// A Follow Terrain area takes its chunk heights from the terrain: raised 50 m, the chunks are only near an eye up there.
void GrassFollowsTerrain() {
    GrassAreaComponent g;
    g.SizeX = 40.0f;
    g.SizeZ = 40.0f;
    g.Density = 100.0f;
    g.DrawDistance = 20.0f;
    g.FollowTerrain = true;
    std::vector<GrassRenderer::AreaDesc> areas{GrassRenderer::Describe(g, glm::mat4(1.0f))};
    CHECK(areas[0].Gpu.Wind.y == 1.0f);
    GrassRenderer::TerrainRef terrain;
    auto grid = std::make_shared<TerrainGrid>();
    CHECK(TerrainGrid::Decode(2, 2, 100.0f, 100.0f, 0.0f, 1.0f, TerrainGrid::Encode({0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, 1.0f), *grid));
    terrain.Grid = grid;
    terrain.Origin = glm::vec3(0.0f, 50.0f, 0.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 2000.0f);
    const glm::vec3 eye(0.0f, 55.0f, 0.0f);
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 50.0f, -10.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    std::vector<GrassRenderer::ChunkGpu> out;
    GrassRenderer::SelectChunks(areas, Frustum::FromViewProj(proj * view), eye, out);
    CHECK(out.empty()); // on its own plane (y 0) the field is 50 m below the eye, past the draw distance
    GrassRenderer::SelectChunks(areas, Frustum::FromViewProj(proj * view), eye, out, &terrain);
    CHECK(!out.empty());
}
} // namespace

void RegisterGrassTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Grass: cells per chunk from density", GrassCells});
    tests.push_back({"Grass: distance thinning is full, falling, then none", GrassKeep});
    tests.push_back({"Grass: chunk selection by frustum and draw distance", GrassChunks});
    tests.push_back({"Grass: area axes follow yaw, scale ignored", GrassDescribe});
    tests.push_back({"Grass: Follow Terrain chunks stand on the terrain", GrassFollowsTerrain});
    tests.push_back({"Terrain: decode checks the sample count and base64", TerrainDecode});
    tests.push_back({"Terrain: height matches the mesh triangles", TerrainMatchesMesh});
    tests.push_back({"Terrain: slope normal and height range", TerrainNormalRange});
}
