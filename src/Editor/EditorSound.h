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
    float Master = 0.44f; // all on, low (the user's call; 25% up from the first 0.35)
    bool UsePremium = false; // the paid Drive set where an event has one (opt-in: audition it on the Sound Board first)
    std::array<bool, EditorSoundEvents::kCategoryCount> CategoryOn{true, true, true, true, true, true, true, true, true};
    std::array<float, EditorSoundEvents::kCategoryCount> CategoryVolume{1, 1, 1, 1, 1, 1, 1, 1, 1};
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

// Phase 2: the sounds read off ImGui's own state once a frame, so every widget gets them - built-in ImGui sliders and
// combos as much as the shared primitives, in the host and in modules alike (they share one ImGuiContext). The decision is
// this pure state machine over a per-frame snapshot (unit-tested); ObserveImGui fills the snapshot from the live context.
struct UiFrame {
    unsigned ActiveId = 0;      // the widget held / being edited
    bool Edited = false;        // ...changed its value this frame
    bool MouseDown = false, MouseReleased = false, EnterPressed = false;
    float MouseX01 = 0.5f;      // across the window
    float MouseDx = 0.0f, MouseDy = 0.0f; // px this frame
    unsigned HoveredId = 0;
    bool HoverInMenu = false;   // the hovered item is in a menu, popup, dropdown, the menu bar or the toolbar
    bool HoverInPickMenu = false; // ...in a menu / dropdown / context menu whose items are picks (not a modal)
    int PopupDepth = 0;         // open popups (menus, dropdowns, context menus, modals)
    bool DragActive = false;    // an in-editor drag & drop payload is held
    bool DropTargetHovered = false; // ...over a target that would take it
};
struct UiSound {
    Event E;
    float X01;
    float Pitch = 1.0f;
};
class UiObserver {
public:
    static constexpr float kTickPx = 7.0f;       // mouse travel per slider detent
    static constexpr float kPitchSpanPx = 500.0f; // net travel for the full pitch swing...
    static constexpr float kPitchSwing = 0.08f;   // ...of +-8%
    void Step(const UiFrame& f, std::vector<UiSound>& out);
    void Reset() { *this = UiObserver{}; }

private:
    UiFrame m_Prev;
    bool m_Primed = false;
    bool m_Sliding = false;
    unsigned m_SlideId = 0;
    float m_TickAccum = 0.0f, m_Net = 0.0f;
};
void ObserveImGui(); // host, once a frame before ImGui::Render

// The viewport's transform gizmo, which isn't an ImGui item: the slider sounds for its drag. A grab when it engages, a release when
// it lets go, and between them a tick for each snap step it lands on (snapping) or per stretch of mouse travel (free).
struct GizmoTracker {
    static constexpr float kFreeTickPx = 14.0f; // free drags tick half as densely as a slider
    bool Using = false;
    float TickAccum = 0.0f, Net = 0.0f;
    void Step(bool usingNow, bool moved, bool snapped, float mouseDx, float mouseDy, float x01, std::vector<UiSound>& out);
};
void GizmoFrame(bool usingNow, bool moved, bool snapped);

// The files of `event` in `dir`: <Name>.wav, else <Name>_1.wav, <Name>_2.wav ... in order.
std::vector<std::string> FindVariants(const std::string& dir, Event e);

// The service. Init finds and preloads every event's files: the override folder's (premium sounds fetched from the Drive)
// replace the built-in set's event by event. Missing files just mean that event is silent.
void Init(const std::string& builtInDir, const std::string& overrideDir = {});
void Shutdown();
void Apply(const Settings& s);
Settings FromEditorSettings();      // EditorSettings' saved values over the defaults
void ToEditorSettings(const Settings& s); // back into EditorSettings (the caller saves)
// The Sound Board: one variant of an event, now - no cooldown, focus or switch gets in the way (only the master volume).
bool Audition(Event e, int variant, bool premium = false);
bool IsOverride(Event e);           // it plays the Drive's premium set now
const std::vector<std::string>& BuiltInFiles(Event e);
const std::vector<std::string>& PremiumFiles(Event e); // empty when the Drive set has none for it (or isn't fetched)
int PremiumEventCount();
// Once a frame (host): plays the notification sounds for what was logged since the last call - errors, warnings, hot reloads
// and imports - with nothing for what was logged before the first call plus a short start-up grace.
void Update();
const Settings& Current();
void SetWindowFocused(bool focused);
void Play(Event e, float screenX01 = -1.0f, float pitchScale = 1.0f);
// Something played this frame (an action's own sound): the selection change it caused stays quiet, so Delete, Paste or Undo
// isn't followed by a Select / Deselect on top.
bool PlayedThisFrame();
// The action sound for an undo step's label (PushUndo): "Create Cube" -> Create, "Add Rigidbody" -> ComponentAdd... Count: none.
Event UndoLabelEvent(const std::string& label);
// The selection sounds: once a frame (host, before ObserveImGui), from the selection's primary entity and count.
void SelectionFrame(uint32_t primary, int count);
// The tool sounds, the same way: the active tool (-1 hand, 0..4 the gizmo modes, 5 measure), gizmo space and grid snap.
void ToolFrame(int tool, bool localSpace, bool snap);
// ...and the panels: a bit per dock panel that is open.
void PanelsFrame(uint32_t openMask);
// Screen x of the last ImGui item, as Play's screenX01 (needs a current ImGui frame).
float LastItemScreenX();
// For tests and the (Phase 3) Sound Board.
int PlayedCount();
int VariantCount(Event e);
const std::vector<std::string>& Files(Event e);

} // namespace EditorSound
