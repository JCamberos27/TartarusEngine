// Editor smoke suite: SceneTools. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The Scene view's own tools: the left tool palette (EditorLayer_ToolPalette.cpp) clicked button by
// button, the same tools from their viewport shortcuts (Q/W/E/R/T/Y/M with the Scene hovered, the
// Alt chords for space / pivot / grid / snap / draw mode), view presets (1/3/7, Ctrl for the
// opposite face, 0, 5 and the nav gizmo's axis bubbles), Frame Selected, picking and box select
// in the viewport, and a real ImGuizmo drag. Then a long mixed undo/redo chain checked against a
// full SceneSerializer dump (and the selection) at every depth, and the History panel's rows
// against the undo stack. The View menu copies of these (presets, Orthographic, Measure, Transform
// Gizmo) are in the Menus suite.
//
// The palette is one column of ~17 buttons. In CI's ~1044x788 window the Scene view is shorter
// than that, so tests that click its lower buttons hide the Assets panel first (RoomForRail),
// which gives the Scene view the full height, and show it again afterwards.

} // namespace
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4505) // ImViewGuizmo.h's static helpers this file doesn't call
#endif
#include <ImViewGuizmo.h> // the nav gizmo's hover state (which axis bubble is under the pointer)
#ifdef _MSC_VER
#pragma warning(pop)
#endif

