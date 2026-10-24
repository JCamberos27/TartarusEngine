#include "BloodRenderer.h"

#include "DecalAsset.h"
#include "DecalSystem.h"
#include "Frustum.h"
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
    if (!ListSets()) Log::Warn("Blood: no decal images in assets/Effects/Blood/decals - blood leaves no stains");
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

void BloodRenderer::WarmUp() { EnsureProgram(); }

void BloodRenderer::BeginFrame() {
    m_Sprays.clear();
    m_QueuedDecals = 0;
    m_SplatRanges.clear();
    m_SplatCount = 0;
    // The stains and holes are the DecalSystem's runtime decals now: a game frame starts their list over
    // (PlaySession::Stop's BeginFrame clears them out of the editor too).
    if (DecalSystem* ds = DecalSystem::Current()) ds->BeginRuntimeFrame();
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

void BloodRenderer::SetSplatPalette(unsigned entity, int palette) {
    if (palette <= 0) m_SplatPalettes.erase(entity);
    else m_SplatPalettes[entity] = palette;
}

void BloodRenderer::SplatLook(unsigned entity, glm::vec4& fresh, glm::vec4& dried) const {
    const auto it = m_SplatPalettes.find(entity);
    const BloodPalette::Look& l = BloodPalette::Of(it == m_SplatPalettes.end() ? 0 : it->second);
    fresh = glm::vec4(l.Fresh, l.RoughCloth);
    dried = glm::vec4(l.Dried, l.RoughDried);
}

void BloodRenderer::SetSplats(const std::vector<Splat>& splats, const std::vector<std::pair<unsigned, glm::ivec2>>& ranges) {
    DecalPerf::SubmitScope perf;
    m_SplatRanges.clear();
    m_SplatPalettes.clear();
    m_SplatCount = 0;
    DecalSystem* ds = DecalSystem::Current();
    if (splats.empty() || m_SetNames.empty() || !ds) return;
    static std::vector<GpuSplat> gpu;
    gpu.clear();
    for (const Splat& s : splats) {
        // The set's slot in the decal arrays (the record itself stays in bind space). A set still being made or
        // decoded keeps its place in the list at no opacity, so the entities' runs hold.
        const int set = s.Set >= 0 && s.Set < (int)m_SetNames.size() ? s.Set : 0;
        const DecalTextureArrays::Source* src = SetMaps(set);
        const DecalTextureArrays::Slot* slot = src ? ds->Textures().Request(*src) : nullptr;
        const bool ready = slot && (slot->TexFlags & Decals::FLAG_HAS_AUX_TEX);
        gpu.push_back({glm::vec4(s.Center, s.Radius), glm::vec4(s.Normal, s.Depth),
                       glm::vec4(s.Tangent, ready ? (float)slot->MaxLod : 0.0f), ready ? slot->Rect : glm::vec4(0.0f),
                       glm::vec4(s.Cutout, s.Dry, ready ? s.Opacity : 0.0f, ready ? (float)slot->Alloc.Layer : 0.0f)});
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
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSplatBinding, m_SplatBuffer); // the maps: the decal arrays (units 22-25)
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
    // By palette, then sim: each palette's fluid is one material (its colour, roughness, the light through it).
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return m_Sprays[a].Palette != m_Sprays[b].Palette ? m_Sprays[a].Palette < m_Sprays[b].Palette : m_Sprays[a].Sim < m_Sprays[b].Sim;
    });
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
    int palette = 0; // a person's, set above
    for (size_t first = 0; first < order.size();) {
        const int sim = m_Sprays[order[first]].Sim;
        const int pal = m_Sprays[order[first]].Palette;
        size_t last = first;
        while (last < order.size() && m_Sprays[order[last]].Sim == sim && m_Sprays[order[last]].Palette == pal) ++last;
        if (pal != palette) {
            palette = pal;
            const BloodPalette::Look& l = BloodPalette::Of(pal);
            prog.SetVec3("uBaseColor", pal == 0 ? FluidAlbedo : l.Fresh);
            prog.SetFloat("uRoughness", pal == 0 ? FluidRoughness : l.RoughSpray);
            prog.SetVec3("uSubsurfaceColor", l.Subsurface);
            prog.SetFloat("uThickness", l.Thickness);
        }
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
// Each stain or hole is a clustered decal (DecalSystem's runtime list). The maps are made once into Library/DecalMaps:
// a KriptoFX set from its _norm / _mask PNGs, a Knife entry from its layer of the library's arrays (read back), the
// procedural hole from WeaponFx's formula (GeneratedDecalMaps.h, DecalBake). The response (fades, channels,
// roughness) comes from DA_Blood.decal and DA_BulletHole.decal.

namespace {
constexpr int kSetTier = 512;
// Bump when what makes the maps changes (the files under Library/DecalMaps are then made again).
constexpr const char* kMapsVersion = "v3";
constexpr int kBloodSort = 100, kHoleSort = 101; // over the authored decals; holes over the blood
constexpr float kDryWear = 0.15f;                // a fully dried stain flakes this much (Wear chips; more reads as holes)

const char* ModeName(DecalBake::KnifeMode m) {
    return m == DecalBake::KnifeMode::Hole ? "hole" : m == DecalBake::KnifeMode::BloodAlbedo ? "albedo" : "mask";
}
} // namespace

bool BloodRenderer::ListSets() {
    namespace fs = std::filesystem;
    m_SetDir = ProjectPaths::Resolve("assets/Effects/Blood/decals");
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(m_SetDir), ec)) {
        const std::string file = e.path().filename().u8string();
        const size_t at = file.rfind("_norm.png");
        if (at == std::string::npos || at + 9 != file.size()) continue;
        const std::string name = file.substr(0, at);
        if (!fs::exists(fs::u8path(m_SetDir) / (name + "_mask.png"), ec)) {
            Log::Warn("Blood: decal set '" + name + "' is missing its mask image");
            continue;
        }
        m_SetNames.push_back(name);
    }
    std::sort(m_SetNames.begin(), m_SetNames.end());
    if (!m_SetNames.empty()) Log::Info("Blood: " + std::to_string(m_SetNames.size()) + " decal sets (clustered decals)");
    return !m_SetNames.empty();
}

