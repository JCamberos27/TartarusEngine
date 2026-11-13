#include "GrassRenderer.h"

#include "GLStateCache.h"
#include "Material.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"
#include "UploadStaging.h"
#include "Core/Env.h"
#include "Core/Profiler.h"
#include "Core/TimeService.h"
#include "Game/Components.h"
#include "Game/World.h"
#include "Game/TerrainSystem.h"
#include "TerrainGrid.h"
#include "GrassDensityMap.h"
#include "Physics/Frustum.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
// SSBO bindings (Grass.comp / .vert / .frag.glsl).
constexpr unsigned kAreaBinding = 30, kChunkBinding = 31, kBladeBinding = 32, kCmdBinding = 33, kTerrainBinding = 34, kMapBinding = 35;
constexpr std::size_t kBladeBytes = 3 * sizeof(glm::vec4); // GrassBlade: three vec4s
constexpr int kNearVerts = 2 * GrassRenderer::kNearSegments + 1;
constexpr int kFarVerts = 3;

// The two DrawArraysIndirect commands (count, instanceCount, first, baseInstance), counts zeroed for the cull.
constexpr std::uint32_t kCmdReset[8] = {(std::uint32_t)kNearVerts, 0u, 0u, 0u, (std::uint32_t)kFarVerts, 0u, 0u, 0u};

// Clamps the culled counts to the lists' capacities: the cull keeps counting past a full list so its atomics
// stay simple; the draw must not read past the end.
const char* kClampSrc = R"(#version 460 core
layout(local_size_x = 1) in;
layout(std430, binding = 33) buffer GrassCmd { uint uCmd[8]; };
uniform int uNearCap;
uniform int uFarCap;
void main() {
    uCmd[1] = min(uCmd[1], uint(uNearCap));
    uCmd[5] = min(uCmd[5], uint(uFarCap));
}
)";

float DistanceToBox(const glm::vec3& p, const glm::vec3& mn, const glm::vec3& mx) {
    const glm::vec3 d = glm::max(glm::max(mn - p, p - mx), glm::vec3(0.0f));
    return glm::length(d);
}
} // namespace

GrassRenderer& GrassRenderer::Get() {
    static GrassRenderer s_Instance;
    return s_Instance;
}

bool GrassRenderer::Disabled() {
    static const bool off = Env::Flag(Env::Var::NoGrass);
    return off;
}

int GrassRenderer::CellsPerChunk(float density) {
    if (!(density > 0.0f)) return 1;
    const float cell = 1.0f / std::sqrt(density);
    return std::clamp((int)std::lround(kChunkSize / cell), 1, 256);
}

float GrassRenderer::KeepFraction(float dist, float nearDist, float drawDist) {
    if (!(dist < drawDist)) return 0.0f;
    if (dist <= nearDist || drawDist <= nearDist) return 1.0f;
    const float s = std::clamp((dist - nearDist) / (drawDist - nearDist), 0.0f, 1.0f);
    // Falls fast just past the near range (where blades are still big on screen, thinning shows least once the
    // survivors widen) and levels out toward the draw distance.
    const float f = 1.0f - s;
    return kMinKeep + (1.0f - kMinKeep) * f * f;
}

