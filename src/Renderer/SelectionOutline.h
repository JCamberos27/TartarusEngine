#pragma once
#include <vector>

#include <glm/glm.hpp>

#include <entt/entity/entity.hpp>

class World;
class HdrTarget;
class Model;
class Shader;

struct SelectionOutlineInputs {
    World* world = nullptr;
    const std::vector<entt::entity>* selection = nullptr; // the editor's selection
    entt::entity hovered = entt::null;                    // the hover pre-highlight's entity (null: none)
    glm::vec3 color{1.0f};                                // the ring's colour
    float reveal = 1.0f;                                  // the selection ring's sweep-on, 0..1 (EditorLayer)
    Shader* maskShader = nullptr;   // OutlineModel: flat white silhouettes into the mask
    Shader* dilateShader = nullptr; // OutlineDilate: the ring around the mask
    unsigned int fsQuadVao = 0;     // attribute-less fullscreen triangle pair
    HdrTarget* target = nullptr;    // the Scene view's HDR target the ring is painted into
    glm::mat4 view{1.0f}, proj{1.0f}; // unjittered, so the outline doesn't shimmer through the TAA resolve
    int width = 0, height = 0;      // the Scene view's size
    bool wireframe = false;         // F5 Wireframe: the caller's polygon mode is GL_LINE
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
    void EnsureMask(int w, int h);
    void Free();

    // One RG8 silhouette mask at the view's size: R the selection, G the hover pre-highlight. Drawn into the 4x MSAA
    // target, resolved into m_Mask for the ring pass (a fetch at a pixel centre reads that pixel's 4-sample coverage).
    unsigned int m_MaskMsFbo = 0, m_MaskMsColor = 0;
    unsigned int m_MaskFbo = 0, m_Mask = 0;
    int m_MaskW = 0, m_MaskH = 0;
};
