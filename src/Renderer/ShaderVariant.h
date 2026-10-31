#pragma once
#include "ShaderAsset.h" // ShaderVariantKey

struct Material;

// Derive the keyword bitmask for `sa` from a material's authored lobe state (audit #354):
//
//   _CLEARCOAT         <- ClearCoat > 0
//   _ANISO             <- Anisotropy != 0
//   _SHEEN             <- any Sheen channel > 0
//   _SUBSURFACE        <- Material::SubsurfaceEnabled
//   _TRANSMISSION      <- TransmissionStrength > 0
//   _REFLECTION_PROBES <- Material::ReflectionProbes
//
// plus every custom keyword in Material::ShaderKeywords (#104).
// Bit i corresponds to sa.Keywords()[i]; a keyword the shader doesn't declare contributes
// nothing. A material with no lobes active returns 0 — the byte-identical zero-keyword variant.
ShaderVariantKey ShaderVariantKeyFor(const Material& mat, const ShaderAsset& sa);
// The same, worked out once per frame per material (Material::Hash()'s rule: an edit shows from the next
// frame). The scene draw asks for it per mesh per pass; the keyword string compares added up.
ShaderVariantKey ShaderVariantKeyForCached(const Material& mat, const ShaderAsset& sa);

// Engine variant bits (the top byte of a key; material keywords use the low 24): code the scene draw knows a
// program will never run, compiled out of ModelFragment.glsl so the shader the GPU runs is smaller - fewer registers,
// more fragments in flight. Every bit only removes a block whose uniform-driven early out would have skipped it, so
// the image is the same. A key without them (the default program, previews) keeps the whole shader.
//   _ENGINE_NO_LOCAL_LIGHTS <- the frame uploads no point or spot light (the clustered light loop runs 0 times)
//   _ENGINE_NO_DECALS       <- the view has no clustered decal (ApplyDecals returns at uDecalCount 0)
//   _ENGINE_NO_DEBUG_VIEW   <- the view draws no debug mode (uDebugView 0)
//   _ENGINE_NO_GRUNGE       <- the material's Grunge Strength is 0 (ApplyGrunge returns)
//   _ENGINE_NO_PARALLAX     <- the material's Parallax Scale is 0 (no height-map march)
// TARTARUS_NO_ENGINE_VARIANTS=1 turns them all off (the whole shader everywhere).
constexpr ShaderVariantKey kVariantNoLocalLights = 1u << 31;
constexpr ShaderVariantKey kVariantNoDecals      = 1u << 30;
constexpr ShaderVariantKey kVariantNoDebugView   = 1u << 29;
constexpr ShaderVariantKey kVariantNoGrunge      = 1u << 28;
constexpr ShaderVariantKey kVariantNoParallax    = 1u << 27;
constexpr ShaderVariantKey kVariantEngineMask    = 0xFF000000u;
constexpr int kVariantMaterialKeywords = 24; // material keyword bits 0..23
// The material's own engine bits (grunge, parallax); 0 when engine variants are off.
ShaderVariantKey ShaderVariantEngineBits(const Material& mat);
// Whether engine variant bits are used at all (TARTARUS_NO_ENGINE_VARIANTS unset).
bool EngineVariantsEnabled();
