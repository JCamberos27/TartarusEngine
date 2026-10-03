#include "BloodRenderer.h"

#include "Frustum.h"
#include "AABB.h"
#include "GLStateCache.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"
#include "Core/Profiler.h"

#include <algorithm>
#include <filesystem>

#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000
#endif
#ifndef GL_RGBA16UI
#define GL_RGBA16UI 0x8D76
#endif
#ifndef GL_RGBA_INTEGER
#define GL_RGBA_INTEGER 0x8D99
#endif
#ifndef GL_CW
#define GL_CW 0x0900
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT 0x1403
#endif

namespace {
constexpr unsigned kSprayBinding = 7, kFrameBinding = 8, kVatUnit = 16;

// Inserts `#define`s after a source's #version line.
std::string WithDefines(const std::string& src, const char* defines) {
    const size_t eol = src.find('\n');
    if (eol == std::string::npos) return src;
    return src.substr(0, eol + 1) + defines + src.substr(eol + 1);
}

struct GpuSpray { // std430, matches BloodVat.vert.glsl
    glm::mat4 Model;
    glm::vec4 Tint;
    glm::vec4 ClipPlane;
    glm::ivec4 Info;
};
static_assert(sizeof(GpuSpray) == 112, "std430 layout");
} // namespace

BloodRenderer& BloodRenderer::Get() {
    static BloodRenderer* r = new BloodRenderer();
    return *r;
}

int BloodRenderer::SimIndex(const std::string& name) const {
    for (size_t i = 0; i < m_Sims.size(); ++i)
        if (m_Sims[i].Name == name) return (int)i;
    return -1;
}