struct EditorSmokeAccess_SceneTools {
    static int GizmoOp(EditorLayer& e) { return (int)e.m_GizmoOp; }
    static bool Hand(EditorLayer& e) { return e.m_HandTool; }
    static bool Measure(EditorLayer& e) { return e.m_MeasureTool; }
    static size_t MeasurePoints(EditorLayer& e) { return e.m_MeasurePoints.size(); }
    static bool Local(EditorLayer& e) { return e.m_GizmoLocalSpace; }
    static bool Center(EditorLayer& e) { return e.m_GizmoPivotCenter; }
    static bool Grid(EditorLayer& e) { return e.m_ShowGrid; }
    static bool Snap(EditorLayer& e) { return e.m_GridSnapEnabled; }
    static float SnapStep(EditorLayer& e) { return e.m_SnapTranslation; }
    static bool Gizmos(EditorLayer& e) { return e.m_GizmosMasterVisible; }
    static bool EntityIcons(EditorLayer& e) { return e.m_ShowEntityIcons; }
    static int Shading(EditorLayer& e) { return (int)e.m_ShadingMode; }
    static bool Gliding(EditorLayer& e) { return e.m_ViewTransition.Active; }
    static bool GizmoEngaged(EditorLayer& e) { return e.m_GizmoEngaged; }
    static bool ArrayDupShown(EditorLayer& e) { return e.m_ShowArrayDuplicate; }
    static void SetArrayDup(EditorLayer& e, int nx, int ny, int nz, glm::vec3 step) {
        e.m_ArrayDupCount[0] = nx; e.m_ArrayDupCount[1] = ny; e.m_ArrayDupCount[2] = nz;
        e.m_ArrayDupStep[0] = step.x; e.m_ArrayDupStep[1] = step.y; e.m_ArrayDupStep[2] = step.z;
    }
    static bool& ShowHistory(EditorLayer& e) { return e.m_ShowHistory; }
    static bool& ShowAssets(EditorLayer& e) { return e.m_ShowAssetBrowser; }
    static size_t RedoDepth(EditorLayer& e) { return e.m_RedoStack.size(); }
    static void ClearUndo(EditorLayer& e) { e.ClearUndoHistory(); }
    // The History panel's rows as DrawHistoryListBody draws them: the undo stack oldest first
    // (selection steps behind a pointer glyph), "Current", then the redo stack oldest first.
    static std::vector<std::string> HistoryRows(EditorLayer& e) {
        std::vector<std::string> rows;
        for (const auto& u : e.m_UndoStack) rows.push_back((u.SelectionOnly ? std::string(ICON_FA_ARROW_POINTER "  ") : std::string()) + u.Label);
        rows.push_back(ICON_FA_LOCATION_DOT "  Current");
        for (size_t i = e.m_RedoStack.size(); i-- > 0;) rows.push_back(e.m_RedoStack[i].Label);
        return rows;
    }
    static std::string UndoLabels(EditorLayer& e) {
        std::string s;
        for (const auto& u : e.m_UndoStack) {
            s += (s.empty() ? "" : " | ") + u.Label + (u.SelectionOnly ? " (sel)" : "");
            for (const auto& f : u.Files) s += " [" + f.Path.filename().string() + "]";
        }
        s += " || redo:";
        for (size_t i = e.m_RedoStack.size(); i-- > 0;) s += " " + e.m_RedoStack[i].Label;
        return s;
    }
    static std::string TopLabel(EditorLayer& e) {
        return e.m_UndoStack.empty() ? std::string("(empty)") : e.m_UndoStack.back().Label + (e.m_UndoStack.back().SelectionOnly ? " (sel)" : "");
    }
    // The tool state every test starts and ends with (it outlives FreshScene).
    static void Defaults(EditorLayer& e) {
        e.m_GizmoOp = ::GizmoOp::Translate;
        e.m_HandTool = false;
        e.m_MeasureTool = false;
        e.m_MeasurePoints.clear();
        e.m_GizmoLocalSpace = false;
        e.m_GizmoPivotCenter = false;
        e.m_ShowGrid = true;
        e.m_GridSnapEnabled = true;
        e.m_GizmosMasterVisible = true;
        e.m_ShowEntityIcons = true;
        e.m_ShowGizmos = true;
        e.m_ShadingMode = EditorLayer::ShadingMode::Shaded;
        e.m_LockViewToSelection = false;
        e.m_SurfaceSnap = false;
        e.m_ShowArrayDuplicate = false;
        e.m_ViewTransition.Active = false;
    }
};
namespace {
namespace smoke_tools {
using ST = EditorSmokeAccess_SceneTools;
using ChromeUi::Steps;
using ChromeUi::Seq;

// A palette button: icon-only, labelled by its glyph, inside the Scene window's ##ToolPalette child.
Query Rail(const char* icon) {
    Query q = ByExact(icon, kScene);
    q.Pred = [](const UiItem& it) { return Contains(it.Window, "##ToolPalette"); };
    return q;
}
const UiItem* LastRailButton() {
    const UiItem* it = Find(Rail(EDITOR_ICON_PERSPECTIVE));
    return it ? it : Find(Rail(EDITOR_ICON_ORTHOGRAPHIC));
}
bool FullyShown(const UiItem& it) {
    ImRect r = it.Rect;
    r.ClipWithFull(it.Clip);
    return r.GetWidth() >= it.Rect.GetWidth() - 0.5f && r.GetHeight() >= it.Rect.GetHeight() - 0.5f;
}
// The palette is taller than CI's Scene view: hide Assets so the Scene view gets its height.
Steps RoomForRail() {
    return {
        Do("hide the Assets panel (room for the whole palette)", [](Ctx& c) { ST::ShowAssets(c.E) = false; }),
        Until("the whole tool palette fits", [](Ctx&) { const UiItem* it = LastRailButton(); return it && FullyShown(*it); }, 180),
    };
}
// Every test ends here: tools back to their defaults, the camera reset, panels as they were.
Steps Tidy() {
    return {
        ChromeUi::CloseMenus(),
        Do("tools back to their defaults, panels back", [](Ctx& c) {
            ST::Defaults(c.E);
            ST::ShowAssets(c.E) = true;
            ST::ShowHistory(c.E) = false;
        }),
        ChromeUi::ResetCamera(), Wait(3),
    };
}

// The fixture on top of FreshScene's Alpha (-3,0,0), Beta (0,0,0), Gamma (3,0,0): a 1 m cube behind
// them so draw modes have a surface to show, and a sun to light it. The undo history starts after it.
Steps ToolScene() {
    return {
        FreshScene(),
        Do("add a cube and a sun; tools at their defaults", [](Ctx& c) {
            const glm::vec3 one(1.0f);
            c.W.CreateModelEntity(c.A.CreatePrimitive("cube"), glm::vec3(0, 0.5f, -4), glm::vec3(0, 30, 0), one, "Cube");
            const entt::entity sun = c.W.CreateEmptyEntity(glm::vec3(0, 8, 0), glm::vec3(-40, 50, 0), one, "Sun");
            auto& l = c.W.Registry.emplace<LightComponent>(sun);
            l.Kind = LightComponent::Type::Directional;
            l.Intensity = 6.0f;
            ST::Defaults(c.E);
            ST::ClearUndo(c.E);
        }),
        ChromeUi::ResetCamera(),
        ChromeUi::BringToFront(EditorPanels::Scene),
        Wait(3),
    };
}

// The undo depth is `n` (tool and view changes are not undo steps).
Step ExpectDepth(std::string what, size_t n) {
    return Expect(std::move(what), [n](Ctx& c) {
        return X::UndoDepth(c.E) == n ? std::string() : "history: " + ST::UndoLabels(c.E);
    });
}

// --- Viewport geometry ---------------------------------------------------------------------------
bool ToScreen(Ctx& c, glm::vec3 p, ImVec2& out) {
    const glm::vec2 pos = c.E.ViewportPos(), size = c.E.ViewportSize();
    if (size.x <= 1.0f || size.y <= 1.0f) return false;
    const glm::vec4 clip = c.Cam.ProjectionMatrix(size.x / size.y) * c.Cam.ViewMatrix() * glm::vec4(p, 1.0f);
    if (clip.w <= 1e-4f) return false;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    out = ImVec2(pos.x + (ndc.x * 0.5f + 0.5f) * size.x, pos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * size.y);
    return true;
}
glm::vec3 WorldPos(Ctx& c, const char* name) {
    const entt::entity e = ByName(c.W, name);
    return e == entt::null ? glm::vec3(NAN) : glm::vec3(c.W.ComposeWorldTransform(e)[3]);
}
glm::vec3 LocalPos(Ctx& c, const char* name) {
    const entt::entity e = ByName(c.W, name);
    return e == entt::null ? glm::vec3(NAN) : c.W.Registry.get<TransformComponent>(e).Position;
}
// A point at (fx, fy) of the viewport: the bottom right is empty ground in the fixture's view.
ImVec2 Spot(Ctx& c, float fx, float fy) {
    const glm::vec2 pos = c.E.ViewportPos(), size = c.E.ViewportSize();
    return ImVec2(pos.x + size.x * fx, pos.y + size.y * fy);
}
using Where = std::function<bool(Ctx&, ImVec2&)>;
Where AtObject(const char* name) {
    return [name](Ctx& c, ImVec2& out) { return ToScreen(c, WorldPos(c, name), out); };
}
Where AtSpot(float fx, float fy) {
    return [fx, fy](Ctx& c, ImVec2& out) { out = Spot(c, fx, fy); return true; };
}
// Moves there, waits a frame (hover settles), presses and releases `button` with `mods` held.
Step ClickAt(std::string what, Where where, int mods = 0, int button = 0) {
    auto phase = std::make_shared<int>(0);
    return {"click " + what, [where, mods, button, phase](Ctx& c) {
        ImGuiIO& io = ImGui::GetIO();
        switch ((*phase)++) {
            case 0: { ImVec2 p; if (!where(c, p)) { *phase = 0; return false; } MouseTo(p); Mods(mods, true); return false; }
            case 1: return false;
            case 2: io.AddMouseButtonEvent(button, true); return false;
            case 3: io.AddMouseButtonEvent(button, false); return false;
            default: Mods(mods, false); return true;
        }
    }};
}
// Press at `from`, move to `to` in a few frames, release there.
Step DragBetween(std::string what, Where from, Where to, int mods = 0) {
    auto phase = std::make_shared<int>(0);
    auto a = std::make_shared<ImVec2>(), b = std::make_shared<ImVec2>();
    return {"drag " + what, [from, to, mods, phase, a, b](Ctx& c) {
        ImGuiIO& io = ImGui::GetIO();
        const int k = (*phase)++;
        if (k == 0) {
            if (!from(c, *a) || !to(c, *b)) { *phase = 0; return false; }
            MouseTo(*a); Mods(mods, true); return false;
        }
        if (k == 1) return false;
        if (k == 2) { io.AddMouseButtonEvent(0, true); return false; }
        if (k < 3 + 8) {
            const float t = (float)(k - 2) / 8.0f;
            MouseTo(ImVec2(a->x + (b->x - a->x) * t, a->y + (b->y - a->y) * t));
            return false;
        }
        if (k < 14) return false;
        if (k == 14) { io.AddMouseButtonEvent(0, false); return false; }
        Mods(mods, false);
        return true;
    }};
}

// --- The transform gizmo ---------------------------------------------------------------------------
ImVec2 g_DragFrom, g_DragDir; // where the gizmo drag was grabbed, and the screen direction of its axis
// Moves the pointer inward along the selection's `axis` handle, starting well beyond its tip,
// until the gizmo reports the pointer over it. m_GizmoEngaged trails the pointer by a frame or
// two, so each position is held for a few frames and has to read "over" twice in a row.
Step HoverGizmoAxis(glm::vec3 axis) {
    auto t = std::make_shared<float>(-1.0f);
    auto held = std::make_shared<int>(0);
    auto over = std::make_shared<int>(0);
    return {"hover the gizmo's axis handle", [axis, t, held, over](Ctx& c) {
        if (*t >= 0.0f && ++*held < 3) return false; // let the hover state catch up
        *held = 0;
        const entt::entity s = X::Selected(c.E);
        if (s == entt::null) return false;
        const glm::vec3 p = glm::vec3(c.W.ComposeWorldTransform(s)[3]);
        ImVec2 o, q;
        if (!ToScreen(c, p, o) || !ToScreen(c, p + axis * 0.25f, q)) return false;
        const float len = std::sqrt((q.x - o.x) * (q.x - o.x) + (q.y - o.y) * (q.y - o.y));
        if (len < 1e-3f) { Fail("the axis is edge-on to the view"); return true; }
        const ImVec2 d((q.x - o.x) / len, (q.y - o.y) / len);
        if (*t < 0.0f) {
            *t = c.E.ViewportSize().x * 0.2f;
        } else if (ST::GizmoEngaged(c.E)) {
            if (++*over < 2) return false; // same spot, read it again
            g_DragFrom = ImGui::GetIO().MousePos;
            g_DragDir = d;
            return true;
        } else if (*over = 0, (*t -= 3.0f) < 6.0f) {
            Fail("the pointer never got over the gizmo's handle");
            return true;
        }
        MouseTo(ImVec2(o.x + d.x * *t, o.y + d.y * *t));
        return false;
    }, 1200};
}
// With the button down on the handle, slides the pointer `px` along the axis over a few frames
// (or across it: a rotate ring turns for a tangential drag, not a radial one).
Step SlideAlongAxis(float px, bool across = false) {
    auto k = std::make_shared<int>(0);
    return {"slide along the axis", [px, across, k](Ctx&) {
        const float t = std::min(1.0f, (float)++*k / 10.0f);
        const ImVec2 d = across ? ImVec2(-g_DragDir.y, g_DragDir.x) : g_DragDir;
        MouseTo(ImVec2(g_DragFrom.x + d.x * px * t, g_DragFrom.y + d.y * px * t));
        return *k >= 12;
    }};
}
// Grab the selection's X handle, drag it `px` along the axis, release.
Steps GizmoDragX(float px, const char* shot = nullptr) {
    Steps s = {HoverGizmoAxis(glm::vec3(1, 0, 0)), Wait(1), MouseButton(0, true), Wait(2), SlideAlongAxis(px)};
    if (shot) s.push_back(Shot(shot, {kScene}));
    s.push_back(MouseButton(0, false));
    s.push_back(Wait(3));
    return s;
}

// --- The camera ------------------------------------------------------------------------------------
glm::vec3 Look(float yawDeg, float pitchDeg) {
    const float y = glm::radians(yawDeg), p = glm::radians(pitchDeg);
    return glm::normalize(glm::vec3(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p)));
}
Step UntilLooks(std::string what, glm::vec3 dir, int ortho /* 1 ortho, 0 perspective, -1 either */) {
    return Until("the camera looks " + what, [dir, ortho](Ctx& c) {
        return !ST::Gliding(c.E) && glm::dot(c.Cam.Front(), dir) > 0.999f && (ortho < 0 || c.Cam.Orthographic == (ortho == 1));
    }, 300);
}
Step UntilStill() { return Until("the camera stops gliding", [](Ctx& c) { return !ST::Gliding(c.E); }, 300); }

// --- Undo bookkeeping ------------------------------------------------------------------------------
std::string SelectionNames(Ctx& c) {
    std::vector<std::string> names;
    for (entt::entity e : X::Selection(c.E)) names.push_back(NameOf(c.W, e));
    std::sort(names.begin(), names.end());
    std::string s;
    for (const std::string& n : names) s += (s.empty() ? "" : ",") + n;
    return s.empty() ? "(none)" : s;
}
std::string FirstDifference(const std::string& a, const std::string& b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    const size_t from = i > 80 ? i - 80 : 0;
    return "differs at byte " + std::to_string(i) + " of " + std::to_string(a.size()) + "/" + std::to_string(b.size()) +
           ": '" + a.substr(from, 160) + "' vs '" + b.substr(from, 160) + "'";
}
// The scene (and selection) recorded at each undo depth while the chain was built.
struct Snapshot { bool Has = false; std::string Dump, Selection, Top; };
std::vector<Snapshot> g_At;
size_t g_TopDepth = 0;
Step Record(std::string after) {
    return Do("record the scene after " + after, [](Ctx& c) {
        const size_t d = X::UndoDepth(c.E);
        if (g_At.size() <= d) g_At.resize(d + 1);
        g_At[d] = {true, SceneSerializer::SaveToString(c.W, c.A), SelectionNames(c), ST::TopLabel(c.E)};
        g_TopDepth = std::max(g_TopDepth, d);
    });
}
// "" when the scene and selection match what was recorded at the current depth (or nothing was).
std::string MatchesRecord(Ctx& c) {
    const size_t d = X::UndoDepth(c.E);
    if (d >= g_At.size() || !g_At[d].Has) return std::string();
    const std::string now = SceneSerializer::SaveToString(c.W, c.A);
    if (now != g_At[d].Dump) return "depth " + std::to_string(d) + " (after '" + g_At[d].Top + "'): scene " + FirstDifference(g_At[d].Dump, now);
    if (SelectionNames(c) != g_At[d].Selection)
        return "depth " + std::to_string(d) + " (after '" + g_At[d].Top + "'): selected " + SelectionNames(c) + ", recorded " + g_At[d].Selection;
    return std::string();
}
// Ctrl+Z (or Ctrl+Y) one press at a time until the undo depth reaches `target`, checking the scene
// against the recording at every depth on the way. Each press must move the depth by exactly one.
Step Walk(bool undo, size_t target, std::shared_ptr<int> checked) {
    auto phase = std::make_shared<int>(0);
    auto from = std::make_shared<size_t>(0);
    auto frames = std::make_shared<int>(0);
    const ImGuiKey key = undo ? ImGuiKey_Z : ImGuiKey_Y;
    return {std::string(undo ? "undo" : "redo") + " step by step to depth " + std::to_string(target),
            [undo, target, checked, phase, from, frames, key](Ctx& c) {
        ImGuiIO& io = ImGui::GetIO();
        const size_t d = X::UndoDepth(c.E);
        switch (*phase) {
            case 0:
                if (undo ? d <= target : d >= target) return true;
                *from = d;
                Mods(ImGuiMod_Ctrl, true);
                *phase = 1;
                return false;
            case 1: io.AddKeyEvent(key, true); *phase = 2; return false;
            case 2: io.AddKeyEvent(key, false); Mods(ImGuiMod_Ctrl, false); *phase = 3; *frames = 0; return false;
            case 3:
                if (d == *from) {
                    if (++*frames > 60) { Fail(std::string(undo ? "Ctrl+Z" : "Ctrl+Y") + " did nothing at depth " + std::to_string(d)); return true; }
                    return false;
                }
                if (d != (undo ? *from - 1 : *from + 1)) {
                    Fail("one press moved the depth from " + std::to_string(*from) + " to " + std::to_string(d));
                    return true;
                }
                *phase = 4; *frames = 0;
                return false;
            default: {
                if (++*frames < 3) return false; // let the restored scene settle
                const std::string why = MatchesRecord(c);
                if (!why.empty()) { Fail(std::string(undo ? "undo" : "redo") + " to " + why); return true; }
                if (d < g_At.size() && g_At[d].Has) ++*checked;
                *phase = 0;
                return false;
            }
        }
    }, 6000};
}
} // namespace smoke_tools

