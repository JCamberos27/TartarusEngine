#include "HiZCuller.h"
#include "GpuMemTrack.h"
#include "gl.h"
#include "Shader.h"
#include "GLStateCache.h"
#include "InstanceBatch.h"
#include "ProjectSettings.h"
#include "Core/Env.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
constexpr GLuint kRemapBinding = 28; // ModelVertex.glsl InstanceRemapBlock
constexpr float kNearMargin = 0.5f;  // metres: a box this close to the eye always draws (its corners clip)

// Level 0: the depth as it is.
const char* kCopySrc = R"(#version 460 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 40) uniform sampler2D uDepth;
layout(r32f, binding = 1) writeonly uniform image2D uDst;
uniform ivec2 uSize;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uSize.x || p.y >= uSize.y) return;
    imageStore(uDst, p, vec4(texelFetch(uDepth, p, 0).r));
}
)";

// Each level above: the farthest of the 2x2 texels under it, and the odd row / column at the edge too, so every
// texel of the level below is under one of this level's.
const char* kDownSrc = R"(#version 460 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(r32f, binding = 0) readonly uniform image2D uSrc;
layout(r32f, binding = 1) writeonly uniform image2D uDst;
uniform ivec2 uSrcSize;
uniform ivec2 uDstSize;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uDstSize.x || p.y >= uDstSize.y) return;
    ivec2 s = p * 2;
    int ex = (p.x == uDstSize.x - 1 && (uSrcSize.x & 1) == 1) ? 2 : 1;
    int ey = (p.y == uDstSize.y - 1 && (uSrcSize.y & 1) == 1) ? 2 : 1;
    float m = 0.0;
    for (int y = 0; y <= ey; ++y)
        for (int x = 0; x <= ex; ++x)
            m = max(m, imageLoad(uSrc, min(s + ivec2(x, y), uSrcSize - 1)).r);
    imageStore(uDst, p, vec4(m));
}
)";

// One work group per multi-draw group: its copies test 64 at a time, the visible ones are packed in order, and the
// group's commands take the count.
const char* kCullSrc = R"(#version 460 core
layout(local_size_x = 64) in;
layout(binding = 40) uniform sampler2D uPyramid;
layout(std430, binding = 22) readonly buffer BoxBlock { vec4 uBoxes[]; };
layout(std430, binding = 23) readonly buffer GroupBlock { uvec4 uGroups[]; };
layout(std430, binding = 24) readonly buffer CmdListBlock { uint uCmdList[]; };
layout(std430, binding = 29) buffer CommandBlock { uint uCommands[]; };
layout(std430, binding = 28) writeonly buffer RemapBlock { int uRemap[]; };
layout(std430, binding = 25) readonly buffer SceneSlotBlock { uint uSceneSlot[]; }; // read when uSceneSlots is 1 or 2
// 0: a group's copies are its instance slots, boxes per instance slot. 1: the same, remapped to GPU scene slots (25).
// 2 (CullChained, after GpuCull's stage A): a group's run at 25 holds stage A's survivors (scene slots), as many as its
// first command's instanceCount; the boxes are the GPU scene's Bounds by scene slot, padded here as AddBox pads them
// (group .y = its foliage sway, as float bits).
uniform int uSceneSlots;
uniform mat4 uViewProj;
uniform vec4 uViewport; // x, y, w, h in level-0 pixels
uniform ivec2 uSize;
uniform int uMaxLevel;
uniform vec3 uEye;
uniform float uNearMargin;
shared uint sScan[64];

