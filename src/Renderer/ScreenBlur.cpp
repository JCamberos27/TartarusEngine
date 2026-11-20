#include "ScreenBlur.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"

#include <glm/glm.hpp>

ScreenBlur::ScreenBlur() = default;
ScreenBlur::~ScreenBlur() = default;

void ScreenBlur::Apply(unsigned int srcTex, int width, int height, unsigned int dstFbo, int iterations) {
    const int hw = width > 2 ? width / 2 : 1;
    const int hh = height > 2 ? height / 2 : 1;
    if (!m_Shader)
        m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("Fullscreen.vert.glsl"),
                                            ShaderLibrary::ReadFile("ScreenBlur.frag.glsl"));
    for (ColorTarget& t : m_Half) t.Ensure(hw, hh, GL_RGBA8, "ScreenBlur ping-pong");

    FullscreenPass fullscreen;
    m_Shader->Bind();
    m_Shader->SetInt("uTex", 0);

    auto pass = [&](unsigned int tex, unsigned int fbo, int vpW, int vpH, float dx, float dy) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, vpW, vpH);
        GLStateCache::BindTexture2D(0, tex);
        m_Shader->SetVec2("uDir", glm::vec2(dx, dy));
        FullscreenPass::Draw();
    };

    const float tx = 1.0f / (float)hw;
    const float ty = 1.0f / (float)hh;

    // Downsample + first horizontal blur: full-res source into the half-res ping buffer.
    pass(srcTex, m_Half[0].Fbo(), hw, hh, tx, 0.0f);
    pass(m_Half[0].Texture(), m_Half[1].Fbo(), hw, hh, 0.0f, ty);
    for (int i = 1; i < iterations; ++i) {
        pass(m_Half[1].Texture(), m_Half[0].Fbo(), hw, hh, tx, 0.0f);
        pass(m_Half[0].Texture(), m_Half[1].Fbo(), hw, hh, 0.0f, ty);
    }

    // Upsample to the destination (plain bilinear copy — uDir 0).
    pass(m_Half[1].Texture(), dstFbo, width, height, 0.0f, 0.0f);
}
