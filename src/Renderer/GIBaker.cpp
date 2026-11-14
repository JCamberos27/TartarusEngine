#include "GIBaker.h"

#include "GIVolumes.h"
#include "HdrTarget.h"
#include "ReflectionProbeCapture.h"
#include "RenderFrameContext.h"
#include "GLStateCache.h"
#include "gl.h"
#include "Log.h"
#include "Core/Paths.h"
#include "../Game/World.h"
#include "../Game/Components.h"
#include "../Game/LightSwitch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>

namespace {
double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
std::string Fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), f, a, b, c);
    return buf;
}
} // namespace

GIBaker::GIBaker() = default;
GIBaker::~GIBaker() { if (m_Atlas) glDeleteTextures(1, &m_Atlas); }

GI::ProbeSH GIBaker::ProjectFaces(const float* rgba, int n, float* backfaceShare) {
    GI::Projector p;
    float back = 0.0f, total = 0.0f;
    for (int f = 0; f < 6; ++f)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const float* t = rgba + (((std::size_t)f * n + y) * n + x) * 4;
                const float sa = GI::CubeTexelSolidAngle(x, y, n);
                total += sa;
                glm::vec3 L(t[0], t[1], t[2]);
                if (t[3] < 0.5f) { back += sa; L = glm::vec3(0.0f); }
                if (!std::isfinite(L.x) || !std::isfinite(L.y) || !std::isfinite(L.z)) L = glm::vec3(0.0f);
                p.Add(GI::CubeTexelDir(f, x, y, n), glm::max(L, glm::vec3(0.0f)), sa);
            }
    if (backfaceShare) *backfaceShare = total > 0.0f ? back / total : 0.0f;
    return p.Finish();
}

int GIBaker::ProbeCount() const {
    int n = 0;
    for (const auto& v : m_Volumes) n += v.Dims.x * v.Dims.y * v.Dims.z;
    return n;
}

