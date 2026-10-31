#include "GpuMemTrack.h"
#include "ReflectionProbeCapture.h"
#include "HdrTarget.h"
#include "IblProbe.h"
#include "ReflectionProbeArray.h"
#include "RenderFrameContext.h"
#include "GLStateCache.h"
#include "gl.h"
#include "Log.h"
#include <cstdio>
#include <string>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>

namespace {
std::atomic<bool> s_RecaptureAll{false};

bool SameVolume(const ReflectionProbeCapture::Volume& a, const ReflectionProbeCapture::Volume& b) {
    const glm::vec3 dc = glm::abs(a.Center - b.Center), dh = glm::abs(a.HalfSize - b.HalfSize);
    return std::max({dc.x, dc.y, dc.z, dh.x, dh.y, dh.z}) < 1e-3f;
}
bool SameVolumes(const std::vector<ReflectionProbeCapture::Volume>& a,
                 const std::vector<ReflectionProbeCapture::Volume>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!SameVolume(a[i], b[i])) return false;
    return true;
}
} // namespace

ReflectionProbeCapture::ReflectionProbeCapture() : m_Target(std::make_unique<HdrTarget>()) {}
ReflectionProbeCapture::~ReflectionProbeCapture() { Release(); }

void ReflectionProbeCapture::Release() {
    if (m_Array) { glDeleteTextures(1, &m_Array); m_Array = 0; }
    for (unsigned int& c : m_CaptureCube)
        if (c) { glDeleteTextures(1, &c); c = 0; }
    m_ArrayLayers = 0;
    m_Slots.clear();
    m_Queue.clear();
    m_Pending.clear();
    m_Cap = {};
    m_Pre = {};
}

void ReflectionProbeCapture::RequestRecapture() { s_RecaptureAll = true; }

bool ReflectionProbeCapture::Busy() const { return m_Cap.Slot >= 0 || m_Pre.Slot >= 0 || !m_Queue.empty(); }

int ReflectionProbeCapture::ReadyCount() const {
    int n = 0;
    for (const Slot& s : m_Slots) n += s.Ready ? 1 : 0;
    return n;
}

