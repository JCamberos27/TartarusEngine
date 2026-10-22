#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

// A decal palette (.decalpalette JSON, paletteVersion 1): a weighted set of .decal assets the Decal
// Brush paints from and the auto-grime pass places (docs/DECALS.md, "Editor tools"). Paths are
// project-relative, like a scene's asset references.
//
// {
//   "paletteVersion": 1,
//   "name": "Squat",
//   "description": "Peeling plaster, flaking paint and the odd tag.",
//   "density": 1.0,                   // optional: auto-grime density multiplier (default 1)
//   "entries": [
//     { "decal": "assets/Decals/Grime/PeelingPaint/DA_PeelingPaint_007.decal",
//       "weight": 3,                 // relative odds of being picked
//       "scale": [0.8, 1.4],         // times the asset's default size, min / max
//       "opacity": [0.7, 1.0],       // overall opacity, min / max
//       "tint": [1, 1, 1],           // multiplied into the decal's tint
//       "sites": ["wall", "base"] }  // where auto-grime may use it; none = anywhere
//   ]
// }
namespace DecalSites {
// Where on a room a decal is wanted. A palette entry names the ones it suits; an empty set suits all.
enum : uint32_t {
    Wall = 1u << 0,    // open wall
    Base = 1u << 1,    // wall meets floor
    Corner = 1u << 2,  // two walls meet
    Window = 1u << 3,  // under a window or opening
    Door = 1u << 4,    // round a door frame
    Pipe = 1u << 5,    // below a pipe or other protrusion
    Ceiling = 1u << 6, // ceiling and the wall under it
    Floor = 1u << 7,   // floor patches
    All = 0xFFu,
};
const char* Name(uint32_t site);            // one bit's name ("wall", "base", ...), "" for anything else
uint32_t FromName(const std::string& name); // 0 when unknown
} // namespace DecalSites

struct DecalPaletteEntry {
    std::string DecalPath; // project-relative .decal
    float Weight = 1.0f;
    glm::vec2 Scale{1.0f, 1.0f};   // min, max, times the asset's default size
    glm::vec2 Opacity{1.0f, 1.0f}; // min, max
    glm::vec3 Tint{1.0f};
    uint32_t Sites = 0; // DecalSites bits; 0 = any
};

struct DecalPalette {
    std::string Path; // absolute path to the .decalpalette file
    std::string Name; // display name (the file's "name", else its stem)
    std::string Description;
    float Density = 1.0f; // auto-grime: scales how many sites it places (0.25..2); 1 = the default look
    bool Missing = false;
    std::vector<DecalPaletteEntry> Entries;

    // Total weight of the entries that suit `site` (all of them for site 0).
    float TotalWeight(uint32_t site = 0) const;
    // The entry for a roll in [0, 1) among those that suit `site` (site 0: every entry), by weight;
    // -1 when none suits. Deterministic: the same roll always picks the same entry.
    int Pick(float roll, uint32_t site = 0) const;

    // Forgiving, like DecalAsset::Load: a missing key keeps its default, a wrong type keeps its
    // default with a Console warning, unknown keys are ignored, a broken file comes back Missing.
    static std::shared_ptr<DecalPalette> Load(const std::string& path);
    // Stable: keys in schema order, written atomically, so load -> save is byte-identical.
    bool Save() const;
};