GrassRenderer::AreaDesc GrassRenderer::Describe(const GrassAreaComponent& g, const glm::mat4& world) {
    AreaDesc d;
    const float density = std::clamp(g.Density, 1.0f, 2000.0f);
    const float cell = 1.0f / std::sqrt(density);
    glm::vec2 ax(world[0][0], world[0][2]), az(world[2][0], world[2][2]);
    ax = glm::dot(ax, ax) > 1e-10f ? glm::normalize(ax) : glm::vec2(1.0f, 0.0f);
    az = glm::dot(az, az) > 1e-10f ? glm::normalize(az) : glm::vec2(0.0f, 1.0f);
    AreaGpu& a = d.Gpu;
    a.Origin = glm::vec4(glm::vec3(world[3]), cell);
    a.Axes = glm::vec4(ax.x, ax.y, az.x, az.y);
    float seedBits;
    const std::int32_t seed = g.Seed;
    std::memcpy(&seedBits, &seed, sizeof(float));
    a.Size = glm::vec4(std::max(g.SizeX, 0.0f), std::max(g.SizeZ, 0.0f), (float)CellsPerChunk(density), seedBits);
    a.Shape = glm::vec4(std::max(g.Height, 0.0f), std::clamp(g.HeightVariation, 0.0f, 0.95f), std::max(g.Width, 0.0f),
                        std::clamp(g.Bend, 0.0f, 1.0f));
    a.Base = glm::vec4(g.BaseColor, std::clamp(g.ColorVariation, 0.0f, 1.0f));
    a.Tip = glm::vec4(g.TipColor, std::clamp(g.Patchiness, 0.0f, 1.0f));
    a.Dry = glm::vec4(g.DryColor, g.ReceiveShadows ? 1.0f : 0.0f);
    const float drawDist = std::max(g.DrawDistance, 1.0f);
    a.Lod = glm::vec4(std::clamp(g.NearDistance, 0.0f, drawDist), drawDist, std::max(g.EdgeFade, 0.0f),
                      std::max(g.WindStrength, 0.0f));
    a.Wind = glm::vec4(std::max(g.WindSpeed, 0.0f), g.FollowTerrain ? 1.0f : 0.0f, 0.0f, 0.0f);
    // Tallest: the top of the height spread, in the tallest patch, at a tall tuft's middle (Grass.comp: hScale).
    d.MaxHeight = a.Shape.x * (1.0f + a.Shape.y) * (1.0f + 0.5f * a.Tip.w) * 1.4f + 0.3f;
    // The density mask, decoded once per change (the component keeps it; a sampled key, not a full hash).
    if (!g.DensityMap.empty() && g.MapResX >= 2 && g.MapResZ >= 2) {
        std::uint64_t key = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            const auto* b = static_cast<const unsigned char*>(p);
            for (size_t i = 0; i < n; ++i) { key ^= b[i]; key *= 1099511628211ull; }
        };
        const size_t n = g.DensityMap.size();
        mix(&n, sizeof(n)); mix(&g.MapResX, sizeof(int)); mix(&g.MapResZ, sizeof(int));
        for (size_t i = 0; i < n; i += 61) mix(&g.DensityMap[i], 1);
        mix(g.DensityMap.data() + (n > 64 ? n - 64 : 0), n > 64 ? 64 : n);
        if (!g.MapCache || g.MapKey != key) {
            auto map = std::make_shared<GrassDensityMap>();
            g.MapCache = GrassDensityMap::Decode(g.MapResX, g.MapResZ, g.DensityMap, *map) ? map : nullptr;
            g.MapKey = key;
        }
        d.Map = g.MapCache;
    }
    a.Map = d.Map ? glm::vec4(0.0f, (float)d.Map->ResX, (float)d.Map->ResZ, 1.0f) : glm::vec4(0.0f);
    return d;
}