bool BloodRenderer::Load() {
    if (m_LoadTried) return m_Loaded;
    m_LoadTried = true;
    namespace fs = std::filesystem;
    const fs::path dir = fs::u8path(ProjectPaths::Resolve("assets/Effects/Blood"));
    std::error_code ec;
    std::vector<std::string> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".bvat") files.push_back(e.path().u8string());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        Log::Warn("Blood: no volumetric blood data in assets/Effects/Blood - run TartarusEngine --import-blood-fx <package> (docs/BLOOD_FX.md)");
        return false;
    }
    std::vector<glm::vec4> frameBounds;
    size_t bytes = 0;
    for (const std::string& f : files) {
        BloodFxImport::VatData vat;
        std::string err;
        if (!BloodFxImport::ReadVat(f, vat, &err)) {
            Log::Error("Blood: " + f + ": " + err);
            continue;
        }
        const auto& h = vat.Header;
        GpuSim g;
        g.FrameBase = (int)frameBounds.size();
        for (const auto& fr : vat.Frames) {
            frameBounds.emplace_back(fr.Min[0], fr.Min[1], fr.Min[2], 0.0f);
            frameBounds.emplace_back(fr.Max[0], fr.Max[1], fr.Max[2], 0.0f);
        }
        const int rows = (int)(h.Frames * h.RowsPerFrame);
        glCreateTextures(GL_TEXTURE_2D, 1, &g.Tex);
        glTextureStorage2D(g.Tex, 1, GL_RGBA16UI, (int)h.TexWidth, rows);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        glTextureSubImage2D(g.Tex, 0, 0, 0, (int)h.TexWidth, rows, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, vat.Texels.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTextureParameteri(g.Tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(g.Tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        bytes += vat.Texels.size() * 2;
        SimInfo info;
        info.Name = fs::u8path(f).stem().u8string();
        info.Header = h;
        info.Frames = std::move(vat.Frames);
        m_Sims.push_back(std::move(info));
        m_Gpu.push_back(g);
    }
    if (m_Sims.empty()) return false;
    glCreateBuffers(1, &m_FrameBuffer);
    glNamedBufferStorage(m_FrameBuffer, (GLsizeiptr)(frameBounds.size() * sizeof(glm::vec4)), frameBounds.data(), 0);
    glCreateVertexArrays(1, &m_EmptyVao);
    m_Loaded = true;
    Log::Info("Blood: " + std::to_string(m_Sims.size()) + " volumetric sims loaded (" + std::to_string(bytes / (1024 * 1024)) + " MB)");
    return true;
}

void BloodRenderer::EnsureProgram() {
    if (m_Program) return;
    // ModelFragment's subsurface lobe gives thin sheets and drops their back-lit red glow.
    m_Program = std::make_unique<Shader>(ShaderLibrary::ReadFile("BloodVat.vert.glsl"),
                                         WithDefines(ShaderLibrary::ReadFile("ModelFragment.glsl"), "#define _SUBSURFACE 1\n"),
                                         "BloodVat");
}

void BloodRenderer::BeginFrame() { m_Sprays.clear(); }

void BloodRenderer::AddSpray(const Spray& s) {
    if (s.Sim >= 0 && s.Sim < (int)m_Sims.size()) m_Sprays.push_back(s);
}

int BloodRenderer::DrawSprays(const glm::mat4& view, const glm::mat4& proj,
                              const std::function<void(Shader&)>& applyFrameState) {
    if (!m_Loaded || m_Sprays.empty()) return 0;
    PROFILE_GPU_SCOPE("Blood Sprays");
    EnsureProgram();

    // Cull, then group by sim: one instanced draw each.
    const Frustum frustum = Frustum::FromViewProj(proj * view);
    static std::vector<GpuSpray> gpu;
    static std::vector<int> order;
    order.clear();
    for (int i = 0; i < (int)m_Sprays.size(); ++i) {
        const Spray& s = m_Sprays[i];
        const auto& h = m_Sims[s.Sim].Header;
        const AABB box = AABB{glm::vec3(h.BoundsMin[0], h.BoundsMin[1], h.BoundsMin[2]),
                              glm::vec3(h.BoundsMax[0], h.BoundsMax[1], h.BoundsMax[2])}.Transformed(s.Model);
        if (frustum.Intersects(box)) order.push_back(i);
    }
    if (order.empty()) return 0;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return m_Sprays[a].Sim < m_Sprays[b].Sim; });
    gpu.clear();
    for (int i : order) {
        const Spray& s = m_Sprays[i];
        const int frame = std::clamp(s.Frame, 0, (int)m_Sims[s.Sim].Header.Frames - 1);
        gpu.push_back({s.Model, glm::vec4(s.Tint, 1.0f), s.ClipPlane, glm::ivec4(frame, m_Gpu[s.Sim].FrameBase, 0, 0)});
    }
    if (gpu.size() > m_SprayCapacity) {
        if (m_SprayBuffer) glDeleteBuffers(1, &m_SprayBuffer);
        m_SprayCapacity = std::max<size_t>(gpu.size(), 32);
        glCreateBuffers(1, &m_SprayBuffer);
        glNamedBufferStorage(m_SprayBuffer, (GLsizeiptr)(m_SprayCapacity * sizeof(GpuSpray)), nullptr, GL_DYNAMIC_STORAGE_BIT);
    }
    glNamedBufferSubData(m_SprayBuffer, 0, (GLsizeiptr)(gpu.size() * sizeof(GpuSpray)), gpu.data());

    Shader& prog = *m_Program;
    applyFrameState(prog);
    prog.SetMat4("uView", view);
    prog.SetMat4("uProj", proj);
    // The fluid's surface, through the model shader's material inputs.
    prog.SetVec3("uBaseColor", FluidAlbedo);
    prog.SetFloat("uMetallic", 0.0f);
    prog.SetFloat("uRoughness", FluidRoughness);
    prog.SetVec3("uEmissiveColor", glm::vec3(0.0f));
    prog.SetInt("uUseVertexColor", 1);
    prog.SetFloat("uOpacity", 1.0f);
    prog.SetInt("uAlphaBlend", 0);
    prog.SetInt("uSSAOEnabled", 0); // the AO pre-pass never saw the fluid
    prog.SetInt("uNoReceiveShadows", 0);
    prog.SetVec3("uSubsurfaceColor", glm::vec3(0.9f, 0.04f, 0.03f));
    prog.SetFloat("uThickness", 0.35f);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSprayBinding, m_SprayBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kFrameBinding, m_FrameBuffer);
    glBindVertexArray(m_EmptyVao);
    glEnable(GL_CLIP_DISTANCE0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    int draws = 0;
    for (size_t first = 0; first < order.size();) {
        const int sim = m_Sprays[order[first]].Sim;
        size_t last = first;
        while (last < order.size() && m_Sprays[order[last]].Sim == sim) ++last;
        const auto& h = m_Sims[sim].Header;
        // The flag describes the soup's corner order against its own normals; the fluid's outside winds the other way.
        glFrontFace((h.Flags & BloodFxImport::VatFlagClockwise) ? GL_CCW : GL_CW);
        glBindTextureUnit(kVatUnit, m_Gpu[sim].Tex);
        prog.SetInt("uTrisPerRow", (int)h.TrisPerRow);
        prog.SetInt("uRowsPerFrame", (int)h.RowsPerFrame);
        glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, (int)h.VertexCount, (int)(last - first), (unsigned)first);
        ++draws;
        first = last;
    }
    glFrontFace(GL_CCW);
    glDisable(GL_CLIP_DISTANCE0);
    glBindVertexArray(0);
    GLStateCache::Invalidate();
    return draws;
}