bool Visible(uint slot, float sway) {
    vec4 lo = uBoxes[slot * 2u], hi = uBoxes[slot * 2u + 1u];
    if (lo.w == 0.0) return true;
    if (uSceneSlots == 2) { // AddBox: PadBounds, then the sway
        vec3 pad = (hi.xyz - lo.xyz) * 0.01 + vec3(0.02);
        lo.xyz -= pad;
        hi.xyz += pad;
        lo.xyz -= vec3(sway);
        hi.xyz += vec3(sway);
    }
    vec3 d = max(max(lo.xyz - uEye, uEye - hi.xyz), vec3(0.0));
    if (dot(d, d) < uNearMargin * uNearMargin) return true;
    vec2 smin = vec2(1e30), smax = vec2(-1e30);
    float zmin = 1.0;
    for (int i = 0; i < 8; ++i) {
        vec3 c = mix(lo.xyz, hi.xyz, vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1)));
        vec4 clip = uViewProj * vec4(c, 1.0);
        if (clip.w <= 1e-4) return true; // behind the eye: no rectangle to test
        vec3 ndc = clip.xyz / clip.w;
        smin = min(smin, ndc.xy);
        smax = max(smax, ndc.xy);
        zmin = min(zmin, ndc.z * 0.5 + 0.5);
    }
    if (zmin <= 0.0) return true;
    // One pixel of margin all round: the depth came from one sample per pixel (MSAA resolves pick one).
    vec2 p0 = uViewport.xy + (smin * 0.5 + 0.5) * uViewport.zw - 1.0;
    vec2 p1 = uViewport.xy + (smax * 0.5 + 0.5) * uViewport.zw + 1.0;
    if (p1.x < 0.0 || p1.y < 0.0 || p0.x > float(uSize.x - 1) || p0.y > float(uSize.y - 1)) return true;
    ivec2 i0 = clamp(ivec2(floor(p0)), ivec2(0), uSize - 1);
    ivec2 i1 = clamp(ivec2(floor(p1)), ivec2(0), uSize - 1);
    int level = 0;
    while ((i1.x >> level) - (i0.x >> level) > 1 || (i1.y >> level) - (i0.y >> level) > 1) ++level;
    if (level > uMaxLevel) return true;
    ivec2 last = textureSize(uPyramid, level) - 1;
    ivec2 a = min(i0 >> level, last), b = min(i1 >> level, last);
    float far = max(max(texelFetch(uPyramid, a, level).r, texelFetch(uPyramid, ivec2(b.x, a.y), level).r),
                    max(texelFetch(uPyramid, ivec2(a.x, b.y), level).r, texelFetch(uPyramid, b, level).r));
    return zmin <= far;
}

void main() {
    uvec4 g = uGroups[gl_WorkGroupID.x];
    uint lid = gl_LocalInvocationID.x;
    bool chained = uSceneSlots == 2;
    uint count = chained ? (g.w > 0u ? uCommands[uCmdList[g.z] * 5u + 1u] : 0u) : g.y;
    float sway = chained ? uintBitsToFloat(g.y) : 0.0;
    barrier(); // every lane has read the count before any lane writes the commands below
    uint written = 0u;
    for (uint c = 0u; c < count; c += 64u) {
        uint k = c + lid;
        uint slot = chained ? (k < count ? uSceneSlot[g.x + k] : 0u) : g.x + k;
        bool v = k < count && Visible(slot, sway);
        sScan[lid] = v ? 1u : 0u;
        barrier();
        for (uint off = 1u; off < 64u; off <<= 1u) {
            uint t = lid >= off ? sScan[lid - off] : 0u;
            barrier();
            sScan[lid] += t;
            barrier();
        }
        if (v) uRemap[g.x + written + sScan[lid] - 1u] = int(chained ? slot : uSceneSlots != 0 ? uSceneSlot[g.x + k] : g.x + k);
        uint total = sScan[63];
        barrier();
        written += total;
    }
    for (uint i = lid; i < g.w; i += 64u) uCommands[uCmdList[g.z + i] * 5u + 1u] = written;
}
)";

