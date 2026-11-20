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

// A pass that writes every pixel it touches with nothing to test against (bakes, conversions): depth test, depth
// writes, culling and blending off for its scope, the caller's restored after.
class GLPlainPassScope {
public:
    GLPlainPassScope() {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glDepthMask(GL_FALSE);
    }
private:
    GLStateScope m_Saved; // constructed first: saves before the changes above
};

// The sky's pixels only: a quad on the far plane tested GL_LEQUAL against the scene's depth, no depth writes.
// Saves exactly what it changes (#112) and restores it on scope exit.
class GLSkyDepthScope {
public:
    GLSkyDepthScope() {
        m_DepthTest = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
        glGetIntegerv(GL_DEPTH_WRITEMASK, &m_Mask);
        glGetIntegerv(GL_DEPTH_FUNC, &m_Func);
        glDepthMask(GL_FALSE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    }
    ~GLSkyDepthScope() {
        glDepthFunc((GLenum)m_Func);
        if (!m_DepthTest) glDisable(GL_DEPTH_TEST);
        glDepthMask(m_Mask ? GL_TRUE : GL_FALSE);
    }
    GLSkyDepthScope(const GLSkyDepthScope&) = delete;
    GLSkyDepthScope& operator=(const GLSkyDepthScope&) = delete;
private:
    bool m_DepthTest = true;
    GLint m_Mask = GL_TRUE, m_Func = GL_LESS;
};

// Face culling off for a scope (double-sided cards, blades), the caller's setting restored after.
class GLCullOffScope {
public:
    GLCullOffScope() : m_On(glIsEnabled(GL_CULL_FACE) == GL_TRUE) { glDisable(GL_CULL_FACE); }
    ~GLCullOffScope() { if (m_On) glEnable(GL_CULL_FACE); }
    GLCullOffScope(const GLCullOffScope&) = delete;
    GLCullOffScope& operator=(const GLCullOffScope&) = delete;
private:
    bool m_On;
};

// Depth writes and the depth function set for a scope, the caller's restored after.
class GLDepthStateScope {
public:
    GLDepthStateScope(bool writes, GLenum func) {
        glGetIntegerv(GL_DEPTH_WRITEMASK, &m_Mask);
        glGetIntegerv(GL_DEPTH_FUNC, &m_Func);
        glDepthMask(writes ? GL_TRUE : GL_FALSE);
        glDepthFunc(func);
    }
    ~GLDepthStateScope() {
        glDepthMask(m_Mask ? GL_TRUE : GL_FALSE);
        glDepthFunc((GLenum)m_Func);
    }
    GLDepthStateScope(const GLDepthStateScope&) = delete;
    GLDepthStateScope& operator=(const GLDepthStateScope&) = delete;
private:
    GLint m_Mask = GL_TRUE, m_Func = GL_LESS;
};
