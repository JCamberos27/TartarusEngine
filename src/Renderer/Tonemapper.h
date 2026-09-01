#pragma once

class Shader;

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
    void Apply(unsigned int srcHdrTexture, unsigned int dstFbo, int dstW, int dstH,
               float exposureEV, Operator op);

private:
    void EnsureCreated();
    Shader* m_Shader = nullptr;       // owned; raw ptr to keep this header free of <memory>
    unsigned int m_Vao = 0;
};
