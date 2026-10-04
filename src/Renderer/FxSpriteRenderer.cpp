#include "FxSpriteRenderer.h"

#include "GLStateCache.h"
#include "HdrTarget.h"
#include "BloodPalette.h"
#include "KnifeFxLibrary.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"
#include "Core/Profiler.h"

#include <algorithm>
#include <utility>

namespace {
struct GpuSprite { // std430, matches FxSprite.*.glsl
    glm::vec4 PosSize;
    glm::vec4 AxisRot;
    glm::vec4 Color;
    glm::vec4 Params; // erosion, softness, aspect, blend
    glm::ivec4 Tex;   // colour layer (-1 procedural), normal layer, cell a, cell b
    glm::ivec4 Info;  // cols, rows, mode, entry flags
    glm::vec4 Extra;  // smoothness, seed
};
static_assert(sizeof(GpuSprite) == 112, "std430 layout");
constexpr unsigned kSpriteBinding = 10, kDepthUnit = 17, kColorUnit = 21, kNormalUnit = 22;
} // namespace

FxSpriteRenderer& FxSpriteRenderer::Get() {
    static FxSpriteRenderer* r = new FxSpriteRenderer();
    return *r;
}

int FxSpriteRenderer::Draw(const glm::mat4& view, const glm::mat4& proj, const int viewport[4], const HdrTarget* target, bool viewModel,
                           const std::function<void(Shader&)>& applyFrameState) {
    if (m_Sprites.empty()) return 0;
    const KnifeFxLibrary& lib = KnifeFxLibrary::Get();
    const unsigned colorArray = lib.ColorArray(KnifeFxImport::Library::Sprite);
    static std::vector<std::pair<float, GpuSprite>> sorted;
    sorted.clear();
    for (const Sprite& s : m_Sprites) {
        if (s.ViewModel != viewModel) continue;
        const float depth = -(view * glm::vec4(s.Pos, 1.0f)).z;
        if (depth < 0.02f - s.Size) continue; // behind the eye
        if (!viewModel && (depth > 150.0f || s.Size / std::max(depth, 0.01f) < 0.002f)) continue; // too far / too small to see
        GpuSprite g;
        g.PosSize = glm::vec4(s.Pos, s.Size);
        g.AxisRot = glm::vec4(s.Axis, s.Rot);
        g.Color = s.Color;
        g.Params = glm::vec4(s.Erosion, std::max(s.Softness, 1e-3f), s.Aspect, s.Blend);
        g.Tex = glm::ivec4(-1, -1, 0, 0);
        g.Info = glm::ivec4(1, 1, s.Mode, 0);
        g.Extra = glm::vec4(0.5f, s.Seed, 0.0f, 0.0f);
        if (const KnifeFxLibrary::Entry* e = lib.At(s.Entry); e && colorArray && e->Lib == KnifeFxImport::Library::Sprite) {
            g.Tex = glm::ivec4(e->ColorLayer, e->NormalLayer, s.CellA, s.CellB);
            g.Info = glm::ivec4(e->Cols, e->Rows, s.Mode, (int)e->Flags);
            g.Extra.x = s.Mode == 0 ? 1.0f - BloodPalette::RoughSprite : e->Smoothness; // blood: one material
        } else if (s.Entry >= 0) {
            continue; // the library isn't there: nothing to draw it with
        }
        sorted.emplace_back(depth, g);
    }
    if (sorted.empty()) return 0;
    PROFILE_GPU_SCOPE("FX Sprites");
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    static std::vector<GpuSprite> gpu;
    gpu.clear();
    for (const auto& p : sorted) gpu.push_back(p.second);

    if (!m_Program) {
        m_Program = std::make_unique<Shader>(ShaderLibrary::ReadFile("FxSprite.vert.glsl"), ShaderLibrary::ReadFile("FxSprite.frag.glsl"), "FxSprite");
        glCreateVertexArrays(1, &m_Vao);
    }
    if (gpu.size() > m_Capacity) {
        if (m_Buffer) glDeleteBuffers(1, &m_Buffer);
        m_Capacity = std::max<size_t>(gpu.size() * 2, 256);
        glCreateBuffers(1, &m_Buffer);
        glNamedBufferStorage(m_Buffer, (GLsizeiptr)(m_Capacity * sizeof(GpuSprite)), nullptr, GL_DYNAMIC_STORAGE_BIT);
    }
    glNamedBufferSubData(m_Buffer, 0, (GLsizeiptr)(gpu.size() * sizeof(GpuSprite)), gpu.data());

    // Soft against what's behind: the scene's depth so far (the world pass only - the arms-and-gun pass
    // cleared it for itself).
    const bool soft = !viewModel && target && target->IsValid();
    if (soft) target->ResolveDepthOnly();
    Shader& prog = *m_Program;
    applyFrameState(prog);
    prog.SetMat4("uView", view);
    prog.SetMat4("uProj", proj);
    prog.SetVec4("uViewport", glm::vec4((float)viewport[0], (float)viewport[1], (float)viewport[2], (float)viewport[3]));
    prog.SetInt("uSoft", soft ? 1 : 0);
    prog.SetInt("uNoReceiveShadows", 0);
    if (soft) glBindTextureUnit(kDepthUnit, target->ResolvedDepthTexture());
    glBindTextureUnit(kColorUnit, colorArray);
    glBindTextureUnit(kNormalUnit, lib.NormalArray(KnifeFxImport::Library::Sprite));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSpriteBinding, m_Buffer);
    glBindVertexArray(m_Vao);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied; additive = no coverage
    glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 6, (GLsizei)gpu.size(), 0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    GLStateCache::Invalidate();
    return 1;
}
