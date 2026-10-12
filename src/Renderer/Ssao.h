#pragma once
#include <glm/glm.hpp>
#include <vector>

class Shader;

// Screen-space ambient occlusion (PR15). Owns:
//   - A depth-only FBO for the camera pre-pass (filled by main.cpp using the shadow shader).
//   - A half-resolution RG16F raw occlusion FBO written by Compute() (occlusion + view z).
//   - A full-resolution R8 blurred occlusion FBO written by Blur() (a depth-aware upsample) —
//     this is what the model shader reads.
// Noise texture and kernel samples are fixed-seed so the output is deterministic.
class Ssao {
public:
    ~Ssao();
    Ssao() = default;
    Ssao(const Ssao&) = delete;
    Ssao& operator=(const Ssao&) = delete;

    // Runs the compute and blur programs once on a tiny throwaway target, so the driver finishes
    // building them at load instead of on the first frame that uses SSAO (~100 ms stall).
    static void WarmUp(Shader& ssaoShader, Shader& blurShader);

    // Lazily (re)creates all GL objects. No-op when width/height already match.
    void Resize(int width, int height);

    // Depth-only FBO to bind for the camera pre-pass. GL_DEPTH_ATTACHMENT only;
    // glDrawBuffer is GL_NONE. Clear GL_DEPTH_BUFFER_BIT before drawing into it.
    unsigned int DepthFbo() const { return m_DepthFbo; }
    unsigned int DepthTex() const { return m_DepthTex; }

    // SSAO compute pass — reads DepthTex() and the noise texture, writes the raw occlusion at half
    // resolution (a quarter of the 32-tap work; occlusion is low-frequency and Blur() upsamples it
    // by depth). proj is the same projection matrix used for the depth pre-pass.
    // #160: radius (view-space units) and depth bias used to be hard-coded 0.5 / 0.025.
    void Compute(Shader& ssaoShader, const glm::mat4& proj, float radius = 0.5f, float bias = 0.025f);

    // Full-resolution 4x4 (half-resolution texel) box blur, weighed by depth — reads the raw
    // occlusion and DepthTex(), writes the final blurred occlusion.
    void Blur(Shader& blurShader);

    // Final blurred R8 occlusion texture. Bind as uSSAOMap (unit 15) before drawScene.
    unsigned int OcclusionTexture() const { return m_BlurColor; }

    // True only when every owned FBO created AND validated complete (audit #358). Callers must
    // gate both the SSAO passes and the model shader's uSSAOMap read on this.
    bool IsValid() const { return m_DepthFbo != 0 && m_Valid; }
    int  Width()   const { return m_Width; }
    int  Height()  const { return m_Height; }

private:
    unsigned int m_DepthFbo  = 0, m_DepthTex  = 0;
    unsigned int m_SsaoFbo   = 0, m_SsaoColor = 0;
    unsigned int m_BlurFbo   = 0, m_BlurColor = 0;
    unsigned int m_NoiseTex  = 0;
    unsigned int m_Vao       = 0;

    std::vector<glm::vec3> m_Kernel;
    unsigned int m_KernelUploadedTo = 0; // program the kernel uniforms were last set on (#160)

    int m_Width = 0, m_Height = 0;
    int m_HalfWidth = 0, m_HalfHeight = 0; // the raw occlusion's size
    glm::mat4 m_Proj{1.0f};                // Compute()'s projection, for Blur()'s view depths
    bool m_Valid = false; // all FBOs validated complete this Create() (audit #358)

    void Release();
    void Create(int width, int height);
    void InitKernelAndNoise();
};