bool GIBaker::Begin(World& world, const Options& opt) {
    m_Opt = opt;
    m_Volumes.clear();
    m_Passes.clear();
    m_Bakes.clear();
    m_Error.clear();
    m_Finished = false;
    m_Pass = -1;
    m_Vol = m_Probe = m_Settle = 0;
    m_Done = m_Total = 0;

    auto view = world.Registry.view<const TransformComponent, const GIVolumeComponent>();
    for (auto [e, tc, gv] : view.each()) {
        if (world.Registry.any_of<InactiveTag, DeactivatedTag>(e)) continue;
        const auto* nc = world.Registry.try_get<NameComponent>(e);
        VolumeJob j;
        j.Entity = e;
        j.Name = nc ? nc->Name : std::string();
        if (!opt.Only.empty() && j.Name.find(opt.Only) == std::string::npos) continue;
        if (j.Name.empty()) { Log::Warn("[GI bake] a GI Volume with no name is skipped (its data is found by name)"); continue; }
        if (gv.BakeFile.empty()) { Log::Warn("[GI bake] '" + j.Name + "' has no Bake File: skipped"); continue; }
        j.BakeFile = gv.BakeFile;
        j.Bounds = GI::Box::FromTransform(world.GetCachedWorldTransform(e), gv.Size);
        j.Dims = GI::GridDims(j.Bounds.Half, gv.Spacing);
        j.Layers = GI::SplitCircuits(gv.Circuits);
        if (gv.Outside) j.Layers.push_back("");
        if ((int)j.Layers.size() > GIVolumes::kMaxLayers) {
            Log::Warn("[GI bake] '" + j.Name + "' has more than " + std::to_string(GIVolumes::kMaxLayers) +
                      " layers: the rest are dropped");
            j.Layers.resize(GIVolumes::kMaxLayers);
        }
        if (j.Layers.empty()) { Log::Warn("[GI bake] '" + j.Name + "' has no circuits and no Outside: skipped"); continue; }
        j.Valid.assign((std::size_t)j.Dims.x * j.Dims.y * j.Dims.z, true);
        m_Volumes.push_back(std::move(j));
    }
    if (m_Volumes.empty()) { m_Error = "no GI Volume to bake"; return false; }

    // Passes: each circuit (the Outside last), each bounce.
    std::vector<std::string> circuits;
    bool outside = false;
    for (const auto& v : m_Volumes)
        for (const auto& c : v.Layers) {
            if (c.empty()) outside = true;
            else if (std::find(circuits.begin(), circuits.end(), c) == circuits.end()) circuits.push_back(c);
        }
    if (outside) circuits.push_back("");
    const int bounces = std::clamp(opt.Bounces, 1, 4);
    for (const auto& c : circuits)
        for (int b = 0; b < bounces; ++b) m_Passes.push_back({c, b});

    for (int b = 0; b < bounces; ++b) {
        auto file = std::make_shared<GI::BakeFile>();
        for (const auto& v : m_Volumes) {
            GI::VolumeBake vb;
            vb.Name = v.Name;
            vb.Bounds = v.Bounds;
            vb.Dims = v.Dims;
            for (const auto& c : v.Layers) vb.Layers.push_back({c, std::vector<GI::ProbeSH>(v.Valid.size())});
            file->Volumes.push_back(std::move(vb));
        }
        m_Bakes.push_back(file);
    }
    for (const auto& p : m_Passes)
        for (const auto& v : m_Volumes)
            if (std::find(v.Layers.begin(), v.Layers.end(), p.Circuit) != v.Layers.end()) m_Total += (long long)v.Valid.size();

    m_ClipMin = glm::vec3(1e30f);
    m_ClipMax = glm::vec3(-1e30f);
    for (const auto& v : m_Volumes)
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 q((c & 1) ? v.Bounds.Half.x : -v.Bounds.Half.x, (c & 2) ? v.Bounds.Half.y : -v.Bounds.Half.y,
                              (c & 4) ? v.Bounds.Half.z : -v.Bounds.Half.z);
            m_ClipMin = glm::min(m_ClipMin, v.Bounds.ToWorld(q));
            m_ClipMax = glm::max(m_ClipMax, v.Bounds.ToWorld(q));
        }
    m_ClipMin -= glm::vec3(opt.ClipMargin);
    m_ClipMax += glm::vec3(opt.ClipMargin);
    m_Target = std::make_unique<HdrTarget>();
    m_Faces.assign((std::size_t)opt.FaceSize * opt.FaceSize * 4 * 6, 0.0f);
    m_StartTime = Now();
    std::string names;
    for (const auto& v : m_Volumes)
        names += (names.empty() ? "" : ", ") + v.Name + " (" + std::to_string(v.Dims.x) + "x" + std::to_string(v.Dims.y) +
                 "x" + std::to_string(v.Dims.z) + ", " + std::to_string(v.Layers.size()) + " layers)";
    Log::Info("[GI bake] " + std::to_string(m_Volumes.size()) + " volumes: " + names + "; " +
              std::to_string(m_Passes.size()) + " passes, " + std::to_string(m_Total) + " probe captures");
    return true;
}

float GIBaker::Progress() const { return m_Total > 0 ? (float)((double)m_Done / (double)m_Total) : 1.0f; }

void GIBaker::ApplyLighting(World& world, const Pass& p) {
    if (!m_LightingSaved) {
        m_SkyAmbient = world.SkyAmbientIntensity;
        m_LightingSaved = true;
    }
    for (entt::entity e : m_Parked)
        if (world.Registry.valid(e)) world.Registry.remove<DeactivatedTag>(e);
    m_Parked.clear();
    // Every circuit off, then this one on (its lights, its fittings' glow).
    LightSwitch::SetAll(world, false);
    if (!p.Circuit.empty()) LightSwitch::Set(world, p.Circuit, true);
    // Lights on no circuit (the sun, the moon, street lamps) shine only in the Outside pass; the hand-placed bounce fakes
    // never do.
    auto lights = world.Registry.view<const LightComponent>();
    for (auto e : lights) {
        if (world.Registry.all_of<DeactivatedTag>(e)) continue;
        const auto* nc = world.Registry.try_get<NameComponent>(e);
        const bool fake = nc && !m_Opt.SkipLightPrefix.empty() && nc->Name.rfind(m_Opt.SkipLightPrefix, 0) == 0;
        const bool onCircuit = world.Registry.all_of<LightCircuitComponent>(e);
        if (fake || (!onCircuit && !p.Circuit.empty())) {
            world.Registry.emplace<DeactivatedTag>(e);
            m_Parked.push_back(e);
        }
    }
    world.SyncActiveInHierarchy();
    m_Settle = m_Opt.SettleFrames;
}

