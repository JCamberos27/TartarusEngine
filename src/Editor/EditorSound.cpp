#include "EditorSound.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include "../Audio/AudioEngine.h"
#include "Log.h"
#include "EditorSettings.h"

namespace EditorSound {
namespace {

using EditorSoundEvents::kCategoryCount;
using EditorSoundEvents::kEventCount;

//                         name                category                 dB    cooldown jitter centred unfocused
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
    {"Hover",            Category::Hover,        -4.0f,  45.0f, 0.03f, false}, // levelled as a tick: sits under them
    {"SliderGrab",       Category::Sliders,       0.0f,  60.0f, 0.02f, false},
    {"SliderTick",       Category::Sliders,      -4.0f,  40.0f, 0.0f,  false},
    {"SliderRelease",    Category::Sliders,       0.0f,  60.0f, 0.02f, false},
    {"MenuOpen",         Category::Panels,        0.0f,  60.0f, 0.02f, false},
    {"MenuClose",        Category::Panels,        0.0f,  60.0f, 0.02f, false},
    {"DragStart",        Category::DragDrop,      0.0f, 150.0f, 0.02f, false},
    {"DropAccept",       Category::DragDrop,      0.0f, 150.0f, 0.0f,  false},
    {"DropReject",       Category::DragDrop,      0.0f, 150.0f, 0.0f,  false},
    {"Select",           Category::Objects,       0.0f,  60.0f, 0.03f, false},
    {"Deselect",         Category::Objects,       0.0f,  80.0f, 0.02f, false},
    {"Create",           Category::Objects,       0.0f, 120.0f, 0.02f, false},
    {"Duplicate",        Category::Objects,       0.0f, 100.0f, 0.02f, false},
    {"Delete",           Category::Objects,       0.0f, 120.0f, 0.02f, false},
    {"Copy",             Category::Objects,       0.0f,  80.0f, 0.02f, false},
    {"Paste",            Category::Objects,       0.0f,  80.0f, 0.02f, false},
    {"Rename",           Category::Objects,       0.0f,  80.0f, 0.02f, false},
    {"ComponentAdd",     Category::Objects,       0.0f, 100.0f, 0.02f, false},
    {"ComponentRemove",  Category::Objects,       0.0f, 100.0f, 0.02f, false},
    {"PrefabApply",      Category::Objects,       0.0f, 200.0f, 0.0f,  true},
    {"PrefabRevert",     Category::Objects,       0.0f, 200.0f, 0.0f,  true},
    {"PrefabEnter",      Category::Objects,       0.0f, 250.0f, 0.0f,  true},
    {"PrefabExit",       Category::Objects,       0.0f, 250.0f, 0.0f,  true},
    {"ToolSwitch",       Category::Tools,         0.0f,  60.0f, 0.0f,  false},
    {"SnapToggle",       Category::Tools,         0.0f,  60.0f, 0.02f, false},
    {"SpaceToggle",      Category::Tools,         0.0f,  60.0f, 0.02f, false},
    {"FrameSelected",    Category::Tools,         0.0f, 150.0f, 0.02f, true},
    {"ViewPreset",       Category::Tools,         0.0f,  80.0f, 0.02f, true},
    {"PanelOpen",        Category::Panels,        0.0f,  80.0f, 0.02f, false},
    {"PanelClose",       Category::Panels,        0.0f,  80.0f, 0.02f, false},
    {"TreeExpand",       Category::Panels,       -3.0f,  35.0f, 0.03f, false},
    {"TreeCollapse",     Category::Panels,       -3.0f,  35.0f, 0.03f, false},
    {"SceneOpen",        Category::Notifications, 0.0f, 500.0f, 0.0f,  true},
    {"BuildStart",       Category::Notifications, 0.0f, 500.0f, 0.0f,  true},
    {"BuildSuccess",     Category::Notifications, 0.0f, 500.0f, 0.0f,  true},
    {"BuildFail",        Category::Notifications, 0.0f, 500.0f, 0.0f,  true},
    {"ConsoleClear",     Category::Notifications,-2.0f, 300.0f, 0.0f,  true},
    {"AssetImport",      Category::System,       -2.0f, 400.0f, 0.0f,  true,  true},
    {"FolderEnter",      Category::Assets,        0.0f,  50.0f, 0.02f, false},
    {"FolderBack",       Category::Assets,        0.0f,  50.0f, 0.02f, false},
    {"AssetOpen",        Category::Assets,        0.0f, 200.0f, 0.0f,  false},
    {"AssetMove",        Category::Assets,        0.0f, 120.0f, 0.02f, false},
    {"AssetReimport",    Category::Assets,        0.0f, 200.0f, 0.0f,  false},
    {"SearchEmpty",      Category::Assets,       -2.0f, 400.0f, 0.0f,  false},
    {"FavoriteOn",       Category::Assets,        0.0f,  60.0f, 0.02f, false},
    {"FavoriteOff",      Category::Assets,        0.0f,  60.0f, 0.02f, false},
    {"ResetDefault",     Category::Objects,       0.0f, 120.0f, 0.0f,  false},
    {"Randomize",        Category::Objects,       0.0f,  90.0f, 0.03f, false},
    {"Equip",            Category::Objects,       0.0f,  80.0f, 0.02f, false},
    {"Unequip",          Category::Objects,       0.0f,  80.0f, 0.02f, false},
    {"StateAdd",         Category::Animation,     0.0f, 100.0f, 0.02f, false},
    {"StateDelete",      Category::Animation,     0.0f, 100.0f, 0.02f, false},
    {"TransitionLink",   Category::Animation,     0.0f,  80.0f, 0.02f, false},
    {"TransitionBreak",  Category::Animation,     0.0f,  80.0f, 0.02f, false},
    {"DefaultState",     Category::Animation,     0.0f, 120.0f, 0.0f,  false},
    {"LayerAdd",         Category::Animation,     0.0f, 100.0f, 0.02f, false},
    {"LayerRemove",      Category::Animation,     0.0f, 100.0f, 0.02f, false},
    {"ParamAdd",         Category::Animation,     0.0f, 100.0f, 0.02f, false},
    {"ParamRemove",      Category::Animation,     0.0f, 100.0f, 0.02f, false},
    {"LiveState",        Category::Animation,    -6.0f, 120.0f, 0.02f, false}, // can run continuously: well under the rest
    {"KeyAdd",           Category::Animation,     0.0f,  60.0f, 0.02f, false},
    {"KeyDelete",        Category::Animation,     0.0f,  60.0f, 0.02f, false},
    {"TangentMode",      Category::Animation,     0.0f,  60.0f, 0.02f, false},
    {"PresetApply",      Category::Animation,     0.0f, 150.0f, 0.0f,  false},
    {"ShaderReload",     Category::System,       -2.0f, 600.0f, 0.0f,  true,  true},
    {"SyncDone",         Category::System,       -2.0f, 600.0f, 0.0f,  true,  true},
    {"ModuleReload",     Category::System,        0.0f, 600.0f, 0.0f,  true,  true},
    {"Blocked",          Category::System,        0.0f, 250.0f, 0.0f,  false},
    {"ShortcutInvalid",  Category::System,       -2.0f, 250.0f, 0.0f,  true},
    {"GotoLine",         Category::System,        0.0f,  80.0f, 0.02f, false},
    {"Screenshot",       Category::System,        0.0f, 250.0f, 0.0f,  true},
    {"ResolutionChange", Category::System,        0.0f,  80.0f, 0.02f, false},
    {"ViewMaximize",     Category::System,        0.0f, 150.0f, 0.0f,  true},
    {"ViewRestore",      Category::System,        0.0f, 150.0f, 0.0f,  true},
    {"CursorLock",       Category::System,       -2.0f, 150.0f, 0.0f,  true},
    {"CursorRelease",    Category::System,       -2.0f, 150.0f, 0.0f,  true},
    // P7. Busy ones (fly speed, marquee, build progress) sit about 4 dB under the clicks; whispers (dirty, autosave) 8.
    {"BoxSelectStart",   Category::Tools,        -4.0f,  80.0f, 0.02f, false},
    {"BoxSelectEnd",     Category::Tools,        -4.0f,  80.0f, 0.0f,  false},
    {"HideOn",           Category::Objects,       0.0f,  50.0f, 0.02f, false},
    {"HideOff",          Category::Objects,       0.0f,  50.0f, 0.02f, false},
    {"LockOn",           Category::Objects,       0.0f,  50.0f, 0.02f, false},
    {"LockOff",          Category::Objects,       0.0f,  50.0f, 0.02f, false},
    {"FlySpeed",         Category::Tools,        -4.0f,  40.0f, 0.0f,  true},
    {"SnapGround",       Category::Tools,         0.0f, 120.0f, 0.02f, false},
    {"MeasurePoint",     Category::Tools,         0.0f,  60.0f, 0.02f, false},
    {"MeasureClear",     Category::Tools,         0.0f, 120.0f, 0.0f,  false},
    {"Reparent",         Category::Objects,       0.0f, 100.0f, 0.02f, false},
    {"Reorder",          Category::Objects,       0.0f,  80.0f, 0.02f, false},
    {"ExpandAllSweep",   Category::Panels,       -2.0f, 150.0f, 0.0f,  false},
    {"CollapseAllSweep", Category::Panels,       -2.0f, 150.0f, 0.0f,  false},
    {"Isolate",          Category::Panels,        0.0f, 200.0f, 0.0f,  false},
    {"DockAttach",       Category::Workflow,      0.0f, 120.0f, 0.02f, false},
    {"DockDetach",       Category::Workflow,      0.0f, 120.0f, 0.02f, false},
    {"TabReorder",       Category::Workflow,     -3.0f,  80.0f, 0.02f, false},
    {"LayoutApply",      Category::Workflow,      0.0f, 250.0f, 0.0f,  true},
    {"LayoutSave",       Category::Workflow,      0.0f, 250.0f, 0.0f,  true},
    {"ToastIn",          Category::Workflow,     -3.0f, 200.0f, 0.0f,  false},
    {"ToastOut",         Category::Workflow,     -4.0f, 120.0f, 0.0f,  false},
    {"DirtyCue",         Category::Workflow,     -8.0f, 500.0f, 0.0f,  true},
    {"Autosave",         Category::Workflow,     -8.0f,2000.0f, 0.0f,  true},
    {"RecoveryPrompt",   Category::Notifications, 0.0f, 500.0f, 0.0f,  true},
    {"TimeSlower",       Category::PlayMode,      0.0f,  60.0f, 0.0f,  true},
    {"TimeFaster",       Category::PlayMode,      0.0f,  60.0f, 0.0f,  true},
    {"TimeReset",        Category::PlayMode,      0.0f, 120.0f, 0.0f,  true},
    {"DebugOn",          Category::PlayMode,      0.0f,  80.0f, 0.0f,  true},
    {"DebugOff",         Category::PlayMode,      0.0f,  80.0f, 0.0f,  true},
    {"StatsOn",          Category::PlayMode,      0.0f,  80.0f, 0.0f,  true},
    {"StatsOff",         Category::PlayMode,      0.0f,  80.0f, 0.0f,  true},
    {"FieldCommit",      Category::Fields,       -3.0f,  60.0f, 0.02f, false},
    {"FieldCancel",      Category::Fields,       -5.0f,  60.0f, 0.02f, false}, // very soft
    {"PickerOpen",       Category::Fields,        0.0f,  80.0f, 0.02f, false},
    {"PickerClose",      Category::Fields,        0.0f,  80.0f, 0.02f, false},
    {"EyedropperArm",    Category::Fields,        0.0f,  80.0f, 0.02f, false},
    {"EyedropperPick",   Category::Fields,        0.0f, 100.0f, 0.0f,  false},
    {"SwatchPick",       Category::Fields,        0.0f,  50.0f, 0.03f, false},
    {"BindListen",       Category::Fields,        0.0f, 100.0f, 0.0f,  false},
    {"BindCaptured",     Category::Fields,        0.0f, 100.0f, 0.0f,  false},
    {"BindConflict",     Category::Fields,        0.0f, 200.0f, 0.0f,  false},
    {"SearchClear",      Category::Fields,        0.0f,  80.0f, 0.02f, false},
    {"FilesDropped",     Category::Assets,        0.0f, 300.0f, 0.0f,  false},
    {"MissingRefs",      Category::Notifications, 0.0f, 500.0f, 0.0f,  true},
    {"EditorStart",      Category::Notifications, 0.0f,2000.0f, 0.0f,  true},
    {"EditorExit",       Category::Notifications, 0.0f,2000.0f, 0.0f,  true},
    {"PackageSuccess",   Category::Notifications, 0.0f, 500.0f, 0.0f,  true,  true},
    {"PackageFail",      Category::Notifications, 0.0f, 500.0f, 0.0f,  true,  true},
    {"BuildProgress",    Category::System,       -4.0f, 150.0f, 0.0f,  true}, // focused only: System, not Notifications
    {"Digest",           Category::System,        0.0f,1000.0f, 0.0f,  true},
};

const char* kCategoryNames[kCategoryCount] = {"Clicks", "Sliders", "Hover",  "Panels",             "Drag & Drop", "Notifications",
                                              "Play Mode", "Objects", "Tools", "Assets", "Animation & Curves", "System",
                                              "Fields", "Workflow"};

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
    double LastErrorMs = -1e9; // the last error logged, sounded or not: a burst re-arms only after kErrorQuietMs of none
    int TailCount = 0;         // the newest entry's repeat count: an identical error again only bumps it, no new Seq
    std::array<std::vector<std::string>, kEventCount> Files;
    std::array<std::vector<std::string>, kEventCount> BuiltIn, Premium; // both sets, loaded; Files is the one in use
    std::array<bool, kEventCount> Override{};
    double LastBuildResultMs = -1e9;
    unsigned Frame = 0, LastPlayFrame = ~0u; // ObserveImGui counts frames; Play notes the one it last sounded in
    unsigned DropQuietFrame = ~0u - 8u;      // SuppressDropSound's frame
    unsigned PickerFrame = ~0u;              // the frame a colour picker sounded its own open / close
    int AwayDone = 0;                        // done chimes while unfocused
    double DigestAtMs = -1.0;                // a Digest is due at this time (OnFocusRegained), < 0: none
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
    // In the background only notifications and the "done" chimes speak.
    if (!windowFocused && info.Cat != Category::Notifications && !info.Unfocused) return false;
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

// Which set each event plays: the premium file where the user opted in and the Drive has one, else the built-in.
void SelectSets() {
    for (int i = 0; i < kEventCount; ++i) {
        s.Override[i] = s.Values.UsePremium && !s.Premium[i].empty();
        s.Files[i] = s.Override[i] ? s.Premium[i] : s.BuiltIn[i];
        s.Sched.SetVariantCount((Event)i, (int)s.Files[i].size());
    }
}

void Init(const std::string& builtInDir, const std::string& overrideDir) {
    s.Sched.Reset();
    auto load = [](std::vector<std::string> files) {
        files.erase(std::remove_if(files.begin(), files.end(), [](const std::string& f) { return !AudioEngine::Load(f); }), files.end());
        return files;
    };
    int found = 0, premium = 0;
    for (int i = 0; i < kEventCount; ++i) {
        s.BuiltIn[i] = load(FindVariants(builtInDir, (Event)i));
        s.Premium[i] = load(FindVariants(overrideDir, (Event)i));
        if (!s.BuiltIn[i].empty() || !s.Premium[i].empty()) ++found;
        if (!s.Premium[i].empty()) ++premium;
    }
    SelectSets();
    s.Ready = true;
    ApplyOutputVolume();
    Log::Info("Editor sounds: " + std::to_string(found) + " of " + std::to_string(kEventCount) + " events" +
              (premium ? " (" + std::to_string(premium) + " with a premium Drive version)" : std::string()) + ".");
}

void Shutdown() {
    for (auto* set : {&s.BuiltIn, &s.Premium})
        for (auto& files : *set) {
            for (const std::string& f : files) AudioEngine::Unload(f);
            files.clear();
        }
    for (auto& files : s.Files) files.clear();
    for (int i = 0; i < kEventCount; ++i) s.Sched.SetVariantCount((Event)i, 0);
    s.Ready = false;
}

Settings FromEditorSettings() {
    const EditorSettings& e = EditorSettings::Get();
    Settings v;
    v.Enabled = e.EditorSoundsEnabled;
    v.Master = std::clamp(e.EditorSoundsVolume, 0.0f, 1.0f);
    v.UsePremium = e.EditorSoundsPremium;
    for (int c = 0; c < kCategoryCount; ++c) {
        if (c < (int)e.EditorSoundCategoryOn.size()) v.CategoryOn[c] = e.EditorSoundCategoryOn[c];
        if (c < (int)e.EditorSoundCategoryVolume.size()) v.CategoryVolume[c] = std::clamp(e.EditorSoundCategoryVolume[c], 0.0f, 1.0f);
    }
    return v;
}

void ToEditorSettings(const Settings& v) {
    EditorSettings& e = EditorSettings::Get();
    e.EditorSoundsEnabled = v.Enabled;
    e.EditorSoundsVolume = std::clamp(v.Master, 0.0f, 1.0f);
    e.EditorSoundsPremium = v.UsePremium;
    e.EditorSoundCategoryOn.assign(v.CategoryOn.begin(), v.CategoryOn.end());
    e.EditorSoundCategoryVolume.assign(v.CategoryVolume.begin(), v.CategoryVolume.end());
}

bool Audition(Event e, int variant, bool premium) {
    if (!s.Ready) return false;
    const int i = std::clamp((int)e, 0, kEventCount - 1);
    const auto& files = premium ? s.Premium[i] : s.BuiltIn[i];
    if (variant < 0 || variant >= (int)files.size()) return false;
    const bool ok = AudioEngine::PlayEditor(files[variant], DbToGain(Info(e).GainDb));
    if (ok) ++s.Played;
    return ok;
}

bool IsOverride(Event e) { return s.Override[std::clamp((int)e, 0, kEventCount - 1)]; }
const std::vector<std::string>& BuiltInFiles(Event e) { return s.BuiltIn[std::clamp((int)e, 0, kEventCount - 1)]; }
const std::vector<std::string>& PremiumFiles(Event e) { return s.Premium[std::clamp((int)e, 0, kEventCount - 1)]; }
int PremiumEventCount() {
    int n = 0;
    for (const auto& f : s.Premium) n += f.empty() ? 0 : 1;
    return n;
}

void Update() {
    if (!s.Ready) return;
    if (s.DigestAtMs >= 0.0 && NowMs() >= s.DigestAtMs) { // after the focus edge's own reimport / log sounds settle
        s.DigestAtMs = -1.0;
        if (s.Focused && s.AwayDone > 0) Play(Event::Digest);
        s.AwayDone = 0;
    }
    const std::vector<LogEntry>& entries = Log::Entries();
    const unsigned long long newest = entries.empty() ? 0ull : entries.back().Seq;
    if (!s.LogPrimed) { // start-up's own errors and warnings aren't news
        s.LogPrimed = true;
        s.LastLogSeq = newest;
        s.QuietUntilMs = NowMs() + 3000.0;
        return;
    }
    const int tailCount = entries.empty() ? 0 : entries.back().Count;
    // The same error again (a per-frame failure) only bumps the newest entry's count: that is error activity too.
    const bool repeatedError = !entries.empty() && newest == s.LastLogSeq && tailCount > s.TailCount &&
                               entries.back().Level == LogLevel::Error;
    s.TailCount = tailCount;
    if (newest <= s.LastLogSeq && !repeatedError) return;
    size_t first = entries.size();
    while (first > 0 && entries[first - 1].Seq > s.LastLogSeq) --first;
    s.LastLogSeq = newest;
    bool error = repeatedError, reload = false;
    for (size_t i = first; i < entries.size(); ++i) {
        const LogEntry& e = entries[i];
        if (e.Level == LogLevel::Error) error = true;
        else if (e.Message.rfind("Hot reload: TartarusGame module reloaded", 0) == 0) reload = true;
    }
    const double now = NowMs();
    // An error burst sounds once: the next one only after kErrorQuietMs without any (Play mode scripts can error every frame).
    const bool errorArmed = now - s.LastErrorMs >= kErrorQuietMs;
    if (error) s.LastErrorMs = now;
    if (now < s.QuietUntilMs) return;
    // A build that just sounded its own result already said it: its log lines (the errors, the reload) stay quiet.
    if (now - s.LastBuildResultMs < 5000.0) return;
    if (error && errorArmed) Play(Event::NotifyError);
    else if (reload) Play(Event::ModuleReload);
}

void Apply(const Settings& v) {
    const bool setChanged = v.UsePremium != s.Values.UsePremium;
    s.Values = v;
    ApplyOutputVolume();
    if (setChanged && s.Ready) SelectSets();
}
const Settings& Current() { return s.Values; }
void SetWindowFocused(bool focused) { s.Focused = focused; }

bool IsDoneChime(Event e) {
    switch (e) {
    case Event::AssetImport: case Event::ShaderReload: case Event::SyncDone: case Event::ModuleReload:
    case Event::BuildSuccess: case Event::BuildFail: case Event::PackageSuccess: case Event::PackageFail: return true;
    default: return false;
    }
}
void OnFocusRegained() {
    if (s.AwayDone > 0) s.DigestAtMs = NowMs() + 300.0;
}
int AwayDoneCount() { return s.AwayDone; }
void SuppressDropSound() { s.DropQuietFrame = s.Frame; }

void Play(Event e, float screenX01, float pitchScale) {
    if (!s.Ready) return;
    if (!s.Focused && IsDoneChime(e)) ++s.AwayDone;
    Voice v;
    if (!s.Sched.Decide(e, NowMs(), screenX01, s.Focused, s.Values, v)) return;
    v.Pitch *= pitchScale;
    const auto& files = s.Files[(int)e];
    if (v.Variant < 0 || v.Variant >= (int)files.size()) return;
    if (AudioEngine::PlayEditor(files[v.Variant], v.Gain, v.Pitch, v.Pan)) ++s.Played;
    s.LastPlayFrame = s.Frame;
    if (e == Event::PickerOpen || e == Event::PickerClose) s.PickerFrame = s.Frame;
    if (e == Event::BuildSuccess || e == Event::BuildFail || e == Event::PackageSuccess || e == Event::PackageFail)
        s.LastBuildResultMs = NowMs();
}

bool PlayedThisFrame() { return s.LastPlayFrame == s.Frame; }

void UiObserver::Step(const UiFrame& f, std::vector<UiSound>& out) {
    if (!m_Primed) { // the first frame only sets the baseline
        m_Primed = true;
        m_Prev = f;
        return;
    }
    const UiFrame& p = m_Prev;
    // Sliders, drag fields, colour pickers: a held widget whose value changes under a pressed mouse. Buttons and checkboxes
    // never edit while the mouse is down (they act on release), and typing has no mouse down, so neither reads as a slide.
    if (!m_Sliding && f.ActiveId != 0 && f.Edited && f.MouseDown) {
        m_Sliding = true;
        m_SlideId = f.ActiveId;
        m_TickAccum = m_Net = 0.0f;
        out.push_back({Event::SliderGrab, f.MouseX01});
    } else if (m_Sliding && f.ActiveId == m_SlideId) {
        if (f.Edited) {
            m_TickAccum += std::fabs(f.MouseDx) + std::fabs(f.MouseDy);
            m_Net += f.MouseDx - f.MouseDy; // right and up raise the pitch
            if (m_TickAccum >= kTickPx) {
                m_TickAccum = std::fmod(m_TickAccum, kTickPx);
                const float pitch = 1.0f + std::clamp(m_Net / kPitchSpanPx, -1.0f, 1.0f) * kPitchSwing;
                out.push_back({Event::SliderTick, f.MouseX01, pitch});
            }
        }
    } else if (m_Sliding) { // let go (or focus moved on)
        m_Sliding = false;
        out.push_back({Event::SliderRelease, f.MouseX01});
    }

    // Text / number fields: an edit confirmed with Enter, or abandoned with Escape. Clicking away and Tab stay silent (as does a
    // field left without an edit). ImGui drops the active id inside the widget on Enter / Escape, while the key still reads pressed.
    bool textSound = false;
    if (p.TextActive && f.ActiveId == p.ActiveId) m_TextEdited = m_TextEdited || f.Edited;
    else if (p.TextActive) { // the field let go this frame
        if (m_TextEdited && (f.EnterPressed || f.EscPressed)) {
            out.push_back({f.EnterPressed ? Event::FieldCommit : Event::FieldCancel, f.MouseX01});
            textSound = true;
        }
        m_TextEdited = false;
    }
    if (f.TextActive && f.ActiveId != p.ActiveId) m_TextEdited = f.Edited; // a field newly typed into

    // Popups: one more is a menu opening; one fewer is a pick (released on / Enter in it) or a close.
    if (f.PopupDepth > p.PopupDepth) out.push_back({Event::MenuOpen, f.MouseX01});
    else if (f.PopupDepth < p.PopupDepth && !textSound)
        out.push_back({(p.HoverInPickMenu && p.HoveredId != 0 && (f.MouseReleased || f.EnterPressed)) ? Event::Click : Event::MenuClose,
                       f.MouseX01});

    // Hover: a new item under a moving cursor, only in menus, dropdowns, the menu bar and the toolbar, never while a button is down.
    if (f.HoverInMenu && f.HoveredId != 0 && f.HoveredId != p.HoveredId && !f.MouseDown && (f.MouseDx != 0.0f || f.MouseDy != 0.0f))
        out.push_back({Event::Hover, f.MouseX01});

    // Drag & drop: picked up; dropped where the last frame had a target that would take it, or not.
    if (f.DragActive && !p.DragActive) out.push_back({Event::DragStart, f.MouseX01});
    else if (!f.DragActive && p.DragActive) out.push_back({p.DropTargetHovered ? Event::DropAccept : Event::DropReject, f.MouseX01});

    m_Prev = f;
}

void GizmoTracker::Step(bool usingNow, bool moved, bool snapped, float dx, float dy, float x01, std::vector<UiSound>& out) {
    if (usingNow && !Using) {
        TickAccum = Net = 0.0f;
        out.push_back({Event::SliderGrab, x01});
    } else if (!usingNow && Using) {
        out.push_back({Event::SliderRelease, x01});
    } else if (usingNow) {
        Net += dx - dy;
        const float pitch = 1.0f + std::clamp(Net / UiObserver::kPitchSpanPx, -1.0f, 1.0f) * UiObserver::kPitchSwing;
        if (snapped) {
            if (moved) out.push_back({Event::SliderTick, x01, pitch}); // landed on the next step
        } else if (moved) {
            TickAccum += std::fabs(dx) + std::fabs(dy);
            if (TickAccum >= kFreeTickPx) {
                TickAccum = std::fmod(TickAccum, kFreeTickPx);
                out.push_back({Event::SliderTick, x01, pitch});
            }
        }
    }
    Using = usingNow;
}

void GizmoFrame(bool usingNow, bool moved, bool snapped) {
    static GizmoTracker tracker;
    static std::vector<UiSound> sounds;
    if (!s.Ready || !ImGui::GetCurrentContext()) return;
    const ImGuiIO& io = ImGui::GetIO();
    float x = -1.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp && vp->Size.x > 0.0f && ImGui::IsMousePosValid()) x = std::clamp((io.MousePos.x - vp->Pos.x) / vp->Size.x, 0.0f, 1.0f);
    const bool valid = ImGui::IsMousePosValid() && ImGui::IsMousePosValid(&io.MousePosPrev);
    sounds.clear();
    tracker.Step(usingNow, moved, snapped, valid ? io.MouseDelta.x : 0.0f, valid ? io.MouseDelta.y : 0.0f, x, sounds);
    for (const UiSound& u : sounds) Play(u.E, u.X01, u.Pitch);
}

