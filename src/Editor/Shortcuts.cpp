#include "Shortcuts.h"

#include "Log.h"
#include "ProjectPaths.h"
#include "Input.h" // TriggeredGlfw() — main-loop keys, evaluated before an ImGui frame exists

#include <json.hpp>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <fstream>
#include <unordered_map>

using json = nlohmann::json;

namespace Shortcuts {
namespace {

std::vector<Shortcut> g_Table;
std::uint64_t         g_Frame = 0;

// --- half-entered chord prefix -------------------------------------------------------------
// A single global pending prefix (key + mods), shared by every chorded binding. Set when a
// prefix chord is pressed on its own; cleared on timeout, on any other key press, or when a
// chord completes.
bool     g_PrefixActive = false;
ImGuiKey g_PrefixKey     = ImGuiKey_None;
bool     g_PrefixCtrl = false, g_PrefixShift = false, g_PrefixAlt = false;
double   g_PrefixTime = 0.0;

// Frame the last Triggered() edge was recorded on, per shortcut id. Compared against g_Frame.
std::unordered_map<std::string, std::uint64_t> g_Fired;

const std::string& Path() {
    static const std::string p = ProjectPaths::Resolve("shortcuts.json");
    return p;
}

// Modifier keys are never a shortcut's main key — skip them in scans / capture.
bool IsModifierKey(ImGuiKey k) {
    switch (k) {
        case ImGuiKey_LeftCtrl:  case ImGuiKey_RightCtrl:
        case ImGuiKey_LeftShift: case ImGuiKey_RightShift:
        case ImGuiKey_LeftAlt:   case ImGuiKey_RightAlt:
        case ImGuiKey_LeftSuper: case ImGuiKey_RightSuper:
        case ImGuiKey_ReservedForModCtrl: case ImGuiKey_ReservedForModShift:
        case ImGuiKey_ReservedForModAlt:  case ImGuiKey_ReservedForModSuper:
            return true;
        default:
            return false;
    }
}

// Digit rows and the numpad are interchangeable for the view-preset bindings, so a binding on
// ImGuiKey_7 also matches Keypad7 (and vice versa).
ImGuiKey DigitSibling(ImGuiKey k) {
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9)                return (ImGuiKey)(ImGuiKey_Keypad0 + (k - ImGuiKey_0));
    if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9)    return (ImGuiKey)(ImGuiKey_0 + (k - ImGuiKey_Keypad0));
    return ImGuiKey_None;
}

bool KeyPressedInclSibling(ImGuiKey k) {
    if (k == ImGuiKey_None) return false;
    if (ImGui::IsKeyPressed(k, false)) return true;
    ImGuiKey sib = DigitSibling(k);
    return sib != ImGuiKey_None && ImGui::IsKeyPressed(sib, false);
}

bool ModsMatch(const ImGuiIO& io, bool ctrl, bool shift, bool alt, bool super) {
    return io.KeyCtrl == ctrl && io.KeyShift == shift && io.KeyAlt == alt && io.KeySuper == super;
}

bool ChordFires(const Chord& c, const ImGuiIO& io) {
    if (!c.IsBound()) return false;

    if (!c.HasPrefix()) {
        return KeyPressedInclSibling(c.Key) && ModsMatch(io, c.Ctrl, c.Shift, c.Alt, c.Super);
    }

    // Two-key sequence: the prefix must have been latched on an earlier frame and still be
    // within the timeout, then this frame the main key goes down with its modifiers.
    if (!g_PrefixActive) return false;
    if (g_PrefixKey != c.PrefixKey || g_PrefixCtrl != c.PrefixCtrl ||
        g_PrefixShift != c.PrefixShift || g_PrefixAlt != c.PrefixAlt) return false;
    return KeyPressedInclSibling(c.Key) && ModsMatch(io, c.Ctrl, c.Shift, c.Alt, c.Super);
}

