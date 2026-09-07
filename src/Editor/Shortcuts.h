#pragma once
#include <imgui.h>
#include <cstdint>
#include <string>
#include <vector>

// Central editor keyboard-shortcut registry + dispatcher (#236 F — Shortcuts Manager).
//
// Replaces the scattered `if (io.KeyCtrl && ImGui::IsKeyPressed(...))` sites in EditorLayer
// with a named table and one edge-triggered query: Shortcuts::Triggered("editor.undo").
// User overrides persist to project/shortcuts.json — content-adjacent, like layers.json /
// settings.json — because a shortcut sheet is a per-project preference, not per-user editor
// chrome. contexts scope a binding so viewport tool letters (Q/W/E/R/T/Y) don't fire while
// the Hierarchy has keyboard focus for type-to-select.
namespace Shortcuts {

// Bit flags: BeginFrame() gets an OR of the contexts active this frame. Global is always set
// unless input is fully suppressed (text field focused / game owns input -> pass 0).
enum Context : std::uint32_t {
    Ctx_Global    = 1u << 0,
    Ctx_Viewport  = 1u << 1,
    Ctx_Hierarchy = 1u << 2,
    Ctx_Project   = 1u << 3, // Asset Browser
    Ctx_Inspector = 1u << 4,
    // Application-level keys evaluated in the main loop (play/pause/step/maximize, window
    // fullscreen) via TriggeredGlfw() — before an ImGui frame exists. BeginFrame() never sets
    // this bit, so the editor-side Triggered() never fires them.
    Ctx_App       = 1u << 5,
};

// One key + modifiers, optionally preceded by a prefix key to form a two-key sequence
// (the UI calls this a "sequence"; the struct keeps the classic name): press the prefix,
// release, then press the key within kChordTimeoutSeconds. Modifiers on the main key must
// match exactly (Ctrl+D never fires for Ctrl+Shift+D).
struct Chord {
    ImGuiKey Key        = ImGuiKey_None;
    bool     Ctrl = false, Shift = false, Alt = false, Super = false;
    ImGuiKey PrefixKey  = ImGuiKey_None; // ImGuiKey_None => plain single-key binding
    bool     PrefixCtrl = false, PrefixShift = false, PrefixAlt = false;

    bool IsBound()   const { return Key != ImGuiKey_None; }
    bool HasPrefix() const { return PrefixKey != ImGuiKey_None; }
    bool operator==(const Chord& o) const;
    bool operator!=(const Chord& o) const { return !(*this == o); }
};

struct Shortcut {
    std::string   Id;
    std::string   Label;
    std::uint32_t Ctx = Ctx_Global; // a single Context bit
    Chord         Default;
    Chord         Current;
    bool          Overridden = false; // Current != Default (from json, or set in Preferences)
};

// Registers the builtin table, then applies project/shortcuts.json. Call once at editor init.
void Init();
void Load();  // re-apply overrides from disk (defaults already registered by Init)
void Save();  // write only the overridden entries

// Per-frame, before any Triggered() call. `contextMask` = Ctx_Global | <focused panel bit>.
// Pass 0 to suppress everything (text input focused, game owns keyboard) — also clears any
// half-entered chord prefix.
void BeginFrame(std::uint32_t contextMask);

// True once, on the frame the (possibly two-key) chord completes while its context is active.
bool Triggered(const char* id);

// GLFW-input variant for Ctx_App shortcuts the main loop evaluates before an ImGui frame
// exists (F1-F4 play controls, F11 fullscreen). Polls Input:: directly; edge-triggered via
// Input::IsKeyPressed. Single-chord only — a sequence's prefix is ignored here.
bool TriggeredGlfw(const char* id);

// --- registry access, for the Preferences > Shortcuts editor --------------------------------
const std::vector<Shortcut>& All();
Shortcut*   Find(const char* id);
std::string ToString(const Chord&);                  // "Ctrl+Shift+S", "G  S", "(unbound)"
void        SetChord(const char* id, const Chord&);  // updates Current + Overridden; no auto-save
void        ResetToDefault(const char* id);
void        ResetAllToDefault();
// ids of other shortcuts whose Current chord collides with `chord` in an overlapping context
// (Ctx_Global overlaps every context). Excludes `selfId`.
std::vector<std::string> Conflicts(const char* selfId, const Chord& chord);

// Reads whatever key/modifiers are pressed THIS frame into `out` (single chord, no prefix).
// Returns false until a non-modifier key goes down. For the press-to-bind capture box.
bool CaptureChord(Chord& out);

// Human name for a key ("A", "F2", "Comma", "LeftArrow") and its inverse; "" / ImGuiKey_None
// on miss. Exposed for json (de)serialisation and tests.
const char* KeyName(ImGuiKey);
ImGuiKey    KeyFromName(const char*);

constexpr float kChordTimeoutSeconds = 1.0f;

} // namespace Shortcuts
