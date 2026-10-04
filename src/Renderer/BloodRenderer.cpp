#include "BloodRenderer.h"

#include "Frustum.h"
#include "HdrTarget.h"
#include "KnifeFxLibrary.h"
#include <stb_image.h>
#include "AABB.h"
#include "GLStateCache.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"
#include "Core/Profiler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
#ifndef GL_SRC1_COLOR
#define GL_SRC1_COLOR 0x88F9
#endif
#ifndef GL_FRONT
#define GL_FRONT 0x0404
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
        Log::Warn("Blood: no volumetric blood data in assets/Effects/Blood - run TartarusEngine --import-blood-fx <package>");
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
    if (!BuildAtlas()) Log::Warn("Blood: no decal images in assets/Effects/Blood/decals - blood leaves no stains");
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

void BloodRenderer::BeginFrame() {
    m_Sprays.clear();
    m_Decals.clear();
    m_SplatRanges.clear();
    m_SplatCount = 0;
}

namespace {
struct GpuSplat { // std430, matches ModelFragment.glsl's BloodSplat
    glm::vec4 CenterRadius;
    glm::vec4 NormalDepth;
    glm::vec4 Tangent;
    glm::vec4 Rect;
    glm::vec4 Params;
};
static_assert(sizeof(GpuSplat) == 80, "std430 layout");
constexpr unsigned kSplatBinding = 6;
} // namespace

void BloodRenderer::SetSplats(const std::vector<Splat>& splats, const std::vector<std::pair<unsigned, glm::ivec2>>& ranges) {
    m_SplatRanges.clear();
    m_SplatCount = 0;
    if (splats.empty() || !m_AtlasNorm) return;
    static std::vector<GpuSplat> gpu;
    gpu.clear();
    for (const Splat& s : splats) {
        const int set = s.Set >= 0 && s.Set < (int)m_RectNorm.size() ? s.Set : 0;
        gpu.push_back({glm::vec4(s.Center, s.Radius), glm::vec4(s.Normal, s.Depth), glm::vec4(s.Tangent, 0.0f), m_RectNorm[set],
                       glm::vec4(s.Cutout, s.Dry, s.Opacity, 0.0f)});
    }
    if (gpu.size() > m_SplatCapacity) {
        if (m_SplatBuffer) glDeleteBuffers(1, &m_SplatBuffer);
        m_SplatCapacity = std::max<size_t>(gpu.size() * 2, 64);
        glCreateBuffers(1, &m_SplatBuffer);
        glNamedBufferStorage(m_SplatBuffer, (GLsizeiptr)(m_SplatCapacity * sizeof(GpuSplat)), nullptr, GL_DYNAMIC_STORAGE_BIT);
    }
    glNamedBufferSubData(m_SplatBuffer, 0, (GLsizeiptr)(gpu.size() * sizeof(GpuSplat)), gpu.data());
    m_SplatCount = (int)gpu.size();
    for (const auto& [entity, range] : ranges)
        if (range.x >= 0 && range.y > 0 && range.x + range.y <= m_SplatCount) m_SplatRanges[entity] = range;
}

bool BloodRenderer::SplatRange(unsigned entity, int& first, int& count) const {
    first = count = 0;
    if (m_SplatRanges.empty()) return false;
    const auto it = m_SplatRanges.find(entity);
    if (it == m_SplatRanges.end()) return false;
    first = it->second.x;
    count = it->second.y;
    return true;
}

