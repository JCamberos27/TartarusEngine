#include "UnitTestSupport.h"
#include "../Renderer/GrassRenderer.h"
#include "../Game/Components.h"
#include "../Physics/Frustum.h"
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
} // namespace

void RegisterGrassTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Grass: cells per chunk from density", GrassCells});
    tests.push_back({"Grass: distance thinning is full, falling, then none", GrassKeep});
    tests.push_back({"Grass: chunk selection by frustum and draw distance", GrassChunks});
    tests.push_back({"Grass: area axes follow yaw, scale ignored", GrassDescribe});
}
