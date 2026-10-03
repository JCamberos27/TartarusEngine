#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "../Assets/BloodFxImport.h"

class Shader;

// Volumetric blood rendering (docs/BLOOD_FX.md). Owns the GPU side of the imported sims and draws
// what the game side (Game/Combat/BloodFx) queues each frame:
//   - sprays: the airborne fluid, a vertex-animation-texture sim per spray, one instanced draw per
//     sim, depth-written and lit through ModelFragment.glsl in the opaque pass.
// Queue with Add* after BeginFrame; SceneRenderer draws the queue at the right points of the pass.
class BloodRenderer {
public:
    static BloodRenderer& Get(); // deliberately leaked (GL objects outlive static destruction otherwise)

    // CPU side of one sim (what the game needs to time and place sprays); valid after Load().
    struct SimInfo {
        std::string Name;
        BloodFxImport::VatHeader Header;
        std::vector<BloodFxImport::VatFrame> Frames;
    };

    // Reads project/assets/Effects/Blood/*.bvat once (needs the GL context). False when the data
    // isn't there (the import hasn't been run) - blood then stays off without errors past one log line.
    bool Load();
    bool Loaded() const { return m_Loaded; }
    int SimIndex(const std::string& name) const; // -1 if unknown
    const SimInfo* Sim(int index) const { return index >= 0 && index < (int)m_Sims.size() ? &m_Sims[index] : nullptr; }

    struct Spray {
        int Sim = -1;
        glm::mat4 Model{1.0f};       // sim space -> world
        int Frame = 0;
        glm::vec3 Tint{1.0f};        // x the fluid's albedo
        glm::vec4 ClipPlane{0, 0, 0, 1}; // the fluid behind this plane (dot < 0) isn't drawn
    };
    void BeginFrame();
    void AddSpray(const Spray& s);
    int QueuedSprays() const { return (int)m_Sprays.size(); }

    // Draws the queued sprays (opaque, depth-written). `applyFrameState` pushes the scene's
    // per-frame lighting state onto a program (SceneRenderer::ApplyFrameState). Returns draw calls.
    int DrawSprays(const glm::mat4& view, const glm::mat4& proj,
                   const std::function<void(Shader&)>& applyFrameState);

    // The look (linear albedo, roughness), shared with the decals so a pool matches its drops.
    glm::vec3 FluidAlbedo{0.32f, 0.012f, 0.009f};
    float FluidRoughness = 0.07f;

private:
    BloodRenderer() = default;
    void EnsureProgram();

    struct GpuSim { unsigned int Tex = 0; int FrameBase = 0; };
    std::vector<SimInfo> m_Sims;
    std::vector<GpuSim> m_Gpu;
    bool m_Loaded = false, m_LoadTried = false;
    unsigned int m_FrameBuffer = 0; // SSBO: every sim's per-frame bounds
    unsigned int m_SprayBuffer = 0; // SSBO: this frame's spray instances
    size_t m_SprayCapacity = 0;
    unsigned int m_EmptyVao = 0;
    std::unique_ptr<Shader> m_Program;
    std::vector<Spray> m_Sprays;
};
