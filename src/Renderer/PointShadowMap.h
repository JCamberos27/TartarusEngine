#pragma once

// Cube-map shadow maps for point lights (#119). Companion to SpotShadowMap.
//
// One GL_TEXTURE_CUBE_MAP_ARRAY of DEPTH_COMPONENT32F — one cube per slot (the scene's Max point
// shadows setting, #110, up to kMaxPoints), six faces each,
// sampled in the model shader as a samplerCubeArrayShadow. Per casting point light the caller
// renders scene depth into all six faces (BeginFace) with a 90-degree perspective frustum, then
// the shader reconstructs the dominant-axis NDC depth from the fragment->light vector and does a
// hardware depth-compare. Keep the budget small — six depth passes per light is not cheap.
class PointShadowMap {
public:
    // Hard ceiling: the size of ModelFragment.glsl's uPointShadow* arrays. The per-scene budget
    // (World::MaxPointShadows) picks how many cubes are actually allocated and rendered.
    static constexpr int kMaxPoints = 8;

    PointShadowMap() = default;
    ~PointShadowMap();
    PointShadowMap(const PointShadowMap&) = delete;
    PointShadowMap& operator=(const PointShadowMap&) = delete;

    // Lazily (re)creates the depth cube array when the resolution or cube count changes.
    void Configure(int resolution, int cubes);

    // Binds the shadow FBO targeting cube `slot`'s face `face` (0..5 = +X,-X,+Y,-Y,+Z,-Z),
    // sets the viewport, clears its depth. Does NOT restore the previous framebuffer.
    void BeginFace(int slot, int face) const;

    unsigned int DepthCubeArray() const { return m_DepthArray; }
    int Resolution() const { return m_Resolution; }
    int Cubes() const { return m_Cubes; }

    void Release();

private:
    unsigned int m_DepthArray = 0;
    unsigned int m_Fbo = 0;
    int m_Resolution = 0;
    int m_Cubes = 0;
    mutable bool m_CompleteChecked = false; // audit #358 — one-shot FBO completeness check in Begin()
};