void BloodRenderer::BindSplatResources() const {
    if (!m_SplatCount || !m_SplatBuffer) return;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSplatBinding, m_SplatBuffer);
    glBindTextureUnit(28, m_AtlasNorm); // ModelFragment.glsl: units 28 and 29, kept out of the material units
    glBindTextureUnit(29, m_AtlasMask);
}

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
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    static std::vector<GpuSpray> gpu;
    static std::vector<int> order;
    order.clear();
    m_CulledSprays = 0;
    for (int i = 0; i < (int)m_Sprays.size(); ++i) {
        const Spray& s = m_Sprays[i];
        const auto& h = m_Sims[s.Sim].Header;
        const AABB box = AABB{glm::vec3(h.BoundsMin[0], h.BoundsMin[1], h.BoundsMin[2]),
                              glm::vec3(h.BoundsMax[0], h.BoundsMax[1], h.BoundsMax[2])}.Transformed(s.Model);
        if (!frustum.Intersects(box)) continue;
        if (!WorthDrawing((box.Min + box.Max) * 0.5f, glm::length(box.Max - box.Min) * 0.5f, eye, SprayMaxDistance)) { ++m_CulledSprays; continue; }
        order.push_back(i);
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
    prog.SetVec3("uSubsurfaceColor", BloodPalette::Subsurface);
    prog.SetFloat("uThickness", 0.55f);

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

// --- decals --------------------------------------------------------------------------------------------------------

namespace {
struct GpuDecal { // std430, matches BloodDecal.*.glsl
    glm::mat4 Model;
    glm::mat4 InvModel;
    glm::vec4 RectNorm;
    glm::vec4 RectMask;
    glm::vec4 Params;
    glm::vec4 Axis;
    glm::ivec4 Knife; // colour layer (-1: a KriptoFX set), normal layer, cell, next cell
    glm::vec4 Grid;   // cols, rows, cell blend, smoothness
    glm::ivec4 Kind;  // library (0 large, 1 small), entry flags
};
static_assert(sizeof(GpuDecal) == 240, "std430 layout");
constexpr unsigned kDecalBinding = 9, kDepthUnit = 17, kNormUnit = 18, kMaskUnit = 19, kLookupUnit = 20;
constexpr unsigned kLargeColorUnit = 21, kLargeNormalUnit = 22, kSmallColorUnit = 23, kSmallNormalUnit = 24;
constexpr int kAtlasWidth = 2048, kAtlasPad = 4, kMaxDecalSide = 512;

// Bilinear resample of an RGBA8 image (at exact 2:1 it's a 2x2 box filter).
std::vector<unsigned char> Resample(const unsigned char* src, int w, int h, int dw, int dh) {
    std::vector<unsigned char> out((size_t)dw * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const float sy = std::clamp(((float)y + 0.5f) * (float)h / (float)dh - 0.5f, 0.0f, (float)(h - 1));
        const int y0 = (int)sy, y1 = std::min(y0 + 1, h - 1);
        const float fy = sy - (float)y0;
        for (int x = 0; x < dw; ++x) {
            const float sx = std::clamp(((float)x + 0.5f) * (float)w / (float)dw - 0.5f, 0.0f, (float)(w - 1));
            const int x0 = (int)sx, x1 = std::min(x0 + 1, w - 1);
            const float fx = sx - (float)x0;
            for (int c = 0; c < 4; ++c) {
                const float a = src[((size_t)y0 * w + x0) * 4 + c], b = src[((size_t)y0 * w + x1) * 4 + c];
                const float d = src[((size_t)y1 * w + x0) * 4 + c], e = src[((size_t)y1 * w + x1) * 4 + c];
                const float v = (a + (b - a) * fx) + ((d + (e - d) * fx) - (a + (b - a) * fx)) * fy;
                out[((size_t)y * dw + x) * 4 + c] = (unsigned char)std::clamp((int)std::lround(v), 0, 255);
            }
        }
    }
    return out;
}
} // namespace

int BloodRenderer::DecalSet(const std::string& name) const {
    for (size_t i = 0; i < m_SetNames.size(); ++i)
        if (m_SetNames[i] == name) return (int)i;
    return -1;
}

bool BloodRenderer::BuildAtlas() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::u8path(ProjectPaths::Resolve("assets/Effects/Blood/decals"));
    struct Img { std::string Name; int W, H; std::vector<unsigned char> Norm, Mask; int X = 0, Y = 0; };
    std::vector<Img> imgs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string file = e.path().filename().u8string();
        const size_t at = file.rfind("_norm.png");
        if (at == std::string::npos || at + 9 != file.size()) continue;
        const std::string name = file.substr(0, at);
        int nw, nh, nc, mw, mh, mc;
        unsigned char* n = stbi_load((dir / file).u8string().c_str(), &nw, &nh, &nc, 4);
        unsigned char* m = stbi_load((dir / (name + "_mask.png")).u8string().c_str(), &mw, &mh, &mc, 4);
        if (n && m) {
            const float s = std::min(1.0f, (float)kMaxDecalSide / (float)std::max(nw, nh));
            Img img;
            img.Name = name;
            img.W = std::max(4, (int)std::lround(nw * s));
            img.H = std::max(4, (int)std::lround(nh * s));
            img.Norm = Resample(n, nw, nh, img.W, img.H);
            img.Mask = Resample(m, mw, mh, img.W, img.H);
            imgs.push_back(std::move(img));
        } else {
            Log::Warn("Blood: decal set '" + name + "' is missing its normal or mask image");
        }
        if (n) stbi_image_free(n);
        if (m) stbi_image_free(m);
    }
    if (imgs.empty()) return false;
    std::sort(imgs.begin(), imgs.end(), [](const Img& a, const Img& b) { return a.H != b.H ? a.H > b.H : a.Name < b.Name; });
    // Shelves, tallest first.
    int x = kAtlasPad, y = kAtlasPad, shelf = 0;
    for (Img& img : imgs) {
        if (x + img.W + kAtlasPad > kAtlasWidth) { x = kAtlasPad; y += shelf + kAtlasPad; shelf = 0; }
        img.X = x;
        img.Y = y;
        x += img.W + kAtlasPad;
        shelf = std::max(shelf, img.H);
    }
    int height = 64;
    while (height < y + shelf + kAtlasPad) height *= 2;
    std::vector<unsigned char> norm((size_t)kAtlasWidth * height * 4, 0), mask((size_t)kAtlasWidth * height * 4, 0);
    for (const Img& img : imgs) {
        for (int r = 0; r < img.H; ++r) {
            std::memcpy(&norm[((size_t)(img.Y + r) * kAtlasWidth + img.X) * 4], &img.Norm[(size_t)r * img.W * 4], (size_t)img.W * 4);
            std::memcpy(&mask[((size_t)(img.Y + r) * kAtlasWidth + img.X) * 4], &img.Mask[(size_t)r * img.W * 4], (size_t)img.W * 4);
        }
        m_SetNames.push_back(img.Name);
        // Half a texel in, so bilinear never reads the padding at the rect's edge.
        const glm::vec4 rect(((float)img.X + 0.5f) / kAtlasWidth, ((float)img.Y + 0.5f) / height, ((float)img.W - 1.0f) / kAtlasWidth,
                             ((float)img.H - 1.0f) / height);
        m_RectNorm.push_back(rect);
        m_RectMask.push_back(rect);
    }
    auto upload = [&](unsigned int& tex, const std::vector<unsigned char>& px) {
        glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        glTextureStorage2D(tex, 5, GL_RGBA8, kAtlasWidth, height);
        glTextureSubImage2D(tex, 0, 0, 0, kAtlasWidth, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glGenerateTextureMipmap(tex);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const float aniso = 4.0f;
        glTextureParameterfv(tex, GL_TEXTURE_MAX_ANISOTROPY, &aniso);
    };
    upload(m_AtlasNorm, norm);
    upload(m_AtlasMask, mask);
    {   // The fade across a box's depth; a flat 1 when the image is missing.
        int lw = 0, lh = 0, lc = 0;
        unsigned char* l = stbi_load((dir / "lookup.png").u8string().c_str(), &lw, &lh, &lc, 4);
        std::vector<unsigned char> row = l ? std::vector<unsigned char>(l, l + (size_t)lw * 4) : std::vector<unsigned char>(4, 255);
        if (!l) lw = 1;
        if (l) stbi_image_free(l);
        glCreateTextures(GL_TEXTURE_2D, 1, &m_Lookup);
        glTextureStorage2D(m_Lookup, 1, GL_RGBA8, lw, 1);
        glTextureSubImage2D(m_Lookup, 0, 0, 0, lw, 1, GL_RGBA, GL_UNSIGNED_BYTE, row.data());
        glTextureParameteri(m_Lookup, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_Lookup, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_Lookup, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_Lookup, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    Log::Info("Blood: " + std::to_string(imgs.size()) + " decal sets in a " + std::to_string(kAtlasWidth) + " x " +
              std::to_string(height) + " atlas");
    return true;
}

void BloodRenderer::AddDecal(const Decal& d) {
    const bool set = d.Set >= 0 && d.Set < (int)m_SetNames.size();
    const KnifeFxLibrary::Entry* k = KnifeFxLibrary::Get().At(d.Knife);
    const bool knife = k && k->Lib != KnifeFxImport::Library::Sprite;
    if ((set || knife) && d.Opacity > 0.0f && d.Cutout < 1.0f) m_Decals.push_back(d);
}

int BloodRenderer::DrawDecals(const glm::mat4& view, const glm::mat4& proj, const int viewport[4], const HdrTarget& target,
                              const std::function<void(Shader&)>& applyFrameState) {
    if (m_Decals.empty()) return 0;
    const KnifeFxLibrary& lib = KnifeFxLibrary::Get();
    PROFILE_GPU_SCOPE("Blood Decals");
    const Frustum frustum = Frustum::FromViewProj(proj * view);
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    static std::vector<GpuDecal> gpu;
    gpu.clear();
    for (const Decal& d : m_Decals) {
        const AABB box = AABB{glm::vec3(-0.5f), glm::vec3(0.5f)}.Transformed(d.Model);
        if (!frustum.Intersects(box)) continue;
        if (!WorthDrawing((box.Min + box.Max) * 0.5f, glm::length(box.Max - box.Min) * 0.5f, eye, DecalMaxDistance)) continue;
        GpuDecal g;
        g.Model = d.Model;
        g.InvModel = glm::inverse(d.Model);
        g.Params = glm::vec4(d.Cutout, d.Dry, d.Opacity, d.NormalStrength);
        g.Axis = glm::vec4(glm::normalize(glm::vec3(d.Model[1])), d.Rim);
        g.Knife = glm::ivec4(-1, -1, 0, 0);
        g.Grid = glm::vec4(1.0f, 1.0f, 0.0f, 0.5f);
        g.Kind = glm::ivec4(0, 0, 0, d.Mirror ? 1 : 0);
        if (const KnifeFxLibrary::Entry* k = lib.At(d.Knife)) {
            if (k->Lib == KnifeFxImport::Library::Sprite) continue;
            g.RectNorm = g.RectMask = glm::vec4(0.0f);
            g.Knife = glm::ivec4(k->ColorLayer, k->NormalLayer, d.Cell, d.NextCell);
            g.Grid = glm::vec4((float)k->Cols, (float)k->Rows, d.CellBlend, k->Smoothness);
            g.Kind = glm::ivec4(k->Lib == KnifeFxImport::Library::DecalLarge ? 0 : 1, (int)k->Flags, d.Blood ? 1 : 0, d.Mirror ? 1 : 0);
        } else {
            if (!m_AtlasNorm || d.Set < 0) continue;
            g.RectNorm = m_RectNorm[d.Set];
            g.RectMask = m_RectMask[d.Set];
        }
        gpu.push_back(g);
    }
    if (gpu.empty()) return 0;
    if (!m_DecalProgram)
        m_DecalProgram = std::make_unique<Shader>(ShaderLibrary::ReadFile("BloodDecal.vert.glsl"),
                                                  ShaderLibrary::ReadFile("BloodDecal.frag.glsl"), "BloodDecal");
    if (gpu.size() > m_DecalCapacity) {
        if (m_DecalBuffer) glDeleteBuffers(1, &m_DecalBuffer);
        m_DecalCapacity = std::max<size_t>(gpu.size(), 128);
        glCreateBuffers(1, &m_DecalBuffer);
        glNamedBufferStorage(m_DecalBuffer, (GLsizeiptr)(m_DecalCapacity * sizeof(GpuDecal)), nullptr, GL_DYNAMIC_STORAGE_BIT);
    }
    glNamedBufferSubData(m_DecalBuffer, 0, (GLsizeiptr)(gpu.size() * sizeof(GpuDecal)), gpu.data());

    target.ResolveDepthOnly(); // the static geometry's depth, read while the MSAA target stays bound
    Shader& prog = *m_DecalProgram;
    applyFrameState(prog);
    prog.SetMat4("uView", view);
    prog.SetMat4("uProj", proj);
    prog.SetMat4("uInvViewProj", glm::inverse(proj * view));
    prog.SetVec4("uViewport", glm::vec4((float)viewport[0], (float)viewport[1], (float)viewport[2], (float)viewport[3]));
    prog.SetVec3("uFreshColor", FilmFresh);
    prog.SetVec3("uDriedColor", FilmDried);
    prog.SetVec3("uRough", glm::vec3(BloodPalette::RoughFresh, BloodPalette::RoughPool, BloodPalette::RoughDried));
    prog.SetInt("uNoReceiveShadows", 0);
    prog.SetInt("uObjectLayerBit", 0);
    glBindTextureUnit(kDepthUnit, target.ResolvedDepthTexture());
    glBindTextureUnit(kNormUnit, m_AtlasNorm);
    glBindTextureUnit(kMaskUnit, m_AtlasMask);
    glBindTextureUnit(kLookupUnit, m_Lookup);
    glBindTextureUnit(kLargeColorUnit, lib.ColorArray(KnifeFxImport::Library::DecalLarge));
    glBindTextureUnit(kLargeNormalUnit, lib.NormalArray(KnifeFxImport::Library::DecalLarge));
    glBindTextureUnit(kSmallColorUnit, lib.ColorArray(KnifeFxImport::Library::DecalSmall));
    glBindTextureUnit(kSmallNormalUnit, lib.NormalArray(KnifeFxImport::Library::DecalSmall));
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kDecalBinding, m_DecalBuffer);
    glBindVertexArray(m_EmptyVao);
    // The box's far faces only (the eye may be inside it), no depth test: the depth texture decides.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_SRC1_COLOR); // dst = add + dst * mul
    glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 36, (int)gpu.size(), 0);
    glDisable(GL_BLEND);
    glCullFace(GL_BACK);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
    GLStateCache::Invalidate();
    return 1;
}
