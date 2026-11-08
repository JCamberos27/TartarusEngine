#include "PointShadowMap.h"
#include "GLFramebufferCheck.h"
#include "Log.h"
#include "gl.h"

#include <algorithm>
#include <string>

void PointShadowMap::Configure(int resolution, int cubes) {
    resolution = std::clamp(resolution, 256, 2048);
    cubes = std::clamp(cubes, 1, kMaxPoints);
    if (m_Target.Texture() != 0 && resolution == m_Resolution && cubes == m_Cubes) return;

    m_Resolution = resolution;
    m_Cubes = cubes;
    m_CompleteChecked = false; // re-check after a resolution change (audit #358)

    // A cube-map array is a 2D array with depth = numCubes * 6; layer = cube*6 + face. The static casters' depth
    // (never sampled, only copied from) is made on first use: DepthArrayTarget::EnsureStatic.
    DepthArrayTarget::Desc d;
    d.target = GL_TEXTURE_CUBE_MAP_ARRAY;
    d.resolution = resolution;
    d.layers = cubes * 6;
    d.borderClamp = false;
    d.liveName = "PointShadowMap (live)";
    d.staticName = "PointShadowMap (static cache)";
    m_Target.Create(d, this);

    if (!m_Target.Fbo() || !m_Target.Texture())
        Log::Error("PointShadowMap: failed to create depth cube array (" + std::to_string(resolution) + ")");
}

void PointShadowMap::BeginFace(int slot, int face) const {
    slot = std::clamp(slot, 0, std::max(m_Cubes, 1) - 1);
    face = std::clamp(face, 0, 5);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Target.Fbo());
    m_Target.AttachLayer(slot * 6 + face);
    // audit #358 — a layered depth FBO only reports COMPLETE once a layer is attached, so the
    // check belongs here, once, rather than in Configure().
    if (!m_CompleteChecked) {
        m_CompleteChecked = true;
        GLFramebufferCheck::Complete("PointShadowMap", m_Target.Fbo(), m_Resolution, m_Resolution);
    }
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}

// The static-caster cache is allocated when a face first needs it: a scene with no shadowed point light never pays for it.
void PointShadowMap::BeginStaticFace(int slot, int face) const {
    m_Target.EnsureStatic();
    slot = std::clamp(slot, 0, std::max(m_Cubes, 1) - 1);
    face = std::clamp(face, 0, 5);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Target.StaticFbo());
    m_Target.AttachStaticLayer(slot * 6 + face);
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void PointShadowMap::BeginFaceFromStatic(int slot, int face) const {
    m_Target.EnsureStatic();
    slot = std::clamp(slot, 0, std::max(m_Cubes, 1) - 1);
    face = std::clamp(face, 0, 5);
    m_Target.AttachLayer(slot * 6 + face);
    m_Target.AttachStaticLayer(slot * 6 + face);
    glBlitNamedFramebuffer(m_Target.StaticFbo(), m_Target.Fbo(), 0, 0, m_Resolution, m_Resolution, 0, 0, m_Resolution, m_Resolution,
                           GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Target.Fbo());
    glViewport(0, 0, m_Resolution, m_Resolution);
}
