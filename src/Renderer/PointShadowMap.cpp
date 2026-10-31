#include "GpuMemTrack.h"
#include "PointShadowMap.h"
#include "GLFramebufferCheck.h"
#include "Log.h"
#include "gl.h"

#include <algorithm>
#include <string>

PointShadowMap::~PointShadowMap() { Release(); }

void PointShadowMap::Release() {
    if (m_Fbo)        { glDeleteFramebuffers(1, &m_Fbo);    m_Fbo = 0; }
    if (m_DepthArray) { glDeleteTextures(1, &m_DepthArray); m_DepthArray = 0; }
    if (m_StaticFbo)   { glDeleteFramebuffers(1, &m_StaticFbo);  m_StaticFbo = 0; }
    if (m_StaticArray) { glDeleteTextures(1, &m_StaticArray);    m_StaticArray = 0; }
}

void PointShadowMap::Configure(int resolution, int cubes) {
    resolution = std::clamp(resolution, 256, 2048);
    cubes = std::clamp(cubes, 1, kMaxPoints);
    if (m_DepthArray != 0 && resolution == m_Resolution && cubes == m_Cubes) return;

    Release();
    m_Resolution = resolution;
    m_Cubes = cubes;
    m_CompleteChecked = false; // re-check after a resolution change (audit #358)

    // A cube-map array is a 2D array with depth = numCubes * 6; layer = cube*6 + face.
    glCreateTextures(GL_TEXTURE_CUBE_MAP_ARRAY, 1, &m_DepthArray);
    glTextureStorage3D(m_DepthArray, 1, GL_DEPTH_COMPONENT32F, resolution, resolution, cubes * 6);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTextureParameteri(m_DepthArray, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glCreateFramebuffers(1, &m_Fbo);
    glNamedFramebufferDrawBuffers(m_Fbo, 0, nullptr); // depth only

    // The static casters' depth, never sampled: only copied from.
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_StaticArray);
    glTextureStorage3D(m_StaticArray, 1, GL_DEPTH_COMPONENT32F, resolution, resolution, cubes * 6);
    GpuMemTrack::Set("PointShadowMap (dynamic + static)", (size_t)resolution * resolution * cubes * 6 * 4 * 2);
    glCreateFramebuffers(1, &m_StaticFbo);
    glNamedFramebufferDrawBuffers(m_StaticFbo, 0, nullptr);

    if (!m_Fbo || !m_DepthArray)
        Log::Error("PointShadowMap: failed to create depth cube array (" + std::to_string(resolution) + ")");
}

void PointShadowMap::BeginFace(int slot, int face) const {
    slot = std::clamp(slot, 0, std::max(m_Cubes, 1) - 1);
    face = std::clamp(face, 0, 5);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    glNamedFramebufferTextureLayer(m_Fbo, GL_DEPTH_ATTACHMENT, m_DepthArray, 0, slot * 6 + face);
    // audit #358 — a layered depth FBO only reports COMPLETE once a layer is attached, so the
    // check belongs here, once, rather than in Configure().
    if (!m_CompleteChecked) {
        m_CompleteChecked = true;
        GLFramebufferCheck::Complete("PointShadowMap", m_Fbo, m_Resolution, m_Resolution);
    }
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void PointShadowMap::BeginStaticFace(int slot, int face) const {
    slot = std::clamp(slot, 0, std::max(m_Cubes, 1) - 1);
    face = std::clamp(face, 0, 5);
    glBindFramebuffer(GL_FRAMEBUFFER, m_StaticFbo);
    glNamedFramebufferTextureLayer(m_StaticFbo, GL_DEPTH_ATTACHMENT, m_StaticArray, 0, slot * 6 + face);
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void PointShadowMap::BeginFaceFromStatic(int slot, int face) const {
    slot = std::clamp(slot, 0, std::max(m_Cubes, 1) - 1);
    face = std::clamp(face, 0, 5);
    glNamedFramebufferTextureLayer(m_Fbo, GL_DEPTH_ATTACHMENT, m_DepthArray, 0, slot * 6 + face);
    glNamedFramebufferTextureLayer(m_StaticFbo, GL_DEPTH_ATTACHMENT, m_StaticArray, 0, slot * 6 + face);
    glBlitNamedFramebuffer(m_StaticFbo, m_Fbo, 0, 0, m_Resolution, m_Resolution, 0, 0, m_Resolution, m_Resolution,
                           GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    glViewport(0, 0, m_Resolution, m_Resolution);
}
