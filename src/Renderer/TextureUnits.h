#pragma once

// The engine's texture-unit map for the model/material programs, in one place.
//
// GL texture units are one pool of GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS (>= 96 on any GL 4.3+ driver,
// llvmpipe included); GL_MAX_TEXTURE_IMAGE_UNITS (32 on our targets) only caps how many DIFFERENT
// samplers one fragment stage reads, not which unit numbers they use. 0-31 used to be treated as the
// whole space, so once it filled up the cover layer had to share unit 20 with _ThicknessMap. Units 32
// and up are now used too, so no two samplers of one program share a unit; new features take the next
// free unit from the extended range below (and must still keep each variant at <= 32 active samplers,
// which the GL linker enforces).
namespace TextureUnits {

// 0-31: the original range.
constexpr int DepthAlbedo    = 0;  // depth / shadow programs' alpha-test albedo only
constexpr int MaterialFirst  = 1;  // material maps 1..7 ...
constexpr int SunShadow      = 8;
constexpr int SpotShadow     = 9;
constexpr int PointShadow    = 10;
constexpr int Irradiance     = 11;
constexpr int Prefiltered    = 12;
constexpr int BrdfLut        = 13;
constexpr int OpaqueColor    = 14;
constexpr int Ssao           = 15;
constexpr int MaterialResume = 16; // ... then 16..20
constexpr int Grunge         = 21; // Decals::UNIT_GRUNGE
constexpr int DecalAlbedo    = 22; // Decals::UNIT_ALBEDO .. UNIT_AUX = 22..25
constexpr int ProbeCubes     = 26;
constexpr int VolumeFog      = 27;
constexpr int Spare28        = 28; // free
constexpr int PuddleSsr      = 29;
constexpr int Aerial         = 30; // SkyAtmosphere::kAerialUnit
constexpr int CloudShadow    = 31; // SkyAtmosphere::kCloudShadowUnit

// 32+: the extended range.
constexpr int SsrDepth       = 32; // PuddleSsr.comp only (raw binds, cache invalidated after)
constexpr int SsrPrevColor   = 33; // PuddleSsr.comp only
constexpr int CoverAlbedo    = 34; // ModelShading.glsl _COVER layout(binding = 34/35/36)
constexpr int CoverNormal    = 35;
constexpr int CoverMask      = 36;
constexpr int ExtendedMaterialFirst = 37; // material maps past 20 (custom shaders) continue here

// Size of GLStateCache's per-unit shadow; units at or past it are bound uncached. GL 4.3+ guarantees 96.
constexpr int Count = 48;

} // namespace TextureUnits