Event UndoLabelEvent(const std::string& label) {
    auto starts = [&](const char* p) { return label.rfind(p, 0) == 0; };
    if (starts("Create ") || starts("Place ") || starts("New ")) return Event::Create;
    if (starts("Add ")) return Event::ComponentAdd;
    if (starts("Remove ")) return Event::ComponentRemove;
    if (starts("Paste ")) return Event::Paste; // component pastes ("Paste Rigidbody"...); the entity Paste plays its own
    if (starts("Apply to Prefab")) return Event::PrefabApply;
    if (starts("Revert to Prefab")) return Event::PrefabRevert;
    if (starts("Delete ")) return Event::Delete;
    if (starts("Duplicate ")) return Event::Duplicate;
    if (starts("Rename ")) return Event::Rename;
    if (starts("Move Asset") || starts("Move Folder")) return Event::AssetMove; // not "Move Vertex" and the like
    if (starts("Reset ")) return Event::ResetDefault;
    if (starts("Reimport ")) return Event::AssetReimport;
    if (starts("Align ")) return Event::ViewPreset;
    return Event::Count;
}

void SelectionFrame(uint32_t primary, int count) {
    static bool primed = false;
    static uint32_t prevPrimary = 0;
    static int prevCount = 0;
    const bool changed = primary != prevPrimary || count != prevCount;
    if (changed && primed && !PlayedThisFrame()) {
        float x01 = -1.0f;
        if (ImGui::GetCurrentContext() && ImGui::GetIO().DisplaySize.x > 0.0f)
            x01 = std::clamp(ImGui::GetIO().MousePos.x / ImGui::GetIO().DisplaySize.x, 0.0f, 1.0f);
        Play(count > 0 ? Event::Select : Event::Deselect, x01);
    }
    primed = true;
    prevPrimary = primary;
    prevCount = count;
}