int BloodRenderer::DecalSet(const std::string& name) const {
    for (size_t i = 0; i < m_SetNames.size(); ++i)
        if (m_SetNames[i] == name) return (int)i;
    return -1;
}

const DecalTextureArrays::Source* BloodRenderer::SetMaps(int set) {
    if (set < 0 || set >= (int)m_SetNames.size()) return nullptr;
    if (m_SetSrc.size() != m_SetNames.size()) m_SetSrc.assign(m_SetNames.size(), nullptr);
    if (m_SetSrc[(size_t)set]) return m_SetSrc[(size_t)set];
    const std::string name = m_SetNames[set];
    const std::string dir = m_SetDir;
    return m_SetSrc[(size_t)set] = GeneratedDecalMaps::Get().Find("blood_" + name + "_" + kMapsVersion, kSetTier, [name, dir]() -> GeneratedDecalMaps::Job {
        return [name, dir]() {
            namespace fs = std::filesystem;
            int nw = 0, nh = 0, nc = 0, mw = 0, mh = 0, mc = 0;
            unsigned char* n = stbi_load((fs::u8path(dir) / (name + "_norm.png")).u8string().c_str(), &nw, &nh, &nc, 4);
            unsigned char* m = stbi_load((fs::u8path(dir) / (name + "_mask.png")).u8string().c_str(), &mw, &mh, &mc, 4);
            GeneratedDecalMaps::Images im;
            if (n && m && nw == mw && nh == mh) im = DecalBake::BloodSet(n, m, nw, nh);
            if (n) stbi_image_free(n);
            if (m) stbi_image_free(m);
            return im;
        };
    });
}

const DecalTextureArrays::Source* BloodRenderer::KnifeMaps(int knife, DecalBake::KnifeMode mode) {
    const KnifeFxLibrary& lib = KnifeFxLibrary::Get();
    const KnifeFxLibrary::Entry* k = lib.At(knife);
    if (!k || k->Lib == KnifeFxImport::Library::Sprite) return nullptr;
    const unsigned color = lib.ColorArray(k->Lib), normal = lib.NormalArray(k->Lib);
    if (!color) return nullptr;
    const int size = KnifeFxImport::LibraryLayerSize(k->Lib);
    if (size <= 0 || !glGetTextureSubImage) return nullptr;
    const int tier = std::min(size, 2048);
    const int cacheKey = knife * 4 + (int)mode;
    if (const auto it = m_KnifeSrc.find(cacheKey); it != m_KnifeSrc.end() && it->second) return it->second;
    const std::string key = "knife_" + k->Name + "_" + ModeName(mode) + "_" + kMapsVersion;
    return m_KnifeSrc[cacheKey] = GeneratedDecalMaps::Get().Find(key, tier, [&, k, color, normal, size, mode]() -> GeneratedDecalMaps::Job {
        // Read the entry's layers back on this (the GL) thread; the worker re-packs them.
        auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)size * size * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTextureSubImage(color, 0, 0, 0, k->ColorLayer, size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE, (GLsizei)rgba->size(), rgba->data());
        std::shared_ptr<std::vector<uint8_t>> rg;
        if (normal && k->NormalLayer >= 0) {
            rg = std::make_shared<std::vector<uint8_t>>((size_t)size * size * 2);
            glGetTextureSubImage(normal, 0, 0, 0, k->NormalLayer, size, size, 1, GL_RG, GL_UNSIGNED_BYTE, (GLsizei)rg->size(), rg->data());
        }
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        const int cols = k->Cols, rows = k->Rows;
        return [rgba, rg, size, cols, rows, mode]() {
            return DecalBake::KnifeLayer(rgba->data(), rg ? rg->data() : nullptr, size, cols, rows, mode);
        };
    });
}

