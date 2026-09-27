#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// Which of a body part's vertices a piece of clothing covers (CHARACTER_OUTFITS.md, "skin hiding").
// Pure geometry, in one space (world metres), in the bind pose both models share: each body vertex
// looks along its normal, a little inward and further out, for the cloth. Covered skin isn't drawn,
// so it can't poke through the cloth as the body animates.
namespace OutfitCoverage {

struct Mesh {
    std::vector<glm::vec3> Positions;
    std::vector<unsigned int> Indices; // triangles
};

struct Settings {
    float Outward = 0.05f; // how far outside the skin the cloth may float, metres
    float Inward = 0.02f;  // and how far inside it may sink (where skin would show through)
};

// 1 per body vertex the cloth covers. Vertices of no triangle stay 0.
std::vector<std::uint8_t> Covered(const Mesh& body, const Mesh& cloth, const Settings& settings = {});

// Uncovers the edge: a vertex stays covered only if every vertex it shares a triangle with is too,
// `rings` times over - so a hem or a cuff never opens a hole where the cloth ends.
void Erode(const Mesh& body, std::vector<std::uint8_t>& covered, int rings = 1);

// The mask as bits, 32 vertices per word (the shader's layout).
std::vector<std::uint32_t> Pack(const std::vector<std::uint8_t>& covered);

// Area-weighted vertex normals (0 for a vertex of no triangle).
std::vector<glm::vec3> VertexNormals(const Mesh& mesh);

} // namespace OutfitCoverage
