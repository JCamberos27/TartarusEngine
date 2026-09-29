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
// looking back along its normal (up to `maxDepth`), the first cloth it meets faces the same way it does,
// and, kFarWallDepth or more in, isn't within kFarWallSlack of the body's own far side (then it's cloth
// sunk into that side).
constexpr float kFarWallSlack = 0.02f;
constexpr float kFarWallDepth = 0.05f; // ... only for cloth this deep in: a limb's far side, not an ear's back

std::vector<float> PokeDepth(const Mesh& body, const Mesh& cloth, float maxDepth);

// Uncovers the edge: a vertex stays covered only if every vertex it shares a triangle with is too,
// `rings` times over - so a hem or a cuff never opens a hole where the cloth ends.
void Erode(const Mesh& body, std::vector<std::uint8_t>& covered, int rings = 1);

// Uncovers a band `radius` metres wide inside the edge, whatever the mesh's density: a covered vertex
// stays covered only if it's further than that from every uncovered one (straight-line). One ring of a
// dense mesh is ~1 cm, and a loose sleeve or collar swings further than that off the skin it was worked
// out against - the hidden skin came out from under it as a hole. `only` (when given): just the vertices
// it's 1 for are uncovered - the rest stay as they are.
void ErodeWithin(const Mesh& body, std::vector<std::uint8_t>& covered, float radius,
                 const std::vector<std::uint8_t>* only = nullptr);

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
// checks the same): Covered then Erode and a kEdgeBand-wide ErodeWithin, plus everything poking out through it up to kPokeReach. `exposed`
// (the head: skin that carries on past the clothing, in view) keeps only what's Backed too - with nothing
// drawn behind it, a hidden neck over a collar's rim is a hole (the torso's neck is hidden as well), where a
// waistband's underpants have the hips' cloth behind them.
constexpr float kPokeReach = 0.10f;
// The band inside the cloth's edge that stays drawn (ErodeWithin). Measured on the Quantum Hawaiian
// shirt's loose sleeve: one ring (~1 cm on the arms) left 13-30 hidden arm vertices out in the open once
// the arm swung down 20-50 degrees from the bind pose; 3 cm left none.
constexpr float kEdgeBand = 0.04f;
// `rigid` (headwear riding the head bone: a balaclava, a hat under a hood): only what pokes out through
// `over` goes. It can't deform out from under the cloth, and the covered rest can be in view - the
// balaclava's sides through a hood's face opening, hidden, showed the ears behind them.
std::vector<std::uint8_t> Hidden(const Mesh& under, const Mesh& over, bool exposed = false, bool rigid = false);

// The mask as bits, 32 vertices per word (the shader's layout).
std::vector<std::uint32_t> Pack(const std::vector<std::uint8_t>& covered);

// Area-weighted vertex normals (0 for a vertex of no triangle).
std::vector<glm::vec3> VertexNormals(const Mesh& mesh);

} // namespace OutfitCoverage
