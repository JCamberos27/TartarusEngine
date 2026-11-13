#include "ImpostorRenderer.h"

#include "Core/Env.h"
#include "Core/Paths.h"
#include "Core/Profiler.h"
#include "Core/ProjectSettings.h"
#include "Core/TimeService.h"
#include "GLStateCache.h"
#include "HiZCuller.h"
#include "Game/World.h"
#include "ImpostorLibrary.h"
#include "Material.h"
#include "WindUniforms.h"
#include "Physics/Frustum.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "Texture.h"
#include "UploadStaging.h"
#include "gl.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
constexpr int kInstanceBinding = 23; // Impostor.vert.glsl's ImpostorInstances
constexpr int kSetBinding = 24;      // ImpostorSets
constexpr int kExtentBinding = 22;   // ImpostorFrameExtents
constexpr int kAtlasSize = 2048;
constexpr int kLayersPerFrame = 6;   // atlas layers uploaded a frame while they come in (~32 MB of BC3)
constexpr float kCutoff = 0.5f;      // on the blended coverage
// Coverage x (1 + mip x this) before the cut: a mip averages the needles' coverage toward grey and the cut then
// thins the canopy with distance (Golus, "Anti-aliased Alpha Test"). 0.25 held the bake's own mips at about the
// covered area of mip 0.
constexpr float kMipAlphaScale = 0.25f;

// As the atlases' .meta files say (albedo sRGB, the rest linear), forced to BC3 so every set packs into one array:
// they load here, not through the asset library.
TextureImportSettings AtlasSettings(bool srgb) {
    TextureImportSettings s;
    s.IsSRGB = srgb;
    s.GenerateMipmaps = true;
    s.FilterMode = TextureImportSettings::Filter::Trilinear;
    s.WrapMode = TextureImportSettings::Wrap::ClampToEdge;
    s.CompressionMode = TextureImportSettings::Compression::Normal;
    s.MaxTextureSize = kAtlasSize;
    s.AnisoLevel = 1;
    s.ForceFormat = TextureImportSettings::ForcedFormat::BC3;
    return s;
}

struct alignas(16) SetGpu { // Impostor.vert.glsl's ImpostorSetGpu
    glm::vec4 BoxMinRadius;
    glm::vec4 BoxMaxBend;
    glm::vec4 Wind;
};
} // namespace

ImpostorRenderer& ImpostorRenderer::Get() {
    static ImpostorRenderer r;
    return r;
}

ImpostorRenderer::~ImpostorRenderer() {
    if (m_Loader.joinable()) m_Loader.join();
}

bool ImpostorRenderer::EnsurePrograms() {
    if (m_Depth) return true;
    if (m_ProgramsFailed) return false;
    const std::string vs = ShaderLibrary::ReadFileRequired("Impostor.vert.glsl");
    m_Depth = std::make_unique<Shader>(vs, ShaderLibrary::ReadFileRequired("ImpostorDepth.frag.glsl"), "ImpostorDepth");
    m_Lit = std::make_unique<Shader>(vs, ShaderLibrary::ReadFileRequired("ImpostorLit.frag.glsl"), "ImpostorLit");
    m_Shadow = std::make_unique<Shader>(vs, ShaderLibrary::ReadFileRequired("ImpostorShadow.frag.glsl"), "ImpostorShadow");
    ShaderLibrary::RegisterForHotReload(*m_Depth, "Impostor.vert.glsl", "ImpostorDepth.frag.glsl");
    ShaderLibrary::RegisterForHotReload(*m_Lit, "Impostor.vert.glsl", "ImpostorLit.frag.glsl");
    ShaderLibrary::RegisterForHotReload(*m_Shadow, "Impostor.vert.glsl", "ImpostorShadow.frag.glsl");
    const auto linked = [](const Shader& s) { // once, at the first draw
        GLint ok = 0;
        if (s.Program()) glGetProgramiv(s.Program(), GL_LINK_STATUS, &ok);
        return ok == GL_TRUE;
    };
    if (!linked(*m_Depth) || !linked(*m_Lit) || !linked(*m_Shadow)) {
        std::fprintf(stderr, "[Impostors] programs failed to build: impostors off\n");
        m_ProgramsFailed = true;
        m_Depth.reset();
        m_Lit.reset();
        m_Shadow.reset();
        return false;
    }
    glGenVertexArrays(1, &m_Vao); // the cards read no attributes, but core GL draws nothing without one bound
    return true;
}

