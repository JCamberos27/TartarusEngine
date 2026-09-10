#pragma once

// Tiny 1x1 fallback GL textures bound to sampler units whose "real" texture is absent, so the
// driver never sees texture object 0 on a unit a live program declares a sampler for (audit
// GL-101 / #366 for the colour maps, #357 for the shadow depth samplers).
//
// Without these, KHR_debug reports — every frame — "texture object (0) ... cannot be used for
// texture mapping" (131204) for each unbound material map, and "sampler (0) ... non-depth
// format ... shadow sampler ... undefined behavior" (131222) for each inactive shadow unit.
// The shader's uHas*Map / count uniforms still gate whether the sample result is USED; this
// just keeps the binding itself a valid sampler contract.
//
// Lazily created on first access (needs a live GL context), matching the other GL-owning
// classes in this renderer. All handles live for the process; there is no teardown.
namespace DefaultTextures {

unsigned int White();       // 1x1 RGBA8 (1,1,1,1) — albedo / AO / metallic / roughness / emissive
unsigned int FlatNormal();  // 1x1 RGBA8 (0.5,0.5,1,1) — tangent-space "no perturbation" normal
unsigned int Black();       // 1x1 RGBA8 (0,0,0,1)

// 1x1 depth textures with GL_TEXTURE_COMPARE_MODE = GL_COMPARE_REF_TO_TEXTURE and depth 1.0,
// for the shadow samplers (sampler2DArrayShadow / samplerCubeArrayShadow) when a light has no
// allocated shadow map. Sampling one always returns "fully lit".
unsigned int DepthArray();      // GL_TEXTURE_2D_ARRAY, one layer
unsigned int DepthCubeArray();  // GL_TEXTURE_CUBE_MAP_ARRAY, one cube

} // namespace DefaultTextures
