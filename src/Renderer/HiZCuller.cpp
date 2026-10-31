#include "HiZCuller.h"
#include "gl.h"
#include "Shader.h"
#include "GLStateCache.h"
#include "InstanceBatch.h"
#include "ProjectSettings.h"
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
uniform mat4 uViewProj;
uniform vec4 uViewport; // x, y, w, h in level-0 pixels
uniform ivec2 uSize;
uniform int uMaxLevel;
uniform vec3 uEye;
uniform float uNearMargin;
shared uint sScan[64];

bool Visible(uint slot) {
    vec4 lo = uBoxes[slot * 2u], hi = uBoxes[slot * 2u + 1u];
    if (lo.w == 0.0) return true;
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
    uint written = 0u;
    for (uint c = 0u; c < g.y; c += 64u) {
        uint k = c + lid;
        bool v = k < g.y && Visible(g.x + k);
        sScan[lid] = v ? 1u : 0u;
        barrier();
        for (uint off = 1u; off < 64u; off <<= 1u) {
            uint t = lid >= off ? sScan[lid - off] : 0u;
            barrier();
            sScan[lid] += t;
            barrier();
        }
        if (v) uRemap[g.x + written + sScan[lid] - 1u] = int(g.x + k);
        uint total = sScan[63];
        barrier();
        written += total;
    }
    for (uint i = lid; i < g.w; i += 64u) uCommands[uCmdList[g.z + i] * 5u + 1u] = written;
}
)";

struct Resources {
    std::unique_ptr<Shader> Copy, Down, Cull;
    GLuint Pyramid = 0, Boxes = 0, Groups = 0, CmdList = 0, Remap = 0;
    int W = 0, H = 0, Levels = 0;
    bool Failed = false;
};
// Never destroyed: the GL context is gone by the time statics are.
Resources& Res() {
    static Resources* r = new Resources();
    return *r;
}
std::vector<glm::vec4> g_Boxes;

bool EnvSet(const char* name, const char* value) {
    char* e = nullptr;
    size_t n = 0;
    bool v = false;
    if (_dupenv_s(&e, &n, name) == 0 && e) v = std::strcmp(e, value) == 0;
    std::free(e);
    return v;
}

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

