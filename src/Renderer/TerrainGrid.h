#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

struct ModelVertex;

// A terrain's height grid (TerrainComponent): ResX x ResZ samples spread evenly over SizeX x SizeZ metres,
// centred on the entity's origin (local X -SizeX/2..SizeX/2, local Z the same), row-major along X.
// The scene stores the samples as 16-bit values between HeightMin and HeightMax, base64-encoded, so a tool
// (tools/scenes/build_forest.py) can write them and seat its placements on the very same surface.
//
// Every cell is two triangles split along the diagonal from (i+1, j) to (i, j+1). Height() interpolates on
// those triangles, so it matches the mesh (and its collider) exactly, not a bilinear blend of the corners.
struct TerrainGrid {
    int ResX = 0, ResZ = 0;
    float SizeX = 0.0f, SizeZ = 0.0f;
    float MinHeight = 0.0f, MaxHeight = 0.0f; // of the decoded samples
    std::vector<float> Heights;               // ResX * ResZ, metres

    bool Valid() const { return ResX >= 2 && ResZ >= 2 && SizeX > 0.0f && SizeZ > 0.0f && (int)Heights.size() == ResX * ResZ; }
    float CellX() const { return SizeX / (float)(ResX - 1); }
    float CellZ() const { return SizeZ / (float)(ResZ - 1); }
    float At(int i, int j) const { return Heights[(size_t)j * (size_t)ResX + (size_t)i]; }

    // Height at a local (x, z), clamped to the grid's edge outside it.
    float Height(float x, float z) const;
    // Upward unit normal of the triangle under (x, z).
    glm::vec3 Normal(float x, float z) const;
    // Lowest and highest surface over a local rectangle (every sample inside plus the corners).
    void RangeIn(float x0, float z0, float x1, float z1, float& outMin, float& outMax) const;

    // The render / collision mesh: one vertex per sample (smooth normals from the neighbours), UVs in metres
    // divided by uvTile, tangents along +X.
    void BuildMesh(float uvTile, std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices) const;

    // From / to the scene's encoding. Decode fails (false) on a size mismatch or bad base64.
    static bool Decode(int resX, int resZ, float sizeX, float sizeZ, float heightMin, float heightMax,
                       const std::string& base64, TerrainGrid& out);
    static std::string Encode(const std::vector<float>& heights, float heightMin, float heightMax);
};
