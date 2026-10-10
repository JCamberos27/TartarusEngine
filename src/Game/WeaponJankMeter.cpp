#include "WeaponJankMeter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
float TurnDegrees(const glm::quat& a, const glm::quat& b) {
    const float d = std::abs(glm::dot(a, b));
    return glm::degrees(2.0f * std::acos(std::clamp(d, 0.0f, 1.0f)));
}
} // namespace

const char* WeaponJankEvent::KindName(Kind k) {
    switch (k) {
    case Kind::GunPop: return "gun pop";
    case Kind::HandPop: return "hand pop";
    case Kind::Settle: return "settle";
    case Kind::Grip: return "off grip";
    }
    return "?";
}

std::string WeaponJankEvent::Describe() const {
    char buf[200];
    const char* side = Side == 0 ? " (left)" : Side == 1 ? " (right)" : "";
    std::snprintf(buf, sizeof buf, "%-9s%-9s %-14s t %6.2f  %5.2f %s", KindName(What), side, State.c_str(), Time, Size, Turn ? "deg" : "cm");
    return buf;
}

void WeaponJankMeter::Reset() {
    m_Events.clear();
    m_Prev.clear();
    m_Time = m_StateTime = m_Still = 0.0f;
    m_State.clear();
    m_SettleCm = m_SettleDeg = 0.0f;
    m_SettleStart = -1.0f;
    for (auto& k : m_LastFlag) k[0] = k[1] = -1.0f;
}

void WeaponJankMeter::Flag(WeaponJankEvent::Kind kind, float size, int side, bool turn) {
    const int k = (int)kind, s = std::max(side, 0);
    // A run of frames (within a tenth of a second of the last) in the same state and kind is one event, at its worst.
    if (m_LastFlag[k][s] >= 0.0f && m_Time - m_LastFlag[k][s] < 0.1f)
        for (auto it = m_Events.rbegin(); it != m_Events.rend(); ++it)
            if (it->What == kind && it->Side == side && it->Turn == turn && it->State == m_State) {
                if (size > it->Size) { it->Size = size; it->Time = m_Time; }
                m_LastFlag[k][s] = m_Time;
                return;
            }
    m_LastFlag[k][s] = m_Time;
    WeaponJankEvent e;
    e.What = kind;
    e.State = m_State;
    e.Time = m_Time;
    e.Size = size;
    e.Side = side;
    e.Turn = turn;
    m_Events.push_back(e);
}

void WeaponJankMeter::CloseSettle() {
    if (m_SettleStart >= 0.0f) {
        const float t = m_Time;
        m_Time = m_SettleStart;
        if (m_SettleCm >= SettleMinCm) Flag(WeaponJankEvent::Kind::Settle, m_SettleCm);
        if (m_SettleDeg >= SettleMinDeg) Flag(WeaponJankEvent::Kind::Settle, m_SettleDeg, -1, true);
        m_Time = t;
    }
    m_SettleCm = m_SettleDeg = 0.0f;
    m_SettleStart = -1.0f;
}

void WeaponJankMeter::Push(const WeaponJankFrame& f) {
    m_Time += f.Dt;
    if (f.State != m_State) {
        CloseSettle();
        m_State = f.State;
        m_StateTime = 0.0f;
    } else {
        m_StateTime += f.Dt;
    }
    if (!f.Valid || f.Dt <= 0.0f) {
        m_Prev.clear();
        CloseSettle();
        return;
    }
    for (int s = 0; s < 2; ++s)
        if (f.HandGap[s] * 100.0f > GripCm) Flag(WeaponJankEvent::Kind::Grip, f.HandGap[s] * 100.0f, s);
    // Still: the player not moving and the view not turning (turning or walking, the body - and the gun on it - moves
    // under the view as it should), and out of a shot's recovery.
    if (!m_Prev.empty()) {
        const float turn = TurnDegrees(f.View, m_Prev.back().View) / f.Dt;
        m_Still = f.Speed < 0.15f && turn < 5.0f && f.SinceShot > 1.5f ? m_Still + f.Dt : 0.0f;
    }
    if (m_Prev.size() == 2) {
        const WeaponJankFrame& a = m_Prev[0];
        const WeaponJankFrame& b = m_Prev[1];
        // The jump past a smooth path: this frame against where the last two put it (constant velocity), scaled to a
        // 60 Hz frame so the thresholds don't depend on the step.
        const float scale = (1.0f / 60.0f) * (1.0f / 60.0f) / std::max(f.Dt * f.Dt, 1e-8f);
        if (!f.Recoil) {
            const float jump = glm::length(f.GunPos - 2.0f * b.GunPos + a.GunPos) * 100.0f * scale;
            if (jump > PopCm) Flag(WeaponJankEvent::Kind::GunPop, jump);
            // Rotations: this frame's turn against the last one's, carried on.
            const glm::quat step = b.GunRot * glm::inverse(a.GunRot);
            const float turnJump = TurnDegrees(f.GunRot, step * b.GunRot) * scale;
            if (turnJump > PopDeg) Flag(WeaponJankEvent::Kind::GunPop, turnJump, -1, true);
            for (int s = 0; s < 2; ++s) {
                const float h = glm::length(f.Hand[s] - 2.0f * b.Hand[s] + a.Hand[s]) * 100.0f * scale;
                const float c = glm::length(f.HandClip[s] - 2.0f * b.HandClip[s] + a.HandClip[s]) * 100.0f * scale;
                if (h - c > PopCm * 1.5f) Flag(WeaponJankEvent::Kind::HandPop, h - c, s);
            }
        }
        // Settling: past SettleAfter into the state, the gun keeps moving on its own.
        if (f.Resting && m_StateTime > SettleAfter && m_Still > StillFor) {
            const float cm = glm::length(f.GunPos - b.GunPos) * 100.0f, deg = TurnDegrees(f.GunRot, b.GunRot);
            const bool moving = cm / f.Dt > SettleCmPerS || deg / f.Dt > SettleDegPerS;
            if (moving) {
                if (m_SettleStart < 0.0f) m_SettleStart = m_Time;
                m_SettleCm += cm;
                m_SettleDeg += deg;
            } else {
                CloseSettle();
            }
        } else {
            CloseSettle();
        }
    }
    m_Prev.push_back(f);
    if (m_Prev.size() > 2) m_Prev.erase(m_Prev.begin());
}