glm::mat4 BloodRenderer::DecalBox(const glm::mat4& model, bool mirror) {
    // The blood box: x the image's right, +y out of the surface, the image's top at -z (Unity's decal uv).
    glm::mat4 m(1.0f);
    m[0] = mirror ? -model[0] : model[0];
    m[1] = -model[2];
    m[2] = model[1];
    m[3] = model[3];
    return m;
}

void BloodRenderer::FetchAssets() {
    DecalSystem* ds = DecalSystem::Current();
    if (!ds || (ds == m_AssetSystem && ds->AssetGeneration() == m_AssetGen && m_BloodAsset && m_HoleAsset)) return;
    m_AssetSystem = ds;
    m_AssetGen = ds->AssetGeneration();
    m_BloodAsset = ds->GetAsset(kBloodDecal);
    m_HoleAsset = ds->GetAsset(kHoleDecal);
}

bool BloodRenderer::HoleLook(int knife, RuntimeDecal& r) {
    DecalSystem* ds = DecalSystem::Current();
    if (!ds) return false;
    FetchAssets();
    const std::shared_ptr<DecalAsset>& a = m_HoleAsset;
    const KnifeFxLibrary::Entry* k = KnifeFxLibrary::Get().At(knife);
    const DecalTextureArrays::Source* src = nullptr;
    if (k) {
        src = KnifeMaps(knife, DecalBake::KnifeMode::Hole);
        r.Cols = k->Cols;
        r.Rows = k->Rows;
        r.Roughness = 1.0f - k->Smoothness;
    } else {
        if (!m_ProceduralHoleSrc)
            m_ProceduralHoleSrc = GeneratedDecalMaps::Get().Find(std::string("hole_procedural_") + kMapsVersion, 512, []() -> GeneratedDecalMaps::Job {
                return [] { return DecalBake::ProceduralHole(256); };
            });
        src = m_ProceduralHoleSrc;
        r.Cols = r.Rows = 2;
        r.Roughness = a && !a->Missing ? a->Roughness : 0.9f;
    }
    if (!src) return false;
    r.Maps = src;
    r.Tint = glm::vec3(1.0f);
    const bool asset = a && !a->Missing;
    r.AlbedoOp = asset ? a->AlbedoOp : 1.0f;
    r.NormalOp = asset ? a->NormalOp : 1.0f;
    r.RoughnessOp = asset ? a->RoughnessOp : 1.0f;
    r.AngleFadeStart = asset ? a->AngleFadeStart : 0.3f;
    r.AngleFadeEnd = asset ? a->AngleFadeEnd : 0.1f;
    r.EdgeFade = asset ? a->EdgeFade : 0.05f;
    r.AffectStatic = r.AffectDynamic = true; // the hole goes with whatever it is in (BulletHoleList keeps it in that entity's space)
    r.SortOrder = kHoleSort;
    return true;
}