// Two bindings genuinely clash only when they can both fire for the same key in the same
// scope: both Global, or the same panel context. A Global binding paired with a panel-scoped
// one on the same key is the intended "panel wins while focused" override (Ctrl+D = duplicate
// entity globally / duplicate asset in the Project panel), not a conflict.
bool SameConflictScope(std::uint32_t a, std::uint32_t b) {
    return a == b;
}

Shortcut* FindMut(const std::string& id) {
    for (auto& s : g_Table) if (s.Id == id) return &s;
    return nullptr;
}

void Register(const char* id, const char* label, std::uint32_t ctx, Chord def) {
    Shortcut s;
    s.Id = id; s.Label = label; s.Ctx = ctx;
    s.Default = def; s.Current = def; s.Overridden = false;
    g_Table.push_back(std::move(s));
}

// Chord builders — keep the table below terse and readable.
Chord K(ImGuiKey k)                            { Chord c; c.Key = k; return c; }
Chord Ck(ImGuiKey k)                           { Chord c; c.Key = k; c.Ctrl = true; return c; }
Chord CSk(ImGuiKey k)                          { Chord c; c.Key = k; c.Ctrl = true; c.Shift = true; return c; }
Chord Sk(ImGuiKey k)                           { Chord c; c.Key = k; c.Shift = true; return c; }
Chord Ak(ImGuiKey k)                           { Chord c; c.Key = k; c.Alt = true; return c; }
Chord ASk(ImGuiKey k)                          { Chord c; c.Key = k; c.Alt = true; c.Shift = true; return c; }

