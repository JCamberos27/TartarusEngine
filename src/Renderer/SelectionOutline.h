#pragma once
#include <vector>

#include <glm/glm.hpp>

#include "Framebuffer.h"

class World;
class Camera;
class EditorLayer;
class HdrTarget;
class Model;
class Shader;

struct SelectionOutlineInputs {
    World* world = nullptr;
    EditorLayer* editor = nullptr;
    Camera* editorCamera = nullptr;       // the hover pick's camera
    Shader* maskShader = nullptr;   // OutlineModel: flat white silhouettes into the mask
    Shader* dilateShader = nullptr; // OutlineDilate: the ring around the mask
    unsigned int fsQuadVao = 0;     // attribute-less fullscreen triangle pair
    HdrTarget* target = nullptr;    // the Scene view's HDR target the ring is painted into
    glm::mat4 view{1.0f}, proj{1.0f}; // unjittered: the outline sits on the TAA-resolved image
    int width = 0, height = 0;      // the Scene view's size
    bool wireframe = false;         // F5 Wireframe: the caller's polygon mode is GL_LINE
    bool playing = false;           // no hover pre-highlight in Play
};

// The Scene view's selection outline: screen-space. Render the selected meshes' silhouettes to a
// 1-bit mask target, then a fullscreen pass paints a uniform-width ring in the
// pixels just OUTSIDE that mask. No surface wash — the wash + an inverted-hull
// that couldn't widen a flat mesh's silhouette were what turned a selected
// flat/thin object into a solid orange blob (audit #51).
class SelectionOutline {
public:
    SelectionOutline() = default;
    ~SelectionOutline(); // frees the multisampled mask while the GL context (window) is still alive
    SelectionOutline(const SelectionOutline&) = delete;
    SelectionOutline& operator=(const SelectionOutline&) = delete;

    // The hover pre-highlight and the selection's outline, into in.target (left bound on return).
    void Render(const SelectionOutlineInputs& in);

private:
    void DrawOutline(const SelectionOutlineInputs& in, const std::vector<Model*>& models,
                     const std::vector<glm::mat4>& xforms, const glm::vec3& outlineColor, float outlineFade,
                     bool outlineSweep);

    Framebuffer m_Mask; // 1-bit silhouette mask (the resolved one the ring samples)
    unsigned int m_MaskMsFbo = 0, m_MaskMsColor = 0; // its 4x MSAA draw target
    int m_MaskMsW = 0, m_MaskMsH = 0;
};
