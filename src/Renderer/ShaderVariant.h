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
// Bit i corresponds to sa.Keywords()[i]; a keyword the shader doesn't declare contributes
// nothing. A material with no lobes active returns 0 — the byte-identical zero-keyword variant.
ShaderVariantKey ShaderVariantKeyFor(const Material& mat, const ShaderAsset& sa);
