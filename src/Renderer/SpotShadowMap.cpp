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
}

void SpotShadowMap::Configure(int resolution) {
    resolution = std::clamp(resolution, 256, 4096);
    if (m_DepthArray != 0 && resolution == m_Resolution) return;

    Release();
    m_Resolution = resolution;
    m_CompleteChecked = false; // re-check after a resolution change (audit #358)

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_DepthArray);
    glTextureStorage3D(m_DepthArray, 1, GL_DEPTH_COMPONENT32F, resolution, resolution, kMaxSpots);
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

    if (!m_Fbo || !m_DepthArray)
        Log::Error("SpotShadowMap: failed to create depth array (" + std::to_string(resolution) + ")");
}

void SpotShadowMap::Begin(int i) const {
    i = std::clamp(i, 0, kMaxSpots - 1);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    glNamedFramebufferTextureLayer(m_Fbo, GL_DEPTH_ATTACHMENT, m_DepthArray, 0, i);
    if (!m_CompleteChecked) { // audit #358 — layered FBO: check once, after a layer is attached
        m_CompleteChecked = true;
        GLFramebufferCheck::Complete("SpotShadowMap", m_Fbo, m_Resolution, m_Resolution);
    }
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}
