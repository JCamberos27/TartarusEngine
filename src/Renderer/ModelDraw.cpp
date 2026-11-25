// Model drawing: material binds (fixed and data-driven), per-mesh render state, the draw and depth-only passes.

#include "Model.h"
#include <chrono>
#include "DecalSystem.h"
#include "SkinHideBuffer.h"
#include "MaterialAsset.h"
#include "ShaderAsset.h"
#include "Texture.h"
#include "Shader.h"
#include "WindUniforms.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "DefaultTextures.h"
#include "gl.h"


#include <cmath>
#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include "ModelDrawInternal.h"

namespace ModelInternal {

MaterialLocs ResolveMaterialLocs(Shader& shader) {
    MaterialLocs L;
    L.baseColor = shader.Loc("uBaseColor");
    L.alphaClip = shader.Loc("uAlphaClip");
    L.alphaCutoff = shader.Loc("uAlphaCutoff");
    L.metallic = shader.Loc("uMetallic");
    L.roughness = shader.Loc("uRoughness");
    L.emissiveColor = shader.Loc("uEmissiveColor");
    L.triplanar = shader.Loc("uTriplanar");
    L.triplanarScale = shader.Loc("uTriplanarScale");
    L.triplanarBreakup = shader.Loc("uTriplanarBreakup");
    L.hasAlbedo = shader.Loc("uHasAlbedoMap");
    L.albedoMap = shader.Loc("uAlbedoMap");
    L.hasNormal = shader.Loc("uHasNormalMap");
    L.normalMap = shader.Loc("uNormalMap");
    L.hasMetallicRoughness = shader.Loc("uHasMetallicRoughnessMap");
    L.metallicRoughnessMap = shader.Loc("uMetallicRoughnessMap");
    L.hasMetallic = shader.Loc("uHasMetallicMap");
    L.metallicMap = shader.Loc("uMetallicMap");
    L.hasRoughness = shader.Loc("uHasRoughnessMap");
    L.roughnessMap = shader.Loc("uRoughnessMap");
    L.hasAO = shader.Loc("uHasAOMap");
    L.aoMap = shader.Loc("uAOMap");
    L.hasEmissive = shader.Loc("uHasEmissiveMap");
    L.emissiveMap = shader.Loc("uEmissiveMap");
    L.hasHeight = shader.Loc("uHasHeightMap");
    L.heightMap = shader.Loc("uHeightMap");
    L.hasDetailAlbedo = shader.Loc("uHasDetailAlbedoMap");
    L.detailAlbedoMap = shader.Loc("uDetailAlbedoMap");
    L.hasDetailNormal = shader.Loc("uHasDetailNormalMap");
    L.detailNormalMap = shader.Loc("uDetailNormalMap");
    L.hasGrunge = shader.Loc("uHasGrungeMap");
    L.grungeMap = shader.Loc("uGrungeMap");
    L.useSkinning = shader.Loc("uUseSkinning");
    L.skinBase = shader.Loc("uSkinBase");
    L.uvTiling = shader.Loc("uUVTiling");
    L.uvOffset = shader.Loc("uUVOffset");
    L.depthAlbedo = shader.Loc("uAlbedo");
    L.alphaTest = shader.Loc("uAlphaTest");
    L.normalStrength = shader.Loc("uNormalStrength");
    L.normalFlipY = shader.Loc("uNormalFlipY");
    L.doubleSided = shader.Loc("uDoubleSided");
    L.useVertexColor = shader.Loc("uUseVertexColor");
    L.noSpecularHighlights = shader.Loc("uNoSpecularHighlights");
    L.noGlossyReflections = shader.Loc("uNoGlossyReflections");
    L.parallaxScale = shader.Loc("uParallaxScale");
    L.detailTiling = shader.Loc("uDetailTiling");
    L.grungeStrength = shader.Loc("uGrungeStrength");
    L.grungeScale = shader.Loc("uGrungeScale");
    L.receiveDecals = shader.Loc("uReceiveDecals");
    L.noWetness = shader.Loc("uNoWetness");
    L.foliageSurface = shader.Loc("uFoliageSurface");
    L.materialSubstrate = shader.Loc("uMaterialSubstrate");
    L.foliage = shader.Loc("uFoliage");
    L.windVertexMask = shader.Loc("uWindVertexMask");
    L.translucency = shader.Loc("uTranslucency");
    L.windStrength = shader.Loc("uWindStrength");
    L.windFrequency = shader.Loc("uWindFrequency");
    L.windFlutter = shader.Loc("uWindFlutter");
    L.windHeight = shader.Loc("uWindHeight");
    L.opacity = shader.Loc("uOpacity");
    L.hideVertBase = shader.Loc("uHideVertBase");
    L.clothInterior = shader.Loc("uClothInterior");
    L.model = shader.Loc("uModel");
    L.normalMatrix = shader.Loc("uNormalMatrix");
    return L;
}

// ResolveMaterialLocs once per program rather than once per draw.
const MaterialLocs& CachedMaterialLocs(Shader& shader) {
    if (const void* block = shader.LocationBlock()) return *static_cast<const MaterialLocs*>(block);
    auto block = std::make_shared<MaterialLocs>(ResolveMaterialLocs(shader));
    const MaterialLocs& locs = *block;
    shader.SetLocationBlock(std::move(block));
    return locs;
}
} // namespace ModelInternal

