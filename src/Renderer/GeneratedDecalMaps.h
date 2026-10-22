#pragma once
#include "DecalTextureArrays.h"

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Decal maps the engine makes itself (docs/DECALS.md, "Runtime decals"): blood stains and bullet holes
// re-packed from the blood and Knife packs into the decal arrays' layout, the procedural bullet hole, and
// a spray-can piece's reveal order. They are written once per machine as PNGs under the project's
// Library/DecalMaps (git-ignored, like Library/Textures) and from there go through the regular decal
// texture path (decode, BCn, TextureCache). A key names one map set; bump its version suffix when what
// makes it changes.
class GeneratedDecalMaps {
public:
    // One map set, rows top first. Albedo RGBA (sRGB colour, opacity), Normal RGBA (tangent xy in rg; may
    // be empty), Aux one channel (may be empty). An aux-only set (a reveal order) has no albedo. Square or not: the decal slot resamples it to its size.
    struct Images {
        int Width = 0, Height = 0;
        std::vector<uint8_t> Albedo, Normal, Aux;
    };
    using Job = std::function<Images()>;      // runs on a worker
    using Prepare = std::function<Job()>;     // runs on the main thread (GL readbacks), only when the files are missing

    static GeneratedDecalMaps& Get(); // deliberately leaked, like the other renderer singletons

    // The maps for `key` once they are on disk (Tier = `tier`); null while they are made. The first
    // call for a key whose files are missing runs `prepare` and its job on a worker.
    const DecalTextureArrays::Source* Find(const std::string& key, int tier, const Prepare& prepare);
    // Waits for every job in flight (tests, shutdown).
    void Flush();
    // The folder and a map's path ("Albedo", "Normal", "Aux").
    static std::string Dir();
    static std::string MapPath(const std::string& key, const char* map);
    // Writes a set (PNGs, atomically, then a <key>.done marker): what a job's result becomes. False on an I/O
    // error, or with neither an albedo nor an aux map.
    static bool Write(const std::string& key, const Images& images);
    static std::string DonePath(const std::string& key);

private:
    struct Entry {
        DecalTextureArrays::Source Src;
        bool Ready = false, Failed = false;
        std::future<bool> Pending;
    };
    std::unordered_map<std::string, std::unique_ptr<Entry>> m_Entries;
};

// The image makers behind the runtime decals: pure CPU, unit-tested.
namespace DecalBake {
// sRGB <-> linear for one 8-bit channel.
uint8_t LinearToSrgb8(float linear);
float Srgb8ToLinear(uint8_t v);

// A KriptoFX blood set (assets/Effects/Blood/decals/<set>_norm.png and _mask.png, RGBA8 w x h): albedo is the
// stain's shading (the pooled core darker, as the old blood decal pass drew it) under the palette tint, the coverage
// in alpha (the old pass's thin film also darkened what it covered by the blood's own colour, so it read as opaque); the normal its rg slope; aux the reveal order (mask red), which the cutout eats.
GeneratedDecalMaps::Images BloodSet(const uint8_t* norm, const uint8_t* mask, int w, int h);

// One Knife library layer (RGBA8 colour, RG8 normal, size x size, cols x rows cells) as a runtime decal's maps.
enum class KnifeMode {
    BloodMask,   // the cell's alpha is a reveal mask; colour from the palette (the old pass's kEntryMask)
    BloodAlbedo, // the pack's own stain image, its hue replaced by the palette's (Real Blood pools, prints)
    Hole,        // a bullet hole's own broken surface; aux is the distance out from the cell's centre (its rim cut)
};
GeneratedDecalMaps::Images KnifeLayer(const uint8_t* rgba, const uint8_t* rg, int size, int cols, int rows, KnifeMode mode);
// The aux value of a hole cell at `r` (0 centre .. 1 the cell's half-width): 1 - r / 2.
inline float HoleAux(float r) { return 1.0f - r * 0.5f; }
// Reveal threshold / band that cuts a hole's rim at `rim` cell half-widths (the old pass's 1 - smoothstep(0.66 rim, rim, r)).
inline float HoleRimThreshold(float rim) { return 1.0f - 0.5f * rim; }
inline float HoleRimBand(float rim) { return 0.17f * rim; }

// The procedural bullet hole (WeaponFx's): a black bore with a ragged edge in a darkened, crushed ring;
// 2 x 2 cells (four seeds) of `cell` texels. The box is 5 hole radii across.
GeneratedDecalMaps::Images ProceduralHole(int cell);
constexpr float kProceduralHoleSpan = 5.0f; // box width in hole radii
} // namespace DecalBake
