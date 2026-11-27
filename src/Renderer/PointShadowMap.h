#pragma once
#include "DepthArrayTarget.h"
#include "ShadowLimits.h"

// Cube-map shadow maps for point lights (#119). Companion to SpotShadowMap.
//
// One GL_TEXTURE_CUBE_MAP_ARRAY of DEPTH_COMPONENT32F — one cube per slot (the scene's Max point
// shadows setting, #110, up to kMaxPoints), six faces each,
// sampled in the model shader as a samplerCubeArrayShadow. Per casting point light the caller
// renders scene depth into all six faces (BeginFaceFromStatic) with a 90-degree perspective frustum, then
// the shader reconstructs the dominant-axis NDC depth from the fragment->light vector and does a
// hardware depth-compare. Keep the budget small — six depth passes per light is not cheap.
class PointShadowMap {
public:
    // Hard ceiling: the size of ModelFragment.glsl's uPointShadow* arrays. The per-scene budget
    // (World::MaxPointShadows) picks how many cubes are actually allocated and rendered.
    static constexpr int kMaxPoints = ShadowLimits::kMaxPoints;

    // Point PCF (fix-3 S2): the filter radius in cube texels for a light's Shadow Softness, which is what the shader's
    // uPointShadowSoftness[] carries. Softness <= 0 (or NaN) returns 0 = the old single hard compare. Otherwise
    // kKernelPerSoftness x softness, clamped to [kKernelMinTexels, kKernelMaxTexels]: x3 puts a point light's edge at
    // the same Softness about as wide in the world as a ceiling spot's (cube texels are ~3.5x smaller than an
    // 80-degree spot's 2048 map: 0.49 vs 1.7 cm at 2.5 m), e.g. Softness 1.3 -> ~3.8 cm vs 4.0 cm. Capped at 2.5 texels
    // (Softness ~0.83, ~1.2 cm radius at 2.5 m): the tent PCF's taps must stay <= 1 texel apart to overlap (each is a
    // 1-texel bilinear ramp), and its 4x4 grid reaches 1.5 x spacing + 1 = 2.5 texels at that limit (from review).
    static constexpr float kKernelPerSoftness = 3.0f;
    static constexpr float kKernelMinTexels = 0.5f;
    static constexpr float kKernelMaxTexels = 2.5f;
    static float KernelTexels(float softness);

    PointShadowMap() = default;
    PointShadowMap(const PointShadowMap&) = delete;
    PointShadowMap& operator=(const PointShadowMap&) = delete;

    // Lazily (re)creates the depth cube array when the resolution or cube count changes.
    void Configure(int resolution, int cubes);

    // The static-caster cache (a second cube array): BeginStaticFace clears and binds cube `slot`'s face
    // (0..5 = +X,-X,+Y,-Y,+Z,-Z) in it; BeginFaceFromStatic copies that cached face into the live map and binds the
    // live face for the moving casters drawn on top (like SpotShadowMap's static layer). Both set the viewport and
    // do NOT restore the previous framebuffer.
    void BeginStaticFace(int slot, int face) const;
    void BeginFaceFromStatic(int slot, int face) const;

    unsigned int DepthCubeArray() const { return m_Target.Texture(); }
    int Resolution() const { return m_Resolution; }
    int Cubes() const { return m_Cubes; }

private:
    DepthArrayTarget m_Target; // the cube array (layer = cube*6 + face), its FBOs and the lazily made static cache
    int m_Resolution = 0;
    int m_Cubes = 0;
};