void GIBaker::RestoreLighting(World& world) {
    for (entt::entity e : m_Parked)
        if (world.Registry.valid(e)) world.Registry.remove<DeactivatedTag>(e);
    m_Parked.clear();
    LightSwitch::SetAll(world, true);
    if (m_LightingSaved) world.SkyAmbientIntensity = m_SkyAmbient;
    m_LightingSaved = false;
    world.SyncActiveInHierarchy();
}

bool GIBaker::Step(World& world, GIVolumes& gi, const std::function<void(const RenderFrameContext&)>& draw,
                   const std::function<void(const glm::vec3&)>& moveCamera) {
    if (m_Finished) return true;
    if (!m_Error.empty()) { m_Finished = true; return true; }

    auto hasLayer = [&](const VolumeJob& v, const std::string& c) {
        return std::find(v.Layers.begin(), v.Layers.end(), c) != v.Layers.end();
    };
    auto startPass = [&](int i) {
        m_Pass = i;
        m_Vol = 0;
        m_Probe = 0;
        const Pass& p = m_Passes[i];
        while (m_Vol < (int)m_Volumes.size() && !hasLayer(m_Volumes[m_Vol], p.Circuit)) ++m_Vol;
        // Bounce one sees direct light only; later bounces see the bounce before them, this circuit's layers alone.
        if (p.Bounce == 0) {
            gi.SetEnabled(false);
        } else {
            gi.SetOverride(m_Bakes[p.Bounce - 1]);
            gi.SetSolo(&p.Circuit);
            gi.SetEnabled(true);
        }
        ApplyLighting(world, p);
        Log::Info("[GI bake] pass " + std::to_string(i + 1) + "/" + std::to_string(m_Passes.size()) + ": " +
                  (p.Circuit.empty() ? std::string("Outside") : p.Circuit) + ", bounce " + std::to_string(p.Bounce + 1));
    };
    if (m_Pass < 0) startPass(0);

    if (m_Vol < (int)m_Volumes.size()) moveCamera(m_Volumes[m_Vol].Bounds.Center);
    if (m_Settle > 0) { --m_Settle; return false; }

    const Pass& pass = m_Passes[m_Pass];
    const int n = m_Opt.FaceSize;
    m_Target->Resize(n, n, 1);
    if (!m_Target->IsValid()) { m_Error = "could not make the capture target"; return false; }

    const bool cullWas = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    // This frame's probes, planned first: every face renders into its own spot of one atlas texture (GPU copies, no
    // waiting), and the atlas comes back in ONE read at the end - a read-back per face stalled on the GPU every time.
    struct Job { int Vol, Probe; };
    std::vector<Job> jobs;
    {
        int vi = m_Vol, pi = m_Probe;
        while ((int)jobs.size() < std::max(m_Opt.ProbesPerFrame, 1) && vi < (int)m_Volumes.size()) {
            jobs.push_back({vi, pi});
            if (++pi >= (int)m_Volumes[vi].Valid.size()) {
                pi = 0;
                do ++vi; while (vi < (int)m_Volumes.size() && !hasLayer(m_Volumes[vi], pass.Circuit));
            }
        }
    }
    const int atlasW = 6 * n, atlasH = n * (int)jobs.size();
    if (m_Atlas && (m_AtlasW != atlasW || m_AtlasH < atlasH)) { glDeleteTextures(1, &m_Atlas); m_Atlas = 0; }
    if (!m_Atlas) {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_Atlas);
        glTextureStorage2D(m_Atlas, 1, GL_RGBA16F, atlasW, std::max(atlasH, n));
        m_AtlasW = atlasW;
        m_AtlasH = std::max(atlasH, n);
    }
    for (std::size_t k = 0; k < jobs.size(); ++k) {
        const VolumeJob& v = m_Volumes[jobs[k].Vol];
        const int pr = jobs[k].Probe;
        const glm::ivec3 ijk(pr % v.Dims.x, (pr / v.Dims.x) % v.Dims.y, pr / (v.Dims.x * v.Dims.y));
        const glm::vec3 pos = v.Bounds.ToWorld(GI::ProbeLocal(ijk, v.Dims, v.Bounds.Half));
        for (int f = 0; f < 6; ++f) {
            m_Target->BindForRender();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE); // back faces must show: they are the inside-a-wall test
            RenderFrameContext ctx;
            ctx.View = ReflectionProbeCapture::FaceView(pos, f);
            ctx.Proj = ReflectionProbeCapture::FaceProj();
            ctx.ViewPos = pos;
            ctx.TxHdr = m_Target.get();
            ctx.ProbeCapture = true;
            ctx.GIBake = pass.Circuit.empty() ? 2 : 1;
            ctx.GIBakeEmissive = m_Opt.Emissive;
            ctx.GIBakeClipMin = m_ClipMin;
            ctx.GIBakeClipMax = m_ClipMax;
            draw(ctx);
            m_Target->ResolveTo();
            glCopyImageSubData(m_Target->ResolvedColorTexture(), GL_TEXTURE_2D, 0, 0, 0, 0, m_Atlas, GL_TEXTURE_2D, 0,
                               f * n, (int)k * n, 0, n, n, 1);
        }
    }
    cullWas ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLStateCache::Invalidate();
    m_AtlasPixels.resize((std::size_t)atlasW * atlasH * 4);
    if (atlasH > 0)
        glGetTextureSubImage(m_Atlas, 0, 0, 0, 0, atlasW, atlasH, 1, GL_RGBA, GL_FLOAT,
                             (GLsizei)(m_AtlasPixels.size() * sizeof(float)), m_AtlasPixels.data());

    for (std::size_t k = 0; k < jobs.size(); ++k) {
        VolumeJob& v = m_Volumes[m_Vol];
        // the six faces of probe k, face-major, out of the atlas rows
        for (int f = 0; f < 6; ++f)
            for (int y = 0; y < n; ++y)
                std::copy_n(m_AtlasPixels.data() + (((std::size_t)k * n + y) * atlasW + (std::size_t)f * n) * 4, (std::size_t)n * 4,
                            m_Faces.data() + (((std::size_t)f * n + y) * n) * 4);

        float back = 0.0f;
        const GI::ProbeSH sh = ProjectFaces(m_Faces.data(), n, &back);
        if (!v.ValidKnown) v.Valid[m_Probe] = back <= m_Opt.InvalidBackfaces;
        GI::VolumeBake& vb = m_Bakes[pass.Bounce]->Volumes[m_Vol];
        for (auto& l : vb.Layers)
            if (l.Circuit == pass.Circuit) l.Probes[m_Probe] = sh;
        ++m_Done;

        if (++m_Probe >= (int)v.Valid.size()) { // this volume's layer is done: fill its invalid probes
            v.ValidKnown = true;
            for (auto& l : vb.Layers)
                if (l.Circuit == pass.Circuit) GI::FillInvalid(l.Probes, v.Valid, v.Dims);
            m_Probe = 0;
            do ++m_Vol; while (m_Vol < (int)m_Volumes.size() && !hasLayer(m_Volumes[m_Vol], pass.Circuit));
        }
    }

    static double lastLog = 0.0;
    if (Now() - lastLog > 5.0) {
        lastLog = Now();
        Log::Info(Fmt("[GI bake] %.0f%% (%.0f s)", 100.0 * Progress(), Now() - m_StartTime));
    }

    if (m_Vol < (int)m_Volumes.size()) return false;
    if (m_Pass + 1 < (int)m_Passes.size()) {
        startPass(m_Pass + 1);
        return false;
    }
    m_Finished = true;
    return Finish(world, gi), true;
}

