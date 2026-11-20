#include "SelectionOutline.h"

#include <algorithm>

#include "Components.h"
#include "GLStateCache.h"
#include "gl.h"
#include "HdrTarget.h"
#include "Model.h"
#include "Shader.h"
#include "World.h"

SelectionOutline::~SelectionOutline() { Free(); }

void SelectionOutline::Free() {
    if (m_MaskMsFbo) glDeleteFramebuffers(1, &m_MaskMsFbo);
    if (m_MaskMsColor) glDeleteTextures(1, &m_MaskMsColor);
    if (m_MaskFbo) glDeleteFramebuffers(1, &m_MaskFbo);
    if (m_Mask) glDeleteTextures(1, &m_Mask);
    m_MaskMsFbo = m_MaskMsColor = m_MaskFbo = m_Mask = 0;
    m_MaskW = m_MaskH = 0;
}

void SelectionOutline::EnsureMask(int w, int h) {
    if (m_Mask && m_MaskW == w && m_MaskH == h) return;
    Free();
    glCreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &m_MaskMsColor);
    glTextureStorage2DMultisample(m_MaskMsColor, 4, GL_RG8, w, h, GL_TRUE);
    glCreateFramebuffers(1, &m_MaskMsFbo);
    glNamedFramebufferTexture(m_MaskMsFbo, GL_COLOR_ATTACHMENT0, m_MaskMsColor, 0);
    glCreateTextures(GL_TEXTURE_2D, 1, &m_Mask);
    glTextureStorage2D(m_Mask, 1, GL_RG8, w, h);
    glTextureParameteri(m_Mask, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_Mask, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_Mask, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_Mask, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCreateFramebuffers(1, &m_MaskFbo);
    glNamedFramebufferTexture(m_MaskFbo, GL_COLOR_ATTACHMENT0, m_Mask, 0);
    m_MaskW = w;
    m_MaskH = h;
}

// The selection's outline and the hover pre-highlight in one go: both silhouettes into one mask (the selection in R,
// the hovered mesh in G), then one fullscreen pass paints both rings (OutlineDilate.frag.glsl).
void SelectionOutline::Render(const SelectionOutlineInputs& in) {
    World& world = *in.world;
    static const std::vector<entt::entity> kNone;
    const std::vector<entt::entity>& selection = in.selection ? *in.selection : kNone;

    std::vector<Model*> models;
    std::vector<glm::mat4> xforms;
    for (entt::entity entity : selection) {
        if (!world.Registry.valid(entity)) continue; // may have been deleted this same frame
        // Lights and empties have nothing to outline - they get a screen-space
        // selection ring from EditorLayer::DrawEntityIcons instead.
        auto* renderablePtr = world.Registry.try_get<RenderableComponent>(entity);
        if (!renderablePtr || !renderablePtr->ModelRef) continue;
        xforms.push_back(world.GetCachedWorldTransform(entity));
        models.push_back(renderablePtr->ModelRef.get());
    }
    const std::size_t selected = models.size();
    // Hover pre-highlight: the mesh under the cursor, unless it is already selected.
    const entt::entity hovered = in.hovered;
    if (hovered != entt::null && world.Registry.valid(hovered) &&
        std::find(selection.begin(), selection.end(), hovered) == selection.end()) {
        auto* hoverRend = world.Registry.try_get<RenderableComponent>(hovered);
        if (hoverRend && hoverRend->ModelRef) {
            xforms.push_back(world.GetCachedWorldTransform(hovered));
            models.push_back(hoverRend->ModelRef.get());
        }
    }
    if (models.empty() || in.width <= 0 || in.height <= 0) return;

    // --- Silhouette mask: flat coverage where a mesh is, depth test off so a partly-occluded selection is still fully
    // outlined (Unity's behaviour). The colour mask routes the selection into R and the hovered mesh into G.
    const int w = in.width, h = in.height;
    EnsureMask(w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, m_MaskMsFbo);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    Shader& maskShader = *in.maskShader;
    maskShader.Bind();
    maskShader.SetMat4("uView", in.view);
    maskShader.SetMat4("uProj", in.proj);
    maskShader.SetVec3("uOutlineColor", glm::vec3(1.0f));
    if (in.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // mask must be solid
    for (std::size_t i = 0; i < models.size(); ++i) {
        const bool hover = i >= selected;
        glColorMask(!hover, hover, GL_FALSE, GL_FALSE); // one draw buffer
        maskShader.SetMat4("uModel", xforms[i]);
        models[i]->Draw(maskShader);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (in.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glBlitNamedFramebuffer(m_MaskMsFbo, m_MaskFbo, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST); // resolve

    // --- Ring pass: back to the scene target, paint both rings.
    const int kOutlinePixels = 1; // a thin ring, like Unity's (3 read as a thick band)
    in.target->BindForRender();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    Shader& dilate = *in.dilateShader;
    dilate.Bind();
    glBindTextureUnit(0, m_Mask);
    dilate.SetInt("uMask", 0);
    dilate.SetVec3("uTexel", glm::vec3(1.0f / (float)w, 1.0f / (float)h, 0.0f));
    dilate.SetVec3("uColor", in.color);
    dilate.SetFloat("uHoverFade", 0.65f);                      // the pre-highlight at ~35%
    dilate.SetInt("uRadius", kOutlinePixels);
    {   // Motion v2: the selection's outline sweeps on around its centre after it changes
        glm::vec3 c(0.0f);
        for (std::size_t i = 0; i < selected; ++i) c += glm::vec3(xforms[i][3]);
        if (selected) c /= (float)selected;
        const glm::vec4 clip = in.proj * in.view * glm::vec4(c, 1.0f);
        const glm::vec2 uv = clip.w > 1e-4f ? glm::vec2(clip) / clip.w * 0.5f + 0.5f : glm::vec2(0.5f);
        dilate.SetFloat("uSweepHide", selected ? 1.0f - in.reveal : 0.0f);
        dilate.SetVec2("uSweepCenter", uv);
    }
    glBindVertexArray(in.fsQuadVao);
    glDrawArrays(GL_TRIANGLES, 0, 3); // Fullscreen.vert's covering triangle
    glBindVertexArray(0);

    // Restore.
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    GLStateCache::Invalidate(); // raw shader/VAO/texture binds above bypass the cache
}
