#include "EditorSound.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <imgui.h>
#include "../Audio/AudioEngine.h"
#include "Log.h"
#include "EditorSettings.h"

namespace EditorSound {
namespace {

using EditorSoundEvents::kCategoryCount;
using EditorSoundEvents::kEventCount;

//                         name                category                 dB    cooldown jitter centred
const EventInfo kEvents[kEventCount] = {
    {"Click",            Category::Clicks,        0.0f,  35.0f, 0.03f, false},
    {"ClickConfirm",     Category::Clicks,        0.0f,  60.0f, 0.02f, false},
    {"ClickCancel",      Category::Clicks,        0.0f,  60.0f, 0.02f, false},
    {"ClickDestructive", Category::Clicks,        0.0f,  80.0f, 0.02f, false},
    {"ToggleOn",         Category::Clicks,        0.0f,  40.0f, 0.02f, false},
    {"ToggleOff",        Category::Clicks,        0.0f,  40.0f, 0.02f, false},
    {"TabSelect",        Category::Clicks,       -2.0f,  40.0f, 0.03f, false},
    {"PlayEnter",        Category::PlayMode,      0.0f, 250.0f, 0.0f,  true},
    {"PlayExit",         Category::PlayMode,      0.0f, 250.0f, 0.0f,  true},
    {"Pause",            Category::PlayMode,      0.0f, 150.0f, 0.0f,  true},
    {"Resume",           Category::PlayMode,      0.0f, 150.0f, 0.0f,  true},
    {"Step",             Category::PlayMode,     -3.0f,  60.0f, 0.02f, true},
    {"NotifyInfo",       Category::Notifications, 0.0f, 400.0f, 0.0f,  true},
    {"NotifySuccess",    Category::Notifications, 0.0f, 400.0f, 0.0f,  true},
    {"NotifyWarning",    Category::Notifications, 0.0f,1500.0f, 0.0f,  true},
    {"NotifyError",      Category::Notifications, 0.0f, 600.0f, 0.0f,  true},
    {"Save",             Category::Notifications,-2.0f, 300.0f, 0.0f,  true},
    {"Undo",             Category::Clicks,       -2.0f,  60.0f, 0.02f, true},
    {"Redo",             Category::Clicks,       -2.0f,  60.0f, 0.02f, true},
};

const char* kCategoryNames[kCategoryCount] = {"Clicks", "Sliders", "Hover", "Panels", "Drag & Drop", "Notifications", "Play Mode"};

float DbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

struct State {
    bool Ready = false;
    Settings Values;
    Scheduler Sched;
    bool Focused = true;
    int Played = 0;
    bool LogPrimed = false;
    unsigned long long LastLogSeq = 0;
    double QuietUntilMs = 0.0;
    double LastAlertMs = -1e9;
    int AlertStorm = 0;
    std::array<std::vector<std::string>, kEventCount> Files;
} s;

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void ApplyOutputVolume() { AudioEngine::SetEditorVolume(s.Values.Enabled ? std::clamp(s.Values.Master, 0.0f, 1.0f) : 0.0f); }

} // namespace

const EventInfo& Info(Event e) { return kEvents[std::clamp((int)e, 0, kEventCount - 1)]; }
const char* CategoryName(Category c) { return kCategoryNames[std::clamp((int)c, 0, kCategoryCount - 1)]; }

float Scheduler::NextUnit() {
    // xorshift32: cheap, and deterministic per seed for the tests.
    m_Rng ^= m_Rng << 13;
    m_Rng ^= m_Rng >> 17;
    m_Rng ^= m_Rng << 5;
    return (float)(m_Rng >> 8) / (float)(1u << 24);
}

void Scheduler::Reset() {
    m_LastMs.fill(0.0);
    m_Played.fill(false);
    m_LastVariant.fill(0);
    m_Repeats.fill(0);
}

bool Scheduler::Decide(Event e, double nowMs, float screenX01, bool windowFocused, const Settings& st, Voice& out) {
    const int i = (int)e;
    if (i < 0 || i >= kEventCount) return false;
    const EventInfo& info = kEvents[i];
    const int cat = (int)info.Cat;
    if (!st.Enabled || st.Master <= 0.0f || !st.CategoryOn[cat] || st.CategoryVolume[cat] <= 0.0f) return false;
    if (!windowFocused && info.Cat != Category::Notifications) return false; // in the background only notifications speak
    const int variants = m_Variants[i];
    if (variants <= 0) return false;
    const double since = m_Played[i] ? nowMs - m_LastMs[i] : 1e9;
    if (since < info.CooldownMs) return false;

    // Rapid repeats get a little quieter each time, so mashing a button doesn't hammer.
    m_Repeats[i] = since < kRepeatWindowMs ? m_Repeats[i] + 1 : 0;
    const float repeatDb = std::max(kRepeatStepDb * (float)m_Repeats[i], kRepeatFloorDb);

    // Round-robin with a random step that never lands on the previous take.
    int v = 0;
    if (variants > 1) {
        const int step = 1 + (int)(NextUnit() * (float)(variants - 1)) % (variants - 1);
        v = m_Played[i] ? (m_LastVariant[i] + step) % variants : (int)(NextUnit() * (float)variants) % variants;
    }
    out.Variant = v;
    out.Gain = DbToGain(info.GainDb + repeatDb) * std::clamp(st.CategoryVolume[cat], 0.0f, 1.0f);
    out.Pitch = 1.0f + info.PitchJitter * (NextUnit() * 2.0f - 1.0f);
    out.Pan = (info.Centred || screenX01 < 0.0f) ? 0.0f : (std::clamp(screenX01, 0.0f, 1.0f) * 2.0f - 1.0f) * kPanWidth;

    m_Played[i] = true;
    m_LastMs[i] = nowMs;
    m_LastVariant[i] = v;
    return true;
}

std::vector<std::string> FindVariants(const std::string& dir, Event e) {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    if (dir.empty()) return files;
    const std::string name = Info(e).Name;
    std::error_code ec;
    const fs::path single = fs::u8path(dir) / (name + ".wav");
    if (fs::is_regular_file(single, ec)) {
        files.push_back(single.u8string());
        return files;
    }
    for (int n = 1;; ++n) {
        const fs::path p = fs::u8path(dir) / (name + "_" + std::to_string(n) + ".wav");
        if (!fs::is_regular_file(p, ec)) break;
        files.push_back(p.u8string());
    }
    return files;
}

void Init(const std::string& builtInDir, const std::string& overrideDir) {
    s.Sched.Reset();
    int found = 0, overridden = 0;
    for (int i = 0; i < kEventCount; ++i) {
        std::vector<std::string> files = FindVariants(overrideDir, (Event)i);
        if (!files.empty()) ++overridden;
        else files = FindVariants(builtInDir, (Event)i);
        files.erase(std::remove_if(files.begin(), files.end(), [](const std::string& f) { return !AudioEngine::Load(f); }), files.end());
        if (!files.empty()) ++found;
        s.Sched.SetVariantCount((Event)i, (int)files.size());
        s.Files[i] = std::move(files);
    }
    s.Ready = true;
    ApplyOutputVolume();
    Log::Info("Editor sounds: " + std::to_string(found) + " of " + std::to_string(kEventCount) + " events" +
              (overridden ? " (" + std::to_string(overridden) + " from the Drive set)" : std::string()) + ".");
}

void Shutdown() {
    for (auto& files : s.Files) {
        for (const std::string& f : files) AudioEngine::Unload(f);
        files.clear();
    }
    for (int i = 0; i < kEventCount; ++i) s.Sched.SetVariantCount((Event)i, 0);
    s.Ready = false;
}

Settings FromEditorSettings() {
    const EditorSettings& e = EditorSettings::Get();
    Settings v;
    v.Enabled = e.EditorSoundsEnabled;
    v.Master = std::clamp(e.EditorSoundsVolume, 0.0f, 1.0f);
    for (int c = 0; c < kCategoryCount; ++c) {
        if (c < (int)e.EditorSoundCategoryOn.size()) v.CategoryOn[c] = e.EditorSoundCategoryOn[c];
        if (c < (int)e.EditorSoundCategoryVolume.size()) v.CategoryVolume[c] = std::clamp(e.EditorSoundCategoryVolume[c], 0.0f, 1.0f);
    }
    return v;
}

void Update() {
    if (!s.Ready) return;
    const std::vector<LogEntry>& entries = Log::Entries();
    const unsigned long long newest = entries.empty() ? 0ull : entries.back().Seq;
    if (!s.LogPrimed) { // start-up's own errors and warnings aren't news
        s.LogPrimed = true;
        s.LastLogSeq = newest;
        s.QuietUntilMs = NowMs() + 3000.0;
        return;
    }
    if (newest <= s.LastLogSeq) return;
    size_t first = entries.size();
    while (first > 0 && entries[first - 1].Seq > s.LastLogSeq) --first;
    s.LastLogSeq = newest;
    if (NowMs() < s.QuietUntilMs) return;
    // One sound for the frame's most serious entry (the cooldowns spread a burst out).
    bool error = false, warning = false, success = false, info = false;
    for (size_t i = first; i < entries.size(); ++i) {
        const LogEntry& e = entries[i];
        if (e.Level == LogLevel::Error) error = true;
        else if (e.Level == LogLevel::Warning) warning = true;
        else if (e.Message.rfind("Hot reload: TartarusGame module reloaded", 0) == 0) success = true;
        else if (e.Message.rfind("Imported ", 0) == 0) info = true;
    }
    // A storm (something failing over and over) gets four alerts, then quiet until it has calmed down for ten seconds.
    if (error || warning) {
        const double now = NowMs();
        s.AlertStorm = now - s.LastAlertMs < 10000.0 ? s.AlertStorm + 1 : 0;
        s.LastAlertMs = now;
        if (s.AlertStorm >= 4) return;
    }
    if (error) Play(Event::NotifyError);
    else if (warning) Play(Event::NotifyWarning);
    else if (success) Play(Event::NotifySuccess);
    else if (info) Play(Event::NotifyInfo);
}

void Apply(const Settings& v) {
    s.Values = v;
    ApplyOutputVolume();
}
const Settings& Current() { return s.Values; }
void SetWindowFocused(bool focused) { s.Focused = focused; }

void Play(Event e, float screenX01) {
    if (!s.Ready) return;
    Voice v;
    if (!s.Sched.Decide(e, NowMs(), screenX01, s.Focused, s.Values, v)) return;
    const auto& files = s.Files[(int)e];
    if (v.Variant < 0 || v.Variant >= (int)files.size()) return;
    if (AudioEngine::PlayEditor(files[v.Variant], v.Gain, v.Pitch, v.Pan)) ++s.Played;
}

float LastItemScreenX() {
    if (!ImGui::GetCurrentContext()) return -1.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp || vp->Size.x <= 0.0f) return -1.0f;
    const float cx = (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f;
    return std::clamp((cx - vp->Pos.x) / vp->Size.x, 0.0f, 1.0f);
}

int PlayedCount() { return s.Played; }
int VariantCount(Event e) { return s.Sched.VariantCount(e); }
const std::vector<std::string>& Files(Event e) { return s.Files[std::clamp((int)e, 0, kEventCount - 1)]; }

} // namespace EditorSound
