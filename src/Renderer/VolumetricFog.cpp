#include "VolumetricFog.h"
#include "SceneRenderer.h"
#include "RenderFrameContext.h"
#include "World.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "CascadedShadowMap.h"
#include "SpotShadowMap.h"
#include "PointShadowMap.h"
#include "IblProbe.h"
#include "ClusterGrid.h"
#include "LightBuffer.h"
#include "DefaultTextures.h"
#include "gl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
constexpr int kNoiseSize = 32;
unsigned long long g_RenderCount = 0;
GLuint Groups(int n, int local) { return (GLuint)((n + local - 1) / local); }

unsigned int MakeVolume(int w, int h, int d) {
    unsigned int tex = 0;
    glCreateTextures(GL_TEXTURE_3D, 1, &tex);
    glTextureStorage3D(tex, 1, GL_RGBA16F, w, h, d);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    return tex;
}

// What the shading needs to find a world position's texel: (near, 1 / ln(far / near), slices,
// pixels per froxel column).
glm::vec4 ApplyParams(int slices, float far, float pixelsPerFroxel) {
    return glm::vec4(VolumetricFog::kNear, 1.0f / std::log(far / VolumetricFog::kNear), (float)slices, pixelsPerFroxel);
}
} // namespace

VolumetricFog::QualityLevel VolumetricFog::Quality(int level) {
    switch (std::clamp(level, 0, 2)) {
    case 0:  return {128, 72, 48, 0.12f};  // Low
    case 2:  return {240, 135, 96, 0.08f}; // High
    default: return {160, 90, 64, 0.1f};   // Medium
    }
}

void VolumetricFog::ViewState::Release() {
    unsigned int texs[3] = {Scatter[0], Scatter[1], Integrated};
    for (unsigned int t : texs)
        if (t) glDeleteTextures(1, &t);
    Scatter[0] = Scatter[1] = Integrated = 0;
    W = H = D = 0;
    HasHistory = false;
    Rendered = false;
}

unsigned long long VolumetricFog::RenderCount() { return g_RenderCount; }

VolumetricFog::VolumetricFog() = default;

VolumetricFog::~VolumetricFog() {
    m_Views.clear();
    if (m_BlueNoise) glDeleteTextures(1, &m_BlueNoise);
    if (m_EmptyVao) glDeleteVertexArrays(1, &m_EmptyVao);
}

// Void-and-cluster (Ulichney 1993) on a 32 x 32 torus: every rank once, each new point dropped
// into the largest remaining void, so any threshold of the ranks is an even, clump-free scatter -
// jitter with it and the leftover noise is high-frequency, which the temporal blend and the
// bilinear apply both average away. Deterministic (fixed seed), ~a millisecond at load.
void VolumetricFog::BuildBlueNoise(unsigned short out[kNoiseSize * kNoiseSize]) {
    constexpr int N = kNoiseSize * kNoiseSize;
    constexpr float kSigma = 1.5f;
    // Gaussian splat weights by toroidal offset.
    std::vector<float> kernel(N);
    for (int y = 0; y < kNoiseSize; ++y)
        for (int x = 0; x < kNoiseSize; ++x) {
            const int dx = std::min(x, kNoiseSize - x), dy = std::min(y, kNoiseSize - y);
            kernel[y * kNoiseSize + x] = std::exp(-(float)(dx * dx + dy * dy) / (2.0f * kSigma * kSigma));
        }
    std::vector<float> energy(N, 0.0f);
    std::vector<char> on(N, 0);
    auto splat = [&](int p, float sign) {
        const int px = p % kNoiseSize, py = p / kNoiseSize;
        for (int y = 0; y < kNoiseSize; ++y)
            for (int x = 0; x < kNoiseSize; ++x) {
                const int ox = (x - px + kNoiseSize) % kNoiseSize, oy = (y - py + kNoiseSize) % kNoiseSize;
                energy[y * kNoiseSize + x] += sign * kernel[oy * kNoiseSize + ox];
            }
    };
    auto tightestCluster = [&] {
        int best = -1;
        for (int i = 0; i < N; ++i) if (on[i] && (best < 0 || energy[i] > energy[best])) best = i;
        return best;
    };
    auto largestVoid = [&] {
        int best = -1;
        for (int i = 0; i < N; ++i) if (!on[i] && (best < 0 || energy[i] < energy[best])) best = i;
        return best;
    };

    // Initial pattern: a tenth of the cells, at random, then swapped until no move improves it.
    std::uint32_t rng = 0x9E3779B9u;
    auto next = [&] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    int ones = 0;
    while (ones < N / 10) {
        const int p = (int)(next() % N);
        if (on[p]) continue;
        on[p] = 1;
        splat(p, 1.0f);
        ++ones;
    }
    for (int guard = 0; guard < N; ++guard) {
        const int c = tightestCluster();
        on[c] = 0;
        splat(c, -1.0f);
        const int v = largestVoid();
        on[v] = 1;
        splat(v, 1.0f);
        if (v == c) break;
    }
    const std::vector<char> initial = on;
    const std::vector<float> initialEnergy = energy;

    // Ranks below the initial count: peel the tightest clusters off the initial pattern.
    for (int r = ones - 1; r >= 0; --r) {
        const int c = tightestCluster();
        on[c] = 0;
        splat(c, -1.0f);
        out[c] = (unsigned short)r;
    }
    // Ranks from there up: fill the largest voids.
    on = initial;
    energy = initialEnergy;
    for (int r = ones; r < N; ++r) {
        const int v = largestVoid();
        on[v] = 1;
        splat(v, 1.0f);
        out[v] = (unsigned short)r;
    }
}