void GrassRenderer::SelectChunks(const std::vector<AreaDesc>& areas, const Frustum& frustum, const glm::vec3& eye,
                                 std::vector<ChunkGpu>& out, const TerrainRef* terrain) {
    out.clear();
    struct Pick { float Dist; ChunkGpu Chunk; };
    static std::vector<Pick> picks;
    picks.clear();
    for (int ai = 0; ai < (int)areas.size(); ++ai) {
        const AreaGpu& a = areas[ai].Gpu;
        const float cell = a.Origin.w;
        const int cpc = (int)a.Size.z;
        const float sx = a.Size.x, sz = a.Size.y;
        if (!(cell > 0.0f) || cpc <= 0 || sx <= 0.0f || sz <= 0.0f) continue;
        const int cellsX = (int)std::ceil(sx / cell), cellsZ = (int)std::ceil(sz / cell);
        const glm::vec3 o(a.Origin);
        const glm::vec2 ax(a.Axes.x, a.Axes.y), az(a.Axes.z, a.Axes.w);
        const float drawDist = a.Lod.y;
        const TerrainGrid* ground = (a.Wind.y > 0.5f && terrain && terrain->Grid) ? terrain->Grid.get() : nullptr;
        const GrassDensityMap* mask = areas[ai].Map.get();
        int bareCols = 0;
        const std::vector<std::uint8_t>* bare = mask ? &mask->BareChunks(sx, sz, cell, cpc, bareCols) : nullptr;
        // Whole area too far: skip its chunks without walking them.
        {
            const float r = 0.5f * std::sqrt(sx * sx + sz * sz);
            if (glm::length(glm::vec2(eye.x - o.x, eye.z - o.z)) - r > drawDist) continue;
        }
        for (int cz = 0; cz < cellsZ; cz += cpc) {
            for (int cx = 0; cx < cellsX; cx += cpc) {
                const float x0 = -0.5f * sx + cx * cell, z0 = -0.5f * sz + cz * cell;
                const float x1 = std::min(x0 + cpc * cell, 0.5f * sx), z1 = std::min(z0 + cpc * cell, 0.5f * sz);
                // A chunk the mask leaves bare (deep shade, under a log) isn't worth a workgroup.
                if (bare && (*bare)[(size_t)(cz / cpc) * bareCols + (size_t)(cx / cpc)]) continue;
                glm::vec3 mn(1e30f), mx(-1e30f);
                for (int k = 0; k < 4; ++k) {
                    const float lx = (k & 1) ? x1 : x0, lz = (k & 2) ? z1 : z0;
                    const glm::vec2 w = glm::vec2(o.x, o.z) + ax * lx + az * lz;
                    mn = glm::min(mn, glm::vec3(w.x, o.y - 0.1f, w.y));
                    mx = glm::max(mx, glm::vec3(w.x, o.y + areas[ai].MaxHeight, w.y));
                }
                if (ground) { // the blades stand on the terrain under the chunk's footprint
                    float lo, hi;
                    ground->RangeIn(mn.x - terrain->Origin.x, mn.z - terrain->Origin.z, mx.x - terrain->Origin.x,
                                    mx.z - terrain->Origin.z, lo, hi);
                    mn.y = terrain->Origin.y + lo - 0.1f;
                    mx.y = terrain->Origin.y + hi + areas[ai].MaxHeight;
                }
                // Blades lean and sway past their cell: pad sideways by the tallest one's reach.
                mn -= glm::vec3(areas[ai].MaxHeight, 0.0f, areas[ai].MaxHeight);
                mx += glm::vec3(areas[ai].MaxHeight, 0.0f, areas[ai].MaxHeight);
                const float d = DistanceToBox(eye, mn, mx);
                if (d > drawDist) continue;
                if (!frustum.Intersects(AABB{mn, mx})) continue;
                picks.push_back({d, {ai, cx, cz, cpc}});
            }
        }
    }
    if ((int)picks.size() > kMaxChunks) {
        std::nth_element(picks.begin(), picks.begin() + kMaxChunks, picks.end(),
                         [](const Pick& a, const Pick& b) { return a.Dist < b.Dist; });
        picks.resize(kMaxChunks);
    }
    out.reserve(picks.size());
    for (const Pick& p : picks) out.push_back(p.Chunk);
}

void GrassRenderer::Gather(const World& world) {
    const std::uint64_t frame = Time::FrameCount();
    if (frame == m_GatheredFrame) return;
    m_GatheredFrame = frame;
    m_Areas.clear();
    if (Disabled()) return;
    m_Terrain = TerrainRef{};
    bool follow = false;
    auto view = world.Registry.view<const TransformComponent, const GrassAreaComponent>();
    for (auto [entity, tc, g] : view.each()) {
        if (world.Registry.all_of<InactiveTag>(entity)) continue;
        if (!(g.Density > 0.0f) || !(g.Height > 0.0f) || !(g.SizeX > 0.0f) || !(g.SizeZ > 0.0f)) continue;
        m_Areas.push_back(Describe(g, world.GetCachedWorldTransform(entity)));
        follow = follow || g.FollowTerrain;
    }
    if (follow) m_Terrain.Grid = TerrainSystem::Find(world, m_Terrain.Origin, &m_Terrain.Key);
    // The masks, packed into one buffer (re-packed when the set changes); each area points at its first byte.
    std::uint64_t set = 0;
    for (const AreaDesc& a : m_Areas)
        if (a.Map) set = set * 1099511628211ull ^ (std::uint64_t)(std::uintptr_t)a.Map.get();
    if (set != m_MapSet) {
        m_MapSet = set;
        m_MapBytes.clear();
    }
    const bool repack = m_MapBytes.empty();
    std::vector<std::pair<const GrassDensityMap*, std::uint32_t>> placed;
    std::uint32_t next = 0;
    for (AreaDesc& a : m_Areas) {
        if (!a.Map) continue;
        std::uint32_t at = next;
        auto it = std::find_if(placed.begin(), placed.end(), [&](const auto& p) { return p.first == a.Map.get(); });
        if (it != placed.end()) at = it->second;
        else {
            placed.emplace_back(a.Map.get(), at);
            const std::uint32_t n = (std::uint32_t)a.Map->Values.size();
            if (repack) {
                m_MapBytes.resize((size_t)(at + n + 3) / 4, 0u);
                for (std::uint32_t k = 0; k < n; ++k)
                    m_MapBytes[(at + k) >> 2] |= (std::uint32_t)a.Map->Values[k] << (((at + k) & 3u) * 8u);
            }
            next = (at + n + 3u) & ~3u;
        }
        a.Gpu.Map.x = (float)at;
    }
}

