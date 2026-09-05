#pragma once

#include <memory>
#include <glm/glm.hpp>

class Shader;

// Image-based lighting probes baked from the procedural sky (#196).
//
// Replaces the model shader's hardcoded `vec3 ambient = vec3(0.03) * albedo * ao` with the
// standard split-sum IBL approximation, so unlit-side surfaces pick up the sky's colour and
// metals actually reflect something. Three products, all owned here:
//
//   1. Environment cube  (RGB16F, 128 px/face, full mip chain) — the Sky.cpp gradient
//      rasterized into a cubemap. This is the input the other two convolve; it exists only
//      because the sky is procedural (no authored HDRI), and its mip chain is what makes the
//      prefilter pass' importance sampling cheap enough to not alias.
//   2. Irradiance cube   (RGB16F, 32 px/face) — cosine-convolved over the hemisphere. Sampled
//      by the surface normal for the diffuse ambient term. 32 px is plenty: the result is a
//      smooth second-order-spherical-harmonic-looking blob no matter how detailed the input.
//   3. Prefiltered specular cube (RGB16F, 128 px/face, kSpecularMips levels) — GGX-importance-
//      sampled at roughness = mip / (kSpecularMips - 1). Sampled by the reflection vector at
//      textureLod(roughness * (kSpecularMips-1)).
//
// Plus a view-and-environment-independent BRDF integration LUT (RG16F, 256x256, NdotV x
// roughness -> scale/bias for F0). That one is baked ONCE, on first use, and never again.
//
// Baking is NOT per-frame. BakeIfDirty() compares the sky colours it was last baked with and
// does nothing when they match, so the trigger needs no UI plumbing at all: it survives colour
// edits, undo, scene load and serialization identically, because it watches the actual state
// rather than any one code path that writes it.
//
// GL-resource ownership mirrors PointShadowMap / HdrTarget: DSA creation, one FBO reused for
// every face/mip, explicit Release(), non-copyable.
class IblProbe {
public:
    static constexpr int kEnvSize = 128;
    static constexpr int kIrradianceSize = 32;
    static constexpr int kSpecularSize = 128;
    static constexpr int kSpecularMips = 6; // 128, 64, 32, 16, 8, 4 -> roughness 0.0 .. 1.0
    static constexpr int kBrdfLutSize = 256;

    IblProbe() = default;
    ~IblProbe();
    IblProbe(const IblProbe&) = delete;
    IblProbe& operator=(const IblProbe&) = delete;

    // True when a Bake() with these colours would produce something different from what's
    // currently stored (including "nothing is stored yet"). Cheap; call every frame.
    bool NeedsBake(const glm::vec3& horizonColor, const glm::vec3& zenithColor) const;

    // Rasterizes the sky gradient to the environment cube, then convolves the irradiance and
    // prefiltered specular cubes from it. Also bakes the BRDF LUT if it doesn't exist yet.
    // Leaves framebuffer 0 bound and does NOT restore the viewport — the caller does, exactly
    // like the shadow passes in main.cpp.
    void Bake(const glm::vec3& horizonColor, const glm::vec3& zenithColor);

    // Bake() if NeedsBake(); returns whether it actually baked.
    bool BakeIfDirty(const glm::vec3& horizonColor, const glm::vec3& zenithColor);

    unsigned int IrradianceMap() const { return m_IrradianceCube; } // samplerCube
    unsigned int SpecularMap() const { return m_SpecularCube; }     // samplerCube, kSpecularMips levels
    unsigned int BrdfLut() const { return m_BrdfLut; }              // sampler2D, RG16F
    bool IsValid() const { return m_IrradianceCube != 0 && m_SpecularCube != 0 && m_BrdfLut != 0; }

    void Release();

private:
    void EnsureCreated();
    void BakeBrdfLut();
    // Binds face `face` (0..5) of cube `tex` at mip `mip` as colour attachment 0 and sets the
    // viewport to `size`.
    void BeginCubeFace(unsigned int tex, int face, int mip, int size) const;
    // Uploads the six face-basis uniforms (uFaceForward / uFaceRight / uFaceUp) for `face`.
    static void SetFaceBasis(const Shader& shader, int face);

    unsigned int m_EnvCube = 0;
    unsigned int m_IrradianceCube = 0;
    unsigned int m_SpecularCube = 0;
    unsigned int m_BrdfLut = 0;
    unsigned int m_Fbo = 0;
    unsigned int m_Vao = 0; // attribute-less full-screen triangle pair, like Sky/Grid

    std::unique_ptr<Shader> m_EnvShader;
    std::unique_ptr<Shader> m_IrradianceShader;
    std::unique_ptr<Shader> m_PrefilterShader;
    std::unique_ptr<Shader> m_BrdfShader;

    bool m_BrdfLutBaked = false; // the LUT is environment-independent: baked once, never rebaked
    bool m_Baked = false;
    glm::vec3 m_BakedHorizon{0.0f};
    glm::vec3 m_BakedZenith{0.0f};
};
