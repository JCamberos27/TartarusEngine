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

    bool Fxaa = false;
    bool Dither = true;
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
    Shader* m_Shader = nullptr;       // owned; raw ptr to keep this header free of <memory>
    Shader* m_Fxaa = nullptr;         // #162
    Framebuffer* m_Ldr = nullptr;     // #162 - tonemapped image FXAA reads from
    unsigned int m_Vao = 0;
};
