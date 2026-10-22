// Editor smoke suite: Decals. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The decal editor tools (EditorLayer_DecalLibrary.cpp, EditorLayer_DecalTools.cpp) in the Apartment
// Hallway: the Library panel opens and indexes the packs, the Place tool stamps a decal where the
// mouse points, the Brush paints a stroke and Ctrl erases it, auto-grime fills a floor of the
// building with each shipped palette, the Space radial offers the brush's sources, and a save +
// reload keeps the decals.

} // namespace
#include "../Editor/DecalEditorState.h"
struct EditorSmokeAccess_Decals {
    static DecalEditorState& Ed(EditorLayer& e) { return e.DecalEd(); }
    static void Library(EditorLayer& e, bool on) { e.m_ShowDecalLibrary = on; }
    static int Tool(EditorLayer& e) { return (int)e.m_DecalTool; }
    static void SetTool(EditorLayer& e, int m) { e.SetDecalTool((EditorLayer::DecalToolMode)m); }
    static void Grime(EditorLayer& e, World& w) { e.RunAutoGrime(w); }
    static void Select(EditorLayer& e, entt::entity en) { e.SelectItem(en, false); }
    static void Gizmos(EditorLayer& e, bool on) { e.m_GizmosMasterVisible = on; e.m_ShowEntityIcons = on; e.m_ShowGrid = on; e.m_CleanShots = !on; }
    static void Deselect(EditorLayer& e) { e.ClearSelection(); }
    static int RadialBuild(EditorLayer& e) { return e.DecalRadialBuild(); }
    static void RadialRun(EditorLayer& e, int i) { e.RunDecalRadialItem(i); }
    static bool RadialOpen(EditorLayer& e) { return e.m_Radial.Open; }
    static bool RadialDecals(EditorLayer& e) { return e.m_Radial.Decals; }
};
namespace {
namespace smoke_decals {
using SA = EditorSmokeAccess_Decals;
namespace fs = std::filesystem;
int CountDecals(World& w) { return (int)w.Registry.view<DecalComponent>().size(); }
int g_Before = 0, g_Saved = 0;
std::string g_File;
entt::entity EntByName(World& w, const char* name) {
    for (auto [e, n] : w.Registry.view<NameComponent>().each())
        if (n.Name == name) return e;
    return entt::null;
}
Step Aim() {
    return Do("stand in the corridor facing the north wall", [](Ctx& c) {
        c.Cam.Position = glm::vec3(8.0f, 1.5f, 0.0f);
        c.Cam.Yaw = -90.0f;
        c.Cam.Pitch = 0.0f;
    });
}
Step ChooseDecal(const char* stem) {
    return Do(std::string("choose ") + stem, [stem](Ctx& c) {
        DecalEditorState& ed = SA::Ed(c.E);
        for (const auto& en : ed.Entries)
            if (en.Name == stem) { ed.PlacePath = en.Path; return; }
        Fail(std::string("decal not indexed: ") + stem);
    });
}
Step SelectRoom() {
    return Do("select the Apartment 303 living room floor", [](Ctx& c) {
        const entt::entity f = EntByName(c.W, "Floor - Apartment 303 - Living Room");
        if (f == entt::null) Fail("no room floor");
        else SA::Select(c.E, f);
        c.Cam.Position = glm::vec3(5.3f, 1.5f, -4.9f); // in the room, looking along it
        c.Cam.Yaw = -50.0f;
        c.Cam.Pitch = -5.0f;
    });
}
Step Pick(const char* palette) {
    return Do(std::string("palette ") + palette, [palette](Ctx& c) {
        DecalEditorState& ed = SA::Ed(c.E);
        for (size_t i = 0; i < ed.Palettes.size(); ++i)
            if (ed.Palettes[i]->Name == palette) {
                DecalEdit::SetPalette(ed, (int)i);
                if (ed.Palettes[i]->Missing || ed.Palettes[i]->Entries.empty()) Fail(std::string(palette) + " is empty");
                return;
            }
        Fail(std::string("palette not found: ") + palette);
    });
}
Step MoveOver(float dx, float dy) {
    return {"mouse over the viewport " + std::to_string((int)dx), [dx, dy](Ctx&) {
        ImVec2 p;
        if (!PanelCentre("###Scene", p)) return false;
        MouseTo(ImVec2(p.x + dx, p.y + dy));
        return true;
    }};
}
} // namespace smoke_decals

void RegisterDecalsTests() {
    using namespace smoke_decals;
    Add("decals: library, place, brush, auto-grime, palettes, radial, save and reload", {
        Do("allow errors naming the external files missing here", [](Ctx&) {
            for (const std::string& name : smoke_scenes::MissingExternalFiles()) g_AllowLog.push_back(name);
            if (!smoke_scenes::MissingExternalFiles().empty())
                g_AllowLog.push_back("Enemy AI: the soldier's body has no animated skeleton");
        }),
        FreshScene(), Wait(2),
        Do("open the Apartment Hallway", [](Ctx& c) {
            EditorSmokeAccess_Scenes::Open(c.E, c.W, c.A, ProjectPaths::Resolve("scenes/Apartment Hallway.json"));
        }),
        Wait(120), Aim(), Do("gizmos, icons and grid off for the shots", [](Ctx& c) { SA::Gizmos(c.E, false); }), Wait(5),
        // --- Library
        Do("open the Decal Library", [](Ctx& c) { SA::Library(c.E, true); }), Wait(4),
        Until("the packs are indexed", [](Ctx& c) { return SA::Ed(c.E).ScanDone; }, 2400),
        ExpectTrue("the library lists decals", [](Ctx& c) { return SA::Ed(c.E).Entries.size() > 1000; }),
        ExpectTrue("three palettes found", [](Ctx& c) { return SA::Ed(c.E).Palettes.size() >= 3; }),
        Wait(40), Shot("library"),
        Do("close the Decal Library", [](Ctx& c) { SA::Library(c.E, false); }), Wait(4),
        // --- Place
        ChooseDecal("Stain_591"),
        Do("Place tool", [](Ctx& c) { SA::SetTool(c.E, 1); g_Before = CountDecals(c.W); }),
        ExpectTrue("Place tool on", [](Ctx& c) { return SA::Tool(c.E) == 1; }),
        MoveOver(0, 0), Wait(6), Shot("place_ghost"),
        MouseButton(0, true), Wait(2), MouseButton(0, false), Wait(8), Shot("place_stamped"),
        ExpectTrue("a decal was placed", [](Ctx& c) { return CountDecals(c.W) == g_Before + 1; }),
        // --- Brush
        Pick("Squat"),
        Do("Brush tool", [](Ctx& c) { SA::SetTool(c.E, 2); SA::Ed(c.E).Brush.Spacing = 0.1f; g_Before = CountDecals(c.W); }),
        MoveOver(-150, 0), Wait(4),
        MouseButton(0, true), Wait(2),
        MoveOver(-100, 10), Wait(3), MoveOver(-50, -10), Wait(3), MoveOver(0, 10), Wait(3),
        MoveOver(50, -10), Wait(3), MoveOver(100, 0), Wait(3), MoveOver(150, 0), Wait(3),
        MouseButton(0, false), Wait(8), Shot("brush_stroke"),
        Do("note count", [](Ctx& c) { g_Saved = CountDecals(c.W); }),
        ExpectTrue("the stroke painted decals", [](Ctx&) { return g_Saved > g_Before + 3; }),
        // Ctrl erases along the same line.
        KeyDown(ImGuiKey_LeftCtrl, ImGuiMod_Ctrl),
        MoveOver(-150, 0), Wait(3), MouseButton(0, true), Wait(2),
        MoveOver(-100, 0), Wait(3), MoveOver(-50, 0), Wait(3), MoveOver(0, 0), Wait(3),
        MoveOver(50, 0), Wait(3), MoveOver(100, 0), Wait(3), MoveOver(150, 0), Wait(3),
        MouseButton(0, false), Wait(3), KeyUp(ImGuiKey_LeftCtrl, ImGuiMod_Ctrl), Wait(6), Shot("brush_erased"),
        ExpectTrue("the Ctrl stroke erased decals", [](Ctx& c) { return CountDecals(c.W) < g_Saved; }),
        // --- Radial
        MoveOver(0, 0), Wait(3),
        KeyDown(ImGuiKey_Space), WaitSeconds(0.6), Wait(10), MoveOver(30, -85), Wait(12), Shot("radial"),
        ExpectTrue("the radial shows the brush's sources", [](Ctx& c) { return SA::RadialOpen(c.E) && SA::RadialDecals(c.E); }),
        KeyUp(ImGuiKey_Space), Wait(30),
        ExpectTrue("the decal radial has slots", [](Ctx& c) { return SA::RadialBuild(c.E) >= 2; }),
        Do("pick radial slot 0", [](Ctx& c) { SA::RadialRun(c.E, 0); }),
        Do("no tool", [](Ctx& c) { SA::SetTool(c.E, 0); g_Saved = CountDecals(c.W); }),
        // --- Palettes, each driving auto-grime
        Pick("Squat"), SelectRoom(), Do("auto-grime Squat", [](Ctx& c) { SA::Grime(c.E, c.W); }), Wait(10),
        ExpectTrue("auto-grime placed decals", [](Ctx& c) { return !SA::Ed(c.E).GrimeStatus.empty() && CountDecals(c.W) > g_Saved + 5; }),
        Wait(10), Do("deselect", [](Ctx& c) { SA::Deselect(c.E); }), Wait(6), Shot("grime_squat"),
        Pick("Water Damage"), SelectRoom(), Do("auto-grime Water Damage", [](Ctx& c) { SA::Grime(c.E, c.W); }), Wait(10), Do("deselect", [](Ctx& c) { SA::Deselect(c.E); }), Wait(6), Shot("grime_water"),
        Pick("Gang Turf"), SelectRoom(), Do("auto-grime Gang Turf", [](Ctx& c) { SA::Grime(c.E, c.W); }), Wait(10), Do("deselect", [](Ctx& c) { SA::Deselect(c.E); }), Wait(6), Shot("grime_turf"),
        Do("log the status", [](Ctx& c) { Log::Info("decals smoke: " + SA::Ed(c.E).GrimeStatus); }),
        ExpectTrue("the status counts decals", [](Ctx& c) { return SA::Ed(c.E).GrimeStatus.find(" decals from ") != std::string::npos; }),
        // --- Save + reload
        Do("save the scene to a scratch file", [](Ctx& c) {
            g_File = (fs::temp_directory_path() / "tartarus_decals_smoke.json").string();
            g_Saved = CountDecals(c.W);
            if (!SceneSerializer::Save(c.W, c.A, g_File)) Fail("save failed");
        }),
        Do("load it back", [](Ctx& c) {
            if (!SceneSerializer::Load(c.W, c.A, g_File, false)) Fail("load failed");
        }), Wait(10),
        Expect("the decals persisted", [](Ctx& c) {
            const int n = CountDecals(c.W);
            return n == g_Saved && n > 0 ? std::string() : "decal count " + std::to_string(n) + " vs saved " + std::to_string(g_Saved);
        }),
        Shot("reloaded"),
        Do("gizmos back", [](Ctx& c) { SA::Gizmos(c.E, true); }),
    }, [](Ctx& c) {
        SA::SetTool(c.E, 0);
        SA::Library(c.E, false);
        std::error_code ec;
        if (!g_File.empty()) fs::remove(g_File, ec);
    });
}

void RegisterDecalsLitTests() {
    using namespace smoke_decals;
    Add("decals: a lit render of an auto-grimed room", {
        Do("allow errors naming the external files missing here", [](Ctx&) {
            for (const std::string& name : smoke_scenes::MissingExternalFiles()) g_AllowLog.push_back(name);
            if (!smoke_scenes::MissingExternalFiles().empty())
                g_AllowLog.push_back("Enemy AI: the soldier's body has no animated skeleton");
        }),
        FreshScene(), Wait(2),
        Do("open the Apartment Hallway", [](Ctx& c) {
            EditorSmokeAccess_Scenes::Open(c.E, c.W, c.A, ProjectPaths::Resolve("scenes/Apartment Hallway.json"));
        }),
        Wait(120),
        Do("open the library to index", [](Ctx& c) { SA::Library(c.E, true); }),
        Until("indexed", [](Ctx& c) { return SA::Ed(c.E).ScanDone; }, 2400),
        Do("close it", [](Ctx& c) { SA::Library(c.E, false); }),
        Pick("Squat"),
        Do("work light, clean view", [](Ctx& c) {
            // The room is night-dark (the Apartment's own lamps are off in the editor): one soft work light so the grime can be judged.
            const entt::entity lamp = c.W.CreateEmptyEntity(glm::vec3(7.6f, 2.1f, -7.1f), glm::vec3(0.0f), glm::vec3(1.0f), "Review light");
            LightComponent& lc = c.W.Registry.emplace<LightComponent>(lamp);
            lc.Intensity = 6.0f;
            lc.Range = 9.0f;
            lc.Color = glm::vec3(1.0f, 0.93f, 0.8f);
            SA::Gizmos(c.E, false);
            c.Cam.Position = glm::vec3(5.3f, 1.5f, -4.9f);
            c.Cam.Yaw = -50.0f;
            c.Cam.Pitch = -3.0f;
        }), Wait(60), Shot("grime-before"),
        SelectRoom(),
        Do("auto-grime", [](Ctx& c) { SA::Grime(c.E, c.W); }), Wait(10),
        Do("deselect, same camera", [](Ctx& c) {
            SA::Deselect(c.E);
            c.Cam.Position = glm::vec3(5.3f, 1.5f, -4.9f);
            c.Cam.Yaw = -50.0f;
            c.Cam.Pitch = -3.0f;
        }), Wait(120), Shot("grime-lit"),
        Do("gizmos back", [](Ctx& c) { SA::Gizmos(c.E, true); }),
    });
}