void GrassRenderer::EnsurePrograms() {
    if (m_Draw) return;
    m_Draw = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("Grass.vert.glsl"),
                                      ShaderLibrary::ReadFileRequired("Grass.frag.glsl"), "Grass");
    ShaderLibrary::RegisterForHotReload(*m_Draw, "Grass.vert.glsl", "Grass.frag.glsl");
    m_Cull = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("Grass.comp.glsl"), "GrassCull");
    m_Clamp = std::make_unique<Shader>(std::string(kClampSrc), "GrassClamp");
}

void GrassRenderer::EnsureBuffers() {
    if (m_CmdBuf) return;
    glGenVertexArrays(1, &m_Vao); // core GL draws nothing without one bound; the blades read no attributes
    glCreateBuffers(1, &m_AreaBuf);
    glCreateBuffers(1, &m_ChunkBuf);
    glCreateBuffers(1, &m_BladeBuf);
    glCreateBuffers(1, &m_CmdBuf);
    glNamedBufferData(m_BladeBuf, (GLsizeiptr)((std::size_t)(kNearCap + kFarCap) * kBladeBytes), nullptr, GL_DYNAMIC_DRAW);
    glNamedBufferData(m_CmdBuf, (GLsizeiptr)sizeof(kCmdReset), kCmdReset, GL_DYNAMIC_DRAW);
}

void GrassRenderer::BufferRange::Bind(unsigned binding) const {
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, binding, Buffer, (GLintptr)Offset, (GLsizeiptr)Size);
}

std::size_t GrassRenderer::TakeUpload(std::size_t size) {
    constexpr std::size_t kNone = ~std::size_t(0);
    constexpr std::size_t kUploadBytes = 8ull << 20;  // ~0.4 MB a frame in the Forest (3 culls)
    constexpr std::uint64_t kUploadSafeFrames = 16;   // SwapBuffers keeps the CPU a few frames ahead at most
    if (size == 0 || size > kUploadBytes / 4 || m_UploadFailed) return kNone;
    if (!m_UploadBase) {
        GLint align = 0;
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
        if (align <= 0 || 256 % align != 0) { m_UploadFailed = true; return kNone; } // RingFit places at multiples of 256
        const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glCreateBuffers(1, &m_UploadBuf);
        glNamedBufferStorage(m_UploadBuf, (GLsizeiptr)kUploadBytes, nullptr, flags);
        m_UploadBase = static_cast<unsigned char*>(glMapNamedBufferRange(m_UploadBuf, 0, (GLsizeiptr)kUploadBytes, flags));
        if (!m_UploadBase) {
            glDeleteBuffers(1, &m_UploadBuf);
            m_UploadBuf = 0;
            m_UploadFailed = true;
            return kNone;
        }
    }
    const std::uint64_t frame = Time::FrameCount();
    std::size_t old = 0;
    while (old < m_UploadSpans.size() && frame - m_UploadSpans[old].Frame > kUploadSafeFrames) ++old;
    m_UploadSpans.erase(m_UploadSpans.begin(), m_UploadSpans.begin() + (std::ptrdiff_t)old);
    const bool empty = m_UploadSpans.empty();
    const std::size_t tail = empty ? m_UploadHead : m_UploadSpans.front().Start;
    const std::size_t at = UploadStaging::RingFit(kUploadBytes, m_UploadHead, tail, empty, size);
    if (at == kNone) return kNone;
    m_UploadSpans.push_back({at, frame});
    m_UploadHead = at + size;
    return at;
}

