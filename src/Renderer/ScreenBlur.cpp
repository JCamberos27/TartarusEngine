#include "ScreenBlur.h"
#include "Shader.h"
#include "GLStateCache.h"
#include "gl.h"

#include <glm/glm.hpp>

namespace {

const char* kVertSrc = R"(#version 460 core
out vec2 vUV;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Standard 9-tap linear-sampled Gaussian (5 texture fetches). uDir carries the per-texel step
// along one axis; (0,0) degenerates to a plain bilinear copy (weights sum to 1), used for the
// final upsample.
const char* kFragSrc = R"(#version 460 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uDir;
void main() {
    vec2 o1 = uDir * 1.3846153846;
    vec2 o2 = uDir * 3.2307692308;
    vec3 c  = texture(uTex, vUV).rgb      * 0.2270270270;
    c += texture(uTex, vUV + o1).rgb * 0.3162162162;
    c += texture(uTex, vUV - o1).rgb * 0.3162162162;
    c += texture(uTex, vUV + o2).rgb * 0.0702702703;
    c += texture(uTex, vUV - o2).rgb * 0.0702702703;
    FragColor = vec4(c, 1.0);
}
)";

} // namespace

ScreenBlur::~ScreenBlur() {
    delete m_Shader;
    if (m_Vao) glDeleteVertexArrays(1, &m_Vao);
    if (m_Tex[0]) glDeleteTextures(2, m_Tex);
    if (m_Fbo[0]) glDeleteFramebuffers(2, m_Fbo);
}

void ScreenBlur::Ensure(int halfW, int halfH) {
    if (!m_Shader) {
        m_Shader = new Shader(kVertSrc, kFragSrc);
        glGenVertexArrays(1, &m_Vao);
    }
    if (m_W == halfW && m_H == halfH && m_Fbo[0]) return;

    if (m_Tex[0]) glDeleteTextures(2, m_Tex);
    if (m_Fbo[0]) glDeleteFramebuffers(2, m_Fbo);
    glGenFramebuffers(2, m_Fbo);
    glGenTextures(2, m_Tex);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, m_Tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, halfW, halfH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_Tex[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_W = halfW;
    m_H = halfH;
}

void ScreenBlur::Apply(unsigned int srcTex, int width, int height, unsigned int dstFbo, int iterations) {
    const int hw = width > 2 ? width / 2 : 1;
    const int hh = height > 2 ? height / 2 : 1;
    Ensure(hw, hh);

    const GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    m_Shader->Bind();
    m_Shader->SetInt("uTex", 0);
    glBindVertexArray(m_Vao);

    auto pass = [&](unsigned int tex, unsigned int fbo, int vpW, int vpH, float dx, float dy) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, vpW, vpH);
        GLStateCache::BindTexture2D(0, tex);
        m_Shader->SetVec2("uDir", glm::vec2(dx, dy));
        glDrawArrays(GL_TRIANGLES, 0, 3);
    };

    const float tx = 1.0f / (float)hw;
    const float ty = 1.0f / (float)hh;

    // Downsample + first horizontal blur: full-res source into the half-res ping buffer.
    pass(srcTex, m_Fbo[0], hw, hh, tx, 0.0f);
    pass(m_Tex[0], m_Fbo[1], hw, hh, 0.0f, ty);
    for (int i = 1; i < iterations; ++i) {
        pass(m_Tex[1], m_Fbo[0], hw, hh, tx, 0.0f);
        pass(m_Tex[0], m_Fbo[1], hw, hh, 0.0f, ty);
    }

    // Upsample to the destination (plain bilinear copy — uDir 0).
    pass(m_Tex[1], dstFbo, width, height, 0.0f, 0.0f);

    glBindVertexArray(0);
    if (prevDepth) glEnable(GL_DEPTH_TEST);
    if (prevBlend) glEnable(GL_BLEND);
    GLStateCache::Invalidate();
}