void BuildDefaultTable() {
    g_Table.clear();

    // --- File / history (Global) ---
    Register("editor.undo",        "Undo",                    Ctx_Global, Ck(ImGuiKey_Z));
    Register("editor.redo",        "Redo",                    Ctx_Global, Ck(ImGuiKey_Y));
    Register("editor.save",        "Save Scene",              Ctx_Global, Ck(ImGuiKey_S));
    Register("editor.saveAs",      "Save Scene As",           Ctx_Global, CSk(ImGuiKey_S));
    Register("editor.newScene",    "New Scene",               Ctx_Global, Ck(ImGuiKey_N));
    Register("editor.openScene",   "Open Scene",              Ctx_Global, Ck(ImGuiKey_O));
    Register("editor.preferences", "Open Preferences",        Ctx_Global, Ck(ImGuiKey_Comma));

    // --- Edit (Global) ---
    Register("edit.duplicate",       "Duplicate (in place)",  Ctx_Global, Ck(ImGuiKey_D));
    Register("edit.duplicateArray",  "Duplicate Array\xE2\x80\xA6", Ctx_Global, Chord{}); // unbound by default
    Register("edit.selectAll",       "Select All",            Ctx_Global, Ck(ImGuiKey_A));
    Register("edit.deselectAll",     "Deselect All",          Ctx_Global, CSk(ImGuiKey_A));
    Register("edit.invertSelection", "Invert Selection",      Ctx_Global, Ck(ImGuiKey_I));
    Register("edit.copy",            "Copy",                  Ctx_Global, Ck(ImGuiKey_C));
    Register("edit.cut",             "Cut",                   Ctx_Global, Ck(ImGuiKey_X));
    Register("edit.paste",           "Paste",                 Ctx_Global, Ck(ImGuiKey_V));
    Register("edit.delete",          "Delete Selection",      Ctx_Global, K(ImGuiKey_Delete));
    Register("edit.rename",          "Rename Selection",      Ctx_Global, K(ImGuiKey_F2));
    Register("select.historyBack",    "Selection: Back",      Ctx_Global, Ck(ImGuiKey_LeftBracket));
    Register("select.historyForward", "Selection: Forward",   Ctx_Global, Ck(ImGuiKey_RightBracket));

    // --- GameObject (Global) ---
    Register("gameobject.createEmptyChild", "Create Empty Child",   Ctx_Global, CSk(ImGuiKey_N));
    Register("gameobject.toggleActive",     "Toggle Active State",  Ctx_Global, ASk(ImGuiKey_A));
    Register("camera.alignToView",          "Align Camera to View", Ctx_Global, CSk(ImGuiKey_F));

    // --- Viewport tools + framing (Viewport) ---
    Register("gameobject.quickAdd",     "Quick Create (at cursor)",  Ctx_Viewport, Sk(ImGuiKey_A));
    Register("view.frameSelection",     "Frame Selection",           Ctx_Viewport, K(ImGuiKey_F));
    Register("view.lockToSelection",    "Lock View to Selection",    Ctx_Viewport, Sk(ImGuiKey_F));
    Register("tools.hand",              "Tool: Hand",                Ctx_Viewport, K(ImGuiKey_Q));
    Register("tools.move",              "Tool: Move",                Ctx_Viewport, K(ImGuiKey_W));
    Register("tools.rotate",            "Tool: Rotate",              Ctx_Viewport, K(ImGuiKey_E));
    Register("tools.scale",             "Tool: Scale",               Ctx_Viewport, K(ImGuiKey_R));
    Register("tools.rect",              "Tool: Rect",                Ctx_Viewport, K(ImGuiKey_T));
    Register("tools.transform",         "Tool: Combined Transform",  Ctx_Viewport, K(ImGuiKey_Y));
    // View presets. The number row and the numpad are interchangeable (see DigitSibling); Ctrl
    // picks the opposite face. In Unity those opposite faces are their own rebindable
    // shortcuts, so they get their own rows rather than a hard-coded Ctrl read.
    Register("view.front",       "View: Front",           Ctx_Viewport, K(ImGuiKey_1));
    Register("view.back",        "View: Back",            Ctx_Viewport, Ck(ImGuiKey_1));
    Register("view.right",       "View: Right",           Ctx_Viewport, K(ImGuiKey_3));
    Register("view.left",        "View: Left",            Ctx_Viewport, Ck(ImGuiKey_3));
    Register("view.top",         "View: Top",             Ctx_Viewport, K(ImGuiKey_7));
    Register("view.bottom",      "View: Bottom",          Ctx_Viewport, Ck(ImGuiKey_7));
    Register("view.persp",       "View: Perspective",     Ctx_Viewport, K(ImGuiKey_0));
    Register("view.toggleOrtho", "Toggle Orthographic",   Ctx_Viewport, K(ImGuiKey_5));

    // --- Asset Browser (Ctx_Project) — only while the panel has focus, so F / Ctrl+D don't
    // collide with the scene-selection bindings. ---
    Register("project.focusSearch",   "Focus Search",        Ctx_Project, Ck(ImGuiKey_F));
    Register("project.refresh",       "Refresh",             Ctx_Project, Ck(ImGuiKey_R));
    Register("project.frameSelected", "Frame Selected Asset", Ctx_Project, K(ImGuiKey_F));
    Register("project.duplicate",     "Duplicate Asset",     Ctx_Project, Ck(ImGuiKey_D));

    // --- Panel focus (Global) — new with the manager (#236 F). Alt+1..4 rather than Ctrl+
    // so it doesn't collide with the Ctrl+digit opposite-view presets above. ---
    Register("panel.focus.hierarchy",  "Focus Hierarchy Panel",  Ctx_Global, Ak(ImGuiKey_1));
    Register("panel.focus.inspector",  "Focus Inspector Panel",  Ctx_Global, Ak(ImGuiKey_2));
    Register("panel.focus.project",    "Focus Asset Browser",    Ctx_Global, Ak(ImGuiKey_3));
    Register("panel.focus.console",    "Focus Console Panel",    Ctx_Global, Ak(ImGuiKey_4));

    // --- Application / play controls (Ctx_App) — evaluated in the main loop via TriggeredGlfw,
    // so they work with the editor UI hidden and never clash (own scope) with the Global F2
    // rename etc. ---
    Register("play.toggle",       "Play / Stop",             Ctx_App, K(ImGuiKey_F1));
    Register("play.pause",        "Pause / Resume",          Ctx_App, K(ImGuiKey_F2));
    Register("play.step",         "Step One Frame",          Ctx_App, K(ImGuiKey_F3));
    Register("play.maximize",     "Maximize / Restore Game", Ctx_App, K(ImGuiKey_F4));
    Register("window.fullscreen", "Toggle Window Fullscreen", Ctx_App, K(ImGuiKey_F11));
}

