// Editor smoke suite: Animator. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The Animator window (EditorLayer_Animator*.cpp): the layers / parameters panel, the graph canvas
// and the selection panel. The graph draws its nodes and arrows straight onto the draw list, so the
// tests work out where a state or an arrow is from the window's pan / zoom and the canvas's own
// InvisibleButton, then click there like a user.
//  - project/assets/AI/npc_soldier.controller is opened, framed, browsed (states selected, every
//    left tab shown) and shot, but never edited: the window saves every edit straight to the file,
//    so the test checks the file is byte-for-byte unchanged afterwards (and puts it back if not).
//  - a scratch controller (a copy of what Assets > Create Animator Controller writes) gets the
//    context-menu edits: Create State, Create Blend Tree State, Make Transition, Set as Layer
//    Default State, Copy / Paste (with the transitions between the copies), Duplicate, Duplicate
//    with transitions, Delete, Delete Transition, Frame All; a parameter and a layer added; the
//    Delete key; and the window's Undo / Redo stepping through them.

} // namespace
#include "EditorLayer_AnimatorInternal.h"
struct EditorSmokeAccess_Animator {
    static AnimatorWindowState* Win(EditorLayer& e) { return e.m_AnimatorWin.get(); }
    static void Open(EditorLayer& e, const std::string& rel) { e.OpenAnimatorWindow(rel); }
    static bool& Show(EditorLayer& e) { return e.m_ShowAnimator; }
};
namespace {
namespace smoke_animator {
namespace fs = std::filesystem;
using AA = EditorSmokeAccess_Animator;

const char* kAnimator = "###Animator";
const char* kSoldierRel = "assets/AI/npc_soldier.controller";
const char* kScratchRel = "editor-smoke-scratch-animator/Smoke.controller";
std::string ScratchDir() { return ProjectPaths::Resolve("editor-smoke-scratch-animator"); }

std::string ReadBytes(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
// The controller as saved on disk.
AnimatorController OnDisk(const std::string& rel) {
    AnimatorController c;
    AnimatorController::LoadFile(ProjectPaths::Resolve(rel), c);
    return c;
}
AnimatorWindowState* Win(Ctx& c) { return AA::Win(c.E); }
AnimatorController::Layer* Layer(Ctx& c) {
    AnimatorWindowState* w = Win(c);
    return w && w->Loaded ? &w->L() : nullptr;
}
int StateCount(Ctx& c) { const auto* l = Layer(c); return l ? (int)l->States.size() : -1; }
int TransitionCount(Ctx& c) { const auto* l = Layer(c); return l ? (int)l->Transitions.size() : -1; }
int StateIndex(Ctx& c, const std::string& name) { const auto* l = Layer(c); return l ? l->FindState(name) : -1; }
bool HasTransition(Ctx& c, const std::string& from, const std::string& to) {
    if (const auto* l = Layer(c))
        for (const auto& t : l->Transitions)
            if (t.FromKind == AnimatorController::Source::State && t.From == from && t.To == to) return true;
    return false;
}

// --- graph geometry (EditorLayer_AnimatorGraph.cpp's toScreen / nodeRect) ------------------------
bool CanvasRect(ImRect& out) {
    Query q = ByExact("##canvasbtn", kAnimator);
    const UiItem* it = Find(q);
    if (!it) return false;
    out = it->Rect;
    return out.GetWidth() > 10.0f && out.GetHeight() > 10.0f;
}
struct Node { ImRect R; std::string Name; };
// Every node's screen rectangle: the layer's states, then Entry, Any State, Exit.
std::vector<Node> Nodes(Ctx& c) {
    std::vector<Node> out;
    AnimatorWindowState* w = Win(c);
    ImRect canvas;
    if (!w || !w->Loaded || !CanvasRect(canvas)) return out;
    const auto& l = w->L();
    auto add = [&](glm::vec2 p, float wd, float ht, const std::string& name) {
        const ImVec2 a(canvas.Min.x + w->Pan.x + p.x * w->Zoom, canvas.Min.y + w->Pan.y + p.y * w->Zoom);
        out.push_back({ImRect(a, ImVec2(a.x + wd * w->Zoom, a.y + ht * w->Zoom)), name});
    };
    for (const auto& s : l.States) add(s.Position, kStateW, kStateH, s.Name);
    add(l.EntryPosition, kSpecialW, kSpecialH, "<Entry>");
    add(l.AnyPosition, kSpecialW, kSpecialH, "<Any>");
    add(l.ExitPosition, kSpecialW, kSpecialH, "<Exit>");
    return out;
}
// A point on the state's node that no copy stacked on top of it (pasted / duplicated at +30,+30)
// covers: near its top-left corner.
bool StatePoint(Ctx& c, const std::string& name, ImVec2& out) {
    AnimatorWindowState* w = Win(c);
    for (const Node& n : Nodes(c))
        if (n.Name == name) {
            out = ImVec2(n.R.Min.x + 12.0f * w->Zoom, n.R.Min.y + 10.0f * w->Zoom);
            ImRect canvas;
            return CanvasRect(canvas) && canvas.Contains(out);
        }
    return false;
}
float SegmentDistance(ImVec2 p, ImVec2 a, ImVec2 b) {
    const ImVec2 ab(b.x - a.x, b.y - a.y), ap(p.x - a.x, p.y - a.y);
    const float l2 = ab.x * ab.x + ab.y * ab.y;
    const float t = l2 > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
    const ImVec2 q(a.x + ab.x * t - p.x, a.y + ab.y * t - p.y);
    return std::sqrt(q.x * q.x + q.y * q.y);
}
// The middle of the arrow between two states (the line joining their centres).
bool ArrowPoint(Ctx& c, const std::string& from, const std::string& to, ImVec2& out) {
    ImVec2 a, b;
    bool fa = false, fb = false;
    for (const Node& n : Nodes(c)) {
        if (n.Name == from) { a = n.R.GetCenter(); fa = true; }
        if (n.Name == to) { b = n.R.GetCenter(); fb = true; }
    }
    if (!fa || !fb) return false;
    out = ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    for (const Node& n : Nodes(c)) if (n.R.Contains(out)) return false;
    return true;
}
// A spot on the canvas with room for a new state around it: clear of every node and arrow.
bool EmptyPoint(Ctx& c, ImVec2& out) {
    AnimatorWindowState* w = Win(c);
    ImRect canvas;
    if (!w || !CanvasRect(canvas)) return false;
    const auto nodes = Nodes(c);
    std::vector<std::pair<ImVec2, ImVec2>> arrows;
    const auto& l = w->L();
    auto centre = [&](const std::string& name) {
        for (const Node& n : nodes) if (n.Name == name) return n.R.GetCenter();
        return ImVec2(-1e6f, -1e6f);
    };
    for (const auto& t : l.Transitions) {
        const std::string from = t.FromKind == AnimatorController::Source::Any ? "<Any>"
                               : t.FromKind == AnimatorController::Source::Entry ? "<Entry>" : t.From;
        const std::string to = t.To == AnimatorController::kExitState ? "<Exit>" : t.To;
        arrows.push_back({centre(from), centre(to)});
    }
    if (const int d = l.DefaultStateIndex(); d >= 0) arrows.push_back({centre("<Entry>"), centre(l.States[(size_t)d].Name)});
    const float hw = kStateW * w->Zoom * 0.5f + 24.0f, hh = kStateH * w->Zoom * 0.5f + 24.0f;
    for (float y = canvas.Min.y + hh + 30.0f; y < canvas.Max.y - hh; y += 20.0f)
        for (float x = canvas.Min.x + hw + 10.0f; x < canvas.Max.x - hw; x += 20.0f) {
            const ImRect room(ImVec2(x - hw, y - hh), ImVec2(x + hw, y + hh));
            bool clear = true;
            for (const Node& n : nodes) if (n.R.Overlaps(room)) { clear = false; break; }
            for (const auto& a : arrows) if (clear && SegmentDistance(ImVec2(x, y), a.first, a.second) < hh + 8.0f) clear = false;
            if (clear) { out = ImVec2(x, y); return true; }
        }
    return false;
}

// Press and release `button` at a point worked out when the step runs (modifiers held around it).
Step ClickAt(std::string what, std::function<bool(Ctx&, ImVec2&)> where, int button = 0, int mods = 0) {
    auto phase = std::make_shared<int>(0);
    return {"click " + what, [where, button, mods, phase](Ctx& c) {
        ImGuiIO& io = ImGui::GetIO();
        switch (*phase) {
            case 0: {
                ImVec2 p;
                if (!where(c, p)) return false;
                MouseTo(p);
                Mods(mods, true);
                *phase = 1;
                return false;
            }
            case 1: io.AddMouseButtonEvent(button, true); *phase = 2; return false;
            default:
                io.AddMouseButtonEvent(button, false);
                Mods(mods, false);
                return true;
        }
    }, 120};
}
Step ClickState(std::string name, int button = 0, int mods = 0) {
    return ClickAt("state " + name, [name](Ctx& c, ImVec2& p) { return StatePoint(c, name, p); }, button, mods);
}
Step RightClickEmpty() { return ClickAt("empty canvas", [](Ctx& c, ImVec2& p) { return EmptyPoint(c, p); }, 1); }
// A context-menu entry of the graph (its ##animctx popup).
Query GraphMenu(const char* label) { return MenuItem(label); }
// Right-click `target`, pick `item` from the graph's menu.
std::vector<Step> ContextOn(Step target, const char* item) {
    return {target, Wait(3), Click(GraphMenu(item)), Wait(4)};
}
// The window's own toolbar / panel buttons (ActionButton: the glyph is the label).
Query WinButton(const char* raw) { return ByExact(raw, kAnimator); }
Query Tab(const char* label) { return ByExact(label, kAnimator); }
// The edit reached the file (every edit saves straight away).
Step SavedStates(int n) {
    return Until("the file has " + std::to_string(n) + " states", [n](Ctx&) {
        return (int)OnDisk(kScratchRel).Layers[0].States.size() == n;
    }, 60);
}
std::vector<Step> Cat(std::vector<std::vector<Step>> parts) {
    std::vector<Step> out;
    for (auto& p : parts) for (auto& s : p) out.push_back(std::move(s));
    return out;
}
Step UntilLoaded(std::string rel) {
    return Until("the Animator shows " + rel, [rel](Ctx& c) {
        AnimatorWindowState* w = Win(c);
        ImRect canvas;
        return w && w->Loaded && w->Rel == rel && !w->FramePending && ChromeUi::Shown(EditorPanels::Animator) && CanvasRect(canvas);
    }, 240);
}

} // namespace smoke_animator

void RegisterAnimatorTests() {
    using namespace smoke_animator;

    // ---------------------------------------------------------------- npc_soldier, read-only
    {
        auto original = std::make_shared<std::string>();
        Add("animator: npc_soldier.controller opens, frames, selects, shows its tabs; the file is untouched", Cat({
            {FreshScene(), Wait(2),
             Do("note the file", [original](Ctx&) {
                 *original = ReadBytes(ProjectPaths::Resolve(kSoldierRel));
                 if (original->empty()) Fail(std::string("can't read ") + kSoldierRel);
             }),
             Do("open it in the Animator", [](Ctx& c) { AA::Open(c.E, kSoldierRel); }),
             UntilLoaded(kSoldierRel), Wait(3),
             Expect("it has states", [](Ctx& c) { return StateCount(c) > 0 ? std::string() : std::string("no states in layer 0"); }),
             Shot("graph", {kAnimator}),
             Click(WinButton(ICON_FA_EXPAND)), Wait(3),
             ExpectTrue("Frame all ran", [](Ctx& c) { return !Win(c)->FramePending; }),
             Expect("every node is on the canvas after Frame all", [](Ctx& c) {
                 ImRect canvas;
                 CanvasRect(canvas);
                 canvas.Expand(2.0f);
                 for (const Node& n : Nodes(c))
                     if (!canvas.Contains(n.R)) return n.Name + " is outside the canvas";
                 return std::string();
             }),
             Do("note the first state", [](Ctx& c) { g_Text = Layer(c)->States[0].Name; }),
             ClickAt("the first state", [](Ctx& c, ImVec2& p) { return StatePoint(c, g_Text, p); }), Wait(3),
             Expect("it is selected", [](Ctx& c) {
                 const auto& sel = Win(c)->SelStates;
                 return sel.size() == 1 && sel[0] == 0 ? std::string() : std::string("selection is not the first state");
             }),
             Shot("state-selected", {kAnimator}),
             Click(Tab("Parameters")), Wait(3),
             Shot("parameters", {kAnimator}),
             Click(Tab("Layers")), Wait(3),
             Shot("layers", {kAnimator}),
             Key(ImGuiKey_Escape), Wait(2),
             Expect("the file is byte-for-byte unchanged", [original](Ctx&) {
                 const std::string now = ReadBytes(ProjectPaths::Resolve(kSoldierRel));
                 if (now == *original) return std::string();
                 std::ofstream(fs::u8path(ProjectPaths::Resolve(kSoldierRel)), std::ios::binary) << *original; // put it back
                 return std::string("the Animator rewrote npc_soldier.controller (restored)");
             })},
        }));
    }

    // ---------------------------------------------------------------- scratch controller, edited
    Add("animator: a scratch controller: states, blend tree, transitions, default, copy/paste, duplicate, delete, undo", Cat({
        {FreshScene(), Wait(2),
         Do("write a scratch controller (Idle)", [](Ctx&) {
             std::error_code ec;
             fs::remove_all(fs::u8path(ScratchDir()), ec);
             fs::create_directories(fs::u8path(ScratchDir()), ec);
             AnimatorController c;
             AnimatorController::State idle;
             idle.Name = "Idle";
             idle.Motions.resize(c.Tracks.size());
             c.Layers[0].States.push_back(std::move(idle));
             c.Layers[0].DefaultState = "Idle";
             if (!c.SaveFile(ProjectPaths::Resolve(kScratchRel))) Fail("couldn't write the scratch controller");
         }),
         Do("open it", [](Ctx& c) { AA::Open(c.E, kScratchRel); }),
         UntilLoaded(kScratchRel), Wait(3),
         Do("note the undo depth", [](Ctx& c) { g_Number = (double)X::UndoDepth(c.E); })},

        ContextOn(RightClickEmpty(), "Create State"),
        {ExpectTrue("New State created", [](Ctx& c) { return StateIndex(c, "New State") >= 0 && StateCount(c) == 2; }),
         SavedStates(2),
         ExpectTrue("an undo entry", [](Ctx& c) { return (double)X::UndoDepth(c.E) > g_Number; })},
        ContextOn(RightClickEmpty(), "Create Blend Tree State"),
        {Expect("Blend Tree created", [](Ctx& c) {
             const int i = StateIndex(c, "Blend Tree");
             if (i < 0 || StateCount(c) != 3) return std::string("no Blend Tree state");
             return Layer(c)->States[(size_t)i].Motions[0].IsBlendTree() ? std::string() : std::string("its motion is not a blend tree");
         }),
         SavedStates(3),
         Shot("created", {kAnimator})},

        // Make Transition: from New State's menu, then click Blend Tree.
        ContextOn(ClickState("New State", 1), "Make Transition"),
        {ExpectTrue("linking", [](Ctx& c) { return Win(c)->Linking; }),
         ClickState("Blend Tree"), Wait(4),
         ExpectTrue("New State -> Blend Tree", [](Ctx& c) { return HasTransition(c, "New State", "Blend Tree") && TransitionCount(c) == 1; }),
         ExpectTrue("the new transition is selected", [](Ctx& c) { return Win(c)->SelTransition == 0; }),
         Shot("transition", {kAnimator})},

        ContextOn(ClickState("Blend Tree", 1), "Set as Layer Default State"),
        {ExpectTrue("Blend Tree is the default", [](Ctx& c) { return Layer(c)->DefaultState == "Blend Tree"; }),
         Until("saved as the default", [](Ctx&) { return OnDisk(kScratchRel).Layers[0].DefaultState == "Blend Tree"; }, 60)},

        // Copy both states (their transition comes along), paste them on empty canvas.
        {ClickState("New State"), Wait(2), ClickState("Blend Tree", 0, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("two states selected", [](Ctx& c) { return Win(c)->SelStates.size() == 2; })},
        ContextOn(ClickState("New State", 1), "Copy"),
        ContextOn(RightClickEmpty(), "Paste"),
        {ExpectTrue("two pasted states", [](Ctx& c) { return StateCount(c) == 5 && Win(c)->SelStates.size() == 2; }),
         Expect("the pasted pair keeps its transition", [](Ctx& c) {
             const auto& l = *Layer(c);
             const std::string a = l.States[3].Name, b = l.States[4].Name;
             return HasTransition(c, a, b) && TransitionCount(c) == 2 ? std::string() : "no " + a + " -> " + b + " transition";
         }),
         SavedStates(5),
         Shot("pasted", {kAnimator})},

        // Duplicate (no transitions) and Duplicate with transitions.
        {ClickState("Idle"), Wait(3)},
        ContextOn(ClickState("Idle", 1), "Duplicate"),
        {ExpectTrue("Idle duplicated, no transitions", [](Ctx& c) { return StateCount(c) == 6 && TransitionCount(c) == 2; }),
         Do("note the duplicate", [](Ctx& c) { g_Text = Layer(c)->States.back().Name; }),
         ClickState("New State"), Wait(3)},
        ContextOn(ClickState("New State", 1), "Duplicate with transitions"),
        {Expect("New State duplicated with its transition", [](Ctx& c) {
             const std::string copy = Layer(c)->States.back().Name;
             return StateCount(c) == 7 && TransitionCount(c) == 3 && HasTransition(c, copy, "Blend Tree")
                        ? std::string() : "states " + std::to_string(StateCount(c)) + ", transitions " + std::to_string(TransitionCount(c));
         }),
         Shot("duplicated", {kAnimator})},

        // Delete (menu) the Idle duplicate.
        {ClickAt("the Idle duplicate", [](Ctx& c, ImVec2& p) { return StatePoint(c, g_Text, p); }), Wait(3)},
        ContextOn(ClickAt("the Idle duplicate", [](Ctx& c, ImVec2& p) { return StatePoint(c, g_Text, p); }, 1), "Delete"),
        {ExpectTrue("deleted", [](Ctx& c) { return StateIndex(c, g_Text) < 0 && StateCount(c) == 6; }),
         SavedStates(6)},

        // Delete Transition: right-click the New State -> Blend Tree arrow.
        ContextOn(ClickAt("the New State -> Blend Tree arrow",
                          [](Ctx& c, ImVec2& p) { return ArrowPoint(c, "New State", "Blend Tree", p); }, 1),
                  "Delete Transition"),
        {ExpectTrue("the transition is gone", [](Ctx& c) { return !HasTransition(c, "New State", "Blend Tree") && TransitionCount(c) == 2; })},

        // Frame All from the menu.
        {Do("scroll the view away", [](Ctx& c) { Win(c)->Pan = ImVec2(-4000.0f, -4000.0f); }), Wait(2)},
        ContextOn(RightClickEmpty(), "Frame All"),
        {Expect("Frame All brings every node back", [](Ctx& c) {
             ImRect canvas;
             CanvasRect(canvas);
             canvas.Expand(2.0f);
             for (const Node& n : Nodes(c))
                 if (!canvas.Contains(n.R)) return n.Name + " is outside the canvas";
             return std::string();
         }),
         Shot("framed", {kAnimator}),

         // A parameter and a layer.
         Click(Tab("Parameters")), Wait(3),
         Do("note the parameters", [](Ctx& c) { g_Number = (double)Win(c)->Doc.Parameters.size(); }),
         Click(WinButton(ICON_FA_PLUS " Parameter")), Wait(4),
         ExpectTrue("a parameter added", [](Ctx& c) { return (double)Win(c)->Doc.Parameters.size() == g_Number + 1; }),
         Shot("parameter", {kAnimator}),
         Click(Tab("Layers")), Wait(3),
         Click(WinButton(ICON_FA_PLUS " Layer")), Wait(4),
         ExpectTrue("a layer added", [](Ctx& c) { return Win(c)->Doc.Layers.size() == 2; }),
         Until("saved with two layers", [](Ctx&) { return OnDisk(kScratchRel).Layers.size() == 2; }, 60),
         Shot("layer", {kAnimator}),

         // The window's Undo / Redo.
         Click(WinButton(ICON_FA_ROTATE_LEFT)), Wait(6),
         Until("Undo removes the layer", [](Ctx& c) { return Win(c)->Doc.Layers.size() == 1; }, 60),
         Click(WinButton(ICON_FA_ROTATE_LEFT)), Wait(6),
         Until("Undo removes the parameter", [](Ctx& c) { return (double)Win(c)->Doc.Parameters.size() == g_Number; }, 60),
         Click(WinButton(ICON_FA_ROTATE_LEFT)), Wait(6),
         Until("Undo puts the transition back", [](Ctx& c) { return HasTransition(c, "New State", "Blend Tree"); }, 60),
         Click(WinButton(ICON_FA_ROTATE_RIGHT)), Wait(6),
         Until("Redo deletes it again", [](Ctx& c) { return !HasTransition(c, "New State", "Blend Tree"); }, 60),
         Click(WinButton(ICON_FA_ROTATE_RIGHT)), Wait(6),
         Until("Redo adds the parameter again", [](Ctx& c) { return (double)Win(c)->Doc.Parameters.size() == g_Number + 1; }, 60),

         // The Delete key on a selected state (canvas focused), then Ctrl+Z.
         Click(Tab("Layers")), Wait(2),
         Do("layer 0", [](Ctx& c) { Win(c)->Layer = 0; }), Wait(3),
         ClickState("Idle"), Wait(3),
         Key(ImGuiKey_Delete), Wait(4),
         ExpectTrue("Delete removed Idle", [](Ctx& c) { return StateIndex(c, "Idle") < 0; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(6),
         Until("Ctrl+Z brings Idle back", [](Ctx& c) { return StateIndex(c, "Idle") >= 0; }, 60),
         Shot("final", {kAnimator})},
    }), [](Ctx& c) { // passed or failed: close the Animator, delete the scratch controller
        AA::Show(c.E) = false;
        std::error_code ec;
        fs::remove_all(fs::u8path(ScratchDir()), ec);
    });
}
