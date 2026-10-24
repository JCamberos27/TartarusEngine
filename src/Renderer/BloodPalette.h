#pragma once
#include <glm/glm.hpp>

// One look for every piece of the blood: the sprays, the stains, the pools, the splats on
// bodies and the flipbook bursts all take their colour and roughness from here, so blood from three packs reads as
// one material. Dark, deep red when fresh, near-black brown dried; satin, not mirror-glossy. Linear albedo.
//
// The constants below are a person's blood. A creature bleeds its own (a Look, picked per body at runtime: BloodFx
// stamps each spray, stain and splat with the palette of the body it came out of, and the renderer draws it with that).
namespace BloodPalette {
constexpr glm::vec3 Fresh{0.16f, 0.006f, 0.005f};   // a fresh film or pool
constexpr glm::vec3 Dried{0.045f, 0.014f, 0.010f};  // dried, hours on
constexpr glm::vec3 Thin{0.20f, 0.0075f, 0.006f};   // a thin sheet in the air (the bursts, droplets)
constexpr glm::vec3 Mist{0.09f, 0.0034f, 0.0028f}; // the fine haze
constexpr glm::vec3 Subsurface{0.34f, 0.010f, 0.007f}; // light through the spray's fluid
constexpr float RoughFresh = 0.38f;  // a film on a surface
constexpr float RoughPool = 0.32f;   // a pool (deeper, wetter)
constexpr float RoughDried = 0.75f;
constexpr float RoughSpray = 0.28f;  // the fluid in the air
constexpr float RoughSprite = 0.30f; // the flipbook bursts
constexpr float RoughCloth = 0.35f;  // soaked into clothing / skin

// A whole palette: what one kind of body bleeds.
struct Look {
    glm::vec3 Fresh, Dried, Thin, Mist, Subsurface;
    float RoughFresh, RoughPool, RoughDried, RoughSpray, RoughSprite, RoughCloth;
    float Thickness;   // the spray's fluid (ModelShading's uThickness): lower lets more light through it
    float FilmOpacity; // x a fresh stain's cover: below 1 the surface shows through a thin film
};
// The palettes by index (a body's blood kind). 0 is always a person's.
enum Kind : int { kHuman = 0, kIchor = 1, kKinds = 2 };

// A person's: exactly the constants above.
constexpr Look Human{Fresh, Dried, Thin, Mist, Subsurface, RoughFresh, RoughPool, RoughDried, RoughSpray, RoughSprite, RoughCloth, 0.55f, 1.0f};
// A spider's ichor: a pale yellow-green, a little translucent while it's fresh (the film lets the surface through,
// the spray lets the light through), drying to a dull olive crust.
constexpr Look Ichor{{0.42f, 0.52f, 0.075f},   // fresh
                     {0.075f, 0.085f, 0.014f}, // dried: olive
                     {0.50f, 0.62f, 0.09f},    // thin: a sheet in the air
                     {0.26f, 0.32f, 0.06f},    // mist
                     {0.55f, 0.72f, 0.12f},    // subsurface
                     0.30f, 0.24f, 0.68f, 0.20f, 0.24f, 0.28f,
                     0.30f, 0.85f};

// The palette of `kind` as the game runs (a tuning tool may change it; out of range: a person's).
inline Look& Runtime(int kind) {
    static Look table[kKinds] = {Human, Ichor};
    return table[kind >= 0 && kind < kKinds ? kind : 0];
}
inline const Look& Of(int kind) { return Runtime(kind); }
} // namespace BloodPalette
