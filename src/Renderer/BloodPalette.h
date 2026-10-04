#pragma once
#include <glm/glm.hpp>

// One look for every piece of the blood: the sprays, the stains, the pools, the splats on
// bodies and the flipbook bursts all take their colour and roughness from here, so blood from three packs reads as
// one material. Dark, deep red when fresh, near-black brown dried; satin, not mirror-glossy. Linear albedo.
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
} // namespace BloodPalette