bool GrassRenderer::Cull(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye, float maxDistance) {
    m_Culled = false;
    if (m_Areas.empty() || Disabled()) return false;
    const glm::mat4 viewProj = proj * view;
    const Frustum frustum = Frustum::FromViewProj(viewProj);
    // A shorter reach (the shadow pass): every area's draw distance capped to it, its near range inside that.
    static std::vector<AreaDesc> capped;
    const std::vector<AreaDesc>* use = &m_Areas;
    if (maxDistance < 1e29f) {
        capped = m_Areas;
        for (AreaDesc& a : capped) {
            a.Gpu.Lod.y = std::min(a.Gpu.Lod.y, std::max(maxDistance, 1.0f));
            a.Gpu.Lod.x = std::min(a.Gpu.Lod.x, a.Gpu.Lod.y);
        }
        use = &capped;
    }
    {
        PROFILE_SCOPE("Grass Chunks");
        SelectChunks(*use, frustum, eye, m_Chunks, &m_Terrain);
    }
    if (m_Chunks.empty()) return false;
    EnsurePrograms();
    EnsureBuffers();
    PROFILE_GPU_SCOPE("Grass Cull");
    static std::vector<AreaGpu> areas;
    areas.clear();
    for (const AreaDesc& a : *use) areas.push_back(a.Gpu);
    const std::size_t areaBytes = areas.size() * sizeof(AreaGpu), chunkBytes = m_Chunks.size() * sizeof(ChunkGpu);
    const std::size_t chunkAt = (areaBytes + 255) & ~std::size_t(255);
    const std::size_t at = TakeUpload(chunkAt + chunkBytes);
    if (at != ~std::size_t(0)) {
        std::memcpy(m_UploadBase + at, areas.data(), areaBytes);
        std::memcpy(m_UploadBase + at + chunkAt, m_Chunks.data(), chunkBytes);
        m_AreaBind = {m_UploadBuf, at, areaBytes};
        m_ChunkBind = {m_UploadBuf, at + chunkAt, chunkBytes};
    } else { // the ring is full or unavailable: a fresh data store, as before
        glNamedBufferData(m_AreaBuf, (GLsizeiptr)areaBytes, areas.data(), GL_STREAM_DRAW);
        glNamedBufferData(m_ChunkBuf, (GLsizeiptr)chunkBytes, m_Chunks.data(), GL_STREAM_DRAW);
        m_AreaBind = {m_AreaBuf, 0, areaBytes};
        m_ChunkBind = {m_ChunkBuf, 0, chunkBytes};
    }
    glNamedBufferSubData(m_CmdBuf, 0, (GLsizeiptr)sizeof(kCmdReset), kCmdReset);
    const TerrainGrid* ground = m_Terrain.Grid.get();
    if (ground && m_TerrainUploaded != m_Terrain.Key) { // the Follow Terrain areas' heights, once per terrain
        if (!m_TerrainBuf) glCreateBuffers(1, &m_TerrainBuf);
        glNamedBufferData(m_TerrainBuf, (GLsizeiptr)(ground->Heights.size() * sizeof(float)), ground->Heights.data(), GL_STATIC_DRAW);
        m_TerrainUploaded = m_Terrain.Key;
    }
    if (ground) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kTerrainBinding, m_TerrainBuf);
    if (!m_MapBytes.empty()) {
        if (m_MapUploaded != m_MapSet || !m_MapBuf) {
            if (!m_MapBuf) glCreateBuffers(1, &m_MapBuf);
            glNamedBufferData(m_MapBuf, (GLsizeiptr)(m_MapBytes.size() * sizeof(std::uint32_t)), m_MapBytes.data(), GL_STATIC_DRAW);
            m_MapUploaded = m_MapSet;
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kMapBinding, m_MapBuf);
    }
    m_AreaBind.Bind(kAreaBinding);
    m_ChunkBind.Bind(kChunkBinding);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBladeBinding, m_BladeBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kCmdBinding, m_CmdBuf);

    Shader& cull = *m_Cull;
    cull.Bind();
    static const char* kPlaneNames[6] = {"uFrustum[0]", "uFrustum[1]", "uFrustum[2]", "uFrustum[3]", "uFrustum[4]", "uFrustum[5]"};
    for (int i = 0; i < 6; ++i) cull.SetVec4(kPlaneNames[i], glm::vec4(frustum.Planes[i].Normal, frustum.Planes[i].D));
    cull.SetVec3("uEye", eye);
    cull.SetInt("uHasTerrain", ground ? 1 : 0);
    if (ground) {
        cull.SetVec4("uTerrainRect", glm::vec4(m_Terrain.Origin.x - 0.5f * ground->SizeX, m_Terrain.Origin.z - 0.5f * ground->SizeZ,
                                               ground->CellX(), ground->CellZ()));
        cull.SetVec3("uTerrainRes", glm::vec3((float)ground->ResX, (float)ground->ResZ, m_Terrain.Origin.y));
    }
    cull.SetInt("uNearCap", (int)kNearCap);
    cull.SetInt("uFarCap", (int)kFarCap);
    cull.DispatchCompute((unsigned)m_Chunks.size(), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    Shader& clamp = *m_Clamp;
    clamp.Bind();
    clamp.SetInt("uNearCap", (int)kNearCap);
    clamp.SetInt("uFarCap", (int)kFarCap);
    clamp.DispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    GLStateCache::Invalidate();
    m_Culled = true;
    return true;
}