void VolumetricFog::EnsureResources() {
    if (m_InjectShader) return;
    m_InjectShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("VolumetricInject.comp.glsl"), "VolumetricInject");
    m_IntegrateShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("VolumetricIntegrate.comp.glsl"), "VolumetricIntegrate");
    m_SkyShader = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("VolumetricSky.vert.glsl"),
                                           ShaderLibrary::ReadFileRequired("VolumetricSky.frag.glsl"), "VolumetricSky");
    unsigned short ranks[kNoiseSize * kNoiseSize];
    BuildBlueNoise(ranks);
    std::vector<float> texels(kNoiseSize * kNoiseSize);
    for (int i = 0; i < kNoiseSize * kNoiseSize; ++i) texels[i] = (float)ranks[i] / (float)(kNoiseSize * kNoiseSize - 1);
    glCreateTextures(GL_TEXTURE_2D, 1, &m_BlueNoise);
    glTextureStorage2D(m_BlueNoise, 1, GL_R16F, kNoiseSize, kNoiseSize);
    glTextureSubImage2D(m_BlueNoise, 0, 0, 0, kNoiseSize, kNoiseSize, GL_RED, GL_FLOAT, texels.data());
    glTextureParameteri(m_BlueNoise, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_BlueNoise, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glCreateVertexArrays(1, &m_EmptyVao);
}

VolumetricFog::ViewState& VolumetricFog::View(const void* key) {
    auto& slot = m_Views[key];
    if (!slot) slot = std::make_unique<ViewState>();
    slot->LastUsed = m_Tick;
    return *slot;
}

bool VolumetricFog::WantedFor(const World& world, const RenderFrameContext& ctx, const int viewport[4]) {
    return world.VolumetricEnabled && world.VolumetricDensity > 0.0f && !ctx.Unlit && ctx.DebugView == 0 &&
           ctx.TxHdr != nullptr && std::abs(ctx.Proj[3][3]) < 0.5f && viewport[2] > 0 && viewport[3] > 0;
}

