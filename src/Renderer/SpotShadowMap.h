#pragma once
#include <glm/glm.hpp>
#include <array>

// Perspective shadow maps for spot lights (#119).
//
// One GL_TEXTURE_2D_ARRAY of DEPTH_COMPONENT32F layers — one per shadow-casting spot, up to
// kMaxSpots. Each frame the caller computes a light view-projection per casting spot (a
// perspective frustum matching the cone), renders scene depth into that layer via Begin(i),
// and the model shader samples the array as a sampler2DArrayShadow in the spot lighting branch.
//
// Point-light (cube-map) shadows are a separate follow-up; this class is spot-only.
class SpotShadowMap {
public:
    static constexpr int kMaxSpots = 4;

    SpotShadowMap() = default;
    ~SpotShadowMap();
    SpotShadowMap(const SpotShadowMap&) = delete;
    SpotShadowMap& operator=(const SpotShadowMap&) = delete;

    // Lazily (re)creates the depth array when the resolution changes.
    void Configure(int resolution);

    // Binds the shadow FBO targeting layer `i`, sets the viewport, clears its depth. Does NOT
    // restore the previous framebuffer — the caller captured and restores it.
    void Begin(int i) const;

    unsigned int DepthArray() const { return m_DepthArray; }
    int Resolution() const { return m_Resolution; }

    void Release();

private:
    unsigned int m_DepthArray = 0;
    unsigned int m_Fbo = 0;
    int m_Resolution = 0;
};
