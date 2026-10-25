#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

class Model;
class Shader;
struct Material;
struct MaterialAsset;

// Shell fur (Material::FurDesc): after the opaque pass, every visible model with a fur material is drawn again
// as a stack of instanced shells - copies of the surface pushed out along its skinned normal - alpha-tested
// against per-strand cells (FurShell.vert / .frag.glsl). Depth-tested and depth-writing like any opaque
// surface, so nothing is sorted; on a multisampled target the strands' edges go out as alpha-to-coverage.
// The shells cast no shadows (the surface under them does) and stay out of the SSAO pre-pass.
// Cost scales with the shells drawn, so the count follows the fur's size on screen (ShellCount) and drops to
// none once a hair is under a pixel. TARTARUS_NO_FUR=1 turns it off for a run (A/B measurements).
class FurRenderer {
public:
    static FurRenderer& Get();

    // Shells for fur `furLength` metres long whose nearest point is `distance` metres from the eye, under a
    // projection whose [1][1] is `projScaleY`, in a view `viewportHeight` pixels tall: about one shell per
    // two-thirds of a pixel the hair spans on screen, at least kMinShells once it is drawn at all, at most
    // `maxShells`, and none when the hair spans less than kMinPixels.
    static int ShellCount(int maxShells, float furLength, float distance, float projScaleY, float viewportHeight);
    static constexpr int   kMinShells = 4;
    static constexpr float kMinPixels = 0.6f;
    static constexpr float kShellsPerPixel = 1.5f;

    // True when any opaque sub-mesh of `model` has fur on (slot materials override the imported ones).
    static bool HasFur(const Model& model, const std::vector<std::shared_ptr<MaterialAsset>>& slots);
    static bool Disabled(); // TARTARUS_NO_FUR

    struct Item {
        Model* Ref = nullptr;
        const std::vector<std::shared_ptr<MaterialAsset>>* Slots = nullptr;
        glm::mat4 Xform{1.0f};
        glm::vec3 Centre{0.0f}; // world-space bounds centre
        float Radius = 0.0f;    // world-space bounds radius
        int  LayerBit = 1;
        bool ReceiveShadows = true;
    };
    // Draws the items' shells into the bound target. `applyFrameState` brings the fur program up to this view's
    // lights, shadows and fog (SceneRenderer::ApplyFrameState). Returns the draw calls; adds the triangles.
    int Draw(const std::vector<Item>& items, const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye,
             float viewportHeight, bool multisampled, const std::function<void(Shader&)>& applyFrameState,
             long long& triangles);

    void WarmUp() { EnsureProgram(); }

private:
    void EnsureProgram();
    std::unique_ptr<Shader> m_Program;
};