struct Resources {
    std::unique_ptr<Shader> Copy, Down, Cull;
    GLuint Boxes = 0, Groups = 0, CmdList = 0, Remap = 0;
    std::uint32_t RemapCapacity = 0; // ints in Remap (grow-only: the cull writes it, nothing on the CPU reads it)
    // One pyramid per view size: the editor's Scene and Game views (and Play's) cull every frame, and a single shared
    // pyramid was reallocated for each of them in turn whenever their sizes differed.
    struct Pyramid {
        GLuint Tex = 0;
        int W = 0, H = 0, Levels = 0;
        std::uint64_t Used = 0;
    };
    std::vector<Pyramid> Pyramids;
    std::uint64_t Clock = 0;
    bool Failed = false;
};
// Never destroyed: the GL context is gone by the time statics are.
Resources& Res() {
    static Resources* r = new Resources();
    return *r;
}
std::vector<glm::vec4> g_Boxes;

bool EnsurePrograms() {
    Resources& r = Res();
    if (r.Cull || r.Failed) return !r.Failed;
    try {
        r.Copy = std::make_unique<Shader>(std::string(kCopySrc), "HiZCopy");
        r.Down = std::make_unique<Shader>(std::string(kDownSrc), "HiZDown");
        r.Cull = std::make_unique<Shader>(std::string(kCullSrc), "HiZCull");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[HiZ] compute programs failed, occlusion culling off: %s\n", e.what());
    }
    if (!r.Copy || !r.Down || !r.Cull || !r.Copy->Program() || !r.Down->Program() || !r.Cull->Program()) {
        r.Copy.reset();
        r.Down.reset();
        r.Cull.reset();
        r.Failed = true;
        return false;
    }
    glCreateBuffers(1, &r.Boxes);
    glCreateBuffers(1, &r.Groups);
    glCreateBuffers(1, &r.CmdList);
    glCreateBuffers(1, &r.Remap);
    return true;
}

