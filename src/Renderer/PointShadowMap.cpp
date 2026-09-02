#include "PointShadowMap.h"
#include "Log.h"
#include "gl.h"

#include <algorithm>
#include <string>

PointShadowMap::~PointShadowMap() { Release(); }

void PointShadowMap::Release() {
    if (m_Fbo)        { glDeleteFramebuffers(1, &m_Fbo);    m_Fbo = 0; }
    if (m_DepthArray) { glDeleteTextures(1, &m_DepthArray); m_DepthArray = 0; }
}

void PointShadowMap::Configure(int resolution) {
    resolution = std::clamp(resolution, 256, 2048);
    if (m_DepthArray != 0 && resolution == m_Resolution) return;

    Release();
    m_Resolution = resolution;

    // A cube-map array is a 2D array with depth = numCubes * 6; layer = cube*6 + face.
    glCreateTextures(GL_TEXTURE_CUBE_MAP_ARRAY, 1, &m_DepthArray);
    glTextureStorage3D(m_DepthArray, 1, GL_DEPTH_COMPONENT32F, resolution, resolution, kMaxPoints * 6);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glCreateFramebuffers(1, &m_Fbo);
    glNamedFramebufferDrawBuffers(m_Fbo, 0, nullptr); // depth only

    if (!m_Fbo || !m_DepthArray)
        Log::Error("PointShadowMap: failed to create depth cube array (" + std::to_string(resolution) + ")");
}

void PointShadowMap::BeginFace(int slot, int face) const {
    slot = std::clamp(slot, 0, kMaxPoints - 1);
    face = std::clamp(face, 0, 5);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    glNamedFramebufferTextureLayer(m_Fbo, GL_DEPTH_ATTACHMENT, m_DepthArray, 0, slot * 6 + face);
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}
