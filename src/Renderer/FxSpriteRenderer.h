#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

class Shader;
class HdrTarget;

// Draws the flipbook particles FxSprites queues from the Knife sprite library
// (KnifeFxLibrary): every sprite of a pass in one instanced draw, back to front, premultiplied - an
// additive sprite is just one with no coverage - soft against the scene's depth, and lit through
// ModelShading.glsl (sun with its shadows, clustered lights, sky, fog). Queue after BeginFrame.
class FxSpriteRenderer {
public:
    static FxSpriteRenderer& Get(); // deliberately leaked (GL objects outlive static destruction otherwise)

    struct Sprite {
        glm::vec3 Pos{0.0f};
        float Size = 0.1f;          // height (metres)
        glm::vec3 Axis{0.0f};       // non-zero: drawn along it, |Axis| x Size long; zero: faces the camera
        float Rot = 0.0f;
        glm::vec4 Color{1.0f};      // rgb linear (additive: x intensity), a opacity
        int Entry = -1;             // KnifeFxLibrary id; -1 a procedural soft dot
        int Mode = 1;               // FxSprites::Shade
        int CellA = 0, CellB = 0;
        float Blend = 0.0f;         // CellA -> CellB
        float Erosion = 0.0f, Softness = 0.25f;
        float Aspect = 1.0f;        // a cell's width / height
        float Seed = 0.0f;
        bool ViewModel = false;
    };
    void BeginFrame() { m_Sprites.clear(); }
    void Add(const Sprite& s) { m_Sprites.push_back(s); }
    int Queued() const { return (int)m_Sprites.size(); }

    // Draws the world's (or with `viewModel`, the arms-and-gun pass's) sprites onto the bound target,
    // soft against its depth (resolved here). Returns draw calls.
    int Draw(const glm::mat4& view, const glm::mat4& proj, const int viewport[4], const HdrTarget* target, bool viewModel,
             const std::function<void(Shader&)>& applyFrameState);

private:
    FxSpriteRenderer() = default;
    std::vector<Sprite> m_Sprites;
    std::unique_ptr<Shader> m_Program;
    unsigned m_Buffer = 0, m_Vao = 0;
    size_t m_Capacity = 0;
};