int BloodRenderer::ToRuntime(const Decal& d, RuntimeDecal out[2]) {
    if (d.Opacity <= 0.0f || d.Cutout >= 1.0f) return 0;
    const KnifeFxLibrary::Entry* k = KnifeFxLibrary::Get().At(d.Knife);
    if (k && k->Lib == KnifeFxImport::Library::Sprite) return 0;
    DecalSystem* ds = DecalSystem::Current();
    if (!ds) return 0;
    RuntimeDecal r;
    r.BoxToWorld = DecalBox(d.Model, d.Mirror);
    if (k && !d.Blood) { // a bullet hole: its surface's own broken material
        if (!HoleLook(d.Knife, r)) return 0;
        r.AlbedoOp *= d.Opacity;
        r.NormalOp *= d.NormalStrength;
        r.Frame = d.Cell;
        if (d.Rim > 0.0f) {
            r.Reveal = true;
            r.RevealThreshold = DecalBake::HoleRimThreshold(d.Rim);
            r.RevealBand = DecalBake::HoleRimBand(d.Rim);
        }
    } else {
        const DecalTextureArrays::Source* src = nullptr;
        float band = 0.05f; // the old pass's (reveal - cutout) x 20
        const BloodPalette::Look& look = BloodPalette::Of(d.Palette);
        float wet = look.RoughFresh;
        if (k) {
            const bool albedo = (k->Flags & KnifeFxImport::EntryAlbedo) != 0; // the entry's own stain image
            src = KnifeMaps(d.Knife, albedo ? DecalBake::KnifeMode::BloodAlbedo : DecalBake::KnifeMode::BloodMask);
            r.Cols = k->Cols;
            r.Rows = k->Rows;
            r.Frame = d.Cell;
            band = 0.18f; // its alpha x 6, through the reveal's (0.6 + 0.6 r) at mid-cell
            if (albedo && k->Lib == KnifeFxImport::Library::DecalLarge) wet = look.RoughPool; // the large library: pools
        } else {
            src = SetMaps(d.Set);
        }
        if (!src) return 0;
        FetchAssets();
        const std::shared_ptr<DecalAsset>& a = m_BloodAsset;
        const bool asset = a && !a->Missing;
        const float dry = std::clamp(d.Dry, 0.0f, 1.0f);
        r.Maps = src;
        // A person's blood is the renderer's film (Blood Lab tunes it); another palette's its own, a thin fresh film of
        // it letting the surface through (FilmOpacity) until it dries.
        r.Tint = d.Palette == 0 ? glm::mix(FilmFresh, FilmDried, dry) : glm::mix(look.Fresh, look.Dried, dry);
        r.AlbedoOp = (asset ? a->AlbedoOp : 1.0f) * d.Opacity * glm::mix(look.FilmOpacity, 1.0f, dry);
        r.NormalOp = (asset ? a->NormalOp : 1.0f) * d.NormalStrength / 0.6f; // the asset's normal is the default strength's
        r.RoughnessOp = (asset ? a->RoughnessOp : 1.0f) * d.Opacity;
        // Wet blood is glossy (the palette's film / pool roughness), dried matte; dried blood also flakes.
        if (asset && d.Palette == 0 && wet == BloodPalette::RoughFresh) wet = a->Roughness;
        r.Roughness = glm::mix(wet, look.RoughDried, dry);
        r.Wear = kDryWear * dry;
        r.AngleFadeStart = asset ? a->AngleFadeStart : 0.65f;
        r.AngleFadeEnd = asset ? a->AngleFadeEnd : 0.35f;
        r.EdgeFade = asset ? a->EdgeFade : 0.08f;
        r.Liquid = true; // a film: it fills the surface's grain, as the old pass's flat film did
        r.CoverGain = k ? 6.0f : 2.0f; // the maps keep the packs' raw alpha; the old pass's gain, after filtering
        r.Reveal = true; // the cutout eats the reveal order
        r.RevealThreshold = d.Cutout;
        r.RevealBand = band;
        r.AffectStatic = true; // the static world, as the blood decals always were
        r.AffectDynamic = false;
        r.SortOrder = kBloodSort;
    }
    out[0] = r;
    if (d.CellBlend > 0.0f && d.NextCell != d.Cell) { // a flipbook between two cells: the next one over it
        out[1] = r;
        out[1].Frame = d.NextCell;
        out[1].AlbedoOp *= d.CellBlend;
        out[1].RoughnessOp *= d.CellBlend;
        out[1].NormalOp *= d.CellBlend;
        return 2;
    }
    return 1;
}

int BloodRenderer::AddDecal(const Decal& d) {
    DecalPerf::SubmitScope perf;
    DecalSystem* ds = DecalSystem::Current();
    if (!ds) return 0;
    RuntimeDecal r[2];
    const int n = ToRuntime(d, r);
    for (int i = 0; i < n; ++i) {
        r[i].Id = (uint32_t)(m_QueuedDecals * 2 + i);
        ds->SubmitRuntime(r[i]);
    }
    if (n > 0) ++m_QueuedDecals;
    return n;
}
