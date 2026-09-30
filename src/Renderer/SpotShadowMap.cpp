#include "SpotShadowMap.h"
#include "GLFramebufferCheck.h"
#include "Log.h"
#include "gl.h"

#include <algorithm>
#include <string>

SpotShadowMap::~SpotShadowMap() { Release(); }

void SpotShadowMap::Release() {
    if (m_Fbo)        { glDeleteFramebuffers(1, &m_Fbo);    m_Fbo = 0; }
    if (m_DepthArray) { glDeleteTextures(1, &m_DepthArray); m_DepthArray = 0; }
    if (m_StaticFbo)   { glDeleteFramebuffers(1, &m_StaticFbo);   m_StaticFbo = 0; }
    if (m_StaticArray) { glDeleteTextures(1, &m_StaticArray); m_StaticArray = 0; }
}

void SpotShadowMap::Configure(int resolution, int layers) {
    resolution = std::clamp(resolution, 256, 4096);
    layers = std::clamp(layers, 1, kMaxSpots);
    if (m_DepthArray != 0 && resolution == m_Resolution && layers == m_Layers) return;

    Release();
    m_Resolution = resolution;
    m_Layers = layers;
    m_CompleteChecked = false; // re-check after a resolution change (audit #358)

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_DepthArray);
    glTextureStorage3D(m_DepthArray, 1, GL_DEPTH_COMPONENT32F, resolution, resolution, layers);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // outside the map = fully lit
    glTextureParameterfv(m_DepthArray, GL_TEXTURE_BORDER_COLOR, border);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glCreateFramebuffers(1, &m_Fbo);
    glNamedFramebufferDrawBuffers(m_Fbo, 0, nullptr); // depth only

    // Only ever rendered to and copied from, never sampled.
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_StaticArray);
    glTextureStorage3D(m_StaticArray, 1, GL_DEPTH_COMPONENT32F, resolution, resolution, layers);
    glCreateFramebuffers(1, &m_StaticFbo);
    glNamedFramebufferDrawBuffers(m_StaticFbo, 0, nullptr);

    if (!m_Fbo || !m_DepthArray)
        Log::Error("SpotShadowMap: failed to create depth array (" + std::to_string(resolution) + ")");
}

void SpotShadowMap::Begin(int i) const {
    i = std::clamp(i, 0, std::max(m_Layers, 1) - 1);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    glNamedFramebufferTextureLayer(m_Fbo, GL_DEPTH_ATTACHMENT, m_DepthArray, 0, i);
    if (!m_CompleteChecked) { // audit #358 — layered FBO: check once, after a layer is attached
        m_CompleteChecked = true;
        GLFramebufferCheck::Complete("SpotShadowMap", m_Fbo, m_Resolution, m_Resolution);
    }
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void SpotShadowMap::BeginStatic(int i) const {
    i = std::clamp(i, 0, std::max(m_Layers, 1) - 1);
    glBindFramebuffer(GL_FRAMEBUFFER, m_StaticFbo);
    glNamedFramebufferTextureLayer(m_StaticFbo, GL_DEPTH_ATTACHMENT, m_StaticArray, 0, i);
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void SpotShadowMap::BeginFromStatic(int i, int x0, int y0, int x1, int y1) const {
    i = std::clamp(i, 0, std::max(m_Layers, 1) - 1);
    glNamedFramebufferTextureLayer(m_Fbo, GL_DEPTH_ATTACHMENT, m_DepthArray, 0, i);
    x0 = std::clamp(x0, 0, m_Resolution); x1 = std::clamp(x1, 0, m_Resolution);
    y0 = std::clamp(y0, 0, m_Resolution); y1 = std::clamp(y1, 0, m_Resolution);
    if (x0 < x1 && y0 < y1) {
        glNamedFramebufferTextureLayer(m_StaticFbo, GL_DEPTH_ATTACHMENT, m_StaticArray, 0, i);
        glBlitNamedFramebuffer(m_StaticFbo, m_Fbo, x0, y0, x1, y1, x0, y0, x1, y1, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    glViewport(0, 0, m_Resolution, m_Resolution);
}
