#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <glm/glm.hpp>

// GPU occlusion culling for the opaque pass's multi-draw batches (Project Settings > Quality > GPU occlusion culling,
// with GPU-driven draws; TARTARUS_NO_HIZ=1 turns it off for A/B benches).
//
// After the depth pre-pass, a compute pass builds a max-depth pyramid from the view's depth (level 0 is the depth
// itself, each level above keeps the farthest of the texels under it). Every copy in a multi-draw group then tests its
// world box against the pyramid: a box whose nearest point is behind the farthest depth over its screen rectangle is
// hidden by what is already there, and the opaque pass's depth test would have thrown away all of it anyway - so the
// result is exact, never a missing object. The visible copies of each group are packed, in their order, into the remap
// buffer (binding 28) at the group's own instance range, and the group's indirect commands get the visible count as
// their instanceCount. ModelVertex.glsl reads the remap when uInstanced is 2. Nothing waits on the CPU.
//
// Off without a depth pre-pass, on llvmpipe (CI's software GL: the compute passes cost more than they save there), in
// probe captures, debug views and orthographic cameras.
class HiZCuller {
public:
    struct Group {
        std::uint32_t Base, Count;           // the group's run of instance slots
        std::uint32_t CmdFirst, CmdCount;    // its commands, in the command list
    };

    // The setting, the env switch and the GL renderer.
    static bool Enabled();

    // The per-frame inputs: one box per instance slot (slot = what the vertex programs index uInstances by, halved),
    // `bounded` false = never culled. Boxes are padded here (PadBounds, plus the foliage sway `pad`).
    static std::vector<glm::vec4>& Boxes(); // two per slot: min (w = 1 bounded, 0 not), max
    static void AddBox(const glm::vec3& wmin, const glm::vec3& wmax, bool bounded, float pad);

    // Builds the pyramid from `depthTex` (single sample, w x h) and culls `groups` against it, rewriting the
    // instanceCount of the commands in `indirectBuffer` (`cmdList` holds each group's command indices). Restores
    // nothing but the program (false: it did not run, draw unculled): the caller re-binds what its next draws need (texture unit 40, images 0 and 1, storage buffers 22-25 and 29).
    // `sceneSlots`: the caller bound one uint per instance slot at storage buffer 25 (its GPU scene slot, GpuScene.h),
    // and the remap gets those instead of the instance slots - the draws then read the GPU scene's transforms.
    static bool Cull(unsigned depthTex, int w, int h, const glm::vec4& viewport, const glm::mat4& viewProj,
                     const glm::vec3& eye, const std::vector<Group>& groups, const std::vector<std::uint32_t>& cmdList,
                     unsigned indirectBuffer, std::uint32_t slotCount, bool sceneSlots = false);

    // G2-b: culls GpuCull's stage-A survivors against the pyramid of `DepthTex` (built here unless the view's Cull
    // above already built it: PyramidBuilt). Each group (uvec4 at `Groups`: survivors' base at `InSlots`, foliage sway
    // as float bits, where its command indices start in `CmdList`, how many) reads its count from its first command's
    // instanceCount, writes its visible scene slots in order to `OutRemap` at the same base and the count to all its
    // commands. Bounds: GpuScene's Bounds buffer. Leaves storage 22-25, 28, 29, texture unit 40 and the program as it
    // used them.
    struct Chained {
        unsigned DepthTex = 0;
        int W = 0, H = 0;
        bool PyramidBuilt = false;
        glm::vec4 Viewport{0.0f};
        glm::mat4 ViewProj{1.0f};
        glm::vec3 Eye{0.0f};
        const glm::uvec4* Groups = nullptr; // this view's groups (uploaded here)
        std::uint32_t GroupCount = 0;
        unsigned Bounds = 0;
        std::size_t BoundsBytes = 0;
        unsigned CmdList = 0, InSlots = 0, OutRemap = 0;
        unsigned Commands = 0; // the indirect commands: `CommandsBytes` from `CommandsOffset` in this buffer
        std::size_t CommandsOffset = 0, CommandsBytes = 0;
    };
    static bool CullChained(const Chained& c);
    // Binds Cull's remap at 28 again (after something else used 28 before the opaque pass draws from it).
    static void RebindRemap();

    // Testable pieces (unit tests). From origin/dev/occlusion-culling's OcclusionCuller.
    static void PadBounds(glm::vec3& wmin, glm::vec3& wmax);
    // The foliage sway a material can add to a vertex, in metres (FoliageWind.glsl's largest offset: the lean's
    // ~1.6 x strength in a full gust, the branches' 0.3 x and a few cm of flutter, x the scene's wind strength).
    static float FoliageSway(float windStrength, float sceneStrength = 1.0f) {
        return (2.5f * windStrength + 0.1f) * std::max(sceneStrength, 0.0f);
    }
};