using ModelInternal::CachedMaterialLocs;
using ModelInternal::MaterialLocs;

namespace {
// Foliage wind (FoliageWind.glsl): the frame's clock and gust direction, once per program per frame - they are the same
// for every material, so the material-bind skip must not be what carries them.
void SetWindFrame(Shader& shader) {
    static std::unordered_map<unsigned int, std::uint32_t> done;
    std::uint32_t& last = done[shader.Program()];
    if (last == Material::HashFrame()) return;
    last = Material::HashFrame();
    SetWindFieldUniforms(shader);
}
// The per-material half. uFoliage = 0 for everything else, so a program last used by a foliage material is reset.
void SetFoliageUniforms(Shader& shader, const Material& mat, const MaterialLocs& L) {
    shader.SetInt(L.foliage, mat.Foliage ? 1 : 0);
    shader.SetInt(L.windVertexMask, mat.UseVertexColor ? 1 : 0);
    if (!mat.Foliage) { shader.SetFloat(L.translucency, 0.0f); return; }
    shader.SetFloat(L.windStrength, mat.WindStrength);
    shader.SetFloat(L.windFrequency, mat.WindFrequency);
    shader.SetFloat(L.windFlutter, mat.WindFlutter);
    shader.SetFloat(L.windHeight, mat.WindHeight);
    shader.SetFloat(L.translucency, mat.Translucency);
}

// #102 / #113 — the surface-option uniforms, set for BOTH the built-in and the data-driven
// (Standard.shader) paths so a shader that declares them gets them either way.
void SetSurfaceOptions(Shader& shader, const Material& mat, const MaterialLocs& L) {
    shader.SetVec2(L.uvTiling, mat.UVTiling);
    shader.SetVec2(L.uvOffset, mat.UVOffset);
    shader.SetFloat(L.normalStrength, mat.NormalStrength);
    shader.SetInt(L.normalFlipY, mat.NormalFlipY ? 1 : 0);
    shader.SetInt(L.doubleSided, mat.DoubleSided ? 1 : 0);
    shader.SetInt(L.useVertexColor, mat.UseVertexColor ? 1 : 0);
    shader.SetInt(L.noSpecularHighlights, mat.SpecularHighlights ? 0 : 1);
    shader.SetInt(L.noGlossyReflections, mat.GlossyReflections ? 0 : 1);
    shader.SetFloat(L.parallaxScale, mat.ParallaxScale);
    shader.SetVec2(L.detailTiling, mat.DetailTiling);
    // Grunge and decals (ModelDecals.glsl); the Standard shaders' properties set the same uniforms.
    shader.SetFloat(L.grungeStrength, mat.GrungeStrength);
    shader.SetFloat(L.grungeScale, mat.GrungeScale);
    shader.SetInt(L.receiveDecals, mat.ReceiveDecals ? 1 : 0);
    shader.SetInt(L.noWetness, mat.NoWetness ? 1 : 0);
    shader.SetInt(L.foliageSurface, mat.Foliage ? 1 : 0);
    SetFoliageUniforms(shader, mat, L);
    // Its substrate's row in the decal system's table (index + 1, 0 = none / still decoding).
    shader.SetInt(L.materialSubstrate, mat.Substrate.Enabled() ? DecalSystem::MaterialSubstrate(mat.Substrate) : 0);
}

void BindMaterial(Shader& shader, const Material& mat, const MaterialLocs& locs) {
    // #192: ~12 uniform uploads + up to 7 texture binds below, almost all redundant when the
    // previous mesh drew with this same material under this same program. The draw loop sorts
    // by material so consecutive meshes usually match; GLStateCache clears this on Invalidate()
    // (end of frame + after every raw-GL pass), so an edited material can't be wrongly skipped.
    if (GLStateCache::MaterialAlreadyBound(mat.Hash(), shader.Program())) return;

    shader.SetVec3(locs.baseColor, mat.BaseColor);
    shader.SetFloat(locs.metallic, mat.Metallic);
    shader.SetFloat(locs.roughness, mat.Roughness);
    shader.SetVec3(locs.emissiveColor, mat.EmissiveColor * mat.EmissiveStrength);
    shader.SetInt(locs.triplanar, mat.Triplanar ? 1 : 0);
    shader.SetFloat(locs.triplanarScale, mat.TriplanarScale);
    shader.SetFloat(locs.triplanarBreakup, mat.TriplanarBreakup);
    shader.SetInt(locs.alphaClip, mat.AlphaClip ? 1 : 0);       // #101
    shader.SetFloat(locs.alphaCutoff, mat.AlphaCutoff);

    // Each map gets a FIXED unit (1..7). An absent map still binds a 1x1 default there, so the
    // driver never sees texture 0 on a sampler unit the program declares (audit GL-101 / #366:
    // was ~7 KHR 131204 warnings per draw). uHas*Map still tells the shader whether to use it.
    auto bindSlot = [&](int unit, const std::shared_ptr<Texture>& tex, int hasLoc, int samplerLoc,
                        unsigned int fallback) {
        // A Texture whose file failed to decode is still a live non-null object with m_ID == 0
        // (Texture.cpp logs and returns from the ctor). Treat it as an empty slot: bind the
        // fallback and leave uHas*Map = 0 so the shader never samples a 0-texture as a real map.
        if (tex && tex->IsValid()) {
            tex->Bind(unit);
            shader.SetInt(hasLoc, 1);
        } else {
            GLStateCache::BindTexture2D(unit, fallback);
            shader.SetInt(hasLoc, 0);
        }
        shader.SetInt(samplerLoc, unit);
    };

    bindSlot(1, mat.AlbedoMap, locs.hasAlbedo, locs.albedoMap, DefaultTextures::White());
    bindSlot(2, mat.NormalMap, locs.hasNormal, locs.normalMap, DefaultTextures::FlatNormal());
    bindSlot(3, mat.MetallicRoughnessMap, locs.hasMetallicRoughness, locs.metallicRoughnessMap, DefaultTextures::White());
    bindSlot(4, mat.MetallicMap, locs.hasMetallic, locs.metallicMap, DefaultTextures::White());
    bindSlot(5, mat.RoughnessMap, locs.hasRoughness, locs.roughnessMap, DefaultTextures::White());
    bindSlot(6, mat.AOMap, locs.hasAO, locs.aoMap, DefaultTextures::White());
    bindSlot(7, mat.EmissiveMap, locs.hasEmissive, locs.emissiveMap, DefaultTextures::Black());
    // #102 — units 16+ (8..15 are the engine's shadow / IBL / SSAO units).
    bindSlot(16, mat.HeightMap, locs.hasHeight, locs.heightMap, DefaultTextures::White());
    bindSlot(17, mat.DetailAlbedoMap, locs.hasDetailAlbedo, locs.detailAlbedoMap, DefaultTextures::White());
    bindSlot(18, mat.DetailNormalMap, locs.hasDetailNormal, locs.detailNormalMap, DefaultTextures::FlatNormal());
    bindSlot(21, mat.GrungeMap, locs.hasGrunge, locs.grungeMap, DefaultTextures::White()); // Decals::UNIT_GRUNGE
    SetSurfaceOptions(shader, mat, locs);
}
// Data-driven BindMaterial using ShaderAsset::Bindings() + MaterialAsset property accessors.
// Activates when the MaterialAsset has a linked ShaderAsset (v2 .mat files referencing a .shader).
// Built-in PBR properties read from `ma.Mat`; every other declared property reads from
// `ma.Mat.ExtraProps` (typed store filled by MaterialAsset::Load, #354).
void BindMaterialDataDriven(Shader& shader, const MaterialAsset& ma, const ShaderAsset& sa) {
    const Material& mat = ma.Mat;
    // #99 — the redundant-bind skip must also see custom (non-builtin) shader properties;
    // Material::Hash() only covers the built-in fields, so two materials differing only in an
    // ExtraProp used to render with whichever was bound first.
    size_t hash = (size_t)mat.HashWithExtraProps();
    hash ^= (size_t)ma.RenderQueue + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    if (GLStateCache::MaterialAlreadyBound(hash, shader.Program())) return;
    const MaterialLocs& locs = CachedMaterialLocs(shader);
    // #101 — cutout for the AlphaTest queue (Standard.shader includes ModelFragment's uAlphaClip).
    shader.SetInt(locs.alphaClip, (mat.AlphaClip || ma.RenderQueue == MaterialAsset::Queue::AlphaTest) ? 1 : 0);
    SetSurfaceOptions(shader, mat, locs); // #102
    shader.SetFloat(locs.alphaCutoff, mat.AlphaCutoff);

    const auto& props    = sa.Properties();
    const auto& bindings = sa.Bindings();
    // The cover layer (Standard shaders' _Cover*): materials without it neither bind its units nor set its uniforms.
    const auto coverIt = mat.ExtraProps.find("_CoverStrength");
    const bool hasCover = coverIt != mat.ExtraProps.end() && coverIt->second.F > 0.0f;
    for (const PropertyBinding& b : bindings) {
        const ShaderProperty& prop = props[b.PropIndex];
        const std::string& pname   = prop.Name;
        if (!hasCover && pname.size() > 6 && pname.compare(0, 6, "_Cover") == 0) continue;
        int slot = 0;
        while (slot < PropertyBinding::kLocSlots && b.LocProgram[slot] != shader.Serial()) ++slot;
        if (slot == PropertyBinding::kLocSlots) { // "_AlbedoMap" → "uAlbedoMap", "uHasAlbedoMap"
            slot = b.LocNext;
            b.LocNext = (std::uint8_t)((b.LocNext + 1) % PropertyBinding::kLocSlots);
            b.LocProgram[slot] = shader.Serial();
            b.UniformLoc[slot] = shader.Loc(b.Uniform);
            b.HasUniformLoc[slot] = b.HasUniform.empty() ? -1 : shader.Loc(b.HasUniform);
        }
        const int uname = b.UniformLoc[slot];
        const bool builtin = b.Builtin;
        const MaterialProp* extra = nullptr;
        if (!builtin) {
            auto it = mat.ExtraProps.find(pname);
            if (it != mat.ExtraProps.end()) extra = &it->second;
        }

        switch (prop.Type) {
        case ShaderPropType::Texture2D: {
            const int hasName = b.HasUniformLoc[slot];
            const std::shared_ptr<Texture>& tex =
                builtin ? (b.GetTex ? b.GetTex(mat) : MaterialAsset::GetTexture(mat, pname))
                        : (extra ? extra->Tex : MaterialAsset::GetTexture(mat, pname) /*null*/);
            if (tex && tex->IsValid()) {
                tex->Bind(b.TextureUnit);
                shader.SetInt(hasName, 1);
            } else {
                // #99 — an absent map still binds its declared default ("white"/"black"/
                // "normal"), like the built-in path does, so the sampler never sees texture 0
                // (KHR 131204) or a stale texture left on that unit by a previous draw.
                const unsigned int fallback = prop.DefaultTex == "normal" ? DefaultTextures::FlatNormal()
                                            : prop.DefaultTex == "black"  ? DefaultTextures::Black()
                                                                          : DefaultTextures::White();
                GLStateCache::BindTexture2D(b.TextureUnit, fallback);
                shader.SetInt(hasName, 0);
            }
            shader.SetInt(uname, b.TextureUnit);
            break;
        }
        case ShaderPropType::Color:
        case ShaderPropType::Vec3:
            shader.SetVec3(uname, builtin ? (b.GetVal ? glm::vec3(b.GetVal(mat)) : MaterialAsset::GetColor(mat, pname))
                                          : (extra ? glm::vec3(extra->V)
                                                   : glm::vec3(prop.DefaultVec)));
            break;
        // #99 — vec2/vec4 uniforms need the matching setter; glUniform3f on them is
        // GL_INVALID_OPERATION and the value was silently never set.
        case ShaderPropType::Vec2:
            shader.SetVec2(uname, builtin ? glm::vec2(b.GetVal ? b.GetVal(mat) : MaterialAsset::GetVec(mat, pname))
                                          : glm::vec2(extra ? extra->V : prop.DefaultVec));
            break;
        case ShaderPropType::Vec4:
            shader.SetVec4(uname, extra ? extra->V : prop.DefaultVec);
            break;
        case ShaderPropType::Float:
            shader.SetFloat(uname, builtin ? (b.GetVal ? b.GetVal(mat).x : MaterialAsset::GetFloat(mat, pname))
                                           : (extra ? extra->F : prop.DefaultFloat));
            break;
        case ShaderPropType::Bool:
        case ShaderPropType::Int:
            shader.SetInt(uname, builtin ? ((b.GetVal ? b.GetVal(mat).x != 0.0f : MaterialAsset::GetBool(mat, pname)) ? 1 : 0)
                                         : (extra ? (prop.Type == ShaderPropType::Bool
                                                         ? (extra->B ? 1 : 0) : extra->I)
                                                  : (prop.DefaultBool ? 1 : 0)));
            break;
        default:
            break;
        }
    }
}
} // namespace