void GrassRenderer::Draw(Shader& prog, const glm::mat4& view, const glm::mat4& proj, int nearSegments) {
    prog.SetMat4("uView", view);
    prog.SetMat4("uProj", proj);
    prog.SetFloat("uWindTime", Material::WindTime());
    prog.SetFloat("uPrevWindTime", Material::PrevWindTime());
    prog.SetVec2("uWindDir", Material::WindDirection());
    m_AreaBind.Bind(kAreaBinding);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBladeBinding, m_BladeBuf);
    glBindVertexArray(m_Vao);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_CmdBuf);
    glDisable(GL_CULL_FACE); // blades are seen from both sides
    prog.SetInt("uInstanceBase", 0);
    prog.SetInt("uSegments", nearSegments);
    glDrawArraysIndirect(GL_TRIANGLE_STRIP, (const void*)0);
    prog.SetInt("uInstanceBase", (int)kNearCap);
    prog.SetInt("uSegments", 1);
    glDrawArraysIndirect(GL_TRIANGLE_STRIP, (const void*)(4 * sizeof(std::uint32_t)));
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    GLStateCache::Invalidate();
}

void GrassRenderer::DrawDepth(const glm::mat4& view, const glm::mat4& proj) {
    if (!m_Culled) return;
    PROFILE_GPU_SCOPE("Grass Depth");
    Shader& prog = *m_Draw;
    prog.Bind();
    prog.SetInt("uDepthOnly", 1);
    prog.SetInt("uTaaVelocity", 0);
    Draw(prog, view, proj);
}

void GrassRenderer::DrawShadow(const glm::mat4& lightViewProj, int nearSegments) {
    if (!m_Culled) return;
    nearSegments = std::clamp(nearSegments, 1, kNearSegments);
    Shader& prog = *m_Draw;
    prog.Bind();
    prog.SetInt("uDepthOnly", 1);
    prog.SetInt("uTaaVelocity", 0);
    // The near command's vertex count to the coarser strip (the next Cull resets it).
    const std::uint32_t verts = (std::uint32_t)(2 * nearSegments + 1);
    glNamedBufferSubData(m_CmdBuf, 0, (GLsizeiptr)sizeof(verts), &verts);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT);
    Draw(prog, glm::mat4(1.0f), lightViewProj, nearSegments);
}

void GrassRenderer::DrawLit(const glm::mat4& view, const glm::mat4& proj, bool depthWrites,
                            const std::function<void(Shader&)>& applyFrameState) {
    if (!m_Culled) return;
    PROFILE_GPU_SCOPE("Grass");
    Shader& prog = *m_Draw;
    applyFrameState(prog);
    prog.Bind();
    prog.SetInt("uDepthOnly", 0);
    prog.SetInt("uObjectLayerBit", 1);
    prog.SetInt("uNoReceiveShadows", 0);
    GLint prevDepthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(depthWrites ? GL_TRUE : GL_FALSE);
    glDisable(GL_BLEND);
    Draw(prog, view, proj);
    glDepthMask(GL_TRUE);
    glDepthFunc((GLenum)prevDepthFunc);
    GLStateCache::Invalidate();
}
