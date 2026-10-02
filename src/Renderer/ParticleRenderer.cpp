#include "ParticleRenderer.h"
#include "Components.h"
#include "GLStateCache.h"
#include "RenderFrameContext.h"
#include "Shader.h"
#include "ProjectPaths.h"
#include "ShaderLibrary.h"
#include "Texture.h"
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
    // Per-instance: vec4 (position, size) + vec4 colour (premultiplied by intensity) + vec4 flame
    // (axis, length) + float seed, 52 bytes.
    glVertexArrayVertexBuffer(m_Vao, 0, m_Vbo, 0, sizeof(float) * 13);
    glVertexArrayBindingDivisor(m_Vao, 0, 1);
    glEnableVertexArrayAttrib(m_Vao, 0);
    glVertexArrayAttribFormat(m_Vao, 0, 4, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_Vao, 0, 0);
    glEnableVertexArrayAttrib(m_Vao, 1);
    glVertexArrayAttribFormat(m_Vao, 1, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 4);
    glVertexArrayAttribBinding(m_Vao, 1, 0);
    glEnableVertexArrayAttrib(m_Vao, 2);
    glVertexArrayAttribFormat(m_Vao, 2, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 8);
    glVertexArrayAttribBinding(m_Vao, 2, 0);
    glEnableVertexArrayAttrib(m_Vao, 3);
    glVertexArrayAttribFormat(m_Vao, 3, 1, GL_FLOAT, GL_FALSE, sizeof(float) * 12);
    glVertexArrayAttribBinding(m_Vao, 3, 0);
}

Texture* ParticleRenderer::FlameTexture(const std::string& path) {
    auto it = m_Textures.find(path);
    if (it == m_Textures.end()) {
        TextureImportSettings s;
        s.IsSRGB = false; // the four flame masks ride in the channels: data, not colour
        s.WrapMode = TextureImportSettings::Wrap::ClampToEdge;
        auto tex = std::make_shared<Texture>(ProjectPaths::Resolve(path), s);
        it = m_Textures.emplace(path, std::move(tex)).first;
    }
    return it->second->IsValid() ? it->second.get() : nullptr;
}

namespace {
struct Instance { float Pos[3]; float Size; float Color[4]; float Flame[4]; float Seed; };
}