// G5-0: the CPU time a draw call on a skinned model takes (Model::SkinFrameStats::PosedDrawMs).
namespace {
struct PosedDrawTimer {
    explicit PosedDrawTimer(bool skinned) : On(skinned) {
        if (On) Start = std::chrono::steady_clock::now();
    }
    ~PosedDrawTimer() {
        if (On) Model::NotePosedDraw(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count());
    }
    bool On;
    std::chrono::steady_clock::time_point Start;
};
} // namespace

void Model::Draw(Shader& shader, const std::vector<std::shared_ptr<MaterialAsset>>& slots) {
    PosedDrawTimer posedTimer(m_D->BoneCounter > 0);
    SetWindFrame(shader);
    const bool preSkinned = UploadBoneMatrices(shader);
    const MaterialLocs& locs = CachedMaterialLocs(shader);
    int skinBase = m_PreSkin.Base; // this sub-mesh's first vertex in the skinned buffer
    for (int i = 0; i < (int)m_D->Meshes.size(); ++i) {
        if (preSkinned) shader.SetInt(locs.skinBase, skinBase);
        skinBase += (int)m_D->Meshes[i]->VertexCount();
        bool hasSlot = i < (int)slots.size() && slots[i];
        const Material& mat = hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat;
        if (hasSlot && slots[i]->Shader)
            BindMaterialDataDriven(shader, *slots[i], *slots[i]->Shader);
        else
            BindMaterial(shader, mat, locs);
        m_D->Meshes[i]->Draw();
    }
}