void VolumetricFog::Render(const FrameState& fs) {
    const World& world = *fs.world;
    const RenderFrameContext& ctx = *fs.ctx;
    const SceneRenderInputs& in = *fs.in;
    const int spotShadowCount = fs.spotCountForView, pointShadowCount = fs.pointCountForView;
    EnsureResources();
    ++m_Tick;
    ++g_RenderCount;
    // Views nobody has drawn for a while (a closed Game tab) give their volumes back.
    for (auto it = m_Views.begin(); it != m_Views.end();) {
        if (m_Tick - it->second->LastUsed > 600) it = m_Views.erase(it);
        else ++it;
    }

    ViewState& vs = View(ctx.TxHdr);
    const QualityLevel q = Quality(world.VolumetricQuality);
    const float far = std::clamp(world.VolumetricDistance, 4.0f, 1000.0f);
    if (vs.W != q.W || vs.H != q.H || vs.D != q.D) {
        vs.Release();
        vs.W = q.W; vs.H = q.H; vs.D = q.D;
        vs.Scatter[0] = MakeVolume(q.W, q.H, q.D);
        vs.Scatter[1] = MakeVolume(q.W, q.H, q.D);
        vs.Integrated = MakeVolume(q.W, q.H, q.D);
    }
    const glm::vec3 camera = ctx.ViewPos;
    const glm::mat4 viewProj = ctx.Proj * ctx.View;
    // A cut (teleport, a camera switch) or a new depth range: last frame's volume says nothing here.
    if (far != vs.Far || glm::length(camera - vs.PrevCamera) > 4.0f) vs.HasHistory = false;
    vs.Far = far;
    vs.PixelsPerFroxel = (float)std::max(fs.vp[2], 1) / (float)q.W;
    ++vs.Frame;

    const glm::vec4 projParams(ctx.Proj[0][0], ctx.Proj[1][1], ctx.Proj[2][0], ctx.Proj[2][1]);
    const glm::vec4 sliceParams(kNear, std::log(far / kNear), 0.0f, 0.0f);
    const int prev = vs.Current, cur = 1 - vs.Current;

    // --- pass 1: inject + light + temporal ---
    in.lightBuffer->Bind(0);
    if (fs.clusterOn) in.clusterGrid->BindForShading();
    Shader& inject = *m_InjectShader;
    inject.Bind();
    inject.SetMat4("uInvView", glm::inverse(ctx.View));
    inject.SetMat4("uView", ctx.View);
    inject.SetVec4("uProjParams", projParams);
    inject.SetVec4("uSliceParams", sliceParams);
    inject.SetVec3("uCameraPos", camera);
    inject.SetMat4("uPrevViewProj", vs.PrevViewProj);
    inject.SetInt("uHistoryValid", vs.HasHistory ? 1 : 0);
    inject.SetFloat("uBlend", q.Blend);
    // Golden-ratio steps: each frame's offset lands in the largest gap the earlier ones left.
    inject.SetFloat("uFrameJitter", (float)std::fmod((double)vs.Frame * 0.6180339887498949, 1.0));
    inject.SetFloat("uDensity", std::max(world.VolumetricDensity, 0.0f));
    inject.SetFloat("uHeightFalloff", std::max(world.VolumetricHeightFalloff, 0.0f));
    inject.SetFloat("uBaseHeight", world.VolumetricBaseHeight);
    inject.SetVec3("uAlbedo", glm::clamp(world.VolumetricAlbedo, glm::vec3(0.0f), glm::vec3(1.0f)));
    inject.SetFloat("uAnisotropy", std::clamp(world.VolumetricAnisotropy, -0.95f, 0.95f));
    inject.SetFloat("uAmbientScale", 1.0f);
    inject.SetInt("uIBLEnabled", fs.iblOn ? 1 : 0);
    inject.SetFloat("uIBLIntensity", std::max(world.SkyAmbientIntensity, 0.0f));
    if (fs.iblOn) glBindTextureUnit(11, in.iblProbe->IrradianceMap());
    inject.SetInt("uClusterEnabled", fs.clusterOn ? 1 : 0);
    if (fs.clusterOn) inject.SetVec4("uClusterZParams", ClusterGrid::ZParams(fs.clusterNearZ, fs.clusterFarZ));

    // The sun's cascades (unit 8) and the local lights' maps (9, 10), as ApplyFrameState binds them.
    CascadedShadowMap& csm = *in.sunShadow;
    inject.SetInt("uShadowEnabled", fs.sunShadowsOn ? 1 : 0);
    inject.SetInt("uShadowCascadeCount", csm.Count());
    {
        glm::mat4 mats[CascadedShadowMap::kMaxCascades];
        for (int i = 0; i < csm.Count(); ++i) mats[i] = csm.LightViewProj(i);
        inject.SetMat4Array("uShadowMatrices[0]", csm.Count(), mats);
    }
    inject.SetVec4("uCascadeSplits", csm.SplitDepthsVec4());
    inject.SetVec4("uShadowTexelWorld", csm.TexelWorldSizesVec4());
    inject.SetFloat("uShadowMapResolution", (float)csm.Resolution());
    glBindTextureUnit(8, csm.DepthArray() ? csm.DepthArray() : DefaultTextures::DepthArray());
    inject.SetInt("uSpotShadowCount", spotShadowCount);
    if (spotShadowCount > 0) {
        inject.SetMat4Array("uSpotShadowVP[0]", spotShadowCount, in.spotShadowVP);
        inject.SetVec3Array("uSpotShadowPos[0]", spotShadowCount, in.spotShadowPos);
        inject.SetFloatArray("uSpotShadowFar[0]", spotShadowCount, in.spotShadowFar);
        inject.SetFloatArray("uSpotShadowHalfTan[0]", spotShadowCount, in.spotShadowHalfTan);
    }
    inject.SetFloat("uSpotShadowMapResolution", (float)in.spotShadow->Resolution());
    glBindTextureUnit(9, in.spotShadow->DepthArray() ? in.spotShadow->DepthArray() : DefaultTextures::DepthArray());
    inject.SetInt("uPointShadowCount", pointShadowCount);
    if (pointShadowCount > 0) inject.SetFloatArray("uPointShadowFar[0]", pointShadowCount, in.pointShadowFar);
    inject.SetFloat("uPointShadowMapResolution", (float)in.pointShadow->Resolution());
    glBindTextureUnit(10, in.pointShadow->DepthCubeArray() ? in.pointShadow->DepthCubeArray() : DefaultTextures::DepthCubeArray());

    glBindTextureUnit(6, vs.Scatter[prev]);
    glBindTextureUnit(7, m_BlueNoise);
    glBindImageTexture(0, vs.Scatter[cur], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    inject.DispatchCompute(Groups(q.W, 8), Groups(q.H, 8), (GLuint)q.D);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // --- pass 2: front-to-back integration ---
    Shader& integrate = *m_IntegrateShader;
    integrate.Bind();
    integrate.SetVec4("uProjParams", projParams);
    integrate.SetVec4("uSliceParams", sliceParams);
    glBindTextureUnit(6, vs.Scatter[cur]);
    glBindImageTexture(0, vs.Integrated, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    integrate.DispatchCompute(Groups(q.W, 8), Groups(q.H, 8), 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    glBindImageTexture(0, 0, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindTextureUnit(6, 0);
    glBindTextureUnit(7, 0);
    vs.Current = cur;
    vs.HasHistory = true;
    vs.Rendered = true;
    vs.ViewProj = viewProj;
    vs.PrevViewProj = viewProj;
    vs.PrevCamera = camera;
}

void VolumetricFog::ApplyToProgram(Shader& program, const void* viewKey, bool enabled) const {
    const auto it = m_Views.find(viewKey);
    const ViewState* vs = enabled && it != m_Views.end() && it->second->Rendered ? it->second.get() : nullptr;
    if (!vs) { ApplyDisabled(program); return; }
    glBindTextureUnit(kVolumeUnit, vs->Integrated);
    program.SetInt("uVolOn", 1);
    program.SetMat4("uVolViewProj", vs->ViewProj);
    program.SetVec4("uVolParams", ApplyParams(vs->D, vs->Far, vs->PixelsPerFroxel));
}

void VolumetricFog::ApplyDisabled(Shader& program) {
    glBindTextureUnit(kVolumeUnit, DefaultTextures::Volume());
    program.SetInt("uVolOn", 0);
}

void VolumetricFog::DrawSky(const void* viewKey, const int viewport[4]) {
    const auto it = m_Views.find(viewKey);
    if (it == m_Views.end() || !it->second->Rendered || !m_SkyShader) return;
    const ViewState& vs = *it->second;
    GLint depthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    // dst * a + rgb on colour; destination alpha kept.
    glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
    m_SkyShader->Bind();
    m_SkyShader->SetVec4("uViewport", glm::vec4((float)viewport[0], (float)viewport[1],
                                                (float)std::max(viewport[2], 1), (float)std::max(viewport[3], 1)));
    m_SkyShader->SetFloat("uLastSlice", 1.0f - 0.5f / (float)vs.D);
    glBindTextureUnit(6, vs.Integrated);
    glBindVertexArray(m_EmptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindTextureUnit(6, 0);
    if (!blend) glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glDepthFunc((GLenum)depthFunc);
}
