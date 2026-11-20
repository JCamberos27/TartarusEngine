#include "UnitTestSupport.h"
#include "../Renderer/GrassRenderer.h"
#include "../Game/Components.h"
#include "../Physics/Frustum.h"
#include "../Renderer/TerrainGrid.h"
#include "../Renderer/GrassDensityMap.h"
#include <memory>
#include <algorithm>
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
        // Every pick's footprint (padded by the blade reach, as SelectChunks does) is within 6 m of the eye
        // and not wholly behind it; the two chunks just ahead of the eye are both in.
        const float cell = areas[0].Gpu.Origin.w, pad = areas[0].MaxHeight;
        bool aheadLeft = false, aheadRight = false;
        for (const auto& c : out) {
            const float x0 = -20.0f + c.CellX * cell - pad, x1 = -20.0f + (c.CellX + c.Cells) * cell + pad;
            const float z0 = -20.0f + c.CellZ * cell - pad, z1 = -20.0f + (c.CellZ + c.Cells) * cell + pad;
            const float dx = std::max({x0 - eye.x, 0.0f, eye.x - x1}), dz = std::max({z0 - eye.z, 0.0f, eye.z - z1});
            CHECK(std::sqrt(dx * dx + dz * dz) <= 6.0f + 1e-4f);
            CHECK(z0 < eye.z);
            aheadLeft |= c.CellX == 160 && c.CellZ == 160;  // x -4..0, z -4..0
            aheadRight |= c.CellX == 200 && c.CellZ == 160; // x 0..4, z -4..0
        }
        CHECK(aheadLeft && aheadRight);
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
// The density mask: base64 round trip, bilinear sample, the max over a rectangle.
void GrassMaskSample() {
    // 3 x 2: a bare left column, full right column, half in the middle.
    const std::vector<float> v = {0.0f, 0.5f, 1.0f, 0.0f, 0.5f, 1.0f};
    GrassDensityMap m;
    CHECK(GrassDensityMap::Decode(3, 2, GrassDensityMap::Encode(v), m) && m.Valid());
    CHECK(!GrassDensityMap::Decode(3, 3, GrassDensityMap::Encode(v), m));
    CHECK(GrassDensityMap::Decode(3, 2, GrassDensityMap::Encode(v), m));
    CHECK(m.Sample(0.0f, 0.3f) == 0.0f);
    CHECK(std::abs(m.Sample(1.0f, 0.7f) - 1.0f) < 1e-6f);
    CHECK(std::abs(m.Sample(0.25f, 0.5f) - 0.25f) < 0.01f); // halfway between 0 and ~0.5
    CHECK(std::abs(m.Sample(-3.0f, 9.0f)) < 1e-6f);          // clamped to the edge
    CHECK(m.MaxIn(0.0f, 0.0f, 0.0f, 1.0f) == 0.0f);          // the bare column alone
    CHECK(m.MaxIn(0.1f, 0.0f, 0.2f, 1.0f) > 0.49f);          // touches the middle column's cells
    CHECK(m.MaxIn(0.6f, 0.2f, 0.9f, 0.4f) == 1.0f);
}

// A masked area: Describe decodes the mask once (cached on the component) and flags it for the GPU; chunk
// selection skips the chunks it leaves bare.
void GrassMaskChunks() {
    GrassAreaComponent g;
    g.SizeX = 40.0f;
    g.SizeZ = 40.0f;
    g.Density = 100.0f; // 10 x 10 chunks of 4 m
    g.DrawDistance = 1000.0f;
    // 11 x 11 samples, 4 m apart: everything bare but the area's +X half.
    std::vector<float> v(11 * 11, 0.0f);
    for (int j = 0; j < 11; ++j)
        for (int i = 6; i < 11; ++i) v[j * 11 + i] = 1.0f;
    g.DensityMap = GrassDensityMap::Encode(v);
    g.MapResX = g.MapResZ = 11;
    const auto d = GrassRenderer::Describe(g, glm::mat4(1.0f));
    CHECK(d.Map && d.Map->ResX == 11 && d.Gpu.Map.w == 1.0f && d.Gpu.Map.y == 11.0f);
    const auto again = GrassRenderer::Describe(g, glm::mat4(1.0f));
    CHECK(again.Map == d.Map); // decoded once
    std::vector<GrassRenderer::AreaDesc> areas{d};
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 2000.0f);
    const glm::vec3 eye(0.0f, 200.0f, 0.0f);
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    std::vector<GrassRenderer::ChunkGpu> out;
    GrassRenderer::SelectChunks(areas, Frustum::FromViewProj(proj * view), eye, out);
    // The full samples start at x = 4 m (local): the chunk columns from x = 0 up touch them (the blades between
    // ramp in), the five left of it are bare.
    CHECK(out.size() == 50);
    for (const auto& c : out) CHECK(c.CellX >= 200);
    // No mask: all 100.
    g.DensityMap.clear();
    std::vector<GrassRenderer::AreaDesc> plain{GrassRenderer::Describe(g, glm::mat4(1.0f))};
    CHECK(!plain[0].Map && plain[0].Gpu.Map.w == 0.0f);
    GrassRenderer::SelectChunks(plain, Frustum::FromViewProj(proj * view), eye, out);
    CHECK(out.size() == 100);
}
} // namespace

void RegisterGrassTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Grass: cells per chunk from density", GrassCells});
    tests.push_back({"Grass: distance thinning is full, falling, then none", GrassKeep});
    tests.push_back({"Grass: chunk selection by frustum and draw distance", GrassChunks});
    tests.push_back({"Grass: area axes follow yaw, scale ignored", GrassDescribe});
    tests.push_back({"Grass: Follow Terrain chunks stand on the terrain", GrassFollowsTerrain});
    tests.push_back({"Grass: density mask decode, sample and max", GrassMaskSample});
    tests.push_back({"Grass: density mask cached and skips bare chunks", GrassMaskChunks});
}
