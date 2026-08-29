#pragma once
#include <string>

// Editor-only preferences — NOT scene data. These control how the editor UI itself behaves
// (independent of any particular scene) and persist across restarts in their own small file,
// the same role Unity's EditorPrefs plays relative to a project's actual assets.
//
// A plain Meyer's-singleton struct (a function-local static, returned by reference) rather than
// a heap-allocated singleton: single-threaded UI code, no dynamic-initialization-order concerns,
// and every call site just writes EditorSettings::Get().Field like a normal member access.
struct EditorSettings {
    // Master switch for every contextual tooltip/help-marker in the editor (Inspector fields,
    // Hierarchy rows, Console controls, Asset Browser, Toolbar Settings). Route all tooltip
    // calls through EditorUI::SetTooltip/HelpMarker (see EditorUIHelpers.h) rather than calling
    // ImGui::SetTooltip directly, so this one flag actually governs all of them.
    bool ShowTooltips = true;

    // Periodically re-saves the current scene to its own file while editing (only when there
    // are actually unsaved changes) — a safety net against a crash/force-quit losing work,
    // independent of the existing always-on "save on clean exit" behavior. Interval is in
    // minutes so the Settings UI can offer a plain, human-sized number rather than raw seconds.
    bool AutoSaveEnabled = true;
    float AutoSaveIntervalMinutes = 5.0f;

    // GameViewPanel's own preferences (see GameViewPanel::LoadSettings/SaveSettings) — kept here
    // rather than in a separate file so they persist through the same Load()/Save() call every
    // other editor preference already goes through. GameViewPresetWidth/Height are only
    // meaningful (nonzero) when the saved preset was a custom fixed resolution, not a built-in.
    bool GameViewMaximizeOnPlay = false;
    bool GameViewShowStats = true;
    std::string GameViewPresetLabel = "Free Aspect";
    int GameViewPresetWidth = 0;
    int GameViewPresetHeight = 0;

    // Asset Browser layout, in actual pixels (already DPI-scaled). 0 = "use the DPI-scaled
    // default" — EditorLayer::Init picks a sensible starting width/icon size on first run, then
    // the user's last splitter drag / icon-size slider position is written back here.
    float AssetBrowserTreeWidth = 0.0f;
    float AssetBrowserIconSize = 0.0f;

    static EditorSettings& Get() {
        static EditorSettings instance;
        return instance;
    }

    // Reads editor_prefs.json from the working directory into Get(), if present. Missing or
    // unparsable file silently keeps the compiled-in defaults above — first launch, or a
    // hand-deleted prefs file, is not an error.
    static void Load();

    // Writes the current Get() state to editor_prefs.json. Called immediately whenever a
    // preference changes (not batched/on-exit-only) so a crash or force-quit never loses a
    // just-made preference change.
    static void Save();

private:
    EditorSettings() = default;
};
