#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

class Shader;
class World;
struct Frustum;
struct GrassAreaComponent;
struct TerrainGrid;
struct GrassDensityMap;

// Procedural grass (GrassAreaComponent). Nothing is stored per blade: each view, the CPU picks the 4 m chunks of
// every Grass Area that the camera can see and that lie within the area's draw distance, and a compute pass
// (Grass.comp.glsl) walks each chunk's jittered cell grid, keeping a blade where the area's patch noise, edge fade
// and distance thinning allow and its bounds are on screen. Kept blades go to one of two instance lists - near:
// curved blades of kNearSegments segments; far: a single tapered triangle - and the pass writes both lists' counts
// into two DrawArraysIndirect commands, so the CPU never reads anything back.
// The same program (Grass.vert / .frag.glsl) draws the blades into the view's depth (after SSAO has copied the
// pre-pass, so the blades leave no occlusion halos) and again lit under LEQUAL, so the lit pass shades each pixel
// once, and TAA gets motion vectors for the wind.
// The blades cast no shadows. TARTARUS_NO_GRASS=1 turns it off for a run (A/B measurements).
class GrassRenderer {
public:
    static GrassRenderer& Get();
    static bool Disabled(); // TARTARUS_NO_GRASS

    static constexpr float kChunkSize = 4.0f;       // metres, before rounding to whole cells
    static constexpr int   kMaxChunks = 8192;       // per view
    static constexpr int   kNearSegments = 4;       // near blades: 2 * 4 + 1 strip vertices
    static constexpr std::uint32_t kNearCap = 400000u;  // instances per list; the cull drops the rest
    static constexpr std::uint32_t kFarCap  = 900000u;
    static constexpr float kMinKeep = 0.1f;         // the share of blades left at the draw distance

    // Cells (one blade each) per chunk side for `density` blades per square metre: a whole number of cells close
    // to kChunkSize metres, at least 1.
    static int CellsPerChunk(float density);
    // The share of blades kept at `dist` metres: all within `nearDist`, falling smoothly to kMinKeep at `drawDist`,
    // none past it.
    static float KeepFraction(float dist, float nearDist, float drawDist);

    // One area as the GPU sees it (std430, Grass.comp / .vert / .frag.glsl's GrassArea).
    struct alignas(16) AreaGpu {
        glm::vec4 Origin;      // xyz the area's centre on its plane, w cell size (metres)
        glm::vec4 Axes;        // local X and Z in the world's XZ plane: (xx, xz, zx, zz)
        glm::vec4 Size;        // x SizeX, y SizeZ, z cells per chunk, w seed (as a float)
        glm::vec4 Shape;       // height, height variation, width, bend
        glm::vec4 Base;        // base colour, colour variation
        glm::vec4 Tip;         // tip colour, patchiness
        glm::vec4 Dry;         // dry colour, receive shadows (0/1)
        glm::vec4 Lod;         // near distance, draw distance, edge fade, wind strength
        glm::vec4 Wind;        // wind speed, follow terrain (0/1), -, -
        glm::vec4 Map;         // density mask: first byte in the mask buffer, res x, res z, has mask (0/1)
    };
    // One chunk to cull: area index, first cell (x, z) on the area's grid, cells per side.
    struct ChunkGpu { std::int32_t Area, CellX, CellZ, Cells; };

    // CPU side of one area for chunk selection.
    struct AreaDesc {
        AreaGpu Gpu;
        float MaxHeight = 0.0f; // tallest a blade can stand, plus sway
        std::shared_ptr<const GrassDensityMap> Map; // the density mask, or null
    };
    static AreaDesc Describe(const GrassAreaComponent& g, const glm::mat4& world);
    // Chunks of `areas` inside `frustum` and nearer than their draw distance to `eye` (at most kMaxChunks, the
    // nearest first). Pure CPU: unit-tested.
    // A terrain the Follow Terrain areas stand on: its grid and its entity's world origin (translation only).
    struct TerrainRef {
        std::shared_ptr<const TerrainGrid> Grid;
        glm::vec3 Origin{0.0f};
        std::uint64_t Key = 0;
    };
    static void SelectChunks(const std::vector<AreaDesc>& areas, const Frustum& frustum, const glm::vec3& eye,
                             std::vector<ChunkGpu>& out, const TerrainRef* terrain = nullptr);

    // This frame's areas from the world (once per frame: later calls the same frame return at once).
    void Gather(const World& world);
    bool HasAreas() const { return !m_Areas.empty(); }

    // Per view: scatter and cull (compute). False when nothing is to be drawn.
    bool Cull(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye);
    // Draw the culled blades: depth only (inside the depth pre-pass, its polygon offset already set), or lit
    // (`applyFrameState` brings the program up to the view's lights, shadows and fog; `depthWrites` false when the
    // pre-pass already holds them). Adds the vertices drawn to `verts` (an upper bound: the caps).
    void DrawDepth(const glm::mat4& view, const glm::mat4& proj);
    void DrawLit(const glm::mat4& view, const glm::mat4& proj, bool depthWrites,
                 const std::function<void(Shader&)>& applyFrameState);

    void WarmUp() { EnsurePrograms(); }

private:
    void EnsurePrograms();
    void EnsureBuffers(); // the blade lists (~57 MB): only once some view has grass to draw
    void Draw(Shader& prog, const glm::mat4& view, const glm::mat4& proj);

    std::unique_ptr<Shader> m_Draw;
    std::unique_ptr<Shader> m_Cull;
    std::unique_ptr<Shader> m_Clamp;
    unsigned m_Vao = 0, m_AreaBuf = 0, m_ChunkBuf = 0, m_BladeBuf = 0, m_CmdBuf = 0, m_TerrainBuf = 0;
    unsigned m_MapBuf = 0;
    std::vector<std::uint32_t> m_MapBytes; // every area's mask, packed 4 samples a uint (Grass.comp's uGrassMaps)
    std::uint64_t m_MapSet = 0, m_MapUploaded = 0; // the masks in m_MapBytes / in m_MapBuf
    TerrainRef m_Terrain;              // this frame's (Grid null: none, or no area follows it)
    std::uint64_t m_TerrainUploaded = 0; // the key of the heights in m_TerrainBuf
    std::vector<AreaDesc> m_Areas;
    std::vector<ChunkGpu> m_Chunks;
    std::uint64_t m_GatheredFrame = ~0ull;
    bool m_Culled = false;
};