namespace {
// #104 — per-mesh ShaderLab render state around a draw. Captures the pass's own state the first
// time a mesh overrides something, applies the override, and puts the pass state back for the
// next mesh without one (and at the end), so passes and meshes with no declared state never pay
// for a glGet or a state change.
class ShaderStateScope {
public:
    // `shared`: a pass state read once for a run of draws (Model::DrawPassState); otherwise this scope's own.
    explicit ShaderStateScope(Model::DrawPassState* shared = nullptr) : m_S(shared ? shared : &m_Own) {}
    ShaderStateScope(const ShaderStateScope&) = delete;
    ShaderStateScope& operator=(const ShaderStateScope&) = delete;
    void Apply(const ShaderRenderState* st) {
        const bool want = st && st->AffectsDraw();
        if (!want) { Restore(); return; }
        if (!m_S->Saved) Save();
        Restore(); // start from the pass state, so fields this shader leaves unset aren't stale
        if (st->Cull == ShaderRenderState::CullMode::Off) glDisable(GL_CULL_FACE);
        else if (st->Cull != ShaderRenderState::CullMode::Unset) {
            glEnable(GL_CULL_FACE);
            glCullFace(st->Cull == ShaderRenderState::CullMode::Front ? GL_FRONT : GL_BACK);
        }
        if (st->ZWrite >= 0) glDepthMask(st->ZWrite ? GL_TRUE : GL_FALSE);
        if (st->ZTest) glDepthFunc(st->ZTest);
        if (st->Blend == 0) glDisable(GL_BLEND);
        else if (st->Blend == 1) { glEnable(GL_BLEND); glBlendFunc(st->BlendSrc, st->BlendDst); }
        m_Dirty = true;
    }
    // Only culling matters to a depth-only (shadow / prepass) draw.
    void ApplyCullOnly(const ShaderRenderState* st) {
        if (!st || st->Cull == ShaderRenderState::CullMode::Unset) { Restore(); return; }
        ShaderRenderState cull;
        cull.Cull = st->Cull;
        Apply(&cull);
    }
    void Restore() {
        if (!m_Dirty) return;
        const Model::DrawPassState& s = *m_S;
        s.CullEnabled ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
        glCullFace((GLenum)s.CullFace);
        glDepthMask(s.DepthMask ? GL_TRUE : GL_FALSE);
        glDepthFunc((GLenum)s.DepthFunc);
        s.BlendEnabled ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
        glBlendFuncSeparate((GLenum)s.BlendSrcRgb, (GLenum)s.BlendDstRgb, (GLenum)s.BlendSrcA, (GLenum)s.BlendDstA);
        m_Dirty = false;
    }
    ~ShaderStateScope() { Restore(); }
private:
    void Save() {
        Model::DrawPassState& s = *m_S;
        s.CullEnabled = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
        glGetIntegerv(GL_CULL_FACE_MODE, &s.CullFace);
        GLint depthMask = GL_TRUE;
        glGetIntegerv(GL_DEPTH_WRITEMASK, &depthMask);
        s.DepthMask = depthMask != 0;
        glGetIntegerv(GL_DEPTH_FUNC, &s.DepthFunc);
        s.BlendEnabled = glIsEnabled(GL_BLEND) == GL_TRUE;
        glGetIntegerv(GL_BLEND_SRC_RGB, &s.BlendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &s.BlendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.BlendSrcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &s.BlendDstA);
        s.Saved = true;
    }
    Model::DrawPassState m_Own;
    Model::DrawPassState* m_S;
    bool m_Dirty = false;
};

const ShaderRenderState* SlotRenderState(const std::vector<std::shared_ptr<MaterialAsset>>& slots, int i) {
    if (i >= (int)slots.size() || !slots[i] || !slots[i]->Shader) return nullptr;
    return &slots[i]->Shader->RenderState();
}

// The shader's state plus the material's Double Sided (#113), or the caller's (`force`): culling off
// unless the shader itself says otherwise. Returns a pointer to a per-call scratch copy when it needs one.
const ShaderRenderState* EffectiveRenderState(const std::vector<std::shared_ptr<MaterialAsset>>& slots, int i,
                                              const Material& mat, bool force = false) {
    const ShaderRenderState* st = SlotRenderState(slots, i);
    if (!(mat.DoubleSided || force) || (st && st->Cull != ShaderRenderState::CullMode::Unset)) return st;
    static thread_local ShaderRenderState scratch;
    scratch = st ? *st : ShaderRenderState{};
    scratch.Cull = ShaderRenderState::CullMode::Off;
    return &scratch;
}
} // namespace

