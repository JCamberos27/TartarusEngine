#include "FirstPersonJankMeter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

const char* FirstPersonJankEvent::KindName(Kind k) {
    switch (k) {
    case Kind::Bounce: return "bounce";
    case Kind::HandPop: return "hand pop";
    case Kind::Grip: return "off grip";
    case Kind::Stretch: return "stretch";
    case Kind::Clip: return "arm in eye";
    }
    return "?";
}

std::string FirstPersonJankEvent::Describe() const {
    char buf[200];
    const char* side = Side == 0 ? " (left)" : Side == 1 ? " (right)" : "";
    const char* unit = What == Kind::Bounce ? "mm" : What == Kind::Stretch ? "%" : "cm";
    std::snprintf(buf, sizeof buf, "%-10s%-9s %-14s t %6.2f  %5.2f %s", KindName(What), side, State.c_str(), Time, Size, unit);
    return buf;
}

void FirstPersonJankMeter::Reset() {
    m_Events.clear();
    m_Prev.clear();
    m_Time = 0.0f;
    m_State.clear();
    for (auto& k : m_LastFlag) k[0] = k[1] = -1.0f;
    m_PrevPhase = -1.0f;
    m_Lo[0] = m_Lo[1] = 1e9f;
    m_Hi[0] = m_Hi[1] = -1e9f;
    m_CycleFrames = 0;
    m_CycleClean = true;
    m_LastCycleMm = -1.0f;
}

float FirstPersonJankMeter::Worst(FirstPersonJankEvent::Kind kind) const {
    float w = 0.0f;
    for (const FirstPersonJankEvent& e : m_Events)
        if (e.What == kind) w = std::max(w, e.Size);
    return w;
}

void FirstPersonJankMeter::Flag(FirstPersonJankEvent::Kind kind, float size, int side) {
    const int k = (int)kind, s = std::max(side, 0);
    // A run of frames (within a tenth of a second of the last) in the same state and kind is one event, at its worst.
    if (m_LastFlag[k][s] >= 0.0f && m_Time - m_LastFlag[k][s] < 0.1f)
        for (auto it = m_Events.rbegin(); it != m_Events.rend(); ++it)
            if (it->What == kind && it->Side == side && it->State == m_State) {
                if (size > it->Size) { it->Size = size; it->Time = m_Time; }
                m_LastFlag[k][s] = m_Time;
                return;
            }
    m_LastFlag[k][s] = m_Time;
    FirstPersonJankEvent e;
    e.What = kind;
    e.State = m_State;
    e.Time = m_Time;
    e.Size = size;
    e.Side = side;
    m_Events.push_back(e);
}

void FirstPersonJankMeter::EndCycle() {
    // A cycle counts when it ran moving, in one state, with no shot in it, and long enough to be a stride.
    if (m_CycleClean && m_CycleFrames >= 8) {
        float mm = 0.0f;
        for (int h = 0; h < 2; ++h)
            if (m_Hi[h] > m_Lo[h]) mm = std::max(mm, (m_Hi[h] - m_Lo[h]) * 1000.0f);
        m_LastCycleMm = mm;
        if (mm > BounceMm) Flag(FirstPersonJankEvent::Kind::Bounce, mm);
    }
    m_Lo[0] = m_Lo[1] = 1e9f;
    m_Hi[0] = m_Hi[1] = -1e9f;
    m_CycleFrames = 0;
    m_CycleClean = true;
}

void FirstPersonJankMeter::Push(const FirstPersonJankSample& f) {
    m_Time += f.Dt;
    if (f.State != m_State) {
        m_State = f.State;
        m_CycleClean = false; // a cycle that spans a state change is the transition's, not the walk's
    }
    if (!f.Valid || f.Dt <= 0.0f) {
        m_Prev.clear();
        m_PrevPhase = -1.0f;
        return;
    }
    for (int s = 0; s < 2; ++s) {
        if (f.HandGap[s] * 100.0f > GripCm) Flag(FirstPersonJankEvent::Kind::Grip, f.HandGap[s] * 100.0f, s);
        if (f.Reach[s] > StretchAt) Flag(FirstPersonJankEvent::Kind::Stretch, (f.Reach[s] - 1.0f) * 100.0f, s);
        if (f.EyeToArm[s] * 100.0f < ClipCm) Flag(FirstPersonJankEvent::Kind::Clip, f.EyeToArm[s] * 100.0f, s);
    }
    if (f.InsideRays > 0) Flag(FirstPersonJankEvent::Kind::Clip, 0.0f);
    // The gait cycle: closed when the phase wraps (it runs 0..1), each hand's vertical range kept while moving.
    const bool moving = f.GaitPhase >= 0.0f && f.Speed >= MinBounceSpeed;
    if (!moving) {
        m_PrevPhase = -1.0f;
        m_Lo[0] = m_Lo[1] = 1e9f;
        m_Hi[0] = m_Hi[1] = -1e9f;
        m_CycleFrames = 0;
        m_CycleClean = true;
    } else {
        if (m_PrevPhase >= 0.0f && f.GaitPhase < m_PrevPhase - 0.5f) EndCycle(); // wrapped
        m_PrevPhase = f.GaitPhase;
        if (f.Recoil) m_CycleClean = false;
        ++m_CycleFrames;
        for (int h = 0; h < 2; ++h) {
            m_Lo[h] = std::min(m_Lo[h], f.Hand[h].y);
            m_Hi[h] = std::max(m_Hi[h], f.Hand[h].y);
        }
    }
    if (m_Prev.size() == 2 && !f.Recoil) {
        const FirstPersonJankSample& a = m_Prev[0];
        const FirstPersonJankSample& b = m_Prev[1];
        // The jump past a smooth path: this frame against where the last two put it (constant velocity), scaled to a 60 Hz
        // frame so the threshold does not depend on the step.
        const float scale = (1.0f / 60.0f) * (1.0f / 60.0f) / std::max(f.Dt * f.Dt, 1e-8f);
        for (int s = 0; s < 2; ++s) {
            const float h = glm::length(f.Hand[s] - 2.0f * b.Hand[s] + a.Hand[s]) * 100.0f * scale;
            if (h > PopCm) Flag(FirstPersonJankEvent::Kind::HandPop, h, s);
        }
    }
    m_Prev.push_back(f);
    if (m_Prev.size() > 2) m_Prev.erase(m_Prev.begin());
}
