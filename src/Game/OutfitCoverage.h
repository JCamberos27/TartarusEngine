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

// Whether hiding each vertex leaves something to see in its place: from every way it can be seen (a look
// out over its hemisphere - kBackedRays of them, up to `reach` metres - that meets neither the cloth nor the
// body), the view carries on to the cloth or the body's outside within kBackedBehind behind it. Skin past a
// collar's rim fails (the view goes on down into the gap: a hole); skin poking through a shirt front passes.
// Worked out only where `only` is 1 (empty = everywhere); 0 elsewhere.
constexpr int kBackedRays = 32;
constexpr float kBackedReach = 0.25f;
constexpr float kBackedBehind = 0.04f;
std::vector<std::uint8_t> Backed(const Mesh& body, const Mesh& cloth, const std::vector<std::uint8_t>& only = {},
                                 float reach = kBackedReach);

// What the game hides of `under` while `over` is worn over it (OutfitSystem::UpdateHiding; the audit
// checks the same): Covered then Erode, plus everything poking out through it up to kPokeReach. `exposed`
// (the head: skin that carries on past the clothing, in view) keeps only what's Backed too - with nothing
// drawn behind it, a hidden neck over a collar's rim is a hole (the torso's neck is hidden as well), where a
// waistband's underpants have the hips' cloth behind them.
constexpr float kPokeReach = 0.10f;
std::vector<std::uint8_t> Hidden(const Mesh& under, const Mesh& over, bool exposed = false);

// The mask as bits, 32 vertices per word (the shader's layout).
std::vector<std::uint32_t> Pack(const std::vector<std::uint8_t>& covered);

// Area-weighted vertex normals (0 for a vertex of no triangle).
std::vector<glm::vec3> VertexNormals(const Mesh& mesh);

} // namespace OutfitCoverage
