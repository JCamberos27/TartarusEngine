#include "FxSprites.h"

#include "FxSpriteRenderer.h"
#include "KnifeFxLibrary.h"

#include <algorithm>
#include <cmath>

float FxSprites::Rand() {
    m_Rng ^= m_Rng << 13;
    m_Rng ^= m_Rng >> 17;
    m_Rng ^= m_Rng << 5;
    return (float)(m_Rng & 0xFFFFFFu) / (float)0x1000000;
}

int FxSprites::Entry(const std::string& name) {
    for (const Cached& c : m_Cache)
        if (c.Name == name) return c.Id;
    Cached c{name, -1, {}};
    if (m_Lookup) {
        c.Id = m_Lookup(name, c.Info);
    } else {
        const KnifeFxLibrary& lib = KnifeFxLibrary::Get();
        c.Id = lib.Find(name);
        if (const auto* e = lib.At(c.Id)) c.Info = {e->Frames, e->Aspect};
    }
    // A miss isn't cached while the library hasn't loaded yet (it loads with the first Play frame).
    if (c.Id >= 0 || m_Lookup || KnifeFxLibrary::Get().Loaded()) m_Cache.push_back(c);
    return c.Id;
}

void FxSprites::Spawn(const Emit& e) {
    if (e.Life <= 0.0f) return;
    Particle p;
    p.E = e;
    p.Seed = (std::uint32_t)(Rand() * 16777216.0f);
    if (e.Entry >= 0) {
        for (const Cached& c : m_Cache)
            if (c.Id == e.Entry) { p.Frames = std::max(1, c.Info.Frames); p.Aspect = c.Info.Aspect; }
        if (!m_Lookup)
            if (const auto* le = KnifeFxLibrary::Get().At(e.Entry)) { p.Frames = std::max(1, le->Frames); p.Aspect = le->Aspect; }
    }
    if (p.E.Frame < 0) p.E.Frame = std::min(p.Frames - 1, (int)(Rand() * (float)p.Frames));
    if ((int)m_Live.size() >= MaxLive) m_Live.erase(m_Live.begin());
    m_Live.push_back(p);
    ++m_Spawned;
}

void FxSprites::Update(float dt) {
    if (dt <= 0.0f) return;
    for (Particle& p : m_Live) {
        Emit& e = p.E;
        p.Age += dt;
        e.Vel.y -= 9.81f * e.Gravity * dt;
        if (e.Drag > 0.0f) e.Vel *= std::exp(-e.Drag * dt);
        e.Pos += e.Vel * dt;
        e.Rot += e.Spin * dt;
        if (e.Plane != glm::vec4(0.0f)) {
            const glm::vec3 n(e.Plane);
            const float d = glm::dot(n, e.Pos) + e.Plane.w;
            if (d < 0.0f) { // through the surface: back onto it, bounced and slowed by friction
                e.Pos -= n * d;
                const float vn = glm::dot(e.Vel, n);
                if (vn < 0.0f) {
                    const glm::vec3 tangential = e.Vel - n * vn;
                    e.Vel = tangential * 0.6f - n * vn * e.Bounce;
                    e.Spin *= 0.5f;
                }
            }
        }
    }
    m_Live.erase(std::remove_if(m_Live.begin(), m_Live.end(), [](const Particle& p) { return p.Age >= p.E.Life; }), m_Live.end());
}

void FxSprites::FrameAt(float t, int frames, int first, bool animate, int& cellA, int& cellB, float& blend) {
    frames = std::max(1, frames);
    if (!animate || frames == 1) {
        cellA = cellB = std::clamp(first, 0, frames - 1);
        blend = 0.0f;
        return;
    }
    first = std::clamp(first, 0, frames - 1);
    const int span = frames - first;
    const float f = std::clamp(t, 0.0f, 1.0f) * (float)span;
    const int a = std::min((int)f, span - 1);
    cellA = first + a;
    cellB = first + std::min(a + 1, span - 1);
    blend = std::clamp(f - (float)a, 0.0f, 1.0f);
}

void FxSprites::Submit(FxSpriteRenderer& r) const {
    for (const Particle& p : m_Live) {
        const Emit& e = p.E;
        const float t = std::clamp(p.Age / e.Life, 0.0f, 1.0f);
        float a = e.Alpha;
        if (e.FadeIn > 0.0f && t < e.FadeIn) a *= t / e.FadeIn;
        if (e.FadeOut > 0.0f && t > 1.0f - e.FadeOut) a *= (1.0f - t) / e.FadeOut;
        const float size = glm::mix(e.Size0, e.Size1, t);
        if (a <= 0.002f || size <= 0.0f) continue;
        FxSpriteRenderer::Sprite s;
        s.Pos = e.Pos;
        s.Size = size;
        s.Rot = e.Rot;
        s.Entry = e.Entry;
        s.Mode = (int)e.Mode;
        s.Color = glm::vec4(e.Color * (e.Mode == Shade::Additive ? e.Intensity : 1.0f), a);
        s.Erosion = glm::mix(e.Erosion0, e.Erosion1, t);
        s.Softness = e.Softness;
        s.Aspect = p.Aspect;
        s.ViewModel = e.ViewModel;
        s.Seed = (float)(p.Seed & 0xFFFF) / 65535.0f;
        FrameAt(t, p.Frames, e.Frame, e.Animate, s.CellA, s.CellB, s.Blend);
        if (!e.BlendFrames) s.Blend = 0.0f;
        if (e.Axis != glm::vec3(0.0f)) {
            s.Axis = e.Axis;
        } else if (e.Stretch > 0.0f) {
            const float speed = glm::length(e.Vel);
            if (speed > 1e-3f) s.Axis = e.Vel / speed * (1.0f + speed * e.Stretch);
        }
        r.Add(s);
    }
}
