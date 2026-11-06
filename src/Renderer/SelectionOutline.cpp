#include "SelectionOutline.h"

#include <algorithm>

#include "Camera.h"
#include "Components.h"
#include "EditorLayer.h"
#include "GLStateCache.h"
#include "gl.h"
#include "HdrTarget.h"
#include "Model.h"
#include "Shader.h"
#include "World.h"

SelectionOutline::~SelectionOutline() {
    if (m_MaskMsFbo) glDeleteFramebuffers(1, &m_MaskMsFbo);
    if (m_MaskMsColor) glDeleteTextures(1, &m_MaskMsColor);
}

void SelectionOutline::Render(const SelectionOutlineInputs& in) {
    World& world = *in.world;
    EditorLayer& editor = *in.editor;
    auto selection = editor.GetSelectedItems();
    const glm::vec3 kOutlineColor = editor.SelectionOutlineColor(); // theme-driven: near-white on Glass

    // Hover pre-highlight: the mesh under the cursor, unless it is already selected.
    if (!in.playing) {
        editor.UpdateHoverPick(world, *in.editorCamera);
        const entt::entity hovered = editor.HoveredEntity();
        if (hovered != entt::null && world.Registry.valid(hovered) && std::find(selection.begin(), selection.end(), hovered) == selection.end()) {
            auto* hoverRend = world.Registry.try_get<RenderableComponent>(hovered);
            if (hoverRend && hoverRend->ModelRef) {
                DrawOutline(in, {hoverRend->ModelRef.get()}, {world.GetCachedWorldTransform(hovered)}, kOutlineColor, 0.65f, false);
            }
        }
    }

    if (!selection.empty()) {
        std::vector<glm::mat4> xforms;
        std::vector<Model*> models;
        for (entt::entity entity : selection) {
            if (!world.Registry.valid(entity)) continue; // may have been deleted this same frame
            // Lights and empties have nothing to outline - they get a screen-space
            // selection ring from EditorLayer::DrawEntityIcons instead.
            auto* renderablePtr = world.Registry.try_get<RenderableComponent>(entity);
            if (!renderablePtr) continue;
            xforms.push_back(world.GetCachedWorldTransform(entity));
            models.push_back(renderablePtr->ModelRef.get());
        }
        if (!models.empty()) DrawOutline(in, models, xforms, kOutlineColor, 0.0f, true);
    }
}

// One outline: the models' silhouettes into the mask, then the ring around it. The
// selection draws it opaque (with the sweep-on); the hover pre-highlight is a second,
// separate mask draw at ~35% (uFade 0.65): one mask can't carry two entities at two
// strengths, and a hovered mesh is a single cheap draw.
void SelectionOutline::DrawOutline(const SelectionOutlineInputs& in, const std::vector<Model*>& models,
                                   const std::vector<glm::mat4>& xforms, const glm::vec3& outlineColor,
                                   float outlineFade, bool outlineSweep) {
    const int kOutlinePixels = 1; // a thin ring, like Unity's (3 read as a thick band)
    Shader& outlineModelShader = *in.maskShader;
    Shader& outlineDilateShader = *in.dilateShader;
    const int scW = in.width, scH = in.height;
    // --- Silhouette mask pass: flat white where a selected mesh is, into its
    // own target. Depth test off so a partly-occluded selection is still fully
    // outlined (Unity's behaviour).
    // Drawn at 2x the scene size with 4x MSAA, then resolved: a linear fetch at a
    // scene pixel centre averages 2x2 texels of 4 samples, i.e. 16-sample edge
    // coverage for the anti-aliased ring (OutlineDilate.frag.glsl).
    const int maskW = scW * 2, maskH = scH * 2;
    m_Mask.Resize(maskW, maskH);
    if (m_MaskMsW != maskW || m_MaskMsH != maskH) {
        if (m_MaskMsFbo) { glDeleteFramebuffers(1, &m_MaskMsFbo); glDeleteTextures(1, &m_MaskMsColor); }
        glCreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &m_MaskMsColor);
        glTextureStorage2DMultisample(m_MaskMsColor, 4, GL_RGB8, maskW, maskH, GL_TRUE);
        glCreateFramebuffers(1, &m_MaskMsFbo);
        glNamedFramebufferTexture(m_MaskMsFbo, GL_COLOR_ATTACHMENT0, m_MaskMsColor, 0);
        m_MaskMsW = maskW;
        m_MaskMsH = maskH;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_MaskMsFbo);
    glViewport(0, 0, maskW, maskH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    outlineModelShader.Bind();
    outlineModelShader.SetMat4("uView", in.view);
    outlineModelShader.SetMat4("uProj", in.proj);
    outlineModelShader.SetFloat("uThickness", 0.0f);
    outlineModelShader.SetVec3("uOutlineColor", glm::vec3(1.0f));
    if (in.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL); // mask must be solid
    for (size_t i = 0; i < models.size(); ++i) {
        outlineModelShader.SetMat4("uModel", xforms[i]);
        models[i]->Draw(outlineModelShader);
    }
    if (in.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glBlitNamedFramebuffer(m_MaskMsFbo, m_Mask.Handle(), 0, 0, maskW, maskH, 0, 0, maskW, maskH,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST); // MSAA resolve (same RGB8 format as the Framebuffer)

    // --- Dilate pass: back to the scene target, paint the ring.
    in.target->BindForRender();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    outlineDilateShader.Bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_Mask.ColorTexture());
    outlineDilateShader.SetInt("uMask", 0);
    outlineDilateShader.SetVec3("uTexel", glm::vec3(1.0f / (float)scW, 1.0f / (float)scH, 0.0f));
    outlineDilateShader.SetVec3("uColor", outlineColor);
    outlineDilateShader.SetFloat("uFade", outlineFade);
    outlineDilateShader.SetInt("uRadius", kOutlinePixels);
    {   // Motion v2: the outline sweeps on around the selection's centre after it changes
        glm::vec3 c(0.0f);
        for (const glm::mat4& x : xforms) c += glm::vec3(x[3]);
        if (!xforms.empty()) c /= (float)xforms.size();
        const glm::vec4 clip = in.proj * in.view * glm::vec4(c, 1.0f);
        const glm::vec2 uv = clip.w > 1e-4f ? glm::vec2(clip) / clip.w * 0.5f + 0.5f : glm::vec2(0.5f);
        outlineDilateShader.SetFloat("uSweepHide", outlineSweep ? 1.0f - in.editor->SelectionOutlineReveal() : 0.0f);
        outlineDilateShader.SetVec2("uSweepCenter", uv);
    }
    glBindVertexArray(in.fsQuadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Restore.
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    GLStateCache::Invalidate(); // raw shader/VAO/texture binds above bypass the cache
}
