#pragma once

#include <cstdint>
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
    // nothing but the program (false: it did not run, draw unculled): the caller re-binds what its next draws need (texture unit 40, images 0 and 1, storage buffers 22-24 and 29).
    static bool Cull(unsigned depthTex, int w, int h, const glm::vec4& viewport, const glm::mat4& viewProj,
                     const glm::vec3& eye, const std::vector<Group>& groups, const std::vector<std::uint32_t>& cmdList,
                     unsigned indirectBuffer, std::uint32_t slotCount);

    // Testable pieces (unit tests). From origin/claude/occlusion-culling's OcclusionCuller.
    static bool NearEye(const glm::vec3& wmin, const glm::vec3& wmax, const glm::vec3& eye, float margin);
    static void PadBounds(glm::vec3& wmin, glm::vec3& wmax);
    // The pyramid level whose texels cover pixels [x0, x1] x [y0, y1] with at most two in each direction.
    static int LevelFor(int x0, int y0, int x1, int y1);
    // The foliage sway a material can add to a vertex, in metres (FoliageWind.glsl's largest offset).
    static float FoliageSway(float windStrength) { return 2.5f * windStrength; }
};
