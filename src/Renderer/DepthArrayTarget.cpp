#include "DepthArrayTarget.h"
#include "GpuMemTrack.h"
#include "gl.h"

unsigned int DepthArrayTarget::CreateDepthFbo() {
    unsigned int fbo = 0;
    glCreateFramebuffers(1, &fbo);
    glNamedFramebufferDrawBuffers(fbo, 0, nullptr); // depth only
    return fbo;
}

void DepthArrayTarget::Release() {
    if (m_Owner) {
        GpuMemTrack::Set(GpuMemTrack::Key(m_LiveName, m_Owner), 0);
        if (!m_StaticName.empty()) GpuMemTrack::Set(GpuMemTrack::Key(m_StaticName, m_Owner), 0);
    }
    if (m_Fbo)       { glDeleteFramebuffers(1, &m_Fbo);       m_Fbo = 0; }
    if (m_Texture)   { glDeleteTextures(1, &m_Texture);       m_Texture = 0; }
    if (m_StaticFbo) { glDeleteFramebuffers(1, &m_StaticFbo); m_StaticFbo = 0; }
    if (m_Static)    { glDeleteTextures(1, &m_Static);        m_Static = 0; }
}

void DepthArrayTarget::Create(const Desc& d, const void* owner) {
    Release();
    m_Owner = owner;
    m_Resolution = d.resolution;
    m_Layers = d.layers;
    m_LiveName = d.liveName;
    m_StaticName = d.staticName ? d.staticName : "";

    glCreateTextures(d.target, 1, &m_Texture);
    glTextureStorage3D(m_Texture, 1, GL_DEPTH_COMPONENT32F, d.resolution, d.resolution, d.layers);
    glTextureParameteri(m_Texture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_Texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (d.borderClamp) {
        glTextureParameteri(m_Texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTextureParameteri(m_Texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        const float border[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // outside the map = fully lit
        glTextureParameterfv(m_Texture, GL_TEXTURE_BORDER_COLOR, border);
    } else {
        glTextureParameteri(m_Texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_Texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_Texture, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }
    // Hardware depth comparison so a shadow-sampler tap returns a filtered 0..1 visibility.
    glTextureParameteri(m_Texture, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTextureParameteri(m_Texture, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    GpuMemTrack::Set(GpuMemTrack::Key(m_LiveName, owner), (size_t)d.resolution * d.resolution * d.layers * 4);
    if (d.fbos) {
        m_Fbo = CreateDepthFbo();
        m_StaticFbo = CreateDepthFbo();
    }
}

// The static-caster cache (only ever rendered to and copied from, never sampled) is made on first use.
void DepthArrayTarget::EnsureStatic() const {
    if (m_Static) return;
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_Static);
    glTextureStorage3D(m_Static, 1, GL_DEPTH_COMPONENT32F, m_Resolution, m_Resolution, m_Layers);
    GpuMemTrack::Set(GpuMemTrack::Key(m_StaticName, m_Owner), (size_t)m_Resolution * m_Resolution * m_Layers * 4);
}

void DepthArrayTarget::AttachLayer(int layer) const {
    glNamedFramebufferTextureLayer(m_Fbo, GL_DEPTH_ATTACHMENT, m_Texture, 0, layer);
}

void DepthArrayTarget::AttachStaticLayer(int layer) const {
    glNamedFramebufferTextureLayer(m_StaticFbo, GL_DEPTH_ATTACHMENT, m_Static, 0, layer);
}
