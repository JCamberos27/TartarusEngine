#include "Tonemapper.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"

#include <cmath>

Tonemapper::~Tonemapper() {
    delete m_Shader;
    if (m_Vao) glDeleteVertexArrays(1, &m_Vao);
}

void Tonemapper::EnsureCreated() {
    if (m_Shader) return;
    m_Shader = new Shader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"),
                          ShaderLibrary::ReadFile("Tonemapper.frag.glsl"));
    glGenVertexArrays(1, &m_Vao); // attribute-less; positions come from gl_VertexID
}

void Tonemapper::Apply(unsigned int srcHdrTexture, unsigned int dstFbo, int dstW, int dstH,
                       float exposureEV, Operator op,
                       unsigned int bloomTexture, float bloomIntensity) {
    EnsureCreated();

    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, dstW, dstH);

    m_Shader->Bind();
    GLStateCache::BindTexture2D(0, srcHdrTexture);
    m_Shader->SetInt("uHdr", 0);
    m_Shader->SetFloat("uExposure", std::pow(2.0f, exposureEV));
    m_Shader->SetInt("uOperator", (int)op);

    // PR16 — bloom: bind glow texture on unit 1; shader adds it before the tone curve.
    const bool bloomOn = bloomTexture != 0 && bloomIntensity > 0.0f;
    if (bloomOn) {
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, bloomTexture);
        glActiveTexture(GL_TEXTURE0);
        m_Shader->SetInt("uBloom", 1);
        m_Shader->SetFloat("uBloomIntensity", bloomIntensity);
    }
    m_Shader->SetInt("uBloomEnabled", bloomOn ? 1 : 0);

    glBindVertexArray(m_Vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    if (prevBlend) glEnable(GL_BLEND);

    // Shader::Bind goes through GLStateCache, but the raw VAO bind above and ImGui's later raw
    // GL calls mean the next cached bind shouldn't trust stale state.
    GLStateCache::Invalidate();
}
