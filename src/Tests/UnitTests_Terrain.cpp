#include "UnitTestSupport.h"
#include "../Renderer/TerrainGrid.h"
#include "../Renderer/ModelVertex.h"
#include <cmath>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace {
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
} // namespace

void RegisterTerrainTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Terrain: decode checks the sample count and base64", TerrainDecode});
    tests.push_back({"Terrain: height matches the mesh triangles", TerrainMatchesMesh});
    tests.push_back({"Terrain: slope normal and height range", TerrainNormalRange});
}
