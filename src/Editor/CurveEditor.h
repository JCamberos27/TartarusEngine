#pragma once

#include "Curve.h"

#include <imgui.h>
#include <functional>
#include <map>
#include <memory>
#include <string>

// Shared curve controls: previews open independent, dockable editor windows. The canvas offers
// multi-selection, key/tangent editing, clipboard, snapping, transforms and playback scrubbing.
// AssetScope binds previews to files; committed edits use the editor's global history.
namespace CurveEditor {

struct Options {
    float TimeMin = 0.0f;
    float TimeMax = 1.0f;
    const char* ValueFormat = "%.3f";
    // Amplitude the presets use when the curve is flat (otherwise its current peak).
    float PresetAmplitude = 1.0f;
    ImU32 Color = 0; // 0 = the style's plot colour
    const Curve* Default = nullptr; // offered as "Reset to default" in the presets menu
    const char* Title = nullptr;
    bool LinearFallback = false; // legacy animator keys contain time/value only
    bool TimeInSeconds = false; // normalized curves keep their fixed 0..1 domain
    bool ExternalHistory = false; // asset windows use global undo; standalone canvases use local undo
};

struct AssetSource;
// Register asset-owned curves while drawing their Inspector. Windows use file-backed paths,
// so they remain editable after changing the Inspector selection and never keep member pointers.
class AssetScope {
public:
    AssetScope(const std::string& path, const std::string& canonical,
               std::function<bool(const std::string&,std::string&)> validate,
               std::function<void(const std::string&)> onCommit = {});
    ~AssetScope();
    AssetScope(const AssetScope&) = delete;
    AssetScope& operator=(const AssetScope&) = delete;
    void Bind(const Curve& curve, const std::string& jsonPointer, const std::string& title,const Options& options = {});
    std::shared_ptr<AssetSource> Source;
    struct Field { std::string Pointer,Title; };
    std::map<const Curve*,Field> Fields;
private:
    AssetScope* Previous = nullptr;
};

// Read-only preview + Edit Curve button. Bound windows save directly to their asset; unbound
// windows return committed edits to the caller on its next Draw. New controls should bind assets.
bool Draw(const char* id, Curve& curve, const ImVec2& size, const Options& options = {});
// Scene-owned curves use stable identity and checked callbacks, so changing selection
// or removing an emitter cannot leave a curve window writing through a stale pointer.
bool DrawBound(const char* id, Curve& curve, const ImVec2& size, const Options& options,
               const std::string& identity,
               std::function<bool(Curve&,std::string&)> read,
               std::function<bool(const Curve&,std::string&)> write);
// Explicit canvas API for a curve editor window; ordinary curve controls use Draw previews.
bool DrawCanvas(const char* id, Curve& curve, const ImVec2& size, const Options& options = {});
void DrawWindows(const std::function<void()>& undo = {},const std::function<void()>& redo = {},
                 bool canUndo = false,bool canRedo = false);
// A read-only thumbnail. Returns true on double-click to open a full editor.
bool Preview(const char* id,const Curve& curve,const ImVec2& size,const Options& options = {});
// Scene shortcuts yield while a curve window has focus or its tools own the mouse.
bool OwnsKeyboard();

} // namespace CurveEditor