// --- key <-> name (small, covers everything the table + json can hold) --------------------
struct KeyNameEntry { ImGuiKey key; const char* name; };

const std::vector<KeyNameEntry>& KeyNameTable() {
    static std::vector<KeyNameEntry> t = [] {
        std::vector<KeyNameEntry> v;
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            ImGuiKey key = (ImGuiKey)k;
            if (IsModifierKey(key)) continue;
            const char* n = ImGui::GetKeyName(key);
            if (n && n[0] && std::string(n) != "None") v.push_back({key, n});
        }
        return v;
    }();
    return t;
}

} // namespace

bool Chord::operator==(const Chord& o) const {
    return Key == o.Key && Ctrl == o.Ctrl && Shift == o.Shift && Alt == o.Alt && Super == o.Super &&
           PrefixKey == o.PrefixKey && PrefixCtrl == o.PrefixCtrl &&
           PrefixShift == o.PrefixShift && PrefixAlt == o.PrefixAlt;
}

const char* KeyName(ImGuiKey k) {
    for (const auto& e : KeyNameTable()) if (e.key == k) return e.name;
    return "";
}

ImGuiKey KeyFromName(const char* n) {
    if (!n || !n[0]) return ImGuiKey_None;
    for (const auto& e : KeyNameTable()) if (std::string(e.name) == n) return e.key;
    return ImGuiKey_None;
}

std::string ToString(const Chord& c) {
    if (!c.IsBound()) return "(unbound)";
    auto one = [](ImGuiKey key, bool ctrl, bool shift, bool alt) {
        std::string s;
        if (ctrl)  s += "Ctrl+";
        if (shift) s += "Shift+";
        if (alt)   s += "Alt+";
        s += KeyName(key);
        return s;
    };
    std::string s;
    if (c.HasPrefix()) s += one(c.PrefixKey, c.PrefixCtrl, c.PrefixShift, c.PrefixAlt) + "  ";
    s += one(c.Key, c.Ctrl, c.Shift, c.Alt);
    return s;
}

const std::vector<Shortcut>& All() { return g_Table; }

Shortcut* Find(const char* id) { return FindMut(id ? id : ""); }

void SetChord(const char* id, const Chord& chord) {
    Shortcut* s = FindMut(id ? id : "");
    if (!s) return;
    s->Current = chord;
    s->Overridden = (chord != s->Default);
}

void ResetToDefault(const char* id) {
    Shortcut* s = FindMut(id ? id : "");
    if (!s) return;
    s->Current = s->Default;
    s->Overridden = false;
}

void ResetAllToDefault() {
    for (auto& s : g_Table) { s.Current = s.Default; s.Overridden = false; }
}

std::vector<std::string> Conflicts(const char* selfId, const Chord& chord) {
    std::vector<std::string> out;
    if (!chord.IsBound()) return out;
    const Shortcut* self = FindMut(selfId ? selfId : "");
    const std::uint32_t selfCtx = self ? self->Ctx : (std::uint32_t)Ctx_Global;
    for (const auto& s : g_Table) {
        if (&s == self) continue;
        if (s.Current == chord && SameConflictScope(selfCtx, s.Ctx)) out.push_back(s.Id);
    }
    return out;
}

bool CaptureChord(Chord& out) {
    const ImGuiIO& io = ImGui::GetIO();
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        ImGuiKey key = (ImGuiKey)k;
        if (IsModifierKey(key)) continue;
        if (!ImGui::IsKeyPressed(key, false)) continue;
        out = Chord{};
        out.Key = key;
        out.Ctrl = io.KeyCtrl; out.Shift = io.KeyShift; out.Alt = io.KeyAlt; out.Super = io.KeySuper;
        return true;
    }
    return false;
}