void Model::DrawSelected(Shader& fallback, const glm::mat4& xform,
                         const std::vector<std::shared_ptr<MaterialAsset>>& slots,
                         const ProgramSelector& selectProgram, float opacity,
                         const std::function<void(Shader&)>& onProgramBound, MeshPass pass, bool forceDoubleSided,
                         const VisibleIndexBuffer* visible, int instances) {
    PosedDrawTimer posedTimer(m_D->BoneCounter > 0);
    const glm::mat4 nrm = glm::mat4(NormalMatrix(xform));

    Shader*             lastProg = nullptr;
    const MaterialLocs* locs = nullptr;
    bool                preSkinned = false; // the same answer for every program: it's the model's pose
    ShaderStateScope stateScope; // #104

    int firstVertex = 0; // this sub-mesh's first vertex in the model (an outfit piece's hide bits, SkinHideBuffer)
    for (int i = 0; i < (int)m_D->Meshes.size(); ++i) {
        const int meshFirstVertex = firstVertex;
        firstVertex += (int)m_D->Meshes[i]->VertexCount();
        const bool hasSlot = i < (int)slots.size() && slots[i];
        const bool transparent = hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::Transparent;
        if ((pass == MeshPass::Opaque && transparent) || (pass == MeshPass::Transparent && !transparent)) continue;
        const bool useVisible = visible && i < visible->MeshCount();
        if (useVisible && !visible->MeshRange(i).Count) continue; // covered entirely
        // A blended surface at Opacity 0 adds nothing (the Quantum heads' eye overlay and saliva) - unless
        // its shader's own state blends or writes depth some other way.
        if (transparent && slots[i]->Opacity <= 0.0f) {
            const ShaderRenderState* st = SlotRenderState(slots, i);
            if (!st || (st->ZWrite <= 0 && st->Blend < 0)) continue;
        }
        stateScope.Apply(EffectiveRenderState(slots, i, hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat, forceDoubleSided));
        Shader* prog = &fallback;
        if (Shader* p = selectProgram(hasSlot ? slots[i].get() : nullptr)) prog = p;

        if (prog != lastProg) {
            // Per-program state the single-program Model::Draw sets once up front. Bind first —
            // the selector only binds a program the first time it applies frame state to it, and
            // glUniform* always targets the currently bound program.
            prog->Bind();
            preSkinned = UploadBoneMatrices(*prog); // uUseSkinning + bone SSBO (or this frame's skinned vertices)
            locs = &CachedMaterialLocs(*prog);
            prog->SetMat4(locs->model, xform);
            prog->SetMat4(locs->normalMatrix, nrm);
            if (onProgramBound) onProgramBound(*prog); // per-draw uniforms, e.g. probes (#108)
            lastProg = prog;
        }
        // Per-draw: the transparent pass varies opacity per entity. No-op on opaque programs
        // (uOpacity is only read when uAlphaBlend == 1; an absent uniform is loc -1).
        // In the transparent pass each submesh uses its own slot's opacity (#112).
        prog->SetFloat(locs->opacity, pass == MeshPass::Transparent ? slots[i]->Opacity : opacity);
        prog->SetInt(locs->hideVertBase, meshFirstVertex);
        if (preSkinned) prog->SetInt(locs->skinBase, m_PreSkin.Base + meshFirstVertex);

        const Material& mat = hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat;
        if (mat.Foliage) SetWindFrame(*prog); // the main pass's clock and gust, equal to the depth passes'
        if (hasSlot && slots[i]->Shader)
            BindMaterialDataDriven(*prog, *slots[i], *slots[i]->Shader);
        else
            BindMaterial(*prog, mat, *locs);
        // After the bind (which a repeat of the same material skips): back faces lit with a flipped normal.
        prog->SetInt(locs->doubleSided, forceDoubleSided || mat.DoubleSided ? 1 : 0);
        prog->SetInt(locs->clothInterior, forceDoubleSided && !mat.DoubleSided ? 1 : 0); // not hair cards: both sides are hair
        if (useVisible)
            m_D->Meshes[i]->DrawIndices(visible->Id(), visible->MeshRange(i).Offset, visible->MeshRange(i).Count, instances);
        else
            m_D->Meshes[i]->Draw(instances);
    }
}

