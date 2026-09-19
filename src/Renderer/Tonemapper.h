#pragma once

class Shader;
class Framebuffer;

// #162 - everything the final HDR -> display pass applies, gathered so call sites don't grow
// a parameter per effect. Defaults are neutral (grading off, no vignette, no FXAA).
struct PostSettings {
    float ExposureEV = 0.0f;
    int Operator = 1; // Tonemapper::Operator
    unsigned int BloomTexture = 0;
    float BloomIntensity = 0.0f;

    // Colour grading, Unity's ranges: -100..100, 0 = neutral.
    float Temperature = 0.0f, Tint = 0.0f, Contrast = 0.0f, Saturation = 0.0f;
    float ColorFilter[3] = {1.0f, 1.0f, 1.0f};

    float VignetteIntensity = 0.0f; // 0 = off
    float VignetteSmoothness = 0.4f;
    float ChromaticAberration = 0.0f; // 0..1, colour fringing toward the edges
    float FilmGrain = 0.0f;           // 0..1, animated luminance noise
    float FilmGrainResponse = 0.8f;   // 0..1, how much bright areas are spared

    // #162 - depth of field. Needs DepthTexture (this view's resolved depth) and the view's
    // projection terms (proj[2][2], proj[3][2]) to turn it back into distance.
    bool  DepthOfField = false;
    float FocusDistance = 10.0f; // metres
    float FocusRange = 3.0f;     // width of the sharp band
    float MaxBlur = 8.0f;        // blur radius in pixels at 1080p
    unsigned int DepthTexture = 0;
    float ProjA = 0.0f, ProjB = 0.0f;
    bool  Ortho = false;

    bool Fxaa = false;
    bool Dither = true;

    // #162 - auto exposure (Unity's Exposure > Automatic). The meter eases toward the scene's
    // average luminance; Min/Max bound the correction in EV around 18% grey.
    bool  AutoExposure = false;
    float AutoExposureMinEV = -4.0f, AutoExposureMaxEV = 4.0f;
    float AutoExposureSpeedUp = 2.0f, AutoExposureSpeedDown = 1.0f;
    float DeltaTime = 0.0f;  // seconds since this view's last frame; <= 0 snaps instantly
    int   ExposureSlot = 0;  // which adaptation history to use: each view adapts on its own
};

// Fullscreen HDR -> LDR resolve. Samples a linear RGBA16F texture (the resolved HdrTarget),
// applies exposure, a tone-mapping curve, and gamma, and writes 8-bit into a destination
// framebuffer (the Framebuffer the editor shows via ImGui::Image, or FBO 0 for maximized play).
//
// This replaces the Reinhard + gamma that used to be baked into the model fragment shader, so
// every 3D pass now outputs linear HDR and the mapping happens exactly once, here.
class Tonemapper {
public:
    enum class Operator { Reinhard = 0, ACES = 1, AgX = 2 };

    Tonemapper() = default;
    ~Tonemapper();
    Tonemapper(const Tonemapper&) = delete;
    Tonemapper& operator=(const Tonemapper&) = delete;

    // srcHdrTexture: linear RGBA16F, single-sample (HdrTarget::ResolvedColorTexture()).
    // dstFbo: target framebuffer object (0 = default). dstW/dstH: its viewport.
    // exposureEV: stops of exposure compensation applied before the curve (0 = neutral).
    // bloomTexture: half-res blurred glow (Bloom::GlowTexture()); 0 = bloom disabled.
    // bloomIntensity: additive scale for the glow before the tone curve.
    void Apply(unsigned int srcHdrTexture, unsigned int dstFbo, int dstW, int dstH, const PostSettings& post);

private:
    void EnsureCreated();
    // #162 - meters srcHdrTexture and updates the slot's adapted EV; returns that 1x1 texture.
    unsigned int UpdateAutoExposure(unsigned int srcHdrTexture, const PostSettings& post);
    // #162 - blurs srcHdrTexture by depth into m_DofTex (w x h) and returns it.
    unsigned int ApplyDepthOfField(unsigned int srcHdrTexture, int w, int h, const PostSettings& post);
    Shader* m_DofShader = nullptr;
    unsigned int m_DofTex = 0, m_DofFbo = 0;
    int m_DofW = 0, m_DofH = 0;
    static constexpr int kExposureSlots = 2;
    Shader* m_Shader = nullptr;       // owned; raw ptr to keep this header free of <memory>
    Shader* m_Fxaa = nullptr;         // #162
    Framebuffer* m_Ldr = nullptr;     // #162 - tonemapped image FXAA reads from
    unsigned int m_Vao = 0;

    Shader* m_LumShader = nullptr;    // #162 - auto exposure
    Shader* m_AdaptShader = nullptr;
    unsigned int m_LumTex = 0, m_LumFbo = 0;
    unsigned int m_EvTex[kExposureSlots][2] = {}, m_EvFbo[kExposureSlots][2] = {};
    int  m_EvCur[kExposureSlots] = {};
    bool m_EvValid[kExposureSlots] = {};
};