void Init() {
    BuildDefaultTable();
    Load();
}

void Load() {
    for (auto& s : g_Table) { s.Current = s.Default; s.Overridden = false; }

    std::ifstream in(Path());
    if (!in.is_open()) return; // no overrides yet — defaults stand, not an error

    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        Log::Warn(std::string("Shortcuts: failed to parse '") + Path() + "': " + e.what());
        return;
    }

    const auto binds = root.find("bindings");
    if (binds == root.end() || !binds->is_object()) return;

    auto readChord = [](const json& j, Chord& c) {
        c = Chord{};
        c.Key         = KeyFromName(j.value("key", std::string()).c_str());
        c.Ctrl        = j.value("ctrl", false);
        c.Shift       = j.value("shift", false);
        c.Alt         = j.value("alt", false);
        c.Super       = j.value("super", false);
        c.PrefixKey   = KeyFromName(j.value("prefixKey", std::string()).c_str());
        c.PrefixCtrl  = j.value("prefixCtrl", false);
        c.PrefixShift = j.value("prefixShift", false);
        c.PrefixAlt   = j.value("prefixAlt", false);
    };

    for (auto& s : g_Table) {
        const auto it = binds->find(s.Id);
        if (it == binds->end() || !it->is_object()) continue;
        Chord c;
        readChord(*it, c);
        if (!c.IsBound() && !it->value("unbound", false)) continue; // ignore garbage rows
        s.Current = c;
        s.Overridden = (c != s.Default);
    }
}

void Save() {
    json root;
    root["bindings"] = json::object();
    for (const auto& s : g_Table) {
        if (!s.Overridden) continue;
        json j;
        j["key"]   = KeyName(s.Current.Key);
        j["ctrl"]  = s.Current.Ctrl;
        j["shift"] = s.Current.Shift;
        j["alt"]   = s.Current.Alt;
        if (s.Current.Super) j["super"] = true;
        if (!s.Current.IsBound()) j["unbound"] = true;
        if (s.Current.HasPrefix()) {
            j["prefixKey"]   = KeyName(s.Current.PrefixKey);
            j["prefixCtrl"]  = s.Current.PrefixCtrl;
            j["prefixShift"] = s.Current.PrefixShift;
            j["prefixAlt"]   = s.Current.PrefixAlt;
        }
        root["bindings"][s.Id] = j;
    }

    std::ofstream out(Path());
    if (!out.is_open()) {
        Log::Warn(std::string("Shortcuts: could not write '") + Path() + "'");
        return;
    }
    out << root.dump(2) << '\n';
}