void PanelsFrame(uint32_t openMask) {
    static bool primed = false;
    static uint32_t prev = 0;
    const uint32_t opened = openMask & ~prev, closed = prev & ~openMask;
    // One panel at a time is a user's toggle; several at once is a layout being applied, which stays quiet. A toolbar toggle
    // already clicked this frame.
    auto single = [](uint32_t m) { return m != 0 && (m & (m - 1)) == 0; };
    if (primed && !PlayedThisFrame() && (opened | closed) != 0 && single(opened | closed))
        Play(opened ? Event::PanelOpen : Event::PanelClose);
    primed = true;
    prev = openMask;
}

void ToolFrame(int tool, bool localSpace, bool snap) {
    static bool primed = false;
    static int prevTool = 0;
    static bool prevLocal = false, prevSnap = false;
    if (primed) {
        // Each tool its own step up (hand -1 ... measure 5), about a semitone apart, so the ear learns which is which.
        if (tool != prevTool) Play(Event::ToolSwitch, -1.0f, std::pow(2.0f, (float)(std::clamp(tool, -1, 5) - 0) / 12.0f));
        if (localSpace != prevLocal) Play(Event::SpaceToggle, -1.0f, localSpace ? 1.06f : 1.0f);
        if (snap != prevSnap) Play(Event::SnapToggle, -1.0f, snap ? 1.0f : 0.89f);
    }
    primed = true;
    prevTool = tool;
    prevLocal = localSpace;
    prevSnap = snap;
}

