#pragma once
#include "BloodPalette.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

#include "../Assets/BloodFxImport.h"
#include "DecalTextureArrays.h"
#include "GeneratedDecalMaps.h"

class Shader;
struct RuntimeDecal;

// Volumetric blood rendering. Owns the GPU side of the imported sims and draws
// what the game side (Game/Combat/BloodFx) queues each frame:
//   - sprays: the airborne fluid, a vertex-animation-texture sim per spray, one instanced draw per
//     sim, depth-written and lit through ModelFragment.glsl in the opaque pass.
//   - decals: blood on the static world (floors, walls, ceilings, level props). Since Decals P4 these are
//     clustered decals: AddDecal turns each into a RuntimeDecal (the DA_Blood / DA_BulletHole response,
//     maps made once into Library/DecalMaps by GeneratedDecalMaps) and hands it to the DecalSystem, which
//     draws it in the model shader with every other decal (docs/DECALS.md, "Runtime decals").
//   - splats: blood on a mesh, in its bind-pose space, read from the same decal arrays.
// Queue with Add* after BeginFrame (which also starts the DecalSystem's runtime frame).
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
    // The spray and decal programs compiled now (Play's start), not on the first wound mid-fight.
    void WarmUp();
    void AddSpray(const Spray& s);
    int QueuedSprays() const { return (int)m_Sprays.size(); }

    // Draws the queued sprays (opaque, depth-written). `applyFrameState` pushes the scene's
    // per-frame lighting state onto a program (SceneRenderer::ApplyFrameState). Returns draw calls.
    int DrawSprays(const glm::mat4& view, const glm::mat4& proj,
                   const std::function<void(Shader&)>& applyFrameState);

    // Culling: nothing past MaxDistance metres, nor smaller on screen than MinScreen
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
        // A Knife library decal instead of a set: KnifeFxLibrary id, its cell (and the
        // next, blended in - a flipbook such as the wall drips), and for a cell's albedo how the blood dries.
        int Knife = -1;
        int Cell = 0, NextCell = 0;
        float CellBlend = 0.0f;
        bool Blood = true;         // the palette's blood (false: a bullet hole, its surface's own material)
        bool Mirror = false;       // the image flipped across (the same stain, another shape)
        float Rim = 0.0f;          // a bullet hole's chipped rim fades out by here (cell half-widths; 0 = not cut)
    };
    // Submits the decal to the DecalSystem (a stain the first time its maps are asked for shows a moment later,
    // once they are made and decoded). Returns the runtime decals it became (0..2: a flipbook between cells is two).
    int AddDecal(const Decal& d);
    int QueuedDecals() const { return m_QueuedDecals; }
    // A Decal as the runtime decal(s) it becomes: false while its maps are being made. Pure but for the maps.
    int ToRuntime(const Decal& d, RuntimeDecal out[2]);
    // The unit box of a Decal (+Y out of the surface, image top at -Z) as a decal box (+Z out, image top at +Y).
    static glm::mat4 DecalBox(const glm::mat4& model, bool mirror);
    // A bullet hole's maps and response: a Knife hole (`knife` >= 0) or the procedural one. False while the maps are made.
    bool HoleLook(int knife, RuntimeDecal& r);
    // The maps of a blood set / a Knife entry in a mode (null while they're made, or unknown).
    const DecalTextureArrays::Source* SetMaps(int set);
    const DecalTextureArrays::Source* KnifeMaps(int knife, DecalBake::KnifeMode mode);
    // Where the response comes from (project-relative .decal).
    static constexpr const char* kBloodDecal = "assets/Decals/Runtime/DA_Blood.decal";
    static constexpr const char* kHoleDecal = "assets/Decals/Runtime/DA_BulletHole.decal";
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
    // Binds the splat list for the model shader (once per scene draw); the maps are the decal arrays (units 22-25).
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

    bool ListSets();
    std::vector<std::string> m_SetNames;
    std::string m_SetDir;
    int m_QueuedDecals = 0;
    unsigned int m_SplatBuffer = 0;
    size_t m_SplatCapacity = 0;
    int m_SplatCount = 0;
    std::unordered_map<unsigned, glm::ivec2> m_SplatRanges;
};