void BeginFrame(std::uint32_t contextMask) {
    ++g_Frame;

    if (contextMask == 0) {
        g_PrefixActive = false;
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const double now = ImGui::GetTime();

    if (g_PrefixActive && (now - g_PrefixTime) > kChordTimeoutSeconds)
        g_PrefixActive = false;

    // Resolve completed chords first, so the key that completes a sequence isn't also
    // interpreted as the start of a new prefix this same frame.
    bool anyFired = false;
    for (auto& s : g_Table) {
        // A binding fires only when its own context bit is in the active mask. Global bindings
        // carry Ctx_Global, which BeginFrame's caller always includes.
        if (!(contextMask & s.Ctx)) continue;
        if (ChordFires(s.Current, io)) {
            g_Fired[s.Id] = g_Frame;
            anyFired = true;
        }
    }
    if (anyFired) g_PrefixActive = false;

    // Latch a new prefix: a bare press of some binding's prefix key (with its prefix mods and
    // no other non-modifier key this frame). Cheap scan over the distinct prefixes in use.
    if (!g_PrefixActive && !anyFired) {
        for (const auto& s : g_Table) {
            const Chord& c = s.Current;
            if (!c.HasPrefix()) continue;
            if (ImGui::IsKeyPressed(c.PrefixKey, false) &&
                ModsMatch(io, c.PrefixCtrl, c.PrefixShift, c.PrefixAlt, false)) {
                g_PrefixActive = true;
                g_PrefixKey = c.PrefixKey;
                g_PrefixCtrl = c.PrefixCtrl; g_PrefixShift = c.PrefixShift; g_PrefixAlt = c.PrefixAlt;
                g_PrefixTime = now;
                break;
            }
        }
    }
}

bool Triggered(const char* id) {
    if (!id) return false;
    const auto it = g_Fired.find(id);
    return it != g_Fired.end() && it->second == g_Frame;
}

// --- GLFW-input path (main loop, no ImGui frame) ------------------------------------------
namespace {
int GlfwKeyFromImGui(ImGuiKey k) {
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z)  return GLFW_KEY_A + (k - ImGuiKey_A);
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9)  return GLFW_KEY_0 + (k - ImGuiKey_0);
    if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) return GLFW_KEY_KP_0 + (k - ImGuiKey_Keypad0);
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) return GLFW_KEY_F1 + (k - ImGuiKey_F1);
    switch (k) {
        case ImGuiKey_Space:      return GLFW_KEY_SPACE;
        case ImGuiKey_Enter:      return GLFW_KEY_ENTER;
        case ImGuiKey_KeypadEnter:return GLFW_KEY_KP_ENTER;
        case ImGuiKey_Tab:        return GLFW_KEY_TAB;
        case ImGuiKey_Escape:     return GLFW_KEY_ESCAPE;
        case ImGuiKey_Backspace:  return GLFW_KEY_BACKSPACE;
        case ImGuiKey_Delete:     return GLFW_KEY_DELETE;
        case ImGuiKey_Insert:     return GLFW_KEY_INSERT;
        case ImGuiKey_Home:       return GLFW_KEY_HOME;
        case ImGuiKey_End:        return GLFW_KEY_END;
        case ImGuiKey_PageUp:     return GLFW_KEY_PAGE_UP;
        case ImGuiKey_PageDown:   return GLFW_KEY_PAGE_DOWN;
        case ImGuiKey_LeftArrow:  return GLFW_KEY_LEFT;
        case ImGuiKey_RightArrow: return GLFW_KEY_RIGHT;
        case ImGuiKey_UpArrow:    return GLFW_KEY_UP;
        case ImGuiKey_DownArrow:  return GLFW_KEY_DOWN;
        case ImGuiKey_Comma:      return GLFW_KEY_COMMA;
        case ImGuiKey_Period:     return GLFW_KEY_PERIOD;
        case ImGuiKey_Minus:      return GLFW_KEY_MINUS;
        case ImGuiKey_Equal:      return GLFW_KEY_EQUAL;
        default:                  return -1;
    }
}
} // namespace

bool TriggeredGlfw(const char* id) {
    Shortcut* s = FindMut(id ? id : "");
    if (!s || !s->Current.IsBound()) return false;
    const Chord& c = s->Current; // sequences never reach this path; the prefix half is ignored
    const int gk = GlfwKeyFromImGui(c.Key);
    if (gk < 0 || !Input::IsKeyPressed(gk)) return false;
    const bool ctrl  = Input::IsKeyDown(GLFW_KEY_LEFT_CONTROL) || Input::IsKeyDown(GLFW_KEY_RIGHT_CONTROL);
    const bool shift = Input::IsKeyDown(GLFW_KEY_LEFT_SHIFT)   || Input::IsKeyDown(GLFW_KEY_RIGHT_SHIFT);
    const bool alt   = Input::IsKeyDown(GLFW_KEY_LEFT_ALT)     || Input::IsKeyDown(GLFW_KEY_RIGHT_ALT);
    return ctrl == c.Ctrl && shift == c.Shift && alt == c.Alt;
}

} // namespace Shortcuts
