#include "AudioMix.h"

#include "Game/Components.h"

#include <algorithm>
#include <cmath>

namespace {
float DbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
bool Starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
} // namespace

const char* MixGroupName(MixGroup g) {
    switch (g) {
    case MixGroup::Weapon: return "weapon";
    case MixGroup::Threat: return "threat";
    case MixGroup::Feedback: return "feedback";
    case MixGroup::OwnFoley: return "own foley";
    case MixGroup::NpcFoley: return "npc foley";
    case MixGroup::World: return "world";
    case MixGroup::Debris: return "debris";
    case MixGroup::Bed: return "bed";
    default: return "default";
    }
}

MixGroup MixGroupForKey(const std::string& key, bool at2D) {
    if (Starts(key, "snd.amb.")) return MixGroup::Bed;
    if (Starts(key, "snd.ui.") || key == "snd.impact.flesh") return MixGroup::Feedback;
    if (key == "snd.flyby") return MixGroup::Threat;
    if (Starts(key, "snd.impact.") || key == "snd.body_fall") return MixGroup::World;
    if (Starts(key, "snd.casing.")) return MixGroup::Debris;
    if (Starts(key, "snd.foley.")) return at2D ? MixGroup::OwnFoley : MixGroup::NpcFoley;
    if (Starts(key, "snd.")) { // snd.<gun>.<element>
        const size_t dot = key.find('.', 4);
        if (dot != std::string::npos) {
            if (key.compare(dot + 1, 5, "fire_") == 0) return at2D ? MixGroup::Weapon : MixGroup::Threat;
            return at2D ? MixGroup::OwnFoley : MixGroup::NpcFoley;
        }
    }
    return MixGroup::Default;
}

float DistanceModel::Factor() const { return SlopeDb / 6.0206f; }

float DistanceModel::MinDistance() const {
    if (!Valid()) return 1.0f;
    return std::max(RefM * std::pow(2.0f, -NearDb / SlopeDb), 0.05f);
}

float DistanceModel::GainDbAt(float d) const {
    if (!Valid()) return 0.0f;
    const float mn = MinDistance();
    const float dd = std::clamp(d, mn, std::max(MaxM, mn));
    return StartGainDb() - SlopeDb * std::log2(dd / mn);
}

float AirCutoffHz(const AudioMixComponent& s, float distance) {
    if (!s.AirEnabled || distance <= s.AirStartDistance || s.AirStartDistance <= 0.0f) return 20000.0f;
    return std::max(20000.0f * std::pow(s.AirStartDistance / distance, s.AirExponent), s.AirMinHz);
}

void MixDucker::Configure(const AudioMixComponent& s) {
    m_Enabled = s.DuckEnabled;
    m_ThresholdDb = s.DuckThresholdDb;
    m_FullDb = std::max(s.DuckFullDb, s.DuckThresholdDb + 1.0f);
    m_Attack = std::max(s.DuckAttackMs, 0.0f) / 1000.0f;
    m_Hold = std::max(s.DuckHold, 0.0f);
    m_Release = std::max(s.DuckRelease, 0.01f);
    m_FocusEnabled = s.FocusEnabled;
    m_FocusTime = std::max(s.FocusTime, 0.01f);
    std::fill(std::begin(m_DuckDb), std::end(m_DuckDb), 0.0f);
    std::fill(std::begin(m_FocusDb), std::end(m_FocusDb), 0.0f);
    m_DuckDb[(int)MixGroup::Bed] = s.DuckBedDb;
    m_DuckDb[(int)MixGroup::OwnFoley] = s.DuckOwnFoleyDb;
    m_DuckDb[(int)MixGroup::Debris] = s.DuckDebrisDb;
    m_DuckDb[(int)MixGroup::World] = s.DuckWorldDb;
    m_FocusDb[(int)MixGroup::Bed] = s.FocusBedDb;
    m_FocusDb[(int)MixGroup::OwnFoley] = s.FocusOwnFoleyDb;
    m_FocusDb[(int)MixGroup::Debris] = s.FocusDebrisDb;
}

void MixDucker::Key(float levelDb) {
    if (!m_Enabled) return;
    const float a = std::clamp((levelDb - m_ThresholdDb) / (m_FullDb - m_ThresholdDb), 0.0f, 1.0f);
    if (a <= 0.0f) return;
    m_Amount = std::max(m_Amount, a);
    m_HoldLeft = m_Hold;
}

void MixDucker::Update(float dt) {
    dt = std::max(dt, 0.0f);
    if (!m_Enabled) m_Amount = 0.0f;
    if (m_HoldLeft > 0.0f) m_HoldLeft -= dt;
    else m_Amount = std::max(0.0f, m_Amount - dt / m_Release);
    const float target = m_FocusEnabled && m_FocusOn ? 1.0f : 0.0f;
    const float step = dt / m_FocusTime;
    m_Focus += std::clamp(target - m_Focus, -step, step);
}

float MixDucker::GainDb(MixGroup g) const {
    const int i = std::clamp((int)g, 0, (int)MixGroup::Count - 1);
    return -(m_DuckDb[i] * m_Amount + m_FocusDb[i] * m_Focus);
}

float MixDucker::Gain(MixGroup g) const { return DbToLin(GainDb(g)); }

void MixDucker::Reset() {
    m_Amount = m_HoldLeft = m_Focus = 0.0f;
    m_FocusOn = false;
}