void RegisterSceneToolsTests() {
    using namespace smoke_tools;

    // The five transform tools and Hand, by clicking their palette buttons, with Beta selected so
    // each tool's gizmo is in the shot.
    {
        struct Tool { const char* Icon; int Op; const char* Shot; };
        static const Tool kTools[] = {
            {EDITOR_ICON_ROTATE, 1, "rotate"}, {EDITOR_ICON_SCALE, 2, "scale"}, {EDITOR_ICON_RECT_TOOL, 3, "rect"},
            {EDITOR_ICON_UNIVERSAL, 4, "universal"}, {EDITOR_ICON_TRANSLATE, 0, "translate"},
        };
        Steps s = Seq({ToolScene(), {SelectByName("Cube"), Wait(3)}});
        s.push_back(Click(Rail(EDITOR_ICON_HAND_TOOL)));
        s.push_back(Wait(2));
        s.push_back(ExpectTrue("Hand on", [](Ctx& c) { return ST::Hand(c.E); }));
        s.push_back(Shot("hand", {kScene}));
        s.push_back(Click(Rail(EDITOR_ICON_HAND_TOOL)));
        s.push_back(Wait(2));
        s.push_back(ExpectTrue("a second click turns Hand off", [](Ctx& c) { return !ST::Hand(c.E); }));
        s.push_back(Click(Rail(EDITOR_ICON_HAND_TOOL)));
        s.push_back(Wait(2));
        for (const Tool& t : kTools) {
            const Tool tool = t;
            s.push_back(Click(Rail(tool.Icon)));
            s.push_back(Wait(3));
            s.push_back(Expect(std::string(tool.Shot) + " is the tool", [tool](Ctx& c) {
                if (ST::Hand(c.E)) return std::string("still the Hand tool");
                return ST::GizmoOp(c.E) == tool.Op ? std::string() : "gizmo op " + std::to_string(ST::GizmoOp(c.E));
            }));
            s.push_back(Shot(tool.Shot, {kScene}));
        }
        s.push_back(ExpectDepth("picking tools pushed no undo step (just the selection)", 1));
        for (const Step& st : Tidy()) s.push_back(st);
        Add("scenetools: the palette picks Hand, Translate, Rotate, Scale, Rect and Universal", s);
    }

    Add("scenetools: Q W E R T Y and M switch tools over the Scene view", Seq({
        ToolScene(),
        {SelectByName("Cube"), HoverPanel(kScene), Wait(2),
         Key(ImGuiKey_Q), Wait(2),
         ExpectTrue("Q: Hand", [](Ctx& c) { return ST::Hand(c.E); }),
         Key(ImGuiKey_E), Wait(2),
         ExpectTrue("E: Rotate (and out of Hand)", [](Ctx& c) { return !ST::Hand(c.E) && ST::GizmoOp(c.E) == 1; }),
         Key(ImGuiKey_R), Wait(2),
         ExpectTrue("R: Scale", [](Ctx& c) { return ST::GizmoOp(c.E) == 2; }),
         Key(ImGuiKey_T), Wait(2),
         ExpectTrue("T: Rect", [](Ctx& c) { return ST::GizmoOp(c.E) == 3; }),
         Key(ImGuiKey_Y), Wait(2),
         ExpectTrue("Y: Universal", [](Ctx& c) { return ST::GizmoOp(c.E) == 4; }),
         Key(ImGuiKey_W), Wait(2),
         ExpectTrue("W: Translate", [](Ctx& c) { return ST::GizmoOp(c.E) == 0; }),
         Key(ImGuiKey_M), Wait(2),
         ExpectTrue("M: Measure on", [](Ctx& c) { return ST::Measure(c.E); }),
         Key(ImGuiKey_Q), Wait(2),
         ExpectTrue("Q leaves Measure for Hand", [](Ctx& c) { return ST::Hand(c.E) && !ST::Measure(c.E); }),
         Key(ImGuiKey_W), Wait(2),
         ExpectTrue("W leaves Hand", [](Ctx& c) { return !ST::Hand(c.E) && ST::GizmoOp(c.E) == 0; }),
         // Typing in a text field never switches tools.
         Click(ByExact("##search", kHierarchy)),
         Until("the search field has the keyboard", [](Ctx&) { return ImGui::GetIO().WantTextInput; }),
         Key(ImGuiKey_E), Wait(2),
         ExpectTrue("E typed into the Hierarchy search is not a tool switch", [](Ctx& c) { return ST::GizmoOp(c.E) == 0; }),
         Do("leave the field", [](Ctx&) { ImGui::ClearActiveID(); }), Wait(2),
         Key(ImGuiKey_Escape), Wait(2)},
        Tidy(),
    }));

    Add("scenetools: the Hand tool drag pans the camera and selects or moves nothing", Seq({
        ToolScene(),
        {SelectByName("Beta"), HoverPanel(kScene), Wait(2),
         Key(ImGuiKey_Q), Wait(2),
         ExpectTrue("Hand on", [](Ctx& c) { return ST::Hand(c.E); }),
         ChromeUi::NoteCamera(),
         Do("note the scene", [](Ctx& c) { g_Text = SceneSerializer::SaveToString(c.W, c.A); g_Number = (double)X::UndoDepth(c.E); }),
         // From empty ground across Gamma's icon: Hand never picks.
         DragBetween("across the view", AtSpot(0.75f, 0.8f), AtObject("Gamma")), Wait(3),
         ExpectTrue("the camera moved", [](Ctx& c) { return ChromeUi::CameraMoved(c); }),
         Expect("nothing else changed", [](Ctx& c) {
             if (X::UndoDepth(c.E) != (size_t)g_Number) return "undo history: " + ST::TopLabel(c.E);
             const std::string now = SceneSerializer::SaveToString(c.W, c.A);
             return now == g_Text ? ExpectSelected(c, "Beta") : FirstDifference(g_Text, now);
         })},
        Tidy(),
    }));

    Add("scenetools: Measure chains points by click, right-click clears, Escape exits", Seq({
        ToolScene(),
        {SelectByName("Beta"), Wait(2),
         Click(Rail(EDITOR_ICON_MEASURE)), Wait(2),
         ExpectTrue("Measure on", [](Ctx& c) { return ST::Measure(c.E) && ST::MeasurePoints(c.E) == 0; }),
         ClickAt("a first point", AtSpot(0.6f, 0.8f)),
         ClickAt("a second point", AtSpot(0.85f, 0.7f)), Wait(2),
         ExpectTrue("two points", [](Ctx& c) { return ST::MeasurePoints(c.E) == 2; }),
         ClickAt("on Gamma", AtObject("Gamma")), Wait(2),
         Expect("a third point, and the click selected nothing", [](Ctx& c) {
             if (ST::MeasurePoints(c.E) != 3) return "points: " + std::to_string(ST::MeasurePoints(c.E));
             return ExpectSelected(c, "Beta");
         }),
         Shot("three-points", {kScene}),
         ClickAt("right-click", AtSpot(0.7f, 0.8f), 0, 1), Wait(2),
         ExpectTrue("right-click cleared them, still measuring", [](Ctx& c) { return ST::MeasurePoints(c.E) == 0 && ST::Measure(c.E); }),
         ClickAt("one more point", AtSpot(0.6f, 0.8f)), Wait(2),
         Key(ImGuiKey_Escape), Wait(2),
         ExpectTrue("Escape exits Measure and drops the points", [](Ctx& c) { return !ST::Measure(c.E) && ST::MeasurePoints(c.E) == 0; }),
         ExpectDepth("measuring pushed no undo step", 1)},
        Tidy(),
    }));

    Add("scenetools: Duplicate Array from the palette lays out a 3 x 1 x 2 grid; undo removes it", Seq({
        ToolScene(),
        {Do("nothing selected", [](Ctx& c) { X::Deselect(c.E); }), Wait(2),
         Click(Rail(EDITOR_ICON_DUPLICATE_ARRAY)), Wait(3),
         ExpectTrue("with nothing selected the dialog does not open", [](Ctx& c) { return !ST::ArrayDupShown(c.E); }),
         SelectByName("Gamma"), Wait(2),
         Do("note the scene", [](Ctx& c) { g_Text = SceneSerializer::SaveToString(c.W, c.A); }),
         Click(Rail(EDITOR_ICON_DUPLICATE_ARRAY)),
         Until("the dialog", [](Ctx&) { return Find(ByExact("Create", "##ArrayDup")) != nullptr; }),
         Do("3 x 1 x 2 cells, 2 m apart in X and 1.5 m in Z", [](Ctx& c) { ST::SetArrayDup(c.E, 3, 1, 2, glm::vec3(2.0f, 0.0f, 1.5f)); }),
         Wait(2),
         Shot("dialog"),
         Click(ByExact("Create", "##ArrayDup")), Wait(3),
         Expect("five copies on the grid", [](Ctx& c) {
             std::vector<std::string> got, want;
             for (auto [e, n] : c.W.Registry.view<const NameComponent>().each())
                 if (n.Name.rfind("Gamma", 0) == 0) {
                     const glm::vec3 p = c.W.Registry.get<TransformComponent>(e).Position;
                     char buf[64];
                     std::snprintf(buf, sizeof buf, "(%.2f,%.2f,%.2f)", p.x, p.y, p.z);
                     got.push_back(buf);
                 }
             for (int z = 0; z < 2; ++z)
                 for (int x = 0; x < 3; ++x) {
                     char buf[64];
                     std::snprintf(buf, sizeof buf, "(%.2f,%.2f,%.2f)", 3.0f + 2.0f * x, 0.0f, 1.5f * z);
                     want.push_back(buf);
                 }
             std::sort(got.begin(), got.end());
             std::sort(want.begin(), want.end());
             std::string g, w;
             for (const auto& x : got) g += x;
             for (const auto& x : want) w += x;
             return g == w ? std::string() : "Gammas at " + g + ", expected " + w;
         }),
         ExpectTrue("dialog closed", [](Ctx& c) { return !ST::ArrayDupShown(c.E); }),
         Shot("grid", {kScene}),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         Expect("one undo removes every copy", [](Ctx& c) {
             const std::string now = SceneSerializer::SaveToString(c.W, c.A);
             return now == g_Text ? std::string() : FirstDifference(g_Text, now);
         })},
        Tidy(),
    }));

    Add("scenetools: Local/World, Pivot/Center, grid, snap and gizmo toggles by click and by shortcut", Seq({
        ToolScene(), RoomForRail(),
        {SelectByName("Cube"), Wait(2),
         Click(Rail(EDITOR_ICON_WORLD_SPACE)), Wait(2),
         ExpectTrue("Local space", [](Ctx& c) { return ST::Local(c.E); }),
         Until("the button now shows Local", [](Ctx&) { return Find(Rail(EDITOR_ICON_LOCAL_SPACE)) != nullptr; }),
         Shot("local-space", {kScene}),
         HoverPanel(kScene), Key(ImGuiKey_X, ImGuiMod_Alt), Wait(2),
         ExpectTrue("Alt+X: World again", [](Ctx& c) { return !ST::Local(c.E); }),
         Click(Rail(EDITOR_ICON_PIVOT_ORIGIN)), Wait(2),
         ExpectTrue("Center", [](Ctx& c) { return ST::Center(c.E); }),
         Until("the button now shows Center", [](Ctx&) { return Find(Rail(EDITOR_ICON_PIVOT_CENTER)) != nullptr; }),
         HoverPanel(kScene), Key(ImGuiKey_X, ImGuiMod_Alt | ImGuiMod_Shift), Wait(2),
         ExpectTrue("Alt+Shift+X: Pivot again", [](Ctx& c) { return !ST::Center(c.E); }),
         Click(Rail(EDITOR_ICON_GRID)), Wait(2),
         ExpectTrue("grid off", [](Ctx& c) { return !ST::Grid(c.E); }),
         Shot("no-grid", {kScene}),
         HoverPanel(kScene), Key(ImGuiKey_G, ImGuiMod_Alt), Wait(2),
         ExpectTrue("Alt+G: grid on", [](Ctx& c) { return ST::Grid(c.E); }),
         Click(Rail(EDITOR_ICON_SNAP_TO_GRID)), Wait(2),
         ExpectTrue("snap off", [](Ctx& c) { return !ST::Snap(c.E); }),
         HoverPanel(kScene), Key(ImGuiKey_G, ImGuiMod_Alt | ImGuiMod_Shift), Wait(2),
         ExpectTrue("Alt+Shift+G: snap on", [](Ctx& c) { return ST::Snap(c.E); }),
         Click(Rail(EDITOR_ICON_TOGGLE_GIZMOS)), Wait(2),
         ExpectTrue("gizmos off", [](Ctx& c) { return !ST::Gizmos(c.E); }),
         Shot("no-gizmos", {kScene}),
         Click(Rail(EDITOR_ICON_TOGGLE_GIZMOS)), Wait(2),
         ExpectTrue("gizmos on", [](Ctx& c) { return ST::Gizmos(c.E); }),
         // Right-click opens each button's settings popover.
         RightClick(Rail(EDITOR_ICON_GRID)),
         Until("the grid & snap popover", [](Ctx&) { return Find(PopupItem("Show grid")) != nullptr; }),
         Shot("grid-popover"),
         Click(PopupItem("Show grid")), Wait(2),
         ExpectTrue("its Show grid checkbox turns the grid off", [](Ctx& c) { return !ST::Grid(c.E); }),
         Click(PopupItem("Show grid")), Wait(2),
         ExpectTrue("and on", [](Ctx& c) { return ST::Grid(c.E); }),
         ChromeUi::CloseMenus(), Wait(2),
         RightClick(Rail(EDITOR_ICON_TOGGLE_GIZMOS)),
         Until("the gizmos popover", [](Ctx&) { return Find(PopupItem("Entity icons")) != nullptr; }),
         Click(PopupItem("Entity icons")), Wait(2),
         ExpectTrue("entity icons off", [](Ctx& c) { return !ST::EntityIcons(c.E); }),
         Shot("gizmos-popover", {kScene}),
         Click(PopupItem("Entity icons")), Wait(2),
         ExpectTrue("entity icons on", [](Ctx& c) { return ST::EntityIcons(c.E); }),
         Key(ImGuiKey_Escape), Wait(2),
         ExpectTrue("Escape closes the popover", [](Ctx&) { return Find(PopupItem("Entity icons")) == nullptr; }),
         ExpectDepth("toggles pushed no undo step", 1)},
        Tidy(),
    }));

    {
        struct Mode { const char* Name; const char* Icon; };
        static const Mode kModes[] = {
            {"Wireframe", EDITOR_ICON_WIREFRAME_MODE}, {"Unlit", EDITOR_ICON_UNLIT_MODE}, {"Normals", EDITOR_ICON_NORMALS_MODE},
            {"Shadow Cascades", EDITOR_ICON_CASCADES_MODE}, {"Mip / Texel Density", EDITOR_ICON_MIP_MODE}, {"Shaded", EDITOR_ICON_SHADED_MODE},
        };
        Steps s = Seq({ToolScene(), RoomForRail(), {SelectByName("Cube"), Wait(2)}});
        const char* icon = EDITOR_ICON_SHADED_MODE;
        for (int i = 0; i < 6; ++i) {
            const Mode mode = kModes[i];
            const int index = (i + 1) % 6;
            s.push_back(Click(Rail(icon)));
            s.push_back(Until("the draw mode popup", [](Ctx&) { return Find(MenuItem("Wireframe")) != nullptr; }));
            if (i == 0) s.push_back(Shot("draw-mode-popup"));
            s.push_back(Click(MenuItem(mode.Name)));
            s.push_back(Wait(3));
            s.push_back(Expect(std::string("draw mode ") + mode.Name, [index](Ctx& c) {
                return ST::Shading(c.E) == index ? std::string() : "mode " + std::to_string(ST::Shading(c.E));
            }));
            s.push_back(Until("the button shows it", [mode](Ctx&) { return Find(Rail(mode.Icon)) != nullptr; }));
            if (index != 0) s.push_back(Shot(mode.Name, {kScene}));
            icon = mode.Icon;
        }
        for (const Step& st : Steps{
                 HoverPanel(kScene), Key(ImGuiKey_D, ImGuiMod_Alt), Wait(2),
                 ExpectTrue("Alt+D: next mode (Wireframe)", [](Ctx& c) { return ST::Shading(c.E) == 1; }),
                 Key(ImGuiKey_D, ImGuiMod_Alt | ImGuiMod_Shift), Wait(2), Key(ImGuiKey_D, ImGuiMod_Alt | ImGuiMod_Shift), Wait(2),
                 ExpectTrue("Alt+Shift+D twice: wraps back to Mip", [](Ctx& c) { return ST::Shading(c.E) == 5; }),
                 Key(ImGuiKey_D, ImGuiMod_Alt), Wait(2),
                 ExpectTrue("Alt+D wraps forward to Shaded", [](Ctx& c) { return ST::Shading(c.E) == 0; }),
                 ExpectDepth("draw modes pushed no undo step", 1),
             })
            s.push_back(st);
        for (const Step& st : Tidy()) s.push_back(st);
        Add("scenetools: every draw mode from the palette popup, and Alt+D / Alt+Shift+D cycle them", s);
    }

    Add("scenetools: orthographic by click and 5; view presets 1 3 7 (Ctrl: opposite) and 0; the nav gizmo's axis", Seq({
        ToolScene(), RoomForRail(),
        {Do("nothing selected (views pivot on the scene)", [](Ctx& c) { X::Deselect(c.E); }), Wait(2),
         Click(Rail(EDITOR_ICON_PERSPECTIVE)), Wait(2),
         UntilLooks("the same way, orthographic", Look(-90.0f, -10.0f), 1),
         Until("the button shows Orthographic", [](Ctx&) { return Find(Rail(EDITOR_ICON_ORTHOGRAPHIC)) != nullptr; }),
         Shot("ortho", {kScene}),
         Click(Rail(EDITOR_ICON_ORTHOGRAPHIC)), Wait(2),
         UntilLooks("the same way, perspective", Look(-90.0f, -10.0f), 0),
         HoverPanel(kScene), Key(ImGuiKey_5), Wait(2),
         UntilLooks("the same way after 5, orthographic", Look(-90.0f, -10.0f), 1),
         HoverPanel(kScene), Key(ImGuiKey_5), Wait(2),
         UntilLooks("the same way after 5 again, perspective", Look(-90.0f, -10.0f), 0),
         HoverPanel(kScene), Key(ImGuiKey_1), UntilLooks("front (1)", Look(-90.0f, 0.0f), 1), Shot("front", {kScene}),
         HoverPanel(kScene), Key(ImGuiKey_1, ImGuiMod_Ctrl), UntilLooks("back (Ctrl+1)", Look(90.0f, 0.0f), 1),
         HoverPanel(kScene), Key(ImGuiKey_3), UntilLooks("right (3)", Look(180.0f, 0.0f), 1),
         HoverPanel(kScene), Key(ImGuiKey_3, ImGuiMod_Ctrl), UntilLooks("left (Ctrl+3)", Look(0.0f, 0.0f), 1),
         HoverPanel(kScene), Key(ImGuiKey_7), UntilLooks("down from the top (7)", Look(-90.0f, -89.9f), 1), Shot("top", {kScene}),
         HoverPanel(kScene), Key(ImGuiKey_7, ImGuiMod_Ctrl), UntilLooks("up from the bottom (Ctrl+7)", Look(-90.0f, 89.9f), 1),
         HoverPanel(kScene), Key(ImGuiKey_0), UntilLooks("iso (0)", Look(-45.0f, -35.264f), 1), Shot("iso", {kScene}),
         HoverPanel(kScene), Key(ImGuiKey_5), UntilLooks("iso, perspective (5)", Look(-45.0f, -35.264f), 0),
         // The nav gizmo's +Y bubble: sweep the corner until the gizmo reports it hovered, click it.
         Do("start the sweep", [](Ctx&) { g_Number = 0.0; }),
         Until("the pointer is over the nav gizmo's +Y bubble", [](Ctx& c) {
             if (ImViewGuizmo::GetContext().hoveredAxisID == 2) return true;
             const glm::vec2 pos = c.E.ViewportPos(), size = c.E.ViewportSize();
             const float scale = EditorTheme::Px(1.0f) * 0.7f, r = 128.0f * scale;
             const ImVec2 centre(pos.x + size.x - EditorTheme::Px(14.0f) - r, pos.y + EditorTheme::Px(14.0f) + r);
             const int n = (int)g_Number++, cols = 40;
             MouseTo(ImVec2(centre.x - r + 2.0f * r * (float)(n % cols) / (cols - 1), centre.y - r + 2.0f * r * (float)(n / cols) / (cols - 1)));
             return false;
         }, 1700),
         Shot("nav-gizmo-hover", {kScene}),
         MouseButton(0, true), Wait(1), MouseButton(0, false),
         Until("the camera looks straight down", [](Ctx& c) { return glm::dot(c.Cam.Front(), glm::vec3(0, -1, 0)) > 0.99f; }, 300),
         WaitSeconds(0.5),
         ExpectDepth("view changes pushed no undo step", 0)},
        Tidy(),
    }));

    // Every viewport pick is its own undo step, the first one after an edit too (that one used to
    // match the edit's own entry and was dropped as a duplicate).
    Add("scenetools: a viewport pick right after an edit is its own undo step", Seq({
        ToolScene(),
        {ClickAt("Gamma", AtObject("Gamma")), Wait(3),
         ExpectDepth("one selection step", 1),
         Do("rename Alpha (Gamma stays selected)", [](Ctx& c) {
             X::PushUndo(c.E, c.W, "Rename Alpha");
             c.W.Registry.get<NameComponent>(ByName(c.W, "Alpha")).Name = "Alpha 2";
         }), Wait(3),
         ClickAt("Beta", AtObject("Beta")), Wait(3),
         Expect("the pick is listed after the rename", [](Ctx& c) {
             return X::UndoDepth(c.E) == 3 && ST::TopLabel(c.E) == "Select Beta (sel)" ? std::string() : "history: " + ST::UndoLabels(c.E);
         }),
         HoverPanel(kScene), Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         Expect("the first Ctrl+Z only undoes the pick", [](Ctx& c) {
             if (ByName(c.W, "Alpha 2") == entt::null) return std::string("the rename was undone too");
             return ExpectSelected(c, "Gamma");
         }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("the second undoes the rename", [](Ctx& c) { return ByName(c.W, "Alpha") != entt::null; })},
        Tidy(),
    }));

    // Bug #15: Undo restores the snapshot cached after the last undo step, so a scene change made
    // without one went back with whatever edit Ctrl+Z took back. A .prefab dropped onto the editor
    // (the OS drop, or the Asset Library's import) was such a change: instantiated with no undo step.
    {
        auto dropDir = [] { return ProjectPaths::Resolve("editor-smoke-scratch-undo"); };
        auto dropPath = [dropDir] { return (std::filesystem::u8path(dropDir()) / "SmokeDrop.prefab").u8string(); };
        Add("scenetools: a prefab dropped onto the editor is its own undo step; Ctrl+Z after an edit and a drop takes back only the drop", Seq({
            {Do("write a scratch prefab (Dropped Crate)", [dropDir, dropPath](Ctx&) {
                std::error_code ec;
                std::filesystem::remove_all(std::filesystem::u8path(dropDir()), ec);
                std::filesystem::create_directories(std::filesystem::u8path(dropDir()), ec);
                World w;
                const glm::vec3 zero(0.0f), one(1.0f);
                const entt::entity root = w.CreateEmptyEntity(zero, zero, one, "Dropped Crate");
                w.Registry.emplace<LightComponent>(root).Intensity = 3.0f;
                if (!SceneSerializer::SavePrefab(w, root, dropPath())) Fail("SavePrefab failed");
            })},
            ToolScene(),
            {Do("rename Alpha (an undo step)", [](Ctx& c) {
                 X::PushUndo(c.E, c.W, "Rename Alpha");
                 c.W.Registry.get<NameComponent>(ByName(c.W, "Alpha")).Name = "Alpha 2";
             }), Wait(3),
             Do("drop the prefab onto the editor", [dropPath](Ctx& c) { c.E.HandleDroppedFiles(c.W, c.A, c.Cam, true, {dropPath()}); }),
             Until("its instance is in the scene", [](Ctx& c) { return ByName(c.W, "Dropped Crate") != entt::null; }, 240), Wait(3),
             HoverPanel(kScene), Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
             Expect("the first Ctrl+Z takes back only the drop", [](Ctx& c) {
                 if (ByName(c.W, "Alpha 2") == entt::null) return "the rename was undone too; history: " + ST::UndoLabels(c.E);
                 return ByName(c.W, "Dropped Crate") == entt::null ? std::string() : "the instance is still there; history: " + ST::UndoLabels(c.E);
             }),
             Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
             ExpectTrue("the second undoes the rename", [](Ctx& c) { return ByName(c.W, "Alpha") != entt::null; }),
             Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3), Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3),
             Expect("Ctrl+Y twice redoes both", [](Ctx& c) {
                 return ByName(c.W, "Alpha 2") != entt::null && ByName(c.W, "Dropped Crate") != entt::null
                     ? std::string() : "history: " + ST::UndoLabels(c.E);
             })},
            Tidy(),
        }), [dropDir, dropPath](Ctx& c) {
            c.A.RemovePrefab(dropPath());
            // The drop filed the prefab under an Asset Browser folder for its directory.
            const auto& folders = c.A.Folders();
            if (std::find(folders.begin(), folders.end(), "editor-smoke-scratch-undo") != folders.end())
                c.A.DeleteFolderRecursive("editor-smoke-scratch-undo");
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::u8path(dropDir()), ec);
        });
    }

    // Bug #15, for any path that still forgets its undo step: the change it made is not thrown
    // away with the edit before it. Ctrl+Z takes back only that change, as a step of its own.
    Add("scenetools: a scene change made without an undo step is its own step at the next Ctrl+Z, not lost with the edit before it", Seq({
        ToolScene(),
        {Do("note Gamma's intensity", [](Ctx& c) { g_Number = c.W.Registry.get<LightComponent>(ByName(c.W, "Gamma")).Intensity; }),
         Do("rename Alpha (an undo step)", [](Ctx& c) {
             X::PushUndo(c.E, c.W, "Rename Alpha");
             c.W.Registry.get<NameComponent>(ByName(c.W, "Alpha")).Name = "Alpha 2";
         }), Wait(3),
         Do("dim Gamma with no undo step", [](Ctx& c) { c.W.Registry.get<LightComponent>(ByName(c.W, "Gamma")).Intensity = 0.25f; }), Wait(3),
         ExpectDepth("only the rename is a step so far", 1),
         HoverPanel(kScene), Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         Expect("Ctrl+Z takes back only the dimming", [](Ctx& c) {
             if (ByName(c.W, "Alpha 2") == entt::null) return "the rename was undone too; history: " + ST::UndoLabels(c.E);
             const float i = c.W.Registry.get<LightComponent>(ByName(c.W, "Gamma")).Intensity;
             return i == (float)g_Number ? std::string() : "Gamma's intensity is " + std::to_string(i);
         }),
         Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3),
         Expect("Ctrl+Y brings the dimming back", [](Ctx& c) {
             const float i = c.W.Registry.get<LightComponent>(ByName(c.W, "Gamma")).Intensity;
             return i == 0.25f && ByName(c.W, "Alpha 2") != entt::null ? std::string() : "history: " + ST::UndoLabels(c.E);
         }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3), Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         Expect("two more undo the dimming, then the rename", [](Ctx& c) {
             const float i = c.W.Registry.get<LightComponent>(ByName(c.W, "Gamma")).Intensity;
             return i == (float)g_Number && ByName(c.W, "Alpha") != entt::null ? std::string() : "history: " + ST::UndoLabels(c.E);
         })},
        Tidy(),
    }));

    Add("scenetools: F frames the selection; viewport click selects, Ctrl+click adds, empty space clears, a box drag selects", Seq({
        ToolScene(),
        {SelectByName("Gamma"), Wait(2),
         ChromeUi::NoteCamera(),
         HoverPanel(kScene), Key(ImGuiKey_F), Wait(2),
         UntilStill(),
         Expect("Gamma is centred in the view and the camera came closer", [](Ctx& c) {
             ImVec2 p;
             if (!ToScreen(c, WorldPos(c, "Gamma"), p)) return std::string("Gamma is behind the camera");
             const ImVec2 mid = Spot(c, 0.5f, 0.5f);
             const glm::vec2 size = c.E.ViewportSize();
             if (std::fabs(p.x - mid.x) > size.x * 0.02f || std::fabs(p.y - mid.y) > size.y * 0.02f)
                 return "Gamma at (" + std::to_string(p.x) + "," + std::to_string(p.y) + "), centre (" + std::to_string(mid.x) + "," + std::to_string(mid.y) + ")";
             return glm::length(c.Cam.Position - WorldPos(c, "Gamma")) < glm::length(g_Vec - WorldPos(c, "Gamma")) ? std::string() : std::string("not closer");
         }),
         Shot("framed", {kScene}),
         ChromeUi::ResetCamera(),
         Do("nothing selected", [](Ctx& c) { X::Deselect(c.E); }), Wait(3),
         ClickAt("Alpha's icon", AtObject("Alpha")), Wait(2),
         Expect("Alpha selected", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
         ClickAt("Gamma's icon with Ctrl", AtObject("Gamma"), ImGuiMod_Ctrl), Wait(2),
         Expect("Alpha and Gamma selected", [](Ctx& c) { return SelectionNames(c) == "Alpha,Gamma" ? std::string() : SelectionNames(c); }),
         Shot("two-picked", {kScene}),
         ClickAt("empty ground", AtSpot(0.75f, 0.85f)), Wait(2),
         Expect("nothing selected", [](Ctx& c) { return SelectionNames(c) == "(none)" ? std::string() : SelectionNames(c); }),
         ClickAt("the cube", AtObject("Cube")), Wait(2),
         Expect("the cube picked through its mesh", [](Ctx& c) { return ExpectSelected(c, "Cube"); }),
         DragBetween("a box around Alpha, Beta and Gamma",
             [](Ctx& c, ImVec2& out) { if (!ToScreen(c, WorldPos(c, "Alpha"), out)) return false; out.x -= 30.0f; out.y -= 12.0f; return true; },
             [](Ctx& c, ImVec2& out) { if (!ToScreen(c, WorldPos(c, "Gamma"), out)) return false; out.x += 30.0f; out.y += 12.0f; return true; }),
         Wait(3),
         Expect("the three are selected", [](Ctx& c) {
             const std::string s = SelectionNames(c);
             return Contains(s, "Alpha") && Contains(s, "Beta") && Contains(s, "Gamma") ? std::string() : s;
         }),
         Shot("box-selected", {kScene})},
        Tidy(),
    }));

    Add("scenetools: a gizmo drag moves along X in grid steps, undo puts it back, redo again; snap off moves freely", Seq({
        ToolScene(),
        {SelectByName("Beta"), Wait(3),
         Do("note the undo depth", [](Ctx& c) { g_Number = (double)X::UndoDepth(c.E); })},
        GizmoDragX(110.0f, "dragging"),
        {Expect("Beta moved along +X by whole grid steps, nothing else", [](Ctx& c) {
             const glm::vec3 p = LocalPos(c, "Beta");
             const float step = ST::SnapStep(c.E), k = p.x / step;
             if (p.x < step * 0.5f) return "x = " + std::to_string(p.x);
             if (std::fabs(k - std::round(k)) > 1e-3f) return "x = " + std::to_string(p.x) + " is not a multiple of " + std::to_string(step);
             return p.y == 0.0f && p.z == 0.0f ? std::string() : "drifted to y " + std::to_string(p.y) + " z " + std::to_string(p.z);
         }),
         Expect("one Move step", [](Ctx& c) {
             return X::UndoDepth(c.E) == (size_t)g_Number + 1 && ST::TopLabel(c.E) == "Move" ? std::string() : "top " + ST::TopLabel(c.E);
         }),
         Do("note where", [](Ctx& c) { g_Vec = LocalPos(c, "Beta"); }),
         HoverPanel(kScene), Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo: back at the origin", [](Ctx& c) { return LocalPos(c, "Beta") == glm::vec3(0.0f); }),
         Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("redo: moved again", [](Ctx& c) { return LocalPos(c, "Beta") == g_Vec; }),
         Do("snap off", [](Ctx& c) { c.E.SetGridSnapEnabled(false); }), Wait(2)},
        GizmoDragX(83.0f),
        {Expect("moved further, off the grid", [](Ctx& c) {
             const float x = LocalPos(c, "Beta").x, k = x / 0.5f;
             if (x <= g_Vec.x + 0.1f) return "x " + std::to_string(g_Vec.x) + " -> " + std::to_string(x);
             return std::fabs(k - std::round(k)) > 1e-4f ? std::string() : "x = " + std::to_string(x) + " landed on the grid anyway";
         }),
         HoverPanel(kScene), Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo: back to the first drag's spot", [](Ctx& c) { return LocalPos(c, "Beta") == g_Vec; })},
        Tidy(),
    }));

    // One long chain of different kinds of steps (viewport picks, gizmo drags, keyboard edits,
    // Hierarchy and palette actions, a code edit), the scene dumped after each; then all the way
    // back with Ctrl+Z and forward with Ctrl+Y, one press at a time, each depth matching its dump.
    {
        auto checked = std::make_shared<int>(0);
        Steps s = Seq({
            ToolScene(),
            {Do("start recording", [checked](Ctx&) { g_At.clear(); g_TopDepth = 0; *checked = 0; }),
             Record("nothing"),
             ClickAt("Alpha in the viewport", AtObject("Alpha")), Wait(3), Record("picking Alpha")},
            GizmoDragX(90.0f),
            {Record("dragging Alpha"),
             HoverPanel(kScene), Key(ImGuiKey_D, ImGuiMod_Ctrl), Wait(3), Record("Ctrl+D"),
             Key(ImGuiKey_Delete), Wait(3), Record("deleting the copy"),
             Click(ByTag("row:Gamma")), Wait(3), Record("clicking Gamma's row"),
             Do("dim Gamma (a code edit)", [](Ctx& c) {
                 X::PushUndo(c.E, c.W, "Dim Gamma");
                 c.W.Registry.get<LightComponent>(ByName(c.W, "Gamma")).Intensity = 0.25f;
             }), Wait(3), Record("dimming Gamma"),
             Do("Duplicate Array 3 x 1 x 1", [](Ctx& c) { ST::SetArrayDup(c.E, 3, 1, 1, glm::vec3(0.0f, 0.0f, 2.0f)); }),
             Click(Rail(EDITOR_ICON_DUPLICATE_ARRAY)),
             Click(ByExact("Create", "##ArrayDup")), Wait(3), Record("Duplicate Array"),
             Click(Rail(EDITOR_ICON_ROTATE)), Wait(2),
             ClickAt("the cube", AtObject("Cube")), Wait(3), Record("picking the cube")},
            {HoverGizmoAxis(glm::vec3(1, 0, 0))}, // the rotate gizmo's rings are crossed on the way in
            {MouseButton(0, true), Wait(2), SlideAlongAxis(60.0f, true), MouseButton(0, false), Wait(3),
             Expect("the cube turned", [](Ctx& c) {
                 const glm::quat q = c.W.Registry.get<TransformComponent>(ByName(c.W, "Cube")).Rotation;
                 return ST::TopLabel(c.E) == "Rotate" ? std::string() : "top " + ST::TopLabel(c.E) + ", rotation w " + std::to_string(q.w);
             }),
             Record("rotating the cube"),
             Click(Rail(EDITOR_ICON_TRANSLATE)), Wait(2),
             HoverPanel(kScene), Key(ImGuiKey_A, ImGuiMod_Ctrl), Wait(3), Record("Ctrl+A"),
             Key(ImGuiKey_A, ImGuiMod_Ctrl | ImGuiMod_Shift), Wait(3), Record("Ctrl+Shift+A"),
             Key(ImGuiKey_N, ImGuiMod_Ctrl | ImGuiMod_Shift), Wait(3), Record("Ctrl+Shift+N (a new empty)"),
             Shot("top-of-chain", {kScene, kHierarchy}),
             Expect("every step pushed at least one entry", [](Ctx& c) {
                 int recorded = 0;
                 for (const Snapshot& r : g_At) recorded += r.Has ? 1 : 0;
                 return recorded >= 12 ? std::string() : std::to_string(recorded) + " depths recorded; history: " + ST::TopLabel(c.E);
             })},
        });
        s.push_back(Walk(true, 0, checked));
        s.push_back(Expect("back at the start", [](Ctx& c) {
            return X::UndoDepth(c.E) == 0 && ST::RedoDepth(c.E) > 0 ? MatchesRecord(c) : "depth " + std::to_string(X::UndoDepth(c.E));
        }));
        // The top depth is only known once the chain ran: build the redo Walk then.
        auto redo = std::make_shared<Step>();
        s.push_back({"redo step by step to the top", [checked, redo](Ctx& c) {
            if (!redo->Fn) *redo = Walk(false, g_TopDepth, checked);
            const bool done = redo->Fn(c);
            if (done) redo->Fn = nullptr;
            return done;
        }, 6000});
        for (const Step& st : Steps{
                 Expect("at the top again, nothing left to redo", [](Ctx& c) {
                     return X::UndoDepth(c.E) == g_TopDepth && ST::RedoDepth(c.E) == 0 ? MatchesRecord(c) : "depth " + std::to_string(X::UndoDepth(c.E));
                 }),
                 Expect("every recorded depth was checked twice", [checked](Ctx&) {
                     int recorded = 0;
                     for (const Snapshot& r : g_At) recorded += r.Has ? 1 : 0;
                     // depth 0 is reached only by the undo walk and the top only by the redo walk
                     return *checked == 2 * recorded - 2 ? std::string() : std::to_string(*checked) + " checks for " + std::to_string(recorded) + " recorded depths";
                 }),
                 // Undo three, then a new edit: the redo branch is gone and Ctrl+Y does nothing.
                 HoverPanel(kScene),
                 Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3), Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3), Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
                 Expect("three undone", [](Ctx& c) { return ST::RedoDepth(c.E) == 3 ? MatchesRecord(c) : "redo depth " + std::to_string(ST::RedoDepth(c.E)); }),
                 Do("a new edit", [](Ctx& c) {
                     X::PushUndo(c.E, c.W, "Rename Beta");
                     c.W.Registry.get<NameComponent>(ByName(c.W, "Beta")).Name = "Beta Branched";
                 }), Wait(3),
                 ExpectTrue("the redo stack is cleared", [](Ctx& c) { return ST::RedoDepth(c.E) == 0; }),
                 Do("note the scene", [](Ctx& c) { g_Text = SceneSerializer::SaveToString(c.W, c.A); }),
                 Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3),
                 Expect("Ctrl+Y does nothing now", [](Ctx& c) {
                     const std::string now = SceneSerializer::SaveToString(c.W, c.A);
                     return now == g_Text ? std::string() : FirstDifference(g_Text, now);
                 }),
                 Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
                 Expect("one undo takes the branch edit back off", [](Ctx& c) {
                     return ByName(c.W, "Beta") != entt::null ? MatchesRecord(c) : std::string("Beta is still renamed");
                 }),
             })
            s.push_back(st);
        for (const Step& st : Tidy()) s.push_back(st);
        Add("scenetools: a long mixed undo/redo chain matches the scene dump at every step", s);
    }

    // The History panel lists exactly the undo stack, a Current marker, then the redo stack;
    // clicking a row jumps there (undo rows: to the state before that step; redo rows: through it).
    {
        auto rows = [](Ctx&) {
            std::vector<std::string> out;
            ImGuiWindow* w = ImGui::FindWindowByName(EditorPanels::History);
            if (!w) return out;
            for (const UiItem& it : g_Reg.Last)
                if (it.Window == w->Name && !it.Label.empty() && !Contains(it.Label, "###") && it.Label[0] != '#' && it.Rect.GetWidth() > 0.0f)
                    out.push_back(it.Label);
            return out;
        };
        auto rowsMatch = [rows](Ctx& c) {
            const std::vector<std::string> want = ST::HistoryRows(c.E), got = rows(c);
            if (want == got) return std::string();
            std::string s = "panel:";
            for (const auto& r : got) s += " [" + Visible(r) + "]";
            s += " stack:";
            for (const auto& r : want) s += " [" + r + "]";
            return s;
        };
        auto historyRow = [](const char* label) {
            Query q = ByExact(label);
            q.Pred = [](const UiItem& it) { ImGuiWindow* w = ImGui::FindWindowByName(EditorPanels::History); return w && it.Window == w->Name; };
            return q;
        };
        Steps s = Seq({
            ToolScene(),
            {Do("start recording", [](Ctx&) { g_At.clear(); g_TopDepth = 0; }),
             Record("nothing"),
             Do("rename Alpha", [](Ctx& c) { X::PushUndo(c.E, c.W, "Rename Alpha"); c.W.Registry.get<NameComponent>(ByName(c.W, "Alpha")).Name = "Alpha 2"; }),
             Wait(3), Record("the rename"),
             ClickAt("Beta in the viewport", AtObject("Beta")), Wait(3), Record("picking Beta")},
            GizmoDragX(90.0f),
            {Record("the drag"),
             HoverPanel(kScene), Key(ImGuiKey_D, ImGuiMod_Ctrl), Wait(3), Record("Ctrl+D"),
             Do("dim Gamma", [](Ctx& c) { X::PushUndo(c.E, c.W, "Dim Gamma"); c.W.Registry.get<LightComponent>(ByName(c.W, "Gamma")).Intensity = 0.5f; }),
             Wait(3), Record("dimming Gamma"),
             HoverPanel(kScene), Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3), Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
             ExpectTrue("two steps to redo", [](Ctx& c) { return ST::RedoDepth(c.E) == 2; }),
             HoverPanel(kScene), Key(ImGuiKey_H, ImGuiMod_Ctrl | ImGuiMod_Shift),
             ChromeUi::UntilShown(EditorPanels::History),
             ChromeUi::BringToFront(EditorPanels::History), Wait(3),
             Expect("the rows are the undo stack, Current, the redo stack", rowsMatch),
             Expect("the selection step is listed as one", [](Ctx& c) {
                 const auto r = ST::HistoryRows(c.E);
                 return std::count_if(r.begin(), r.end(), [](const std::string& x) { return x.rfind(ICON_FA_ARROW_POINTER, 0) == 0; }) >= 1
                     ? std::string() : std::string("no selection row");
             }),
             Shot("history", {"###History"}),
             Click(historyRow("Rename Alpha")), Wait(3),
             Expect("clicking the first step goes back to before it", [](Ctx& c) {
                 return X::UndoDepth(c.E) == 0 ? MatchesRecord(c) : "depth " + std::to_string(X::UndoDepth(c.E));
             }),
             ExpectTrue("Alpha has its old name", [](Ctx& c) { return ByName(c.W, "Alpha") != entt::null; }),
             Expect("rows follow: everything is redo now", rowsMatch),
             Shot("history-at-start", {"###History"}),
             Click(historyRow("Dim Gamma")), Wait(3),
             Expect("clicking the last redo row replays everything", [](Ctx& c) {
                 return ST::RedoDepth(c.E) == 0 && X::UndoDepth(c.E) == g_TopDepth ? MatchesRecord(c) : "depth " + std::to_string(X::UndoDepth(c.E));
             }),
             Expect("rows follow: nothing to redo", rowsMatch),
             HoverPanel(kScene), Key(ImGuiKey_H, ImGuiMod_Ctrl | ImGuiMod_Shift), Wait(3),
             ExpectTrue("Ctrl+Shift+H hides it again", [](Ctx& c) { return !ST::ShowHistory(c.E); })},
            Tidy(),
        });
        Add("scenetools: the History panel lists the undo stack and jumps to a clicked step", s);
    }
}