bool ImpostorRenderer::EnsureAtlases() {
    if (m_AtlasesReady) return true;
    if (m_AtlasesFailed) return false;
    const auto& sets = ImpostorLibrary::Sets();
    const std::size_t count = sets.size() * 3;
    if (!m_LoadStarted) {
        // Decoded on a worker (the texture cache's BC3 blocks, encoded once on the first run): a set's three 2K maps
        // took ~35 ms on the main thread even from the cache.
        m_LoadStarted = true;
        Texture::InitGpuCaps(); // what DecodeFile needs, from the main thread
        m_Decoded.resize(count);
        std::vector<std::string> paths(count);
        for (std::size_t s = 0; s < sets.size(); ++s) {
            paths[s * 3 + 0] = ProjectPaths::Resolve(sets[s].Albedo);
            paths[s * 3 + 1] = ProjectPaths::Resolve(sets[s].NormalDepth);
            paths[s * 3 + 2] = ProjectPaths::Resolve(sets[s].Mask);
        }
        m_Loader = std::thread([this, paths]() {
            for (std::size_t i = 0; i < paths.size(); ++i) m_Decoded[i] = Texture::DecodeFile(paths[i], AtlasSettings(i % 3 == 0));
            m_LoaderDone.store(true, std::memory_order_release);
        });
        return false;
    }
    if (!m_LoaderDone.load(std::memory_order_acquire)) return false;
    if (m_Loader.joinable()) m_Loader.join();
    if (!m_Arrays[0]) {
        // Every layer of a kind must match the first: one format, size and mip count.
        for (std::size_t i = 0; i < count; ++i) {
            const TextureCpuData& t = m_Decoded[i];
            const TextureCpuData& first = m_Decoded[i % 3];
            if (!t.Ok || !t.GLFormat || t.Width != kAtlasSize || t.Height != kAtlasSize || t.GLFormat != first.GLFormat ||
                t.LevelSizes != first.LevelSizes) {
                std::fprintf(stderr, "[Impostors] %s: an atlas didn't decode to a %d^2 BC3 like the others - impostors off\n",
                             sets[i / 3].Name.c_str(), kAtlasSize);
                m_AtlasesFailed = true;
                m_Decoded.clear();
                return false;
            }
        }
        for (int k = 0; k < 3; ++k) {
            glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_Arrays[k]);
            glTextureStorage3D(m_Arrays[k], (GLsizei)m_Decoded[k].LevelSizes.size(), m_Decoded[k].GLFormat, kAtlasSize,
                               kAtlasSize, (GLsizei)sets.size());
            glTextureParameteri(m_Arrays[k], GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTextureParameteri(m_Arrays[k], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(m_Arrays[k], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(m_Arrays[k], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        // The sets' shape and sway, and their frames' extents: fixed for the session (immutable buffers).
        std::vector<SetGpu> gpu;
        std::vector<glm::vec2> extents;
        for (const ImpostorSet& s : sets) {
            gpu.push_back({glm::vec4(s.BoundsMin - s.Centre, s.Radius), glm::vec4(s.BoundsMax - s.Centre, s.WindBend),
                           glm::vec4(s.WindHeight, s.WindFrequency, 0.0f, 0.0f)});
            extents.insert(extents.end(), s.FrameExtents.begin(), s.FrameExtents.end());
        }
        glCreateBuffers(1, &m_SetBuf);
        glNamedBufferStorage(m_SetBuf, (GLsizeiptr)(gpu.size() * sizeof(SetGpu)), gpu.data(), 0);
        glCreateBuffers(1, &m_ExtentBuf);
        glNamedBufferStorage(m_ExtentBuf, (GLsizeiptr)(extents.size() * sizeof(glm::vec2)), extents.data(), 0);
    }
    // A few layers a frame (each a 2K BC3 with its mips, ~5 MB).
    for (int n = 0; n < kLayersPerFrame && m_Uploaded < count; ++n, ++m_Uploaded) {
        TextureCpuData& t = m_Decoded[m_Uploaded];
        const int kind = (int)(m_Uploaded % 3), layer = (int)(m_Uploaded / 3);
        std::size_t off = 0;
        for (std::size_t l = 0; l < t.LevelSizes.size(); ++l) {
            const int size = std::max(kAtlasSize >> l, 1);
            glCompressedTextureSubImage3D(m_Arrays[kind], (GLint)l, 0, 0, layer, size, size, 1, t.GLFormat,
                                          (GLsizei)t.LevelSizes[l], t.Pixels.data() + off);
            off += t.LevelSizes[l];
        }
        t = TextureCpuData{}; // its blocks are on the GPU now
    }
    if (m_Uploaded < count) return false;
    m_Decoded.clear();
    m_Decoded.shrink_to_fit();
    m_AtlasesReady = true;
    ImpostorLibrary::SetDrawable(true); // the next ApplyLod puts the trees on their impostors
    return true;
}

std::size_t ImpostorRenderer::TakeUpload(std::size_t size) {
    constexpr std::size_t kNone = ~std::size_t(0);
    constexpr std::size_t kUploadBytes = 4ull << 20;  // ~0.17 MB a frame in the Forest (the view and the sun)
    constexpr std::uint64_t kUploadSafeFrames = 16;   // SwapBuffers keeps the CPU a few frames ahead at most
    if (size == 0 || size > kUploadBytes / 4 || m_UploadFailed) return kNone;
    if (!m_UploadBase) {
        GLint align = 0;
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
        if (align <= 0 || 256 % align != 0) { m_UploadFailed = true; return kNone; } // RingFit places at multiples of 256
        const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glCreateBuffers(1, &m_UploadBuf);
        glNamedBufferStorage(m_UploadBuf, (GLsizeiptr)kUploadBytes, nullptr, flags);
        m_UploadBase = static_cast<unsigned char*>(glMapNamedBufferRange(m_UploadBuf, 0, (GLsizeiptr)kUploadBytes, flags));
        if (!m_UploadBase) {
            glDeleteBuffers(1, &m_UploadBuf);
            m_UploadBuf = 0;
            m_UploadFailed = true;
            return kNone;
        }
    }
    const std::uint64_t frame = Time::FrameCount();
    std::size_t old = 0;
    while (old < m_UploadSpans.size() && frame - m_UploadSpans[old].Frame > kUploadSafeFrames) ++old;
    m_UploadSpans.erase(m_UploadSpans.begin(), m_UploadSpans.begin() + (std::ptrdiff_t)old);
    const bool empty = m_UploadSpans.empty();
    const std::size_t tail = empty ? m_UploadHead : m_UploadSpans.front().Start;
    const std::size_t at = UploadStaging::RingFit(kUploadBytes, m_UploadHead, tail, empty, size);
    if (at == kNone) return kNone;
    m_UploadSpans.push_back({at, frame});
    m_UploadHead = at + size;
    return at;
}

bool ImpostorRenderer::Build(const World& world, int buckets,
                             const std::function<unsigned(const glm::vec3&, const glm::vec3&)>& keep,
                             const glm::vec3& sortFrom, const glm::vec3* sortDir) {
    m_HaveList = false;
    if (!ImpostorLibrary::Wanted() || !EnsurePrograms() || !EnsureAtlases() || !ImpostorLibrary::Enabled()) return false;
    const auto& sets = ImpostorLibrary::Sets();
    m_Lists.resize((std::size_t)buckets);
    for (auto& l : m_Lists) l.clear();
    std::size_t total = 0;
    world.ForEachImpostor([&](entt::entity g, int set, float fade) {
        if (set < 0 || set >= (int)sets.size()) return;
        const ImpostorSet& s = sets[set];
        const glm::mat4 m = world.GetCachedWorldTransform(g);
        const float scale = glm::length(glm::vec3(m[1])); // the bake's copies scale uniformly
        if (!(scale > 1e-6f)) return;
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? s.BoundsMax.x : s.BoundsMin.x, (c & 2) ? s.BoundsMax.y : s.BoundsMin.y,
                                   (c & 4) ? s.BoundsMax.z : s.BoundsMin.z);
            const glm::vec3 w = glm::vec3(m * glm::vec4(corner, 1.0f));
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
        const float lean = HiZCuller::FoliageSway(s.WindBend, Material::Wind().Strength); // the card's lean at a full gust
        const glm::vec3 sway(lean, 0.0f, lean);
        const unsigned in = keep(lo - sway, hi + sway);
        if (!in) return;
        const glm::mat3 rot(glm::vec3(m[0]) / glm::length(glm::vec3(m[0])), glm::vec3(m[1]) / scale,
                            glm::vec3(m[2]) / glm::length(glm::vec3(m[2])));
        const glm::quat q = glm::normalize(glm::quat_cast(rot));
        const Instance inst{glm::vec4(glm::vec3(m * glm::vec4(s.Centre, 1.0f)), scale), glm::vec4(q.x, q.y, q.z, q.w),
                            glm::vec4(glm::vec3(m[3]), fade), glm::vec4((float)set, 0.0f, 0.0f, 0.0f)};
        for (int b = 0; b < buckets; ++b)
            if (in & (1u << b)) {
                m_Lists[(std::size_t)b].push_back(inst);
                ++total;
            }
    });
    if (total == 0) return false;
    // Near to far across every set (one draw): early depth testing drops what nearer cards already cover.
    const auto key = [&](const Instance& i) {
        const glm::vec3 d = glm::vec3(i.CentreScale) - sortFrom;
        return sortDir ? glm::dot(d, *sortDir) : glm::dot(d, d);
    };
    for (auto& l : m_Lists)
        if (l.size() > 1) std::sort(l.begin(), l.end(), [&](const Instance& a, const Instance& b) { return key(a) < key(b); });
    const std::size_t at = TakeUpload(total * sizeof(Instance));
    if (at == ~std::size_t(0)) return false;
    m_Draws.assign(m_Lists.size(), {});
    std::uint32_t first = 0;
    for (std::size_t i = 0; i < m_Lists.size(); ++i) {
        if (m_Lists[i].empty()) continue;
        std::memcpy(m_UploadBase + at + (std::size_t)first * sizeof(Instance), m_Lists[i].data(),
                    m_Lists[i].size() * sizeof(Instance));
        m_Draws[i] = {first, (std::uint32_t)m_Lists[i].size()};
        first += (std::uint32_t)m_Lists[i].size();
    }
    if (static const int reportAt = Env::Int(Env::Var::TrisReport, -1); reportAt >= 0 && Time::FrameCount() == (std::uint64_t)reportAt) {
        std::printf("[TrisReport] impostors (%d bucket(s)):", buckets);
        for (const Range& r : m_Draws) std::printf(" %u", r.Count);
        std::printf("\n");
        std::fflush(stdout);
    }
    m_Span = at;
    m_SpanBytes = (std::size_t)first * sizeof(Instance);
    m_HaveList = true;
    return true;
}

bool ImpostorRenderer::Gather(const World& world, const Frustum& frustum, const glm::vec3& eye) {
    return Build(world, 1, [&](const glm::vec3& lo, const glm::vec3& hi) { return frustum.Intersects(AABB{lo, hi}) ? 1u : 0u; },
                 eye, nullptr);
}

void ImpostorRenderer::DrawBucket(Shader& prog, int bucket) {
    const Range& d = m_Draws[(std::size_t)bucket];
    if (!d.Count) return;
    glBindVertexArray(m_Vao);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, kInstanceBinding, m_UploadBuf, (GLintptr)m_Span, (GLsizeiptr)m_SpanBytes);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSetBinding, m_SetBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kExtentBinding, m_ExtentBuf);
    for (int k = 0; k < 3; ++k) glBindTextureUnit((GLuint)k, m_Arrays[k]);
    prog.SetInt("uFrames", ImpostorLibrary::Sets().front().Frames);
    SetWindFieldUniforms(prog);
    prog.SetFloat("uImpCutoff", kCutoff);
    prog.SetFloat("uMipAlphaScale", kMipAlphaScale);
    glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)d.Count, d.First);
    ++m_DrawCalls;
    glBindVertexArray(0);
    GLStateCache::Invalidate();
}