glm::mat4 ReflectionProbeCapture::FaceView(const glm::vec3& pos, int face) {
    // GL cube faces with the up vectors that make a plain render land in the face as the sampler reads it (the
    // face's t axis runs down world -Y on the four sides). Each is a pure rotation, so winding and culling hold.
    static const glm::vec3 kDir[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 kUp[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    face = std::clamp(face, 0, 5);
    return glm::lookAt(pos, pos + kDir[face], kUp[face]);
}

glm::mat4 ReflectionProbeCapture::FaceProj() { return glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, 1000.0f); }

std::vector<bool> ReflectionProbeCapture::Changed(const std::vector<Volume>& have, const std::vector<Volume>& want) {
    std::vector<bool> out(want.size(), true);
    for (std::size_t i = 0; i < want.size() && i < have.size(); ++i) out[i] = !SameVolume(have[i], want[i]);
    return out;
}

void ReflectionProbeCapture::EnsureArray(int layers) {
    if (m_Array && layers <= m_ArrayLayers) return;
    if (m_Array) glDeleteTextures(1, &m_Array);
    m_ArrayLayers = std::max(4, (layers + 3) / 4 * 4); // room to add a few probes without a reallocation
    m_Array = IblProbe::CreateSpecularCubeArray(m_ArrayLayers);
    // A new array holds nothing: every slot captures again.
    m_Cap = {};
    m_Pre = {};
    m_Queue.clear();
    for (int i = 0; i < (int)m_Slots.size(); ++i) {
        m_Slots[i].Ready = false;
        m_Queue.push_back(i);
    }
}

void ReflectionProbeCapture::EnsureCaptureCubes() {
    for (unsigned int& c : m_CaptureCube) {
        if (c) continue;
        int mips = 1;
        while ((kFaceSize >> mips) > 0) ++mips; // the prefilter reads well down the chain (like IblProbe's env cube)
        glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &c);
        glTextureStorage2D(c, mips, GL_RGBA16F, kFaceSize, kFaceSize);
        GpuMemTrack::Set("ReflectionProbeCapture cube " + std::to_string(c), (size_t)kFaceSize * kFaceSize * 6 * 8 * 4 / 3);
        glTextureParameteri(c, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(c, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(c, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(c, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(c, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }
}

void ReflectionProbeCapture::Sync(ReflectionProbeArray& probes, bool environmentReady, double nowSeconds) {
    std::vector<Volume> want;
    std::vector<int> slotOf(probes.Count(), -1);
    for (int i = 0; i < probes.Count(); ++i) {
        const ReflectionProbeArray::ProbeVolume v = probes.Volume(i);
        if (!v.Capture) continue;
        slotOf[i] = (int)want.size();
        want.push_back({v.Center, v.HalfSize});
    }

    if (s_RecaptureAll.exchange(false)) {
        for (int i = 0; i < (int)m_Slots.size(); ++i)
            if (std::find(m_Queue.begin(), m_Queue.end(), i) == m_Queue.end()) m_Queue.push_back(i);
    }

    std::vector<Volume> have;
    have.reserve(m_Slots.size());
    for (const Slot& s : m_Slots) have.push_back(s.Vol);
    if (!SameVolumes(have, want)) {
        if (!SameVolumes(m_Pending, want)) {
            m_Pending = want;
            m_PendingFrames = 0;
            m_PendingSince = nowSeconds;
        } else {
            ++m_PendingFrames;
        }
        const bool fresh = m_Slots.empty();
        const bool settled = environmentReady && m_PendingFrames >= kSettleFrames &&
                             (fresh || nowSeconds - m_PendingSince >= kMoveSettleSeconds);
        if (settled) {
            const std::vector<bool> changed = Changed(have, want);
            m_Slots.resize(want.size());
            if (m_Cap.Slot >= (int)want.size()) m_Cap = {};
            if (m_Pre.Slot >= (int)want.size()) m_Pre = {};
            m_Queue.erase(std::remove_if(m_Queue.begin(), m_Queue.end(), [&](int s) { return s >= (int)want.size(); }),
                          m_Queue.end());
            for (std::size_t i = 0; i < want.size(); ++i) {
                if (!changed[i]) continue;
                // A moved probe keeps showing its old capture until the new one lands; a new one reads the sky.
                if (i >= have.size()) m_Slots[i].Ready = false;
                m_Slots[i].Vol = want[i];
                if (std::find(m_Queue.begin(), m_Queue.end(), (int)i) == m_Queue.end()) m_Queue.push_back((int)i);
            }
            if (!want.empty()) EnsureArray((int)want.size());
            m_Pending.clear();
        }
    } else {
        m_Pending.clear();
    }

    probes.SetCubeArray(m_Array);
    for (int i = 0; i < probes.Count(); ++i) {
        const int s = slotOf[i];
        const bool ready = s >= 0 && s < (int)m_Slots.size() && m_Slots[s].Ready;
        probes.SetLayer(i, ready ? s : -1);
        if (ready) probes.SetSH(i, m_Slots[s].SH);
    }
}

void ReflectionProbeCapture::Step(IblProbe& ibl, ReflectionProbeArray& probes,
                                  const std::function<void(const RenderFrameContext&)>& draw) {
    if (!Busy() || !m_Array) return;
    EnsureCaptureCubes();

    // A finished capture moves on to the prefilter once the previous one is through it.
    if (m_Cap.Done && m_Pre.Slot < 0) {
        m_Pre = {m_Cap.Slot, 0, m_Cap.Buf, false};
        m_Cap = {};
    }
    if (m_Pre.Slot >= 0) {
        ibl.PrefilterIntoArray(m_CaptureCube[m_Pre.Buf], kFaceSize, m_Array, m_Pre.Slot, m_Pre.Face);
        if (++m_Pre.Face == 6) {
            if (m_Pre.Slot < (int)m_Slots.size()) {
                // The room diffuse reads the widest GGX mip along N: project that mip onto L2 SH once, here, so the
                // model shader evaluates nine uniforms instead of fetching the cube for every pixel (ProbeSH.h).
                constexpr int kTop = IblProbe::kSpecularMips - 1;
                constexpr int kTopSize = IblProbe::kSpecularSize >> kTop;
                float px[kTopSize * kTopSize * 6 * 4] = {};
                glGetTextureSubImage(m_Array, kTop, 0, 0, m_Pre.Slot * 6, kTopSize, kTopSize, 6, GL_RGBA, GL_FLOAT, sizeof(px), px);
                m_Slots[m_Pre.Slot].SH = ProbeSH::Project(px, kTopSize);
                m_Slots[m_Pre.Slot].Ready = true;
            }
            m_Pre = {};
        }
    }
    if (m_Cap.Slot < 0 && !m_Queue.empty()) {
        m_Cap = {m_Queue.front(), 0, m_Pre.Slot >= 0 ? 1 - m_Pre.Buf : 0, false};
        m_Queue.erase(m_Queue.begin());
    }
    if (m_Cap.Slot >= 0 && !m_Cap.Done) {
        m_Target->Resize(kFaceSize, kFaceSize, 1);
        if (!m_Target->IsValid()) { m_Cap = {}; return; }
        const Volume& vol = m_Slots[m_Cap.Slot].Vol;
        m_Target->BindForRender();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        RenderFrameContext ctx;
        ctx.View = FaceView(vol.Center, m_Cap.Face);
        ctx.Proj = FaceProj();
        ctx.ViewPos = vol.Center;
        ctx.TxHdr = m_Target.get();
        ctx.ProbeCapture = true;
        probes.SetCapturing(true);
        draw(ctx);
        probes.SetCapturing(false);
        m_Target->ResolveTo();
        glCopyImageSubData(m_Target->ResolvedColorTexture(), GL_TEXTURE_2D, 0, 0, 0, 0, m_CaptureCube[m_Cap.Buf],
                           GL_TEXTURE_CUBE_MAP, 0, 0, 0, m_Cap.Face, kFaceSize, kFaceSize, 1);
        if (++m_Cap.Face == 6) {
            glGenerateTextureMipmap(m_CaptureCube[m_Cap.Buf]);
            m_Cap.Done = true;
            static const bool kLog = [] {
                char* e = nullptr;
                size_t n = 0;
                const bool on = _dupenv_s(&e, &n, "TARTARUS_PROBE_LOG") == 0 && e && *e;
                std::free(e);
                return on;
            }();
            if (kLog) { // the six faces' average colour (the 1x1 mip), for a quick check
                float px[6 * 4] = {};
                int top = 0;
                while ((kFaceSize >> (top + 1)) > 0) ++top;
                glGetTextureSubImage(m_CaptureCube[m_Cap.Buf], top, 0, 0, 0, 1, 1, 6, GL_RGBA, GL_FLOAT, sizeof(px), px);
                char line[256];
                std::snprintf(line, sizeof(line), "ProbeCapture slot %d at (%.1f %.1f %.1f): face avg %.2f %.2f %.2f %.2f %.2f %.2f",
                              m_Cap.Slot, vol.Center.x, vol.Center.y, vol.Center.z, px[0], px[4], px[8], px[12], px[16], px[20]);
                Log::Info(line);
                static std::string dir = [] {
                    char* e = nullptr;
                    size_t n = 0;
                    std::string d = _dupenv_s(&e, &n, "TARTARUS_PROBE_DUMP") == 0 && e ? e : "";
                    std::free(e);
                    return d;
                }();
                if (!dir.empty()) { // the six faces side by side, tonemapped (x / (1 + x), gamma 2.2), as a PPM
                    std::vector<float> f((size_t)kFaceSize * kFaceSize * 4 * 6);
                    glGetTextureSubImage(m_CaptureCube[m_Cap.Buf], 0, 0, 0, 0, kFaceSize, kFaceSize, 6, GL_RGBA, GL_FLOAT, (GLsizei)(f.size() * 4), f.data());
                    std::string path = dir + "/probe" + std::to_string(m_Cap.Slot) + ".ppm";
                    FILE* fp = nullptr;
                    if (fopen_s(&fp, path.c_str(), "wb") == 0 && fp) {
                        std::fprintf(fp, "P6 %d %d 255\n", kFaceSize * 6, kFaceSize);
                        for (int y = kFaceSize - 1; y >= 0; --y)
                            for (int face = 0; face < 6; ++face)
                                for (int x = 0; x < kFaceSize; ++x)
                                    for (int c = 0; c < 3; ++c) {
                                        float v = f[(((size_t)face * kFaceSize + y) * kFaceSize + x) * 4 + c] * 4.0f;
                                        v = std::pow(v / (1.0f + v), 1.0f / 2.2f);
                                        std::fputc((int)(std::clamp(v, 0.0f, 1.0f) * 255.0f), fp);
                                    }
                        std::fclose(fp);
                    }
                }
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        GLStateCache::Invalidate();
    }
}
