#include "ModelPreviewRenderer.h"
#include "Camera.h" // #202 - MakePerspective
#include "MaterialAsset.h"
#include "Model.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "Log.h"
#include "gl.h"

#include <string>

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

unsigned int ModelPreviewRenderer::Render(Model& model, float yaw, float pitch, float distance, int previewW, int previewH,
                                          const std::vector<std::shared_ptr<MaterialAsset>>& slots, Shading shading,
                                          bool studio) {
    if (!m_Shader) {
        m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("ModelVertex.glsl"),
                                            ShaderLibrary::ReadFile("ModelFragment.glsl"));
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

    GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
        Log::Error("ModelPreviewRenderer: FBO incomplete (0x" + std::to_string(fboStatus) + ") at " +
                   std::to_string(previewW) + "x" + std::to_string(previewH) + " - skipping preview");
        glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int)prevFBO);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        return m_ColorTex;
    }

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
    glm::mat4 proj = MakePerspective(45.0f, aspect, nearPlane, farPlane); // #202

    m_Shader->Bind();
    m_Shader->SetMat4("uView", view);
    m_Shader->SetMat4("uProj", proj);
    m_Shader->SetMat4("uModel", glm::mat4(1.0f));
    m_Shader->SetMat4("uNormalMatrix", glm::mat4(1.0f)); // model is identity here (#104)
    m_Shader->SetVec3("uViewPos", eye);
    // Fixed, preview-only key light - independent of the real scene's light so the preview
    // always reads consistently regardless of where/how the asset will actually be placed.
    // Fixed preview key light, into the SSBO the shared model shader reads at binding 0.
    m_Lights.Clear();
    if (studio) {
        // Camera-relative: a key from above the viewer's left shoulder, a softer fill from the right,
        // and a rim from behind - no side ever goes black as the model turns.
        const glm::vec3 fwd = glm::normalize(center - eye);
        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 up = glm::cross(right, fwd);
        m_Lights.AddDirectional(glm::normalize(fwd + 0.6f * right - 0.8f * up), glm::vec3(1.0f), 3.0f);
        m_Lights.AddDirectional(glm::normalize(fwd - 0.9f * right - 0.2f * up), glm::vec3(0.85f, 0.9f, 1.0f), 1.2f);
        m_Lights.AddDirectional(glm::normalize(-fwd - 0.5f * up), glm::vec3(1.0f), 1.5f);
    } else {
        m_Lights.AddDirectional(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), glm::vec3(1.0f), 3.0f);
    }
    m_Lights.Upload();
    m_Lights.Bind(0);
    m_Shader->SetInt("uUnlit", shading == Shading::Unlit ? 1 : 0);
    // This offscreen preview has no shared Tonemapper pass, so keep the baked Reinhard + gamma
    // in the model shader for it (the real scene sets this to 0 and tonemaps separately).
    m_Shader->SetInt("uApplyTonemap", 1);
    m_Shader->SetInt("uShadowEnabled", 0); // preview has no shadow pass
    m_Shader->SetInt("uShadowCascadeCount", 0);
    m_Shader->SetInt("uSpotShadowCount", 0);
    m_Shader->SetInt("uPointShadowCount", 0);

    model.Draw(*m_Shader, slots);

    if (shading == Shading::Wireframe) {
        // The edges over the lit model: flat light grey, pulled toward the camera so they win the
        // depth test against the faces they lie on.
        if (!m_WireMaterial) {
            m_WireMaterial = std::make_shared<MaterialAsset>();
            m_WireMaterial->Mat.BaseColor = glm::vec3(0.85f);
        }
        const std::vector<std::shared_ptr<MaterialAsset>> wire((size_t)std::max(1, model.MeshCount()), m_WireMaterial);
        constexpr GLenum kPolygonOffsetLine = 0x2A02; // GL_POLYGON_OFFSET_LINE (not in the loader's header)
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glEnable(kPolygonOffsetLine);
        glPolygonOffset(-1.0f, -1.0f);
        m_Shader->SetInt("uUnlit", 1);
        model.Draw(*m_Shader, wire);
        glDisable(kPolygonOffsetLine);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // the editor draws filled everywhere else
        m_Shader->SetInt("uUnlit", 0);
    }

    if (!prevDepthTest) glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int)prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

    return m_ColorTex;
}