void ImpostorRenderer::DrawDepth(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye) {
    if (!m_HaveList) return;
    PROFILE_GPU_SCOPE("Impostor Depth");
    m_DrawCalls = 0;
    Shader& p = *m_Depth;
    p.Bind();
    p.SetMat4("uView", view);
    p.SetMat4("uProj", proj);
    p.SetVec3("uEye", eye);
    p.SetInt("uOrtho", 0);
    p.SetFloat("uDepthPush", 0.0f);
    glDisable(GL_CULL_FACE); // a card faces the viewer, but its winding isn't kept
    DrawBucket(p, 0);
    glEnable(GL_CULL_FACE);
}

void ImpostorRenderer::DrawLit(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye, bool depthPrepassed,
                               unsigned viewDepth, int samples, const std::function<void(Shader&)>& applyFrameState) {
    if (!m_HaveList) return;
    PROFILE_GPU_SCOPE("Impostor Lit");
    m_DrawCalls = 0;
    Shader& p = *m_Lit;
    p.Bind();
    applyFrameState(p);
    p.Bind();
    p.SetMat4("uView", view);
    p.SetMat4("uProj", proj);
    p.SetVec3("uEye", eye);
    p.SetInt("uOrtho", 0);
    p.SetFloat("uDepthPush", 0.0f);
    p.SetInt("uObjectLayerBit", 1); // the Default layer, as the trees'
    p.SetInt("uNoReceiveShadows", 0);
    // The pre-pass's depth is final for these pixels: read it to drop hidden ones before the lighting.
    const bool readDepth = viewDepth && depthPrepassed && samples >= 1 && glTextureBarrier;
    p.SetInt("uImpDepthSamples", readDepth ? std::min(samples, 8) : 0);
    if (readDepth) {
        glTextureBarrier(); // the depth just written by the pre-pass and the opaque draws, visible to texel fetches
        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, viewDepth);
        glActiveTexture(GL_TEXTURE0);
    }
    if (depthPrepassed) {
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_EQUAL);
    }
    glDisable(GL_CULL_FACE);
    DrawBucket(p, 0);
    glEnable(GL_CULL_FACE);
    if (depthPrepassed) {
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LEQUAL);
    }
}

