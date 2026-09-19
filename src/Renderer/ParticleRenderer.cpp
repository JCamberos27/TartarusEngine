#include "ParticleRenderer.h"
#include "Components.h"
#include "GLStateCache.h"
#include "RenderFrameContext.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "World.h"
#include "gl.h"

#include <algorithm>
#include <vector>

ParticleRenderer::~ParticleRenderer() {
    delete m_Shader;
    if (m_Vbo) glDeleteBuffers(1, &m_Vbo);
    if (m_Vao) glDeleteVertexArrays(1, &m_Vao);
}

void ParticleRenderer::EnsureCreated() {
    if (m_Shader) return;
    m_Shader = new Shader(ShaderLibrary::ReadFile("Particle.vert.glsl"), ShaderLibrary::ReadFile("Particle.frag.glsl"));
    glCreateVertexArrays(1, &m_Vao);
    glCreateBuffers(1, &m_Vbo);
    // Per-instance: vec4 (position, size) + vec4 colour (premultiplied by intensity), 32 bytes.
    glVertexArrayVertexBuffer(m_Vao, 0, m_Vbo, 0, sizeof(float) * 8);
    glVertexArrayBindingDivisor(m_Vao, 0, 1);
    glEnableVertexArrayAttrib(m_Vao, 0);
    glVertexArrayAttribFormat(m_Vao, 0, 4, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_Vao, 0, 0);
    glEnableVertexArrayAttrib(m_Vao, 1);
    glVertexArrayAttribFormat(m_Vao, 1, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 4);
    glVertexArrayAttribBinding(m_Vao, 1, 0);
}

namespace {
struct Instance { float Pos[3]; float Size; float Color[4]; };
}

int ParticleRenderer::Draw(const World& world, const RenderFrameContext& ctx) {
    static std::vector<std::pair<float, Instance>> alpha;
    static std::vector<Instance> additive;
    alpha.clear();
    additive.clear();

    for (auto [e, ps] : world.Registry.view<const ParticleSystemComponent>().each()) {
        if (ps.Live.empty() || world.Registry.all_of<InactiveTag>(e)) continue;
        if (ctx.EditorView && world.Registry.all_of<HiddenInSceneTag>(e)) continue;
        const bool add = ps.BlendMode == 1;
        for (const auto& p : ps.Live) {
            const float t = std::clamp(p.Age / std::max(p.Life, 1e-4f), 0.0f, 1.0f);
            const glm::vec3 c = glm::mix(ps.StartColor, ps.EndColor, t) * std::max(ps.Intensity, 0.0f);
            const float a = std::clamp(glm::mix(ps.StartAlpha, ps.EndAlpha, t), 0.0f, 1.0f);
            const float size = std::max(glm::mix(ps.StartSize, ps.EndSize, t), 0.0f);
            if (a <= 0.0f || size <= 0.0f) continue;
            Instance in{{p.Pos.x, p.Pos.y, p.Pos.z}, size, {c.r, c.g, c.b, a}};
            if (add) {
                additive.push_back(in);
            } else {
                const float depth = -(ctx.View * glm::vec4(p.Pos, 1.0f)).z;
                alpha.push_back({depth, in});
            }
        }
    }
    if (alpha.empty() && additive.empty()) return 0;

    EnsureCreated();
    std::sort(alpha.begin(), alpha.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const size_t total = alpha.size() + additive.size();
    if (total > m_Capacity) {
        m_Capacity = std::max(total, m_Capacity * 2);
        glNamedBufferData(m_Vbo, (GLsizeiptr)(m_Capacity * sizeof(Instance)), nullptr, GL_DYNAMIC_DRAW);
    }
    std::vector<Instance> upload;
    upload.reserve(total);
    for (const auto& a : alpha) upload.push_back(a.second);
    upload.insert(upload.end(), additive.begin(), additive.end());
    glNamedBufferSubData(m_Vbo, 0, (GLsizeiptr)(upload.size() * sizeof(Instance)), upload.data());

    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    const GLboolean prevCull = glIsEnabled(GL_CULL_FACE);
    GLboolean prevDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);

    m_Shader->Bind();
    m_Shader->SetMat4("uView", ctx.View);
    m_Shader->SetMat4("uProj", ctx.Proj);
    glBindVertexArray(m_Vao);
    if (!alpha.empty()) {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, 0x0001 /*GL_ONE*/, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 6, (GLsizei)alpha.size(), 0);
    }
    if (!additive.empty()) {
        glBlendFuncSeparate(GL_SRC_ALPHA, 0x0001 /*GL_ONE*/, 0 /*GL_ZERO*/, 0x0001 /*GL_ONE*/);
        glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 6, (GLsizei)additive.size(), (GLuint)alpha.size());
    }
    glBindVertexArray(0);

    glDepthMask(prevDepthMask);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevCull) glEnable(GL_CULL_FACE);
    GLStateCache::Invalidate();
    return (int)total;
}
