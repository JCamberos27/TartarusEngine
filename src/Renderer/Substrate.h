#pragma once
#include <glm/glm.hpp>
#include <cstdio>
#include <string>

// A substrate (docs/DECALS.md, "Substrate"): the PBR material under the paint, shown where a peel
// mask says the paint is missing - by a peeling-paint decal, and by a material's Grunge map. Its
// maps go into the decal texture arrays (one slot: albedo, normal, and roughness + AO packed into
// the surface map), so they count against the decal texture budget. Sampled in world space on the
// receiver's dominant axis, so overlapping decals and the Grunge reveal show one continuous wall.
//
// JSON (a .decal's and a material's "substrate" block; SubstrateJson.h):
//   { "albedo": "...", "normal": "", "roughness": "", "ao": "", "tiling": 1.5, "tint": [1, 1, 1],
//     "strength": 1, "roughnessRange": [0, 1], "mirror": false, "crop": [0, 0, 1, 1] }
struct SubstrateDesc {
    std::string Albedo, Normal, Roughness, AO; // project-relative (or absolute); no Albedo = no substrate
    float Tiling = 1.5f;                       // metres per repeat
    glm::vec3 Tint{1.0f};
    float Strength = 1.0f;                     // 0..1: how much of the substrate replaces the paint's look
    glm::vec2 RoughRange{0.0f, 1.0f};          // the roughness map (white without one) remapped to [min, max]
    bool Mirror = false;                       // mirrored repeat (a sheet that doesn't tile on its own)
    glm::vec4 Crop{0.0f, 0.0f, 1.0f, 1.0f};    // the image rect that repeats: x0, y0, x1, y1, v down

    bool Enabled() const { return !Albedo.empty() && Strength > 0.0f; }

    // Identity for the substrate table: the maps plus every value the GPU record carries.
    std::string Key() const {
        char buf[256];
        std::snprintf(buf, sizeof buf, "|%g|%g,%g,%g|%g|%g,%g|%d|%g,%g,%g,%g", Tiling, Tint.r, Tint.g, Tint.b, Strength,
                      RoughRange.x, RoughRange.y, Mirror ? 1 : 0, Crop.x, Crop.y, Crop.z, Crop.w);
        return Albedo + "|" + Normal + "|" + Roughness + "|" + AO + buf;
    }
    bool operator==(const SubstrateDesc& o) const { return Key() == o.Key(); }
    bool operator!=(const SubstrateDesc& o) const { return !(*this == o); }
};
