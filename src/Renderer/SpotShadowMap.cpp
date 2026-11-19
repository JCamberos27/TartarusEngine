#include "SpotShadowMap.h"
#include "Log.h"
#include "gl.h"

#include <algorithm>
#include <string>

void SpotShadowMap::Configure(int resolution, int layers) {
    resolution = std::clamp(resolution, 256, 4096);
    layers = std::clamp(layers, 1, kMaxSpots);
    // Grow-only for a given resolution: the array is sized to the slots in use, and never shrinks back
    // (a shrink then regrow would redraw every cache again for the sake of a few MB).
    if (m_Target.Texture() != 0 && resolution == m_Resolution) layers = std::max(layers, m_Layers);
    if (m_Target.Texture() != 0 && resolution == m_Resolution && layers == m_Layers) return;

    m_Resolution = resolution;
    m_Layers = layers;

    DepthArrayTarget::Desc d;
    d.target = GL_TEXTURE_2D_ARRAY;
    d.resolution = resolution;
    d.layers = layers;
    d.borderClamp = true;
    d.liveName = "SpotShadowMap (live)";
    d.staticName = "SpotShadowMap (static cache)";
    m_Target.Create(d, this);

    if (!m_Target.Fbo() || !m_Target.Texture())
        Log::Error("SpotShadowMap: failed to create depth array (" + std::to_string(resolution) + ")");
}

void SpotShadowMap::Begin(int i) const { m_Target.BeginLive(i); }

// The static-caster cache (DepthArrayTarget::EnsureStatic) is made on first use: a scene whose spots all move (the
// flashlight) never pays for it.
void SpotShadowMap::BeginStatic(int i) const { m_Target.BeginStatic(i); }

void SpotShadowMap::BeginFromStatic(int i, int x0, int y0, int x1, int y1) const { m_Target.BeginFromStatic(i, x0, y0, x1, y1); }