void EnsurePyramid(int w, int h) {
    Resources& r = Res();
    if (r.Pyramid && r.W == w && r.H == h) return;
    if (r.Pyramid) glDeleteTextures(1, &r.Pyramid);
    r.W = w;
    r.H = h;
    r.Levels = 1;
    for (int m = std::max(w, h); m > 1; m >>= 1) ++r.Levels;
    glCreateTextures(GL_TEXTURE_2D, 1, &r.Pyramid);
    glTextureStorage2D(r.Pyramid, r.Levels, GL_R32F, w, h);
    glTextureParameteri(r.Pyramid, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTextureParameteri(r.Pyramid, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(r.Pyramid, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(r.Pyramid, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
} // namespace

bool HiZCuller::Enabled() {
    static const int gate = [] {
        if (EnvSet("TARTARUS_NO_HIZ", "1")) return 0;
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

bool HiZCuller::NearEye(const glm::vec3& wmin, const glm::vec3& wmax, const glm::vec3& eye, float margin) {
    const glm::vec3 d = glm::max(glm::max(wmin - eye, eye - wmax), glm::vec3(0.0f));
    return glm::dot(d, d) < margin * margin;
}

void HiZCuller::PadBounds(glm::vec3& wmin, glm::vec3& wmax) {
    // A hair larger than the object: the boxes and the depth program round positions differently.
    const glm::vec3 pad = (wmax - wmin) * 0.01f + glm::vec3(0.02f);
    wmin -= pad;
    wmax += pad;
}

int HiZCuller::LevelFor(int x0, int y0, int x1, int y1) {
    int level = 0;
    while ((x1 >> level) - (x0 >> level) > 1 || (y1 >> level) - (y0 >> level) > 1) ++level;
    return level;
}

bool HiZCuller::Cull(unsigned depthTex, int w, int h, const glm::vec4& viewport, const glm::mat4& viewProj,
                     const glm::vec3& eye, const std::vector<Group>& groups, const std::vector<std::uint32_t>& cmdList,
                     unsigned indirectBuffer, std::uint32_t slotCount) {
    if (groups.empty() || !depthTex || w <= 0 || h <= 0 || !indirectBuffer || !EnsurePrograms()) return false;
    Resources& r = Res();
    EnsurePyramid(w, h);

    // The pyramid.
    glMemoryBarrier(0x00000400u /* GL_FRAMEBUFFER_BARRIER_BIT */ | GL_TEXTURE_FETCH_BARRIER_BIT);
    r.Copy->Bind();
    glUniform2i(r.Copy->Loc("uSize"), w, h);
    glBindTextureUnit(40, depthTex);
    glBindImageTexture(1, r.Pyramid, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    r.Copy->DispatchCompute((unsigned)(w + 7) / 8, (unsigned)(h + 7) / 8, 1);
    r.Down->Bind();
    const int srcLoc = r.Down->Loc("uSrcSize"), dstLoc = r.Down->Loc("uDstSize");
    int sw = w, sh = h;
    for (int level = 1; level < r.Levels; ++level) {
        const int dw = std::max(1, sw >> 1), dh = std::max(1, sh >> 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        glBindImageTexture(0, r.Pyramid, level - 1, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        glBindImageTexture(1, r.Pyramid, level, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        glUniform2i(srcLoc, sw, sh);
        glUniform2i(dstLoc, dw, dh);
        r.Down->DispatchCompute((unsigned)(dw + 7) / 8, (unsigned)(dh + 7) / 8, 1);
        sw = dw;
        sh = dh;
    }

    // The test.
    glNamedBufferData(r.Boxes, (GLsizeiptr)(g_Boxes.size() * sizeof(glm::vec4)), g_Boxes.data(), GL_STREAM_DRAW);
    glNamedBufferData(r.Groups, (GLsizeiptr)(groups.size() * sizeof(Group)), groups.data(), GL_STREAM_DRAW);
    glNamedBufferData(r.CmdList, (GLsizeiptr)(std::max<std::size_t>(cmdList.size(), 1) * sizeof(std::uint32_t)),
                      cmdList.empty() ? nullptr : cmdList.data(), GL_STREAM_DRAW);
    glNamedBufferData(r.Remap, (GLsizeiptr)(std::max<std::uint32_t>(slotCount, 1) * sizeof(std::int32_t)), nullptr, GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 22, r.Boxes);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 23, r.Groups);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 24, r.CmdList);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 29, indirectBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kRemapBinding, r.Remap);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    r.Cull->Bind();
    r.Cull->SetMat4("uViewProj", viewProj);
    r.Cull->SetVec4("uViewport", viewport);
    glUniform2i(r.Cull->Loc("uSize"), w, h);
    r.Cull->SetInt("uMaxLevel", r.Levels - 1);
    r.Cull->SetVec3("uEye", eye);
    r.Cull->SetFloat("uNearMargin", kNearMargin);
    glBindTextureUnit(40, r.Pyramid);
    r.Cull->DispatchCompute((unsigned)groups.size(), 1, 1);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    // TARTARUS_HIZ_STATS=1: how many copies the test let through, now and then (waits on the GPU; debugging only).
    static const bool stats = EnvSet("TARTARUS_HIZ_STATS", "1");
    static int frame = 0;
    if (stats && ++frame % 120 == 0) {
        std::uint32_t total = 0, kept = 0;
        for (const Group& g : groups) {
            total += g.Count;
            if (!g.CmdCount) continue;
            std::uint32_t n = 0;
            glGetNamedBufferSubData(indirectBuffer, (GLintptr)(cmdList[g.CmdFirst] * 5u + 1u) * 4, 4, &n);
            kept += n;
        }
        std::fprintf(stdout, "[HiZ] %u of %u copies drawn (%u groups)\n", kept, total, (unsigned)groups.size());
        std::fflush(stdout);
    }
    GLStateCache::Invalidate();
    return true;
}