// The pyramid for a w x h view: an existing one of that size, else a new one (the least recently used of
// kMaxPyramids makes way - a resized view's old size stops being used).
constexpr std::size_t kMaxPyramids = 4;
Resources::Pyramid& EnsurePyramid(int w, int h) {
    Resources& r = Res();
    ++r.Clock;
    for (Resources::Pyramid& p : r.Pyramids)
        if (p.W == w && p.H == h) {
            p.Used = r.Clock;
            return p;
        }
    Resources::Pyramid* p = nullptr;
    if (r.Pyramids.size() < kMaxPyramids) {
        p = &r.Pyramids.emplace_back();
    } else {
        p = &*std::min_element(r.Pyramids.begin(), r.Pyramids.end(),
                               [](const Resources::Pyramid& a, const Resources::Pyramid& b) { return a.Used < b.Used; });
        glDeleteTextures(1, &p->Tex);
    }
    p->W = w;
    p->H = h;
    p->Used = r.Clock;
    p->Levels = 1;
    for (int m = std::max(w, h); m > 1; m >>= 1) ++p->Levels;
    glCreateTextures(GL_TEXTURE_2D, 1, &p->Tex);
    glTextureStorage2D(p->Tex, p->Levels, GL_R32F, w, h);
    GpuMemTrack::Set("HiZ pyramid " + std::to_string(p - r.Pyramids.data()), (std::size_t)w * h * 4 * 4 / 3); // R32F + mips
    glTextureParameteri(p->Tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTextureParameteri(p->Tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(p->Tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(p->Tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return *p;
}
} // namespace

namespace {
// The max-depth pyramid of `depthTex` (w x h) in this view size's texture.
const Resources::Pyramid& BuildPyramidFor(unsigned depthTex, int w, int h) {
    Resources& r = Res();
    const Resources::Pyramid& pyr = EnsurePyramid(w, h);
    glMemoryBarrier(0x00000400u /* GL_FRAMEBUFFER_BARRIER_BIT */ | GL_TEXTURE_FETCH_BARRIER_BIT);
    r.Copy->Bind();
    glUniform2i(r.Copy->Loc("uSize"), w, h);
    glBindTextureUnit(40, depthTex);
    glBindImageTexture(1, pyr.Tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    r.Copy->DispatchCompute((unsigned)(w + 7) / 8, (unsigned)(h + 7) / 8, 1);
    r.Down->Bind();
    const int srcLoc = r.Down->Loc("uSrcSize"), dstLoc = r.Down->Loc("uDstSize");
    int sw = w, sh = h;
    for (int level = 1; level < pyr.Levels; ++level) {
        const int dw = std::max(1, sw >> 1), dh = std::max(1, sh >> 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        glBindImageTexture(0, pyr.Tex, level - 1, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(1, pyr.Tex, level, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        glUniform2i(srcLoc, sw, sh);
        glUniform2i(dstLoc, dw, dh);
        r.Down->DispatchCompute((unsigned)(dw + 7) / 8, (unsigned)(dh + 7) / 8, 1);
        sw = dw;
        sh = dh;
    }
    return pyr;
}

void SetCullUniforms(Shader& cull, const glm::mat4& viewProj, const glm::vec4& viewport, int w, int h, int levels,
                     const glm::vec3& eye, int mode) {
    cull.Bind();
    cull.SetMat4("uViewProj", viewProj);
    cull.SetVec4("uViewport", viewport);
    glUniform2i(cull.Loc("uSize"), w, h);
    cull.SetInt("uMaxLevel", levels - 1);
    cull.SetVec3("uEye", eye);
    cull.SetFloat("uNearMargin", kNearMargin);
    cull.SetInt("uSceneSlots", mode);
}
} // namespace

bool HiZCuller::CullChained(const Chained& c) {
    if (!c.GroupCount || !c.DepthTex || c.W <= 0 || c.H <= 0 || !EnsurePrograms()) return false;
    Resources& r = Res();
    const Resources::Pyramid& pyr = c.PyramidBuilt ? EnsurePyramid(c.W, c.H) : BuildPyramidFor(c.DepthTex, c.W, c.H);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 22, c.Bounds, 0, (GLsizeiptr)c.BoundsBytes);
    const std::size_t groupBytes = (std::size_t)c.GroupCount * sizeof(glm::uvec4);
    if (!InstanceBuffer::UploadRange(23, c.Groups, groupBytes)) {
        glNamedBufferData(r.Groups, (GLsizeiptr)groupBytes, c.Groups, GL_STREAM_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 23, r.Groups);
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 24, c.CmdList);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 25, c.InSlots);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 28, c.OutRemap);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 29, c.Commands, (GLintptr)c.CommandsOffset, (GLsizeiptr)c.CommandsBytes);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    SetCullUniforms(*r.Cull, c.ViewProj, c.Viewport, c.W, c.H, pyr.Levels, c.Eye, 2);
    glBindTextureUnit(40, pyr.Tex);
    // One work group per group (GL guarantees 65535 in one dimension: more than any view's groups).
    r.Cull->DispatchCompute(std::min<std::uint32_t>(c.GroupCount, 65535u), 1, 1);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    GLStateCache::Invalidate();
    return true;
}

void HiZCuller::RebindRemap() {
    if (Res().Remap) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kRemapBinding, Res().Remap);
}

bool HiZCuller::Enabled() {
    static const int gate = [] {
        if (Env::Flag(Env::Var::NoHiz)) return 0;
        const char* renderer = reinterpret_cast<const char*>(glGetString(0x1F01 /* GL_RENDERER */));
        if (renderer && std::strstr(renderer, "llvmpipe")) return 0;
        return 1;
    }();
    const auto& q = ProjectSettings::Quality();
    return gate && q.GpuOcclusionCulling && MultiDrawEnabled() && !Res().Failed;
}

std::vector<glm::vec4>& HiZCuller::Boxes() { return g_Boxes; }

void HiZCuller::AddBox(const glm::vec3& wmin, const glm::vec3& wmax, bool bounded, float pad) {
    glm::vec3 lo = wmin, hi = wmax;
    if (bounded) {
        PadBounds(lo, hi);
        lo -= glm::vec3(pad);
        hi += glm::vec3(pad);
    }
    g_Boxes.emplace_back(lo, bounded ? 1.0f : 0.0f);
    g_Boxes.emplace_back(hi, 0.0f);
}

void HiZCuller::PadBounds(glm::vec3& wmin, glm::vec3& wmax) {
    // A hair larger than the object: the boxes and the depth program round positions differently.
    const glm::vec3 pad = (wmax - wmin) * 0.01f + glm::vec3(0.02f);
    wmin -= pad;
    wmax += pad;
}

bool HiZCuller::Cull(unsigned depthTex, int w, int h, const glm::vec4& viewport, const glm::mat4& viewProj,
                     const glm::vec3& eye, const std::vector<Group>& groups, const std::vector<std::uint32_t>& cmdList,
                     unsigned indirectBuffer, std::uint32_t slotCount, bool sceneSlots) {
    if (groups.empty() || !depthTex || w <= 0 || h <= 0 || !indirectBuffer || !EnsurePrograms()) return false;
    Resources& r = Res();
    const Resources::Pyramid& pyr = BuildPyramidFor(depthTex, w, h);

    // The test.
    // The inputs through the instance ring (a fresh span each, bound as a range); re-specifying four buffers per view
    // per frame (glNamedBufferData) waited on NVIDIA's threaded driver. The old way only when the ring can't take them.
    auto upload = [](GLuint fallback, unsigned binding, const void* data, std::size_t bytes) {
        if (InstanceBuffer::UploadRange(binding, data, bytes)) return;
        glNamedBufferData(fallback, (GLsizeiptr)bytes, data, GL_STREAM_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, fallback);
    };
    upload(r.Boxes, 22, g_Boxes.data(), g_Boxes.size() * sizeof(glm::vec4));
    upload(r.Groups, 23, groups.data(), groups.size() * sizeof(Group));
    const std::uint32_t noCmd = 0;
    upload(r.CmdList, 24, cmdList.empty() ? &noCmd : cmdList.data(), std::max<std::size_t>(cmdList.size(), 1) * sizeof(std::uint32_t));
    if (indirectBuffer == IndirectBuffer::Handle()) IndirectBuffer::BindStorage(29);
    else glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 29, indirectBuffer);
    // The remap is the cull's own output, read only by the GPU: one store that grows, not a new one each call.
    if (slotCount > r.RemapCapacity) {
        r.RemapCapacity = std::max<std::uint32_t>(slotCount + slotCount / 2, 1024);
        glDeleteBuffers(1, &r.Remap);
        glCreateBuffers(1, &r.Remap);
        glNamedBufferStorage(r.Remap, (GLsizeiptr)r.RemapCapacity * (GLsizeiptr)sizeof(std::int32_t), nullptr, 0);
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kRemapBinding, r.Remap);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    SetCullUniforms(*r.Cull, viewProj, viewport, w, h, pyr.Levels, eye, sceneSlots ? 1 : 0);
    glBindTextureUnit(40, pyr.Tex);
    r.Cull->DispatchCompute((unsigned)groups.size(), 1, 1);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    // TARTARUS_HIZ_STATS=1: how many copies the test let through, now and then (waits on the GPU; debugging only).
    static const bool stats = Env::Flag(Env::Var::HizStats);
    static int frame = 0;
    if (stats && ++frame % 120 == 0) {
        std::uint32_t total = 0, kept = 0;
        for (const Group& g : groups) {
            total += g.Count;
            if (!g.CmdCount) continue;
            std::uint32_t n = 0;
            const GLintptr base = indirectBuffer == IndirectBuffer::Handle() ? (GLintptr)IndirectBuffer::Base() : 0;
            glGetNamedBufferSubData(indirectBuffer, base + (GLintptr)(cmdList[g.CmdFirst] * 5u + 1u) * 4, 4, &n);
            kept += n;
        }
        std::fprintf(stdout, "[HiZ] %u of %u copies drawn (%u groups)\n", kept, total, (unsigned)groups.size());
        std::fflush(stdout);
    }
    GLStateCache::Invalidate();
    return true;
}
