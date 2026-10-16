#pragma once
#include <cstdint>
// The editor's UI sound events (EditorSound). Dependency-free so the shared UI primitives (EditorUIPrimitives.h), which reloadable
// editor modules compile too, can name them. Each event's file is resources/EditorSounds/<Name>.wav, or <Name>_1.wav, <Name>_2.wav...
// for variants. Values are stable: new events go before Count.
namespace EditorSoundEvents {

enum class Event : uint8_t {
    // Clicks & toggles
    Click = 0,         // an ordinary button
    ClickConfirm,      // the one confirming action of a dialog (PrimaryButton): rises
    ClickCancel,       // cancel / close / dismiss: falls
    ClickDestructive,  // delete, remove: lower and duller
    ToggleOn,
    ToggleOff,
    TabSelect,
    // Play mode
    PlayEnter,
    PlayExit,
    Pause,
    Resume,
    Step,
    // Notifications
    NotifyInfo,
    NotifySuccess,
    NotifyWarning,
    NotifyError,
    Save,
    Undo,
    Redo,
    // Phase 2: read off ImGui's own state once a frame (EditorSound::ObserveImGui), so every widget gets them
    Hover,             // a new item under the cursor in a menu, dropdown or the toolbar
    SliderGrab,        // a slider / drag field / colour picker starts changing under the mouse
    SliderTick,        // ...a detent as it moves (pitch follows the direction)
    SliderRelease,     // ...let go
    MenuOpen,          // a popup, menu, dropdown or modal opens
    MenuClose,         // ...closes without a pick
    DragStart,         // a drag & drop payload is picked up
    DropAccept,        // ...dropped on a target that takes it
    DropReject,        // ...let go anywhere else
    Count
};

// Preferences groups: each has its own toggle and volume.
enum class Category : uint8_t { Clicks = 0, Sliders, Hover, Panels, DragDrop, Notifications, PlayMode, Count };

constexpr int kEventCount = (int)Event::Count;
constexpr int kCategoryCount = (int)Category::Count;

// This binary's way to EditorSound. The host installs EditorSound::Play here; a reloadable module installs the host's
// EditorModuleHostAPI::PlayUISound on every Draw (each binary has its own copy of these function-local statics). Unset: silent.
using PlayFn = void (*)(int event, float screenX01);
inline PlayFn& PlaySlot() {
    static PlayFn fn = nullptr;
    return fn;
}
inline void PlayViaSlot(Event e, float screenX01 = -1.0f) {
    if (PlayFn fn = PlaySlot()) fn((int)e, screenX01);
}

// While one is alive the shared primitives' generic click sounds are held back: for a control whose action plays its own,
// more meaningful sound (the toolbar's Save, Undo, Pause...), so it isn't heard twice.
inline int& SuppressDepth() {
    static int depth = 0;
    return depth;
}
struct ScopedClickSilence {
    ScopedClickSilence() { ++SuppressDepth(); }
    ~ScopedClickSilence() { --SuppressDepth(); }
    ScopedClickSilence(const ScopedClickSilence&) = delete;
    ScopedClickSilence& operator=(const ScopedClickSilence&) = delete;
};

} // namespace EditorSoundEvents
