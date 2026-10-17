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
    // Actions: played by the editor operation itself (once per action, however many entities it touched)
    Select,            // the selection changes to something (Hierarchy click, viewport pick, Select All...)
    Deselect,          // ...to nothing
    Create,            // a new entity
    Duplicate,
    Delete,            // entities deleted
    Copy,
    Paste,
    Rename,
    ComponentAdd,
    ComponentRemove,
    PrefabApply,       // instance overrides pushed to the prefab asset
    PrefabRevert,      // ...thrown away
    PrefabEnter,       // prefab editing mode opens
    PrefabExit,
    ToolSwitch,        // move / rotate / scale / ... (pitch rises with the tool)
    SnapToggle,
    SpaceToggle,       // local / world
    FrameSelected,     // F: the camera frames the selection
    ViewPreset,        // the camera snaps to a view (top, front, ...)
    PanelOpen,         // a dock panel opened from the Window menu
    PanelClose,
    TreeExpand,        // a tree node (Hierarchy, Inspector header, folders) opens
    TreeCollapse,
    SceneOpen,
    BuildStart,
    BuildSuccess,
    BuildFail,
    ConsoleClear,
    AssetImport,       // an import batch finished (one of the "done" chimes, System)
    // P6. Assets
    FolderEnter,       // the Asset Browser goes into a folder
    FolderBack,        // ...back / forward through its history
    AssetOpen,         // an asset opens in its editor (Animator, Script IDE)
    AssetMove,         // assets / folders moved into another folder
    AssetReimport,
    SearchEmpty,       // a search that just stopped finding anything (Asset Browser, Script IDE)
    FavoriteOn,
    FavoriteOff,
    // Objects: Inspector and the specialist editors
    ResetDefault,      // a component, field or section back to its defaults
    Randomize,         // the character outfit dice
    Equip,             // an outfit item put on
    Unequip,           // ...taken off
    // Animation & Curves
    StateAdd,          // Animator: a state added
    StateDelete,
    TransitionLink,    // ...a transition made
    TransitionBreak,   // ...removed
    DefaultState,      // ...the entry state changed
    LayerAdd,
    LayerRemove,
    ParamAdd,
    ParamRemove,
    LiveState,         // ...in Play, the watched state machine moved to another state
    KeyAdd,            // Curve editor: keys added
    KeyDelete,
    TangentMode,
    PresetApply,       // a preset applied (curves, sky, outfit, weapon)
    // System
    ShaderReload,      // shaders recompiled after an edit ("done" chime)
    SyncDone,          // changes made outside the editor picked up ("done" chime)
    ModuleReload,      // a code module hot-reloaded ("done" chime)
    Blocked,           // an action that can't be done (illegal parent, read-only file...)
    ShortcutInvalid,   // Ctrl / Alt + a key that does nothing here
    GotoLine,          // Script IDE
    Screenshot,
    ResolutionChange,  // Game view aspect / resolution preset
    ViewMaximize,      // Game view maximised (or maximise on Play)
    ViewRestore,
    CursorLock,        // Play takes the mouse
    CursorRelease,     // ...gives it back
    Count
};

// Preferences groups: each has its own toggle and volume.
// Appended only: saved settings are per category index.
enum class Category : uint8_t {
    Clicks = 0, Sliders, Hover, Panels, DragDrop, Notifications, PlayMode, Objects, Tools, Assets, Animation, System, Count
};

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
