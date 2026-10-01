#include "SquadVoice.h"

#include "AudioEngine.h"
#include "Log.h"
#include "json.hpp"

#include <algorithm>
#include <fstream>

namespace {

struct KeyDefault { const char* Key; BarkDefault D; };
// Order = the Bark enum. Priority decides who may cut the channel; Responds: a squadmate may say "copy".
const KeyDefault kTable[(int)Bark::Count] = {
    {"contact",       {7, 8.0f,  "Contact. Hostile in sector four, engaging.", true}},
    {"contact_relay", {5, 6.0f,  "Copy contact, moving to engage.", false}},
    {"gunfire",       {6, 6.0f,  "Shots fired, sector two. Investigating.", false}},
    {"flank_left",    {5, 8.0f,  "Flanking left. Hold your fire lane.", true}},
    {"flank_right",   {5, 8.0f,  "Flanking right. Hold your fire lane.", true}},
    {"moving_up",     {4, 8.0f,  "Advancing. Maintain suppression.", true}},
    {"falling_back",  {6, 10.0f, "Falling back. Repositioning to the fallback marker.", true}},
    {"lost_target",   {5, 10.0f, "Lost visual. Last known position marked.", false}},
    {"man_down",      {8, 5.0f,  "Unit down! I repeat, unit down!", true}},
    {"wounded",       {9, 4.0f,  "I am hit! Need assistance!", false}},
    {"reloading",     {3, 10.0f, "Reloading. Cover me.", false}},
    {"covering",      {4, 12.0f, "Covering fire. Keep your heads down.", false}},
    {"target_down",   {6, 20.0f, "Target neutralized. Sector secure.", false}},
    {"player_hurt",   {3, 12.0f, "Hostile is hit. Keep the pressure on.", false}},
    {"suspicious",    {4, 10.0f, "Movement detected. Possible hostile.", true}},
    {"investigating", {3, 10.0f, "Moving to investigate. Cover my approach.", false}},
    {"all_clear",     {2, 15.0f, "Sector clear. Resume patrol.", false}},
    {"idle",          {1, 25.0f, "Unit on station. No activity.", false}},
    {"melee",         {8, 3.0f,  "Close contact! Break away!", false}},
    {"copy",          {0, 0.0f,  "Copy.", false}},
};

float EstimateSeconds(const std::string& text) { return 0.45f + 0.075f * (float)text.size(); }

} // namespace

const char* BarkKey(Bark b) { return kTable[std::min((int)b, (int)Bark::Count - 1)].Key; }
const BarkDefault& BarkDefaults(Bark b) { return kTable[std::min((int)b, (int)Bark::Count - 1)].D; }

SquadVoice::SquadVoice() { Reset(); }

void SquadVoice::Reset() {
    if (AudioEngine::IsInitialized())
        for (const Channel& c : m_Channels) if (c.Handle) AudioEngine::Stop(c.Handle);
    m_Channels.clear();
    m_History.clear();
    m_Pending.clear();
    m_Spoken = m_Cuts = 0;
    m_Rng = 0xBA4Cu;
    if (m_Events.empty()) {
        m_Events.resize((size_t)Bark::Count);
        for (int i = 0; i < (int)Bark::Count; ++i) {
            m_Events[(size_t)i].Priority = kTable[i].D.Priority;
            m_Events[(size_t)i].Cooldown = kTable[i].D.Cooldown;
            Line l;
            l.Text = kTable[i].D.Text;
            m_Events[(size_t)i].Lines.push_back(l);
        }
    }
}

bool SquadVoice::LoadManifest(const std::string& dir) {
    m_Dir = dir;
    std::ifstream in(dir + "/manifest.json");
    if (!in) return false;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.contains("events")) {
        Log::Error("SquadVoice: manifest.json is not valid");
        return false;
    }
    int lines = 0;
    for (int i = 0; i < (int)Bark::Count; ++i) {
        const auto it = j["events"].find(kTable[i].Key);
        if (it == j["events"].end()) continue;
        EventDef def;
        def.Priority = it->value("priority", kTable[i].D.Priority);
        def.Cooldown = it->value("cooldown", kTable[i].D.Cooldown);
        def.FromManifest = true;
        if (it->contains("lines"))
            for (const auto& l : (*it)["lines"]) {
                Line line;
                line.Text = l.value("text", std::string());
                for (int v = 0; v < 2; ++v) {
                    if (l.contains("files") && l["files"].is_array() && (int)l["files"].size() > v) line.File[v] = l["files"][(size_t)v].get<std::string>();
                    if (l.contains("duration") && l["duration"].is_array() && (int)l["duration"].size() > v)
                        line.Duration[v] = l["duration"][(size_t)v].get<float>();
                }
                if (line.File[1].empty()) line.File[1] = line.File[0];
                if (line.Duration[1] <= 0.0f) line.Duration[1] = line.Duration[0];
                if (!line.Text.empty()) def.Lines.push_back(line);
            }
        if (def.Lines.empty()) continue;
        m_Events[(size_t)i] = def;
        lines += (int)def.Lines.size();
    }
    m_Loaded = lines > 0;
    if (m_Loaded && AudioEngine::IsInitialized())
        for (const EventDef& e : m_Events)
            for (const Line& l : e.Lines)
                for (int v = 0; v < 2; ++v)
                    if (!l.File[v].empty() && (v == 0 || l.File[v] != l.File[0])) AudioEngine::Load(m_Dir + "/" + l.File[v]);
    return m_Loaded;
}