void ImpostorRenderer::DrawSun(const World& world, const glm::mat4* cascadeViewProj, const Frustum* cascadeFrusta,
                               int cascades, unsigned mask, const glm::vec3& towardSun,
                               const std::function<bool(int, const glm::vec3&, const glm::vec3&)>& reaches,
                               const std::function<void(int)>& bindCascade) {
    if (!(mask & ((1u << cascades) - 1u))) return;
    const glm::vec3 toLight = -glm::normalize(towardSun); // nearest the sun first
    const bool any = Build(world, cascades, [&](const glm::vec3& lo, const glm::vec3& hi) {
        unsigned in = 0;
        for (int c = 0; c < cascades; ++c)
            if ((mask & (1u << c)) && cascadeFrusta[c].Intersects(AABB{lo, hi}) && reaches(c, lo, hi)) in |= 1u << c;
        return in;
    }, glm::vec3(0.0f), &toLight);
    if (!any) return;
    PROFILE_GPU_SCOPE("Impostor Shadow");
    m_DrawCalls = 0;
    Shader& p = *m_Shadow;
    p.Bind();
    p.SetInt("uOrtho", 1);
    p.SetVec3("uOrthoToViewer", glm::normalize(towardSun));
    glDisable(GL_CULL_FACE);
    for (int c = 0; c < cascades; ++c) {
        if (!m_Draws[(std::size_t)c].Count) continue;
        bindCascade(c);
        p.Bind();
        p.SetMat4("uLightViewProj", cascadeViewProj[c]);
        DrawBucket(p, c);
    }
    glEnable(GL_CULL_FACE);
    m_HaveList = false; // the view's list is gone: Gather again before drawing the view
}