bool GIBaker::Finish(World& world, GIVolumes& gi) {
    RestoreLighting(world);
    gi.SetOverride(nullptr);
    gi.SetSolo(nullptr);
    gi.SetEnabled(true);

    const GI::BakeFile& last = *m_Bakes.back();
    // One write per file: what it held for volumes not baked this time is kept.
    std::map<std::string, std::vector<int>> byFile;
    for (int i = 0; i < (int)m_Volumes.size(); ++i) byFile[m_Volumes[i].BakeFile].push_back(i);
    for (const auto& [rel, idx] : byFile) {
        const std::string path = ProjectPaths::Resolve(rel);
        GI::BakeFile out;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::string err;
            if (!GI::Load(out, path, &err)) Log::Warn("[GI bake] " + rel + " was unreadable (" + err + "): rewritten");
        }
        for (int i : idx) {
            const GI::VolumeBake& vb = last.Volumes[i];
            auto it = std::find_if(out.Volumes.begin(), out.Volumes.end(), [&](const GI::VolumeBake& o) { return o.Name == vb.Name; });
            if (it != out.Volumes.end()) *it = vb;
            else out.Volumes.push_back(vb);
        }
        std::string err;
        if (!GI::Save(out, path, &err)) {
            m_Error = "could not write " + rel + ": " + err;
            Log::Error("[GI bake] " + m_Error);
            return false;
        }
        Log::Info("[GI bake] wrote " + rel + " (" + std::to_string(out.Volumes.size()) + " volumes, " +
                  std::to_string(std::filesystem::file_size(path, ec) / 1024) + " KB)");
    }
    // Sanity: a layer far dimmer than the volume's brightest (a neighbour's circuit behind a wall) is wasted bake time;
    // a volume mostly inside geometry is a box that's wrong. Both are flagged with "[GI check]".
    int flags = 0;
    for (int i = 0; i < (int)m_Volumes.size(); ++i) {
        const auto& v = m_Volumes[i];
        const auto& vb = last.Volumes[i];
        float best = 0.0f;
        std::vector<float> peak(vb.Layers.size(), 0.0f);
        for (std::size_t l = 0; l < vb.Layers.size(); ++l)
            for (const auto& p : vb.Layers[l].Probes) {
                const float dc = std::max(p.c[0].x, std::max(p.c[1].x, p.c[2].x));
                if (!std::isfinite(dc)) { Log::Warn("[GI check] " + v.Name + ": non-finite probe"); ++flags; break; }
                peak[l] = std::max(peak[l], dc);
            }
        for (float pk : peak) best = std::max(best, pk);
        for (std::size_t l = 0; l < vb.Layers.size(); ++l) {
            const std::string ln = vb.Layers[l].Circuit.empty() ? std::string("Outside") : vb.Layers[l].Circuit;
            // The Outside is flagged only when nothing at all came in: at night it is faint beside a lamp, but it is
            // all there is with the lamps off.
            const bool outside = vb.Layers[l].Circuit.empty();
            if (outside ? peak[l] <= 0.0f : (peak[l] < 0.02f * best || peak[l] <= 0.0f)) {
                Log::Warn("[GI check] " + v.Name + ": layer '" + ln + "' is empty (" + Fmt("%.5f", peak[l]) + " vs " +
                          Fmt("%.5f", best) + ") - drop it");
                ++flags;
            }
        }
        // Inside geometry, counted over the probes off the box's faces: the face probes sit in the walls by design.
        int inner = 0, innerBad = 0;
        for (int z = 1; z < v.Dims.z - 1; ++z)
            for (int y = 1; y < v.Dims.y - 1; ++y)
                for (int x = 1; x < v.Dims.x - 1; ++x) {
                    ++inner;
                    innerBad += v.Valid[GI::ProbeIndex({x, y, z}, v.Dims)] ? 0 : 1;
                }
        const float inv = inner ? (float)innerBad / (float)inner : 0.0f;
        if (inv > 0.5f) {
            Log::Warn("[GI check] " + v.Name + Fmt(": %.0f%% of its inner probes inside geometry - check its box", 100.0 * inv));
            ++flags;
        }
    }
    Log::Info("[GI check] " + std::to_string(flags) + " flag(s)");
    for (int i = 0; i < (int)m_Volumes.size(); ++i) {
        const auto& v = m_Volumes[i];
        const int invalid = (int)std::count(v.Valid.begin(), v.Valid.end(), false);
        glm::vec3 avg(0.0f);
        for (const auto& l : last.Volumes[i].Layers)
            for (const auto& p : l.Probes) avg += glm::vec3(p.c[0].x, p.c[1].x, p.c[2].x);
        avg /= (float)std::max<std::size_t>(v.Valid.size(), 1);
        Log::Info("[GI bake] " + v.Name + ": " + std::to_string(v.Valid.size()) + " probes, " + std::to_string(invalid) +
                  " inside geometry (filled); mean irradiance (all layers) " + Fmt("%.4f %.4f %.4f", avg.r, avg.g, avg.b));
    }
    Log::Info(Fmt("[GI bake] done in %.1f s", Now() - m_StartTime));
    return true;
}