void ObserveImGui() {
    struct NextFrame { ~NextFrame() { ++s.Frame; } } nextFrame; // the frame ends here, whatever returns early
    static UiObserver observer;
    static std::vector<UiSound> sounds;
    if (!s.Ready || !ImGui::GetCurrentContext()) return;
    const ImGuiContext& g = *GImGui;
    UiFrame f;
    f.ActiveId = g.ActiveId;
    f.Edited = g.ActiveIdHasBeenEditedThisFrame;
    f.MouseDown = g.IO.MouseDown[0];
    f.MouseReleased = g.IO.MouseReleased[0];
    f.EnterPressed = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp && vp->Size.x > 0.0f && ImGui::IsMousePosValid())
        f.MouseX01 = std::clamp((g.IO.MousePos.x - vp->Pos.x) / vp->Size.x, 0.0f, 1.0f);
    if (ImGui::IsMousePosValid() && ImGui::IsMousePosValid(&g.IO.MousePosPrev)) {
        f.MouseDx = g.IO.MouseDelta.x;
        f.MouseDy = g.IO.MouseDelta.y;
    }
    f.HoveredId = g.HoveredId;
    if (const ImGuiWindow* w = g.HoveredWindow) {
        // Menus, dropdowns and context menus (not modals: their buttons click for themselves) are where an item is a pick.
        f.HoverInPickMenu = (w->Flags & ImGuiWindowFlags_ChildMenu) != 0 ||
                            ((w->Flags & ImGuiWindowFlags_Popup) != 0 && (w->Flags & ImGuiWindowFlags_Modal) == 0);
        f.HoverInMenu = f.HoverInPickMenu || std::strcmp(w->Name, "##MainMenuBar") == 0 || std::strstr(w->Name, "##Toolbar") != nullptr;
    }
    f.PopupDepth = g.OpenPopupStack.Size;
    f.DragActive = g.DragDropActive && (g.DragDropSourceFlags & ImGuiDragDropFlags_SourceExtern) == 0;
    f.DropTargetHovered = f.DragActive && g.DragDropAcceptIdPrev != 0;
    f.TextActive = g.ActiveId != 0 && (g.InputTextState.ID == g.ActiveId || g.TempInputId == g.ActiveId);
    f.EscPressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    sounds.clear();
    observer.Step(f, sounds);
    // A menu pick that ran an action with its own sound this frame (Create, Paste, Add Component...) doesn't click as well, nor does
    // a field whose Enter ran one (Rename, a rejected name's Blocked).
    const bool acted = PlayedThisFrame();
    const bool dropQuiet = s.Frame - s.DropQuietFrame <= 1u;
    for (const UiSound& u : sounds) {
        if (acted && (u.E == Event::Click || u.E == Event::FieldCommit || u.E == Event::FieldCancel)) continue;
        if (dropQuiet && (u.E == Event::DropAccept || u.E == Event::DropReject)) continue;
        if (s.PickerFrame == s.Frame && (u.E == Event::MenuOpen || u.E == Event::MenuClose)) continue; // its popup, said already
        Play(u.E, u.X01, u.Pitch);
    }
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