void Model::DrawDepthOnly(Shader& shader, const std::vector<std::shared_ptr<MaterialAsset>>& slots, int instances,
                          const VisibleIndexBuffer* visible) {
    PosedDrawTimer posedTimer(m_D->BoneCounter > 0);
    SetWindFrame(shader);
    const bool preSkinned = UploadBoneMatrices(shader);
    const MaterialLocs& locs = CachedMaterialLocs(shader);
    ShaderStateScope stateScope; // #104 — a Cull Off (double-sided) shader casts from both sides
    int skinBase = m_PreSkin.Base; // this sub-mesh's first vertex in the skinned buffer
    for (int i = 0; i < (int)m_D->Meshes.size(); ++i) {
        const int meshSkinBase = skinBase;
        skinBase += (int)m_D->Meshes[i]->VertexCount();
        bool hasSlot = i < (int)slots.size() && slots[i];
        // Transparent materials don't cast shadows — skip them in the depth-only pass.
        if (hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::Transparent) continue;
        const bool useVisible = visible && i < visible->MeshCount();
        if (useVisible && !visible->MeshRange(i).Count) continue; // covered entirely
        stateScope.ApplyCullOnly(EffectiveRenderState(slots, i, hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat));
        const Material& mat = hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat;
        // Only cost paid over a pure depth draw: one texture bind + two uniforms, and only for
        // CUTOUT materials with an albedo map (foliage/fences) — the shadow then follows the
        // cutout instead of a solid silhouette (#116). #101: it used to do this for every
        // albedo-mapped mesh, so an opaque material whose albedo alpha means something else
        // (smoothness, a mask) cast holey shadows. #192: skip even that when the previous mesh
        // in this pass drew with the same material.
        const bool clip = mat.AlphaClip || (hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::AlphaTest);
        if (!GLStateCache::MaterialAlreadyBound(mat.Hash() ^ (clip ? 0x5bd1e995ull : 0ull), shader.Program())) {
            SetFoliageUniforms(shader, mat, locs); // the same sway as the main pass, so shadows move with the leaves
            shader.SetVec2(locs.uvTiling, mat.UVTiling); // #102 — the cutout follows the material's tiling
            shader.SetVec2(locs.uvOffset, mat.UVOffset);
            if (clip && mat.AlbedoMap && mat.AlbedoMap->IsValid()) {
                mat.AlbedoMap->Bind(0);
                shader.SetInt(locs.alphaTest, 1);
                shader.SetFloat(locs.alphaCutoff, mat.AlphaCutoff);
            } else {
                // uAlphaTest = 0 means the sampler result is never read, but the ShadowDepth
                // program still declares `sampler2D uAlbedo`, so unit 0 must hold a real texture
                // or the driver reports KHR 131204 ("texture 0 ... cannot be used") every draw —
                // the residual load-time warnings after PR #370 (audit GL-101 / #366 on unit 0).
                GLStateCache::BindTexture2D(0, DefaultTextures::White());
                shader.SetInt(locs.alphaTest, 0);
            }
            shader.SetInt(locs.depthAlbedo, 0);
        }
        if (preSkinned) shader.SetInt(locs.skinBase, meshSkinBase);
        if (useVisible)
            m_D->Meshes[i]->DrawIndices(visible->Id(), visible->MeshRange(i).Offset, visible->MeshRange(i).Count, instances);
        else
            m_D->Meshes[i]->Draw(instances);
    }
}

