// Editor smoke suite: Curves. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The curve editor windows (CurveEditor.cpp / CurveEditorWindows.cpp), opened two ways:
//  - from a field: a Particle System's "Size over life" (Inspector > Lifetime & Emission Curves >
//    Edit Curve), a scene-owned curve written back through the emitter;
//  - from an asset: a scratch .recoil profile selected in Assets, whose Inspector is the Recoil
//    editor (Recoil Targets / Smoothing / Layers / Misc tabs; the curves live under Misc), each
//    curve opening a window that saves straight to the file.
// In a window: Add Key (the Time / Value popup), moving a key by typing its Time and Value, the
// tangent buttons (Flat / Linear / Constant / Auto), a preset, Delete Keys, and the window's
// Undo / Redo (the editor's global history). Every tab and window is shot.
// (A curve window's title holds a "/", which Shot's blank-window check reads as a child window, so
// the windows are shot without it.)

} // namespace
#include "Curve.h"
#include "FirstPersonProcedural.h"
#include "AssetDatabase.h"
namespace {
namespace smoke_curves {
namespace fs = std::filesystem;

const char* kCurveWin = "###curve:"; // every curve window's name contains this

std::string ScratchDir() { return ProjectPaths::Resolve("editor-smoke-scratch-curves"); }
std::string RecoilPath() { return (fs::path(ScratchDir()) / "Smoke Recoil.recoil").generic_string(); }

// The open curve window whose title contains `title` (nullptr when none is shown).
ImGuiWindow* CurveWindow(const std::string& title = {}) {
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        if (w->Active && !w->Hidden && Contains(w->Name, kCurveWin) && Contains(w->Name, title) && !w->ParentWindow) return w;
    return nullptr;
}
int CurveWindowCount() {
    int n = 0;
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        if (w->Active && Contains(w->Name, kCurveWin) && !w->ParentWindow) ++n;
    return n;
}
// An item of the open curve window(s), by its exact raw label.
Query InWin(std::string raw) { return ByExact(std::move(raw), kCurveWin); }
// The bottom-most of several items sharing a label in the curve window (the key's numeric Time /
// Value drags sit below the key list's column headers of the same name).
Query InWinLast(std::string raw) {
    Query q;
    q.Root = kCurveWin;
    q.Pred = [raw](const UiItem& it) {
        if (it.Label != raw) return false;
        for (const UiItem& o : g_Reg.Last)
            if (o.Label == raw && Contains(o.Root, kCurveWin) && o.Rect.Min.y > it.Rect.Min.y + 1.0f && o.Rect.GetHeight() > 0.0f)
                return false;
        return true;
    };
    return q;
}
// A field of the Add Key popup.
Query AddKeyField(std::string raw) {
    Query q = ByExact(std::move(raw));
    q.Pred = [](const UiItem& it) { return Contains(it.Root, "##Popup"); };
    return q;
}
// Click a text / number field, replace what it holds with `text`, confirm.
std::vector<Step> TypeOver(Query field, std::string text, bool ctrlClick = false) {
    return {Click(field, ctrlClick ? ImGuiMod_Ctrl : 0), Wait(3), Key(ImGuiKey_A, ImGuiMod_Ctrl), Wait(1),
            TypeText(std::move(text)), Wait(2), Key(ImGuiKey_Enter), Wait(3)};
}
// Add Key: the popup's Time and Value, then Insert.
std::vector<Step> AddKey(std::string time, std::string value) {
    std::vector<Step> s{Click(InWin("Add Key")), Wait(3),
                        Until("the Add Key popup", [](Ctx&) { return Find(AddKeyField("Insert")) != nullptr; })};
    for (Step& st : TypeOver(AddKeyField("Time"), time)) s.push_back(st);
    for (Step& st : TypeOver(AddKeyField("Value"), value)) s.push_back(st);
    s.push_back(Click(AddKeyField("Insert")));
    s.push_back(Wait(4));
    return s;
}
// Closes every curve window with its own close button.
Step CloseCurveWindows() {
    auto phase = std::make_shared<int>(0);
    return {"close the curve windows", [phase](Ctx&) {
        ImGuiIO& io = ImGui::GetIO();
        if (*phase >= 1) {
            io.AddMouseButtonEvent(0, *phase == 1);
            if (++*phase > 2) *phase = 0;
            return false;
        }
        ImGuiWindow* w = CurveWindow();
        if (!w) return true;
        const ImGuiID closeId = ImHashStr("#CLOSE", 0, w->ID);
        for (const UiItem& it : g_Reg.Last) {
            ImVec2 p;
            if (it.Id == closeId && ClickPoint(it, p)) { MouseTo(p); *phase = 1; return false; }
        }
        // Docked into a tab bar: its tab's close button shows once the tab is hovered.
        for (const UiItem& it : g_Reg.Last) {
            ImVec2 p;
            if (it.Id == w->TabId && ClickPoint(it, p)) { MouseTo(p); return false; }
        }
        return false;
    }, 600};
}

// The particle emitter's Size over life, as the window writes it.
Curve SizeCurve(Ctx& c) {
    Curve curve;
    const entt::entity e = ByName(c.W, "Alpha");
    if (const auto* ps = e == entt::null ? nullptr : c.W.Registry.try_get<ParticleSystemComponent>(e))
        Curve::FromJson(nlohmann::json::parse(ps->SizeCurve, nullptr, false), curve);
    return curve;
}
// The recoil profile's Semi rotation X curve, read back from the file.
Curve RecoilCurve() {
    RecoilAsset a;
    if (!RecoilAsset::LoadFile(RecoilPath(), a)) return {};
    return a.Data.Unity.SemiRotCurve.X;
}
int KeyAt(const Curve& c, float time) {
    for (int i = 0; i < (int)c.Keys.size(); ++i)
        if (std::fabs(c.Keys[i].Time - time) < 1e-3f) return i;
    return -1;
}
std::string Describe(const Curve& c) { return c.ToJson(false).dump(); }

// The edit sequence both windows go through, reading the curve with `read`.
std::vector<Step> EditInWindow(std::function<Curve(Ctx&)> read, std::string shotPrefix) {
    auto start = std::make_shared<size_t>(0);
    std::vector<Step> s{
        Do("note the key count", [read, start](Ctx& c) { *start = read(c).Keys.size(); }),
    };
    for (Step& st : AddKey("0.5", "0.25")) s.push_back(st);
    s.push_back(Expect("Add Key inserted (0.5, 0.25)", [read, start](Ctx& c) {
        const Curve k = read(c);
        const int i = KeyAt(k, 0.5f);
        if (k.Keys.size() != *start + 1 || i < 0) return "keys now " + Describe(k);
        return InspNear(k.Keys[i].Value, 0.25f) ? std::string() : "value " + std::to_string(k.Keys[i].Value);
    }));
    s.push_back(Shot(shotPrefix + "-added"));
    // Move the new (selected) key: type its Time and Value.
    for (Step& st : TypeOver(InWinLast("Value"), "0.75", true)) s.push_back(st);
    for (Step& st : TypeOver(InWinLast("Time"), "0.6", true)) s.push_back(st);
    s.push_back(Expect("the key moved to (0.6, 0.75)", [read](Ctx& c) {
        const Curve k = read(c);
        const int i = KeyAt(k, 0.6f);
        if (i < 0 || KeyAt(k, 0.5f) >= 0) return "keys now " + Describe(k);
        return InspNear(k.Keys[i].Value, 0.75f) ? std::string() : "value " + std::to_string(k.Keys[i].Value);
    }));
    // Tangent / interpolation buttons on the selected key.
    s.push_back(Click(InWin("Linear")));
    s.push_back(Wait(4));
    s.push_back(Expect("Linear", [read](Ctx& c) {
        const Curve k = read(c);
        const int i = KeyAt(k, 0.6f);
        return i >= 0 && k.Keys[i].Interpolation == CurveInterpolation::Linear ? std::string() : "keys now " + Describe(k);
    }));
    s.push_back(Click(InWin("Constant")));
    s.push_back(Wait(4));
    s.push_back(Expect("Constant", [read](Ctx& c) {
        const Curve k = read(c);
        const int i = KeyAt(k, 0.6f);
        return i >= 0 && k.Keys[i].Interpolation == CurveInterpolation::Constant ? std::string() : "keys now " + Describe(k);
    }));
    s.push_back(Click(InWin("Flat")));
    s.push_back(Wait(4));
    s.push_back(Expect("Flat: cubic, zero slopes", [read](Ctx& c) {
        const Curve k = read(c);
        const int i = KeyAt(k, 0.6f);
        if (i < 0) return "keys now " + Describe(k);
        const CurveKey& key = k.Keys[i];
        return key.Interpolation == CurveInterpolation::Cubic && key.InTangent == 0.0f && key.OutTangent == 0.0f
                   ? std::string() : "key " + Describe(k);
    }));
    s.push_back(Click(InWin("Auto")));
    s.push_back(Wait(4));
    s.push_back(Shot(shotPrefix + "-tangents"));
    // Delete Keys removes the selected key.
    s.push_back(Click(InWin("Delete Keys")));
    s.push_back(Wait(4));
    s.push_back(Expect("Delete Keys removed it", [read, start](Ctx& c) {
        const Curve k = read(c);
        return k.Keys.size() == *start && KeyAt(k, 0.6f) < 0 ? std::string() : "keys now " + Describe(k);
    }));
    // A preset replaces the curve.
    s.push_back(Click(InWin("Presets")));
    s.push_back(Wait(3));
    s.push_back(Click(MenuItem("Sine")));
    s.push_back(Wait(4));
    s.push_back(Expect("the Sine preset (9 keys)", [read](Ctx& c) {
        const Curve k = read(c);
        return k.Keys.size() == 9 ? std::string() : "keys now " + Describe(k);
    }));
    s.push_back(Shot(shotPrefix + "-sine"));
    // The window's Undo / Redo: the editor's history.
    s.push_back(Click(InWin("Undo")));
    s.push_back(Wait(6));
    s.push_back(Until("Undo restores the curve before the preset", [read, start](Ctx& c) {
        return read(c).Keys.size() == *start;
    }, 60));
    s.push_back(Click(InWin("Redo")));
    s.push_back(Wait(6));

    s.push_back(Until("Redo puts the preset back", [read](Ctx& c) { return read(c).Keys.size() == 9; }, 60));
    return s;
}

} // namespace smoke_curves

void RegisterCurvesTests() {
    using namespace smoke_curves;

    Add("curves: a Particle System field's curve window: add, move, tangents, delete, preset, undo", InspCat({
        {FreshScene(), CloseCurveWindows(), Wait(2),
         Do("add a Particle System to Alpha", [](Ctx& c) {
             X::PushUndo(c.E, c.W, "Add Particle System");
             c.W.Registry.emplace<ParticleSystemComponent>(ByName(c.W, "Alpha"));
         }), Wait(2),
         SelectByName("Alpha"), Wait(3), InspOpen("Particle System"), InspUntilOpen("Particle System"), Wait(3),
         InspReveal(ByLabel("Lifetime & Emission Curves", kInspector)),
         ClickUnlessShown(ByLabel("Lifetime & Emission Curves", kInspector), ByExact("Edit Curve", kInspector)),
         Wait(3),
         InspReveal(ByExact("Edit Curve", kInspector)),
         Shot("inspector-curves", {kInspector}),
         Click(ByExact("Edit Curve", kInspector)), Wait(4),
         Until("the Size over life window", [](Ctx&) { return CurveWindow("Size over life") != nullptr; }),
         Wait(3),
         Shot("window")},
        EditInWindow([](Ctx& c) { return SizeCurve(c); }, "particle"),
        {CloseCurveWindows(), Wait(2),
         ExpectTrue("the window closed", [](Ctx&) { return CurveWindowCount() == 0; })},
    }));

    Add("curves: a recoil profile's tabs (targets, smoothing, layers, misc, curves) and curve window; undo", InspCat({
        {FreshScene(), CloseCurveWindows(), Wait(2),
         Do("write a scratch recoil profile", [](Ctx&) {
             std::error_code ec;
             fs::remove_all(fs::u8path(ScratchDir()), ec);
             fs::create_directories(fs::u8path(ScratchDir()), ec);
             if (!RecoilAsset{}.SaveFile(RecoilPath())) Fail("couldn't write " + RecoilPath());
             AssetDatabase::EnsureGuid(RecoilPath());
         }),
         Do("select it in Assets", [](Ctx& c) { X::SelectAsset(c.E, RecoilPath()); }), Wait(4),
         Until("the Recoil editor", [](Ctx&) { return Find(ByExact("Recoil Targets", kInspector)) != nullptr; }),
         Click(ByExact("Recoil Targets", kInspector)), Wait(3),
         Shot("targets", {kInspector}),
         Click(ByExact("Smoothing", kInspector)), Wait(3),
         Shot("smoothing", {kInspector}),
         Click(ByExact("Layers", kInspector)), Wait(3),
         Shot("layers", {kInspector}),
         Click(ByExact("Misc", kInspector)), Wait(3),
         Shot("misc", {kInspector}),
         InspReveal(ByLabel("Semi Rot Curve", kInspector)),
         ClickUnlessShown(ByLabel("Semi Rot Curve", kInspector), ByExact("Edit Curve", kInspector)), Wait(3),
         InspReveal(ByExact("Edit Curve", kInspector)), Wait(2),
         Shot("curves", {kInspector}),
         Click(ByExact("Edit Curve", kInspector)), Wait(4),
         Until("a curve window on the profile", [](Ctx&) { return CurveWindow("Smoke Recoil") != nullptr; }),
         Wait(3),
         Shot("window"),
         Expect("its Curves list names the profile's curves", [](Ctx&) {
             return Find(InWin("Semi rotation X")) || Find(ByLabel("Semi rotation", kCurveWin)) ? std::string()
                                                                                               : std::string("no 'Semi rotation' channel listed");
         })},
        EditInWindow([](Ctx&) { return RecoilCurve(); }, "recoil"),
        {CloseCurveWindows(), Wait(2),
         ExpectTrue("the window closed", [](Ctx&) { return CurveWindowCount() == 0; }),
         Do("deselect and delete the scratch profile", [](Ctx& c) {
             X::Deselect(c.E);
             std::error_code ec;
             fs::remove_all(fs::u8path(ScratchDir()), ec);
         }), Wait(2)},
    }));
}
