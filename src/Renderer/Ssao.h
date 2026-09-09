#pragma once
#include <glm/glm.hpp>
#include <vector>

class Shader;

// Screen-space ambient occlusion (PR15). Owns:
//   - A depth-only FBO for the camera pre-pass (filled by main.cpp using the shadow shader).
//   - An R8 raw occlusion FBO written by Compute().
//   - An R8 blurred occlusion FBO written by Blur() — this is what the model shader reads.
// Noise texture and kernel samples are fixed-seed so the output is deterministic.
class Ssao {
public:
    ~Ssao();
    Ssao() = default;
    Ssao(const Ssao&) = delete;
    Ssao& operator=(const Ssao&) = delete;

    // Lazily (re)creates all GL objects. No-op when width/height already match.
    void Resize(int width, int height);

    // Depth-only FBO to bind for the camera pre-pass. GL_DEPTH_ATTACHMENT only;
    // glDrawBuffer is GL_NONE. Clear GL_DEPTH_BUFFER_BIT before drawing into it.
    unsigned int DepthFbo() const { return m_DepthFbo; }
    unsigned int DepthTex() const { return m_DepthTex; }

    // SSAO compute pass — reads DepthTex() and the noise texture, writes raw R8 occlusion.
    // proj is the same projection matrix used for the depth pre-pass.
    void Compute(Shader& ssaoShader, const glm::mat4& proj);

    // 4x4 box blur pass — reads raw occlusion, writes final blurred occlusion.
    void Blur(Shader& blurShader);

    // Final blurred R8 occlusion texture. Bind as uSSAOMap (unit 15) before drawScene.
    unsigned int OcclusionTexture() const { return m_BlurColor; }

    bool IsValid() const { return m_DepthFbo != 0; }
    int  Width()   const { return m_Width; }
    int  Height()  const { return m_Height; }

private:
    unsigned int m_DepthFbo  = 0, m_DepthTex  = 0;
    unsigned int m_SsaoFbo   = 0, m_SsaoColor = 0;
    unsigned int m_BlurFbo   = 0, m_BlurColor = 0;
    unsigned int m_NoiseTex  = 0;
    unsigned int m_Vao       = 0;

    std::vector<glm::vec3> m_Kernel;

    int m_Width = 0, m_Height = 0;

    void Release();
    void Create(int width, int height);
    void InitKernelAndNoise();
};
