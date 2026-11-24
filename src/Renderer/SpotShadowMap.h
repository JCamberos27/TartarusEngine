#pragma once
#include <glm/glm.hpp>
#include <array>
#include "DepthArrayTarget.h"
#include "ShadowLimits.h"

// Perspective shadow maps for spot lights (#119).
//
// One GL_TEXTURE_2D_ARRAY of DEPTH_COMPONENT32F layers — one per shadow-casting spot. The layer
// count is the scene's Max spot shadows setting (#110), up to kMaxSpots. Each frame the caller computes a light view-projection per casting spot (a
// perspective frustum matching the cone), renders scene depth into that layer via Begin(i),
// and the model shader samples the array as a sampler2DArrayShadow in the spot lighting branch.
//
// Point-light (cube-map) shadows are a separate follow-up; this class is spot-only.
class SpotShadowMap {
public:
    // Hard ceiling: the size of ModelFragment.glsl's uSpotShadow* arrays. The per-scene budget
    // (World::MaxSpotShadows) picks how many layers are actually allocated and rendered.
    static constexpr int kMaxSpots = ShadowLimits::kMaxSpots;

    // Spot PCF (fix-3): the filter radius in spot-map texels for a light's Shadow Softness (the shader's
    // uSpotShadowSoftness[]). Softness <= 0 (or NaN) returns 0 = one hard compare. Otherwise 1 x softness texels,
    // clamped to [0.5, 2.5] (the tent PCF's continuous reach, as for the points): a spot texel is ~3.5x a point cube texel (80-degree spot: 1.7 cm at 2.5 m), so x1 puts a
    // ceiling spot's penumbra close to a point light's at the same Softness (kitchen 1.3 -> ~4.4 cm, hall 2.5 -> 8.5 cm).
    static constexpr float kKernelPerSoftness = 1.0f;
    static constexpr float kKernelMinTexels = 0.5f;
    static constexpr float kKernelMaxTexels = 2.5f;
    static float KernelTexels(float softness);

    SpotShadowMap() = default;
    SpotShadowMap(const SpotShadowMap&) = delete;
    SpotShadowMap& operator=(const SpotShadowMap&) = delete;

    // Lazily (re)creates the depth array when the resolution or layer count changes.
    void Configure(int resolution, int layers);

    // Binds the shadow FBO targeting layer `i`, sets the viewport, clears its depth. Does NOT
    // restore the previous framebuffer — the caller captured and restores it.
    void Begin(int i) const;

    // Static-caster cache, one layer per spot in a second array of the same format. A spot whose
    // only changes are animated casters draws its static casters once into BeginStatic(i), then
    // each frame BeginFromStatic(i) copies that depth into the live layer (leaving it bound, not
    // cleared) and only the animated casters are drawn over it. The copy covers texels [x0, x1) x
    // [y0, y1) only - where the animated casters are and were - since a whole layer is 16 MB.
    void BeginStatic(int i) const;
    void BeginFromStatic(int i, int x0, int y0, int x1, int y1) const;

    unsigned int DepthArray() const { return m_Target.Texture(); }
    int Resolution() const { return m_Resolution; }
    int Layers() const { return m_Layers; }

private:
    DepthArrayTarget m_Target; // the layer array, its FBOs and the lazily made static cache
    int m_Resolution = 0;
    int m_Layers = 0;
};
