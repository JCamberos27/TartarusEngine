#pragma once
#include "BloodPalette.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

#include "../Assets/BloodFxImport.h"

class Shader;
class HdrTarget;

// Volumetric blood rendering (docs/BLOOD_FX.md). Owns the GPU side of the imported sims and draws
// what the game side (Game/Combat/BloodFx) queues each frame:
//   - sprays: the airborne fluid, a vertex-animation-texture sim per spray, one instanced draw per
//     sim, depth-written and lit through ModelFragment.glsl in the opaque pass.
//   - decals: blood on the static world (floors, walls, ceilings, level props), box-projected onto
//     the depth of the static geometry before the moving things draw over it - one instanced draw.
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

    // Culling (docs/BLOOD_FX.md, v2 performance): nothing past MaxDistance metres, nor smaller on screen than MinScreen
    // (its bounding radius over its distance - ~2 px at 1080p and a 70 degree view).
    float SprayMaxDistance = 80.0f, DecalMaxDistance = 120.0f, MinScreen = 0.0025f;
    // Bounding sphere (centre, radius) -> drawn at all from `eye`.
    bool WorthDrawing(const glm::vec3& centre, float radius, const glm::vec3& eye, float maxDistance) const {
        const float d = glm::length(centre - eye);
        return d - radius <= maxDistance && (d <= radius || radius / d >= MinScreen);
    }
    int CulledSprays() const { return m_CulledSprays; }
    // The look (linear albedo, roughness): BloodPalette, shared by every piece of the blood.
    glm::vec3 FluidAlbedo = BloodPalette::Fresh;
    float FluidRoughness = BloodPalette::RoughSpray;

    // --- decals ---
    int DecalSet(const std::string& name) const; // a decal texture set ("blood1", "char", ...); -1 unknown
    int DecalSetCount() const { return (int)m_SetNames.size(); }
    struct Decal {
        glm::mat4 Model{1.0f};     // unit box -> world; projects along its +Y (out of the surface)
        int Set = -1;
        float Cutout = 0.0f;       // 0 fully spread .. 1 gone (the mask's reveal order)
        float Dry = 0.0f;          // 0 fresh and glossy .. 1 dried dark and matte
        float Opacity = 1.0f;
        float NormalStrength = 0.6f;
        // A Knife library decal instead of a set (docs/BLOOD_FX.md, v2): KnifeFxLibrary id, its cell (and the
        // next, blended in - a flipbook such as the wall drips), and for a cell's albedo how the blood dries.
        int Knife = -1;
        int Cell = 0, NextCell = 0;
        float CellBlend = 0.0f;
        bool Blood = true;         // the palette's blood (false: a bullet hole, its surface's own material)
        bool Mirror = false;       // the image flipped across (the same stain, another shape)
    };
    void AddDecal(const Decal& d);
    int QueuedDecals() const { return (int)m_Decals.size(); }
    // Draws the queued decals onto what the bound HDR target holds so far (resolving its depth first).
    // `viewport` is x, y, w, h. Returns draw calls.
    int DrawDecals(const glm::mat4& view, const glm::mat4& proj, const int viewport[4], const HdrTarget& target,
                   const std::function<void(Shader&)>& applyFrameState);
    // --- splats: blood on a mesh, pinned in its bind-pose space (ModelFragment.glsl's BloodSplatCover) ---
    struct Splat {
        glm::vec3 Center{0.0f};      // bind space
        float Radius = 0.1f;         // the box's half-size, bind units
        glm::vec3 Normal{0, 1, 0};   // out of the surface
        float Depth = 0.1f;          // the projection's half-depth
        glm::vec3 Tangent{1, 0, 0};  // the decal's u axis
        int Set = -1;
        float Cutout = 0.0f, Dry = 0.0f, Opacity = 1.0f;
    };
    // This frame's splats and which entities draw which run of them (entity -> first, count).
    void SetSplats(const std::vector<Splat>& splats, const std::vector<std::pair<unsigned, glm::ivec2>>& ranges);
    bool SplatRange(unsigned entity, int& first, int& count) const;
    int SplatCount() const { return m_SplatCount; }
    // Binds the splat list and the decal atlas for the model shader (once per scene draw).
    void BindSplatResources() const;

    // The stains' albedo, fresh (matching the drops) and dried dark red-brown (BloodPalette).
    glm::vec3 FilmFresh = BloodPalette::Fresh;
    glm::vec3 FilmDried = BloodPalette::Dried;

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
    int m_CulledSprays = 0;

    bool BuildAtlas();
    unsigned int m_AtlasNorm = 0, m_AtlasMask = 0, m_Lookup = 0;
    std::vector<std::string> m_SetNames;
    std::vector<glm::vec4> m_RectNorm, m_RectMask;
    std::vector<Decal> m_Decals;
    unsigned int m_DecalBuffer = 0;
    size_t m_DecalCapacity = 0;
    std::unique_ptr<Shader> m_DecalProgram;
    unsigned int m_SplatBuffer = 0;
    size_t m_SplatCapacity = 0;
    int m_SplatCount = 0;
    std::unordered_map<unsigned, glm::ivec2> m_SplatRanges;
};
