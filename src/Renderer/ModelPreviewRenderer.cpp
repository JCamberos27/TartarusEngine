#include "ModelPreviewRenderer.h"
#include "Model.h"
#include "Shader.h"
#include "ModelShaderSource.h"
#include "gl.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <algorithm>

ModelPreviewRenderer::ModelPreviewRenderer() = default;

ModelPreviewRenderer::~ModelPreviewRenderer() {
    if (m_ColorTex) glDeleteTextures(1, &m_ColorTex);
    if (m_DepthRBO) glDeleteRenderbuffers(1, &m_DepthRBO);
    if (m_FBO) glDeleteFramebuffers(1, &m_FBO);
}

float ModelPreviewRenderer::ComputeFramingDistance(const Model& model) {
    glm::vec3 boundsMin = model.BoundsMin();
    glm::vec3 boundsMax = model.BoundsMax();
    bool validBounds = boundsMin.x <= boundsMax.x && boundsMin.y <= boundsMax.y && boundsMin.z <= boundsMax.z;
    float radius = validBounds ? glm::length(boundsMax - boundsMin) * 0.5f : 1.0f;
    if (!(radius > 0.001f)) radius = 1.0f; // also catches NaN, since NaN > x is always false

    // Distance that fits the bounding sphere inside a 45-degree-FOV camera, with headroom
    // (1.6x) so the model doesn't touch the preview panel's edges.
    return radius / std::sin(glm::radians(45.0f) * 0.5f) * 1.6f;
}

unsigned int ModelPreviewRenderer::Render(Model& model, float yaw, float pitch, float distance, int previewW, int previewH) {
    if (!m_Shader) {
        m_Shader = std::make_unique<Shader>(kModelVertexSrc, kModelFragmentSrc);
        glGenFramebuffers(1, &m_FBO);
        glGenTextures(1, &m_ColorTex);
        glGenRenderbuffers(1, &m_DepthRBO);
    }

    if (previewW != m_TexW || previewH != m_TexH) {
        m_TexW = previewW;
        m_TexH = previewH;

        glBindTexture(GL_TEXTURE_2D, m_ColorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, previewW, previewH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindRenderbuffer(GL_RENDERBUFFER, m_DepthRBO);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, previewW, previewH);
    }

    // Same "restore whatever was bound before" discipline as ChannelPreviewRenderer - this runs
    // mid-frame, nested inside the editor's ImGui pass, alongside the main 3D viewport rendered
    // elsewhere this same frame.
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ColorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_DepthRBO);
    glViewport(0, 0, previewW, previewH);

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.24f, 0.24f, 0.27f, 1.0f); // mid-grey backdrop so dark models read against it (audit #78)
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glm::vec3 boundsMin = model.BoundsMin();
    glm::vec3 boundsMax = model.BoundsMax();
    bool validBounds = boundsMin.x <= boundsMax.x && boundsMin.y <= boundsMax.y && boundsMin.z <= boundsMax.z;
    glm::vec3 center = validBounds ? (boundsMin + boundsMax) * 0.5f : glm::vec3(0.0f);
    float radius = validBounds ? glm::length(boundsMax - boundsMin) * 0.5f : 1.0f;
    if (!(radius > 0.001f)) radius = 1.0f;

    glm::vec3 eye = center + distance * glm::vec3(
        std::cos(pitch) * std::sin(yaw),
        std::sin(pitch),
        std::cos(pitch) * std::cos(yaw));
    glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
    float aspect = previewH > 0 ? (float)previewW / (float)previewH : 1.0f;
    float nearPlane = std::max(0.01f, distance * 0.01f);
    float farPlane = (radius + distance) * 4.0f + 10.0f;
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, nearPlane, farPlane);

    m_Shader->Bind();
    m_Shader->SetMat4("uView", view);
    m_Shader->SetMat4("uProj", proj);
    m_Shader->SetMat4("uModel", glm::mat4(1.0f));
    m_Shader->SetVec3("uViewPos", eye);
    // Fixed, preview-only key light - independent of the real scene's light so the preview
    // always reads consistently regardless of where/how the asset will actually be placed.
    // Fixed preview key light, into the SSBO the shared model shader reads at binding 0.
    m_Lights.Clear();
    m_Lights.AddDirectional(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), glm::vec3(1.0f), 3.0f);
    m_Lights.Upload();
    m_Lights.Bind(0);
    m_Shader->SetInt("uUnlit", 0);
    // This offscreen preview has no shared Tonemapper pass, so keep the baked Reinhard + gamma
    // in the model shader for it (the real scene sets this to 0 and tonemaps separately).
    m_Shader->SetInt("uApplyTonemap", 1);
    m_Shader->SetInt("uShadowEnabled", 0); // preview has no shadow pass
    m_Shader->SetInt("uShadowCascadeCount", 0);

    model.Draw(*m_Shader);

    if (!prevDepthTest) glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int)prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

    return m_ColorTex;
}
