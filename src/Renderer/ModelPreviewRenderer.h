#pragma once
#include <memory>
#include <glm/glm.hpp>
#include "LightBuffer.h"

class Model;
class Shader;

// Renders a Model into an offscreen texture from an orbiting camera, for the Inspector's "Model
// Import Settings" preview — the same PBR shader/material-binding path Model::Draw() uses in the
// real scene (see ModelShaderSource.h), so what you see here is really the imported materials
// bound and lit, not a placeholder. Lazily creates its GL resources (an FBO with color + depth
// attachments) on first use and releases them in its destructor, matching the RAII convention
// Texture/Grid/Sky/ChannelPreviewRenderer already follow for their own GL objects.
class ModelPreviewRenderer {
public:
    ModelPreviewRenderer();
    ~ModelPreviewRenderer();

    // yaw/pitch in radians, distance in world units — an orbiting camera around the model's own
    // bounding-box center, framed automatically by ComputeFramingDistance() below. Renders every
    // call (unlike ChannelPreviewRenderer, this one needs to redraw whenever the orbit camera
    // moves, which — while dragging — is most frames, so there's no render-state cache here).
    unsigned int Render(Model& model, float yaw, float pitch, float distance, int previewW, int previewH);

    // A starting distance that frames the model's whole bounding box in a ~45-degree-FOV camera,
    // for initializing a freshly-selected model's orbit state before the user has zoomed at all.
    static float ComputeFramingDistance(const Model& model);

private:
    unsigned int m_FBO = 0;
    unsigned int m_ColorTex = 0;
    unsigned int m_DepthRBO = 0;
    int m_TexW = 0, m_TexH = 0;
    std::unique_ptr<Shader> m_Shader;
    LightBuffer m_Lights; // a single fixed key light, so the shared model shader's SSBO read is valid
};