bool Model::InArena() const {
    if (HasBones() || m_D->Meshes.empty()) return false;
    for (const auto& m : m_D->Meshes)
        if (!m->ArenaRange().Page) return false;
    return true;
}

bool Model::DepthMeshState(int i, const std::vector<std::shared_ptr<MaterialAsset>>& slots, int& cull, std::uint64_t& stateKey) const {
    const bool hasSlot = i < (int)slots.size() && slots[i];
    if (hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::Transparent) return false;
    const Material& mat = hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat;
    const ShaderRenderState* st = EffectiveRenderState(slots, i, mat);
    cull = st ? (int)st->Cull : 0;
    const bool clip = mat.AlphaClip || (hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::AlphaTest);
    // What BindDepthMesh sets: a plain opaque mesh only turns the sway and the cutout off, so all of them draw alike.
    if ((clip && mat.AlbedoMap && mat.AlbedoMap->IsValid()) || mat.Foliage)
        stateKey = (mat.Hash() ^ (clip ? 0x5bd1e995ull : 0ull)) | 1ull;
    else
        stateKey = mat.UseVertexColor ? 2ull : 0ull;
    return true;
}

void Model::BindDepthMesh(Shader& shader, int i, const std::vector<std::shared_ptr<MaterialAsset>>& slots) {
    SetWindFrame(shader);
    UploadBoneMatrices(shader); // no bones: uUseSkinning 0
    const MaterialLocs& locs = CachedMaterialLocs(shader);
    const bool hasSlot = i < (int)slots.size() && slots[i];
    const Material& mat = hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat;
    const bool clip = mat.AlphaClip || (hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::AlphaTest);
    if (GLStateCache::MaterialAlreadyBound(mat.Hash() ^ (clip ? 0x5bd1e995ull : 0ull), shader.Program())) return;
    SetFoliageUniforms(shader, mat, locs); // as DrawDepthOnly
    shader.SetVec2(locs.uvTiling, mat.UVTiling);
    shader.SetVec2(locs.uvOffset, mat.UVOffset);
    if (clip && mat.AlbedoMap && mat.AlbedoMap->IsValid()) {
        mat.AlbedoMap->Bind(0);
        shader.SetInt(locs.alphaTest, 1);
        shader.SetFloat(locs.alphaCutoff, mat.AlphaCutoff);
    } else {
        GLStateCache::BindTexture2D(0, DefaultTextures::White());
        shader.SetInt(locs.alphaTest, 0);
    }
    shader.SetInt(locs.depthAlbedo, 0);
}

