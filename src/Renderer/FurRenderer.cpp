#include "FurRenderer.h"

#include "DefaultTextures.h"
#include "GLStateCache.h"
#include "Material.h"
#include "MaterialAsset.h"
#include "Model.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "Texture.h"
#include "gl.h"
#include "Core/Profiler.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {
// Material map units, as BindMaterial uses them (1..7); 8 and up are ApplyFrameState's shadow / IBL maps.
constexpr int kAlbedoUnit = 1, kAOUnit = 2, kMaskUnit = 3;
// Not in the engine's GL loader header: GL_SAMPLE_ALPHA_TO_COVERAGE / GL_SAMPLE_ALPHA_TO_ONE.
constexpr unsigned kAlphaToCoverage = 0x809E, kAlphaToOne = 0x809F;

struct BindContext {
    float Distance = 1.0f;
    float ProjScaleY = 1.0f;
    float ViewportHeight = 1.0f;
};

void BindMap(Shader& prog, const std::shared_ptr<Texture>& tex, int unit, const char* sampler, const char* has) {
    const bool ok = tex && tex->IsValid();
    if (ok) tex->Bind((unsigned)unit);
    else GLStateCache::BindTexture2D(unit, DefaultTextures::White());
    prog.SetInt(sampler, unit);
    prog.SetInt(has, ok ? 1 : 0);
}

// Model::FurBindFn: this material's shells for the item's distance, and its uniforms when there are any.
int BindFurMaterial(Shader& prog, const Material& mat, void* user) {
    const BindContext& c = *static_cast<const BindContext*>(user);
    const Material::FurDesc& fur = mat.Fur;
    const int shells = FurRenderer::ShellCount(fur.Shells, fur.Length, c.Distance, c.ProjScaleY, c.ViewportHeight);
    if (shells <= 0) return 0;
    prog.SetInt("uShellCount", shells);
    prog.SetFloat("uFurLength", fur.Length);
    prog.SetFloat("uFurGravity", fur.Gravity);
    prog.SetFloat("uFurDensity", fur.Density);
    prog.SetVec3("uFurTint", fur.Tint);
    prog.SetFloat("uFurRim", fur.Rim);
    prog.SetVec3("uBaseColor", mat.BaseColor);
    prog.SetVec2("uUVTiling", mat.UVTiling);
    prog.SetVec2("uUVOffset", mat.UVOffset);
    BindMap(prog, mat.AlbedoMap, kAlbedoUnit, "uAlbedoMap", "uHasAlbedoMap");
    BindMap(prog, mat.AOMap, kAOUnit, "uAOMap", "uHasAOMap");
    BindMap(prog, fur.Mask, kMaskUnit, "uFurMask", "uHasFurMask");
    return shells;
}
} // namespace

FurRenderer& FurRenderer::Get() {
    static FurRenderer s_Instance;
    return s_Instance;
}

int FurRenderer::ShellCount(int maxShells, float furLength, float distance, float projScaleY, float viewportHeight) {
    if (maxShells <= 0 || !(furLength > 0.0f) || !(viewportHeight > 0.0f)) return 0;
    // The hair's height in pixels, standing square to the view at `distance`.
    const float pixels = furLength * std::abs(projScaleY) * 0.5f * viewportHeight / std::max(distance, 0.05f);
    if (!(pixels >= kMinPixels)) return 0;
    const int shells = (int)std::ceil(pixels * kShellsPerPixel);
    return std::clamp(shells, std::min(kMinShells, maxShells), maxShells);
}

bool FurRenderer::HasFur(const Model& model, const std::vector<std::shared_ptr<MaterialAsset>>& slots) {
    for (int i = 0; i < model.MeshCount(); ++i) {
        const bool hasSlot = i < (int)slots.size() && slots[i];
        if (hasSlot && slots[i]->RenderQueue == MaterialAsset::Queue::Transparent) continue;
        if ((hasSlot ? slots[i]->Mat : model.MeshMaterial(i)).Fur.Enabled) return true;
    }
    return false;
}

bool FurRenderer::Disabled() {
    static const bool off = [] {
        char* e = nullptr;
        size_t n = 0;
        const bool set = _dupenv_s(&e, &n, "TARTARUS_NO_FUR") == 0 && e && *e && *e != '0';
        std::free(e);
        return set;
    }();
    return off;
}

void FurRenderer::EnsureProgram() {
    if (m_Program) return;
    m_Program = std::make_unique<Shader>(ShaderLibrary::ReadFile("FurShell.vert.glsl"),
                                         ShaderLibrary::ReadFile("FurShell.frag.glsl"), "FurShell");
}

int FurRenderer::Draw(const std::vector<Item>& items, const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye,
                      float viewportHeight, bool multisampled, const std::function<void(Shader&)>& applyFrameState,
                      long long& triangles) {
    if (items.empty()) return 0;
    PROFILE_GPU_SCOPE("Fur Shells");
    EnsureProgram();
    Shader& prog = *m_Program;
    applyFrameState(prog);
    prog.Bind();
    prog.SetMat4("uView", view);
    prog.SetMat4("uProj", proj);
    prog.SetFloat("uNearHide", 0.0f);
    prog.SetInt("uAlphaToCoverage", multisampled ? 1 : 0);

    // Opaque state: depth-tested and written, back faces culled like the surface under the shells.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    if (multisampled) {
        glEnable(kAlphaToCoverage);
        glEnable(kAlphaToOne); // the colour buffer keeps alpha 1, as the opaque pass leaves it
    }

    BindContext ctx;
    ctx.ProjScaleY = proj[1][1];
    ctx.ViewportHeight = viewportHeight;
    int draws = 0;
    for (const Item& it : items) {
        ctx.Distance = std::max(glm::length(it.Centre - eye) - it.Radius, 0.05f);
        prog.SetInt("uNoReceiveShadows", it.ReceiveShadows ? 0 : 1);
        prog.SetInt("uObjectLayerBit", it.LayerBit);
        const long long tris = it.Ref->DrawFurShells(prog, it.Xform, *it.Slots, &BindFurMaterial, &ctx);
        if (tris > 0) {
            triangles += tris;
            ++draws;
        }
    }

    if (multisampled) {
        glDisable(kAlphaToCoverage);
        glDisable(kAlphaToOne);
    }
    GLStateCache::Invalidate();
    return draws;
}
