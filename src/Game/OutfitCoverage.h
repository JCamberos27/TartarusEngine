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
    float Inward = 0.03f;  // and how far the layer under it may poke out through it
};

// 1 per body vertex the cloth covers. Vertices of no triangle stay 0.
std::vector<std::uint8_t> Covered(const Mesh& body, const Mesh& cloth, const Settings& settings = {});

// How far each body vertex sits outside the cloth - poking through it - in metres, 0 where it doesn't:
// looking back along its normal (up to `maxDepth`), the first cloth it meets faces the same way it does.
std::vector<float> PokeDepth(const Mesh& body, const Mesh& cloth, float maxDepth);

// Uncovers the edge: a vertex stays covered only if every vertex it shares a triangle with is too,
// `rings` times over - so a hem or a cuff never opens a hole where the cloth ends.
void Erode(const Mesh& body, std::vector<std::uint8_t>& covered, int rings = 1);

// What the game hides of `under` while `over` is worn over it (OutfitSystem::UpdateHiding; the audit
// checks the same): Covered then Erode, plus everything poking out through it up to kPokeReach.
constexpr float kPokeReach = 0.10f;
std::vector<std::uint8_t> Hidden(const Mesh& under, const Mesh& over);

// The mask as bits, 32 vertices per word (the shader's layout).
std::vector<std::uint32_t> Pack(const std::vector<std::uint8_t>& covered);

// Area-weighted vertex normals (0 for a vertex of no triangle).
std::vector<glm::vec3> VertexNormals(const Mesh& mesh);

} // namespace OutfitCoverage
