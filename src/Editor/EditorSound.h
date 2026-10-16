#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "EditorSoundEvents.h"
// The editor's UI sounds: a soft, glassy sound language over the editor's own controls (clicks, toggles, Play mode,
// notifications), on AudioEngine's editor output - outside the game's mix, unaffected by its Mute and master volume.
//
// What keeps a UI soundscape from tiring: variants played round-robin without an immediate repeat and a few percent of pitch
// jitter, a cooldown per event, a quieter take on rapid repeats, a subtle pan towards where on screen the control is, and only
// notifications while the editor window is in the background.
namespace EditorSound {

using EditorSoundEvents::Category;
using EditorSoundEvents::Event;

struct EventInfo {
    const char* Name;     // file stem
    Category Cat;
    float GainDb;         // trim on top of the file's own level
    float CooldownMs;     // a second trigger sooner than this is dropped
    float PitchJitter;    // +- fraction (0.03 = 3%)
    bool Centred;         // never panned (notifications, Play mode)
};
const EventInfo& Info(Event e);
const char* CategoryName(Category c);

struct Settings {
    bool Enabled = true;
    float Master = 0.35f; // all on, low (the user's call)
    std::array<bool, EditorSoundEvents::kCategoryCount> CategoryOn{true, true, true, true, true, true, true};
    std::array<float, EditorSoundEvents::kCategoryCount> CategoryVolume{1, 1, 1, 1, 1, 1, 1};
};

// The decision half, no audio: whether a trigger plays and how. Deterministic for a given seed (unit-tested).
struct Voice {
    int Variant = 0;
    float Gain = 1.0f;  // linear, includes the category volume (not Master: that is the output's volume)
    float Pitch = 1.0f;
    float Pan = 0.0f;   // -1..1
};
class Scheduler {
public:
    static constexpr float kRepeatWindowMs = 160.0f; // retriggers inside this are a "rapid repeat"
    static constexpr float kRepeatStepDb = -1.5f;    // each rapid repeat this much quieter...
    static constexpr float kRepeatFloorDb = -6.0f;   // ...down to here
    static constexpr float kPanWidth = 0.25f;        // a control at the window's edge pans this far

    explicit Scheduler(uint32_t seed = 0x5eedu) : m_Rng(seed ? seed : 1u) {}
    void SetVariantCount(Event e, int count) { m_Variants[(int)e] = count; }
    int VariantCount(Event e) const { return m_Variants[(int)e]; }
    // screenX01: where the control is across the window (0 = left edge, 1 = right), < 0 = centred.
    bool Decide(Event e, double nowMs, float screenX01, bool windowFocused, const Settings& s, Voice& out);
    void Reset();

private:
    float NextUnit(); // 0..1
    uint32_t m_Rng;
    std::array<int, EditorSoundEvents::kEventCount> m_Variants{};
    std::array<double, EditorSoundEvents::kEventCount> m_LastMs{};
    std::array<bool, EditorSoundEvents::kEventCount> m_Played{};
    std::array<int, EditorSoundEvents::kEventCount> m_LastVariant{};
    std::array<int, EditorSoundEvents::kEventCount> m_Repeats{};
};

// The files of `event` in `dir`: <Name>.wav, else <Name>_1.wav, <Name>_2.wav ... in order.
std::vector<std::string> FindVariants(const std::string& dir, Event e);

// The service. Init finds and preloads every event's files: the override folder's (premium sounds fetched from the Drive)
// replace the built-in set's event by event. Missing files just mean that event is silent.
void Init(const std::string& builtInDir, const std::string& overrideDir = {});
void Shutdown();
void Apply(const Settings& s);
Settings FromEditorSettings();      // EditorSettings' saved values over the defaults
// Once a frame (host): plays the notification sounds for what was logged since the last call - errors, warnings, hot reloads
// and imports - with nothing for what was logged before the first call plus a short start-up grace.
void Update();
const Settings& Current();
void SetWindowFocused(bool focused);
void Play(Event e, float screenX01 = -1.0f);
// Screen x of the last ImGui item, as Play's screenX01 (needs a current ImGui frame).
float LastItemScreenX();
// For tests and the (Phase 3) Sound Board.
int PlayedCount();
int VariantCount(Event e);
const std::vector<std::string>& Files(Event e);

} // namespace EditorSound
