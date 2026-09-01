#pragma once
#include <glm/glm.hpp>
#include <array>

// Cascaded shadow maps for the directional sun.
//
// One GL_TEXTURE_2D_ARRAY of DEPTH_COMPONENT32F layers (one per cascade), sampled in the model
// shader as a sampler2DArrayShadow. Each frame Update() splits the active render camera's view
// range into `Count()` depth slices, fits a texel-snapped orthographic light frustum around
// each slice, and hands back the light view-projection matrices + the slice far-depths. The
// caller then renders scene depth once per cascade via Begin(i).
//
// Fixed contract for now: 4 cascades, resolution from EditorSettings, PCF filtering done in the
// shader. PCSS / contact-hardening is a later toggle.
class CascadedShadowMap {
public:
    static constexpr int kMaxCascades = 4;

    CascadedShadowMap() = default;
    ~CascadedShadowMap();
    CascadedShadowMap(const CascadedShadowMap&) = delete;
    CascadedShadowMap& operator=(const CascadedShadowMap&) = delete;

    // Lazily (re)creates the depth array when resolution/count change. `count` is clamped to
    // [1, kMaxCascades].
    void Configure(int resolution, int count);

    // Recomputes cascade splits + light matrices for this frame. `lightDir` is the direction the
    // sunlight travels (normalized inside). `camView`/`camProj` are the matrices of whichever
    // view is about to be rendered; `shadowDistance` caps how far cascades reach (world units).
    void Update(const glm::mat4& camView, const glm::mat4& camProj,
                const glm::vec3& lightDir, float shadowDistance);

    // Binds the shadow FBO targeting cascade `i`'s layer, sets the viewport, clears its depth.
    // Caller then draws occluders with LightViewProj(i). Does NOT restore the previous
    // framebuffer — the caller captured and restores it.
    void Begin(int i) const;

    unsigned int DepthArray() const { return m_DepthArray; }
    int Resolution() const { return m_Resolution; }
    int Count() const { return m_Count; }
    const glm::mat4& LightViewProj(int i) const { return m_LightViewProj[i]; }
    // Cascade split far-distances in *view space* (positive), one per cascade; [i] is the far
    // edge of cascade i. Packed into a vec4 for the shader (unused lanes hold the last split).
    glm::vec4 SplitDepthsVec4() const;

private:
    unsigned int m_DepthArray = 0;
    unsigned int m_Fbo = 0;
    int m_Resolution = 2048;
    int m_Count = 4;

    std::array<glm::mat4, kMaxCascades> m_LightViewProj{};
    std::array<float, kMaxCascades> m_SplitFar{};

    void Release();
};