float SquadVoice::Rand01() {
    m_Rng = m_Rng * 1664525u + 1013904223u;
    return (float)(m_Rng >> 8) / 16777216.0f;
}

SquadVoice::Channel& SquadVoice::Chan(int squad) {
    squad = std::max(0, squad);
    while ((int)m_Channels.size() <= squad) {
        Channel c;
        for (int i = 0; i < (int)Bark::Count; ++i) { c.LastLine[i] = -1; c.LastEvent[i] = -1e9f; }
        m_Channels.push_back(c);
    }
    return m_Channels[(size_t)squad];
}

int SquadVoice::Priority(Bark ev) const { return m_Events[(size_t)ev].Priority; }
float SquadVoice::Cooldown(Bark ev) const { return m_Events[(size_t)ev].Cooldown; }

bool SquadVoice::Busy(int squad, float now) const {
    return squad >= 0 && squad < (int)m_Channels.size() && now < m_Channels[(size_t)squad].BusyUntil;
}

bool SquadVoice::Start(float now, int squad, int speaker, int unit, Bark ev, const glm::vec3& pos, bool responder) {
    Channel& c = Chan(squad);
    const EventDef& def = m_Events[(size_t)ev];
    // A line the squad hasn't just said (when there's a choice).
    int pick = (int)(Rand01() * (float)def.Lines.size()) % (int)def.Lines.size();
    if (def.Lines.size() > 1 && pick == c.LastLine[(int)ev]) pick = (pick + 1) % (int)def.Lines.size();
    c.LastLine[(int)ev] = pick;
    const Line& line = def.Lines[(size_t)pick];
    const int voice = speaker >= 0 ? speaker % 2 : 0;
    const std::string& file = line.File[voice].empty() ? line.File[0] : line.File[voice];
    const float dur = line.Duration[voice] > 0.0f ? line.Duration[voice] : EstimateSeconds(line.Text);

    BarkPlayed p;
    p.Squad = squad;
    p.Speaker = speaker;
    p.Unit = unit;
    p.Voice = voice;
    p.Event = ev;
    p.Responder = responder;
    p.Text = line.Text;
    p.File = file;
    p.Pos = pos;
    p.Start = now;
    p.End = now + dur;
    p.Priority = def.Priority;
    m_History.push_back(p);
    m_Pending.push_back(p);
    c.Index = (int)m_History.size() - 1;
    c.BusyUntil = p.End + kGap;
    c.LastEvent[(int)ev] = now;
    ++m_Spoken;
    c.Handle = 0;
    if (m_Audio && AudioEngine::IsInitialized() && !file.empty() && !m_Dir.empty()) {
        const AudioEngine::SoundHandle h = AudioEngine::Play(m_Dir + "/" + file, 1.0f, false, AudioEngine::Bus::Voice);
        if (h != AudioEngine::InvalidHandle) {
            AudioEngine::SetPosition(h, pos);
            AudioEngine::SetRolloff(h, AudioEngine::Rolloff::Linear, m_Near, m_Far);
            c.Handle = h;
        }
    }
    return true;
}

bool SquadVoice::Say(float now, int squad, int speaker, int unit, Bark ev, const glm::vec3& pos, int responder, int responderUnit,
                     const glm::vec3& responderPos) {
    if ((int)ev < 0 || ev >= Bark::Count) return false;
    Channel& c = Chan(squad);
    const EventDef& def = m_Events[(size_t)ev];
    if (def.Cooldown > 0.0f && now - c.LastEvent[(int)ev] < def.Cooldown) return false;
    if (now < c.BusyUntil) {
        const int onAir = c.Index >= 0 ? m_History[(size_t)c.Index].Priority : 0;
        if (def.Priority <= onAir) return false;
        // Cut in: the line on air stops where it is.
        if (c.Index >= 0 && m_History[(size_t)c.Index].Cut < 0.0f && now < m_History[(size_t)c.Index].End) {
            m_History[(size_t)c.Index].Cut = now;
            if (m_Audio && AudioEngine::IsInitialized() && c.Handle) AudioEngine::Stop(c.Handle);
            ++m_Cuts;
        }
        c.BusyUntil = now;
    }
    c.RespPending = false;
    if (!Start(now, squad, speaker, unit, ev, pos, false)) return false;
    if (responder >= 0 && responder != speaker && kTable[(int)ev].D.Responds && Rand01() < 0.6f) {
        c.RespPending = true;
        c.RespAt = c.BusyUntil + kCopyDelay;
        c.RespSpeaker = responder;
        c.RespUnit = responderUnit;
        c.RespPos = responderPos;
    }
    return true;
}

void SquadVoice::Update(float now) {
    for (size_t s = 0; s < m_Channels.size(); ++s) {
        Channel& c = m_Channels[s];
        if (!c.RespPending || now < c.RespAt) continue;
        c.RespPending = false;
        if (now >= c.BusyUntil) Start(now, (int)s, c.RespSpeaker, c.RespUnit, Bark::Copy, c.RespPos, true);
    }
}

std::vector<BarkPlayed> SquadVoice::TakeSubtitles() {
    std::vector<BarkPlayed> out;
    out.swap(m_Pending);
    return out;
}

int SquadVoice::FirstOverlap(int squad) const {
    const BarkPlayed* prev = nullptr;
    for (size_t i = 0; i < m_History.size(); ++i) {
        const BarkPlayed& b = m_History[i];
        if (b.Squad != squad) continue;
        if (prev && b.Start < prev->EffectiveEnd() - 1e-4f) return (int)i;
        prev = &b;
    }
    return -1;
}
