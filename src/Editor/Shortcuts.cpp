#include "Shortcuts.h"

#include "Log.h"
#include "ProjectPaths.h"

#include <json.hpp>

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

bool CtxOverlaps(std::uint32_t a, std::uint32_t b) {
    // Global overlaps anything; otherwise the panel bits must intersect.
    if ((a & Ctx_Global) || (b & Ctx_Global)) return true;
    return (a & b) != 0;
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
    Register("edit.selectAll",       "Select All",            Ctx_Global, Ck(ImGuiKey_A));
    Register("edit.deselectAll",     "Deselect All",          Ctx_Global, CSk(ImGuiKey_A));
    Register("edit.invertSelection", "Invert Selection",      Ctx_Global, Ck(ImGuiKey_I));
    Register("edit.copy",            "Copy",                  Ctx_Global, Ck(ImGuiKey_C));
    Register("edit.cut",             "Cut",                   Ctx_Global, Ck(ImGuiKey_X));
    Register("edit.paste",           "Paste",                 Ctx_Global, Ck(ImGuiKey_V));
    Register("edit.delete",          "Delete Selection",      Ctx_Global, K(ImGuiKey_Delete));
    Register("edit.rename",          "Rename Selection",      Ctx_Global, K(ImGuiKey_F2));

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

    // --- Panel focus (Global) — new with the manager (#236 F). Alt+1..4 rather than Ctrl+
    // so it doesn't collide with the Ctrl+digit opposite-view presets above. ---
    Register("panel.focus.hierarchy",  "Focus Hierarchy Panel",  Ctx_Global, Ak(ImGuiKey_1));
    Register("panel.focus.inspector",  "Focus Inspector Panel",  Ctx_Global, Ak(ImGuiKey_2));
    Register("panel.focus.project",    "Focus Asset Browser",    Ctx_Global, Ak(ImGuiKey_3));
    Register("panel.focus.console",    "Focus Console Panel",    Ctx_Global, Ak(ImGuiKey_4));
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
        if (s.Current == chord && CtxOverlaps(selfCtx, s.Ctx)) out.push_back(s.Id);
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

} // namespace Shortcuts
