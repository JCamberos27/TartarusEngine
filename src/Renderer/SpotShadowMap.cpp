#include "SpotShadowMap.h"
#include "GLFramebufferCheck.h"
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
    m_CompleteChecked = false; // re-check after a resolution change (audit #358)

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

void SpotShadowMap::Begin(int i) const {
    i = std::clamp(i, 0, std::max(m_Layers, 1) - 1);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Target.Fbo());
    m_Target.AttachLayer(i);
    if (!m_CompleteChecked) { // audit #358 — layered FBO: check once, after a layer is attached
        m_CompleteChecked = true;
        GLFramebufferCheck::Complete("SpotShadowMap", m_Target.Fbo(), m_Resolution, m_Resolution);
    }
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}

// The static-caster cache (DepthArrayTarget::EnsureStatic) is made on first use: a scene whose spots all move (the
// flashlight) never pays for it.
void SpotShadowMap::BeginStatic(int i) const {
    m_Target.EnsureStatic();
    i = std::clamp(i, 0, std::max(m_Layers, 1) - 1);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Target.StaticFbo());
    m_Target.AttachStaticLayer(i);
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void SpotShadowMap::BeginFromStatic(int i, int x0, int y0, int x1, int y1) const {
    m_Target.EnsureStatic();
    i = std::clamp(i, 0, std::max(m_Layers, 1) - 1);
    m_Target.AttachLayer(i);
    x0 = std::clamp(x0, 0, m_Resolution); x1 = std::clamp(x1, 0, m_Resolution);
    y0 = std::clamp(y0, 0, m_Resolution); y1 = std::clamp(y1, 0, m_Resolution);
    if (x0 < x1 && y0 < y1) {
        m_Target.AttachStaticLayer(i);
        glBlitNamedFramebuffer(m_Target.StaticFbo(), m_Target.Fbo(), x0, y0, x1, y1, x0, y0, x1, y1, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_Target.Fbo());
    glViewport(0, 0, m_Resolution, m_Resolution);
}