int ParticleRenderer::Draw(const World& world, const RenderFrameContext& ctx, bool viewModelPass, bool drawViewModel) {
    static std::vector<std::pair<float, Instance>> alpha;
    static std::vector<Instance> additive;
    static std::vector<std::pair<Texture*, std::vector<Instance>>> flames; // additive tongues, one list per texture
    alpha.clear();
    additive.clear();
    for (auto& f : flames) f.second.clear();

    for (auto [e, ps] : world.Registry.view<const ParticleSystemComponent>().each()) {
        if (ps.Live.empty() || world.Registry.all_of<InactiveTag>(e)) continue;
        if (ctx.EditorView && world.Registry.all_of<HiddenInSceneTag>(e)) continue;
        if (ctx.OwnerView ? world.Registry.all_of<HiddenFromOwnerTag>(e) : world.Registry.all_of<OwnerViewOnlyTag>(e)) continue;
        const bool viewModel = viewModelPass && world.Registry.all_of<ViewModelTag>(e);
        if (viewModel != drawViewModel) continue;
        const bool add = ps.BlendMode == 1;
        Texture* flameTex = ps.Texture.empty() ? nullptr : FlameTexture(ps.Texture);
        std::vector<Instance>* flameList = nullptr;
        if (flameTex) {
            for (auto& f : flames)
                if (f.first == flameTex) flameList = &f.second;
            if (!flameList) flameList = &flames.emplace_back(flameTex, std::vector<Instance>{}).second;
        }
        for (const auto& p : ps.Live) {
            const float t = std::clamp(p.Age / std::max(p.Life, 1e-4f), 0.0f, 1.0f);
            if (flameList && p.Length > 0.0f) {
                // P_AK105_MuzzleFlash's curves: the tongue grows from nothing, fast at first (size over
                // lifetime: Hermite, start slopes 2.9 across and 1.69 along, flat at the end); its
                // emission (custom data 1) starts white hot, is down to 3% at 18% of its life and gone
                // at 36%, leaving the smoky body, whose alpha falls to 0.29 by 55% and then out.
                const float across = t * (2.9f + t * (-2.8f + t * 0.9f));
                const float along = t * (1.69f + t * (-0.38f - t * 0.31f));
                const float hot = t < 0.1824f ? glm::mix(1.0f, 0.03124f, t / 0.1824f)
                                              : std::max(0.0f, glm::mix(0.03124f, 0.0f, (t - 0.1824f) / (0.3618f - 0.1824f)));
                const float a = p.Alpha * (t < 0.55f ? glm::mix(1.0f, 0.294f, t / 0.55f) : glm::mix(0.294f, 0.0f, (t - 0.55f) / 0.45f));
                const glm::vec3 c = ps.StartColor * (std::max(ps.Intensity, 0.0f) * hot * p.Glow);
                if (a <= 0.0f || t <= 0.0f) continue;
                Instance in{{p.Pos.x, p.Pos.y, p.Pos.z}, p.Width * across, {c.r, c.g, c.b, a}, {p.Axis.x, p.Axis.y, p.Axis.z, p.Length * along}, p.Seed};
                flameList->push_back(in);
                continue;
            }
            const glm::vec3 c = glm::mix(ps.StartColor, ps.EndColor, t) * std::max(ps.Intensity, 0.0f);
            const float a = std::clamp(glm::mix(ps.StartAlpha, ps.EndAlpha, t), 0.0f, 1.0f);
            const float size = std::max(glm::mix(ps.StartSize, ps.EndSize, t), 0.0f);
            if (a <= 0.0f || size <= 0.0f) continue;
            Instance in{{p.Pos.x, p.Pos.y, p.Pos.z}, size, {c.r, c.g, c.b, a}, {0, 0, 0, 0}, 0.0f};
            if (add) {
                additive.push_back(in);
            } else {
                const float depth = -(ctx.View * glm::vec4(p.Pos, 1.0f)).z;
                alpha.push_back({depth, in});
            }
        }
    }
    size_t flameCount = 0;
    for (const auto& f : flames) flameCount += f.second.size();
    if (alpha.empty() && additive.empty() && flameCount == 0) return 0;

    EnsureCreated();
    std::sort(alpha.begin(), alpha.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const size_t total = alpha.size() + additive.size() + flameCount;
    if (total > m_Capacity) {
        m_Capacity = std::max(total, m_Capacity * 2);
        glNamedBufferData(m_Vbo, (GLsizeiptr)(m_Capacity * sizeof(Instance)), nullptr, GL_DYNAMIC_DRAW);
    }
    std::vector<Instance> upload;
    upload.reserve(total);
    for (const auto& a : alpha) upload.push_back(a.second);
    upload.insert(upload.end(), additive.begin(), additive.end());
    for (const auto& f : flames) upload.insert(upload.end(), f.second.begin(), f.second.end());
    glNamedBufferSubData(m_Vbo, 0, (GLsizeiptr)(upload.size() * sizeof(Instance)), upload.data());

    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    const GLboolean prevCull = glIsEnabled(GL_CULL_FACE);
    GLboolean prevDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    const GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
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
    GLuint base = (GLuint)(alpha.size() + additive.size());
    for (const auto& f : flames) {
        if (f.second.empty()) continue;
        // Premultiplied, as M_Muzzle_Flash (One, OneMinusSrcAlpha): a smoky body over the scene plus
        // emission added on top.
        glBlendFuncSeparate(0x0001 /*GL_ONE*/, GL_ONE_MINUS_SRC_ALPHA, 0x0001 /*GL_ONE*/, GL_ONE_MINUS_SRC_ALPHA);
        f.first->Bind(0);
        m_Shader->SetInt("uFlameTex", 0);
        glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 6, (GLsizei)f.second.size(), base);
        base += (GLuint)f.second.size();
    }
    glBindVertexArray(0);

    glDepthMask(prevDepthMask);
    if (prevDepthTest) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevCull) glEnable(GL_CULL_FACE);
    GLStateCache::Invalidate();
    return (int)total;
}
