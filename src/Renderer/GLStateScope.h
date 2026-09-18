#pragma once
#include "gl.h"

// Saves the fixed-function toggles a full-screen post pass flips (depth test, depth writes,
// blending, face culling) and restores them on scope exit. #160: Bloom and SSAO used to end by
// force-ENABLING depth test and culling whatever the caller had set, so a later pass that
// expected them off silently broke. GLStateCache doesn't track these, so this is plain GL.
class GLStateScope {
public:
    GLStateScope() {
        m_DepthTest = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
        m_Blend     = glIsEnabled(GL_BLEND) == GL_TRUE;
        m_CullFace  = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
        GLint mask = 1;
        glGetIntegerv(GL_DEPTH_WRITEMASK, &mask);
        m_DepthMask = mask != 0;
    }
    ~GLStateScope() {
        Set(GL_DEPTH_TEST, m_DepthTest);
        Set(GL_BLEND, m_Blend);
        Set(GL_CULL_FACE, m_CullFace);
        glDepthMask(m_DepthMask ? GL_TRUE : GL_FALSE);
    }
    GLStateScope(const GLStateScope&) = delete;
    GLStateScope& operator=(const GLStateScope&) = delete;

private:
    static void Set(GLenum cap, bool on) { if (on) glEnable(cap); else glDisable(cap); }
    bool m_DepthTest = true, m_Blend = false, m_CullFace = true, m_DepthMask = true;
};