std::uint64_t Model::MeshStateKey(int i, const std::vector<std::shared_ptr<MaterialAsset>>& slots, bool forceDoubleSided) const {
    const bool hasSlot = i < (int)slots.size() && slots[i];
    const Material& mat = hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat;
    const ShaderRenderState* st = EffectiveRenderState(slots, i, mat, forceDoubleSided);
    if (!st || !st->AffectsDraw()) return 0;
    std::uint64_t h = 0x9E3779B97F4A7C15ull;
    const auto mix = [&h](std::uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
    mix((std::uint64_t)st->Cull);
    mix((std::uint64_t)(std::int64_t)st->ZWrite);
    mix(st->ZTest);
    mix((std::uint64_t)(std::int64_t)st->Blend);
    mix(st->BlendSrc);
    mix(st->BlendDst);
    return h | 1ull;
}

bool Model::DrawMeshIndirect(Shader& fallback, int i, const std::vector<std::shared_ptr<MaterialAsset>>& slots,
                             const ProgramSelector& selectProgram, const std::function<void(Shader&)>& onProgramBound,
                             bool forceDoubleSided, const void* indirect, int drawCount, DrawPassState* passState) {
    PosedDrawTimer posedTimer(m_D->BoneCounter > 0);
    const bool hasSlot = i < (int)slots.size() && slots[i];
    if (hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::Transparent) return false;
    const ModelMesh& mesh = *m_D->Meshes[i];
    if (!mesh.ArenaRange().Page) return false;
    ShaderStateScope stateScope(passState); // #104
    const Material& mat = hasSlot ? slots[i]->Mat : mesh.Mat;
    stateScope.Apply(EffectiveRenderState(slots, i, mat, forceDoubleSided));
    Shader* prog = &fallback;
    if (Shader* p = selectProgram(hasSlot ? slots[i].get() : nullptr)) prog = p;
    prog->Bind();
    UploadBoneMatrices(*prog);
    const MaterialLocs& locs = CachedMaterialLocs(*prog);
    if (onProgramBound) onProgramBound(*prog);
    prog->SetFloat(locs.opacity, 1.0f);
    prog->SetInt(locs.hideVertBase, 0);
    if (mat.Foliage) SetWindFrame(*prog);
    if (hasSlot && slots[i]->Shader)
        BindMaterialDataDriven(*prog, *slots[i], *slots[i]->Shader);
    else
        BindMaterial(*prog, mat, locs);
    prog->SetInt(locs.doubleSided, forceDoubleSided || mat.DoubleSided ? 1 : 0);
    prog->SetInt(locs.clothInterior, forceDoubleSided && !mat.DoubleSided ? 1 : 0);
    GLStateCache::BindVertexArray(MeshArena::PageVao(mesh.ArenaRange().Page));
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, indirect, drawCount, 0);
    return true;
}

long long Model::DrawFurShells(Shader& prog, const glm::mat4& xform, const std::vector<std::shared_ptr<MaterialAsset>>& slots,
                               FurBindFn bind, void* user) {
    long long tris = 0;
    bool bound = false, preSkinned = false;
    const MaterialLocs* locs = nullptr;
    int firstVertex = 0;
    for (int i = 0; i < (int)m_D->Meshes.size(); ++i) {
        const int meshFirstVertex = firstVertex;
        firstVertex += (int)m_D->Meshes[i]->VertexCount();
        const bool hasSlot = i < (int)slots.size() && slots[i];
        if (hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::Transparent) continue;
        const Material& mat = hasSlot ? slots[i]->Mat : m_D->Meshes[i]->Mat;
        if (!mat.Fur.Enabled) continue;
        if (!bound) { // only a model that has fur pays for the skin binding
            prog.Bind();
            preSkinned = UploadBoneMatrices(prog);
            locs = &CachedMaterialLocs(prog);
            prog.SetMat4(locs->model, xform);
            prog.SetMat4(locs->normalMatrix, glm::mat4(NormalMatrix(xform)));
            bound = true;
        }
        const int shells = bind(prog, mat, user);
        if (shells <= 0) continue;
        if (preSkinned) prog.SetInt(locs->skinBase, m_PreSkin.Base + meshFirstVertex);
        m_D->Meshes[i]->Draw(shells);
        tris += (long long)(m_D->Meshes[i]->IndexCount() / 3u) * shells;
    }
    return tris;
}
