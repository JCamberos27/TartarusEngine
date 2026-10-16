// Editor smoke suite: SaveLoad. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The scene file life cycle: an edit dirties the scene and Ctrl+S cleans it; what was saved
// reopens identical; New Scene over unsaved work asks first (Cancel keeps everything, Don't Save
// drops it, Save writes it then switches). File > Save / Revert / New themselves are in the Menus
// suite. Last, every scene under project/scenes/ goes through a load -> save -> load round trip
// into a scratch copy and must come back byte-identical. Nothing under project/ is written, except
// the Untitled*.json New Scene creates, which each test deletes again.

} // namespace
struct EditorSmokeAccess_SaveLoad {
    static bool Dirty(EditorLayer& e) { return e.m_Dirty; }
    static std::string Title(EditorLayer& e) { return e.SceneTitle(); }
    static bool SwitchPrompt(EditorLayer& e) { return e.m_ScenePromptPending; }
    static void Open(EditorLayer& e, World& w, AssetLibrary& a, const std::string& path) { e.RequestOpenScene(w, a, path); }
};
namespace {
namespace smoke_saveload {
namespace fs = std::filesystem;
using SL = EditorSmokeAccess_SaveLoad;
const char* kPrompt = "Save changes?##SceneSwitch";

std::string g_Dump;  // a scene dump to compare against
std::string g_Path;  // a scene path to come back to / clean up

std::string ReadFile(const std::string& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
// Where the first difference between two dumps is, with some context either side.
std::string FirstDifference(const std::string& a, const std::string& b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    const size_t from = i > 80 ? i - 80 : 0;
    return "differs at byte " + std::to_string(i) + " of " + std::to_string(a.size()) + "/" + std::to_string(b.size()) +
           ": '" + a.substr(from, 160) + "' vs '" + b.substr(from, 160) + "'";
}
Step Rename(const char* from, const char* to) {
    return Do(std::string("rename ") + from + " to " + to, [from, to](Ctx& c) {
        X::PushUndo(c.E, c.W, "Rename");
        c.W.Registry.get<NameComponent>(ByName(c.W, from)).Name = to;
    });
}
// New Scene writes project/scenes/Untitled*.json: delete it (and any .meta the asset scan made).
Step DeleteUntitled() {
    return Do("delete the Untitled scene New Scene wrote", [](Ctx&) {
        std::error_code ec;
        if (!Contains(fs::path(g_Path).filename().string(), "Untitled")) return;
        fs::remove(g_Path, ec);
        fs::remove(g_Path + ".meta", ec);
    });
}
std::vector<fs::path> ProjectScenes() {
    std::vector<fs::path> scenes;
    for (const char* dir : {"scenes", "scenes/OutfitTest"}) {
        std::vector<fs::path> here;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(fs::path(ProjectPaths::Resolve(dir)), ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            const std::string name = entry.path().filename().string();
            if (Contains(name, ".recovery.json") || Contains(name, ".backup.json") || Contains(name, "Untitled")) continue;
            here.push_back(entry.path());
        }
        std::sort(here.begin(), here.end());
        scenes.insert(scenes.end(), here.begin(), here.end());
    }
    return scenes;
}
} // namespace smoke_saveload

void RegisterSaveLoadTests() {
    using namespace smoke_saveload;

    Add("saveload: an edit dirties the scene, Ctrl+S saves it, and it reopens identical", {
        FreshScene(),
        Do("no scratch file yet", [](Ctx&) { std::error_code ec; fs::remove(g_ScratchScene, ec); }),
        Wait(3),
        ExpectTrue("titled after the scratch file", [](Ctx& c) { return SL::Title(c.E) == "editor-tests-scene"; }),
        Rename("Gamma", "Gamma Saved"), Wait(3),
        Do("move Alpha and dim Beta", [](Ctx& c) {
            X::PushUndo(c.E, c.W, "Edit");
            c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Position = glm::vec3(1.25f, -2.5f, 7.0f);
            c.W.Registry.get<LightComponent>(ByName(c.W, "Beta")).Intensity = 0.125f;
        }), Wait(3),
        ExpectTrue("dirty after the edits", [](Ctx& c) { return SL::Dirty(c.E); }),
        HoverPanel(kScene), Wait(2),
        Key(ImGuiKey_S, ImGuiMod_Ctrl),
        Until("saved (clean, file written)", [](Ctx& c) { return !SL::Dirty(c.E) && fs::exists(g_ScratchScene); }),
        Expect("the file holds the edits", [](Ctx&) {
            const std::string s = ReadFile(g_ScratchScene);
            return Contains(s, "Gamma Saved") && Contains(s, "Alpha") ? std::string() : std::string("Gamma Saved / Alpha not in the file");
        }),
        Do("dump the scene", [](Ctx& c) { g_Dump = SceneSerializer::SaveToString(c.W, c.A); }),
        Do("wipe the world, then reopen the file", [](Ctx& c) {
            for (const char* n : {"Alpha", "Beta", "Gamma Saved"}) c.W.Registry.destroy(ByName(c.W, n));
            X::Deselect(c.E);
            X::ScenePath(c.E).clear(); // so the open isn't a no-op revert of the same path
            g_Path = g_ScratchScene;
        }),
        Do("open the saved file", [](Ctx& c) { SL::Open(c.E, c.W, c.A, g_Path); }),
        Until("reopened", [](Ctx& c) { return X::ScenePath(c.E) == g_ScratchScene && ByName(c.W, "Alpha") != entt::null; }),
        Expect("identical to what was saved", [](Ctx& c) {
            const std::string now = SceneSerializer::SaveToString(c.W, c.A);
            return now == g_Dump ? std::string() : FirstDifference(g_Dump, now);
        }),
        ExpectTrue("clean after the open", [](Ctx& c) { return !SL::Dirty(c.E); }),
        Expect("no undo history after the open", [](Ctx& c) {
            return X::UndoDepth(c.E) == 0 ? std::string() : "history: " + EditorSmokeAccess_PlayMode::UndoLabels(c.E);
        }),
    });

    Add("saveload: undo back to the saved state clears the dirty flag; redo sets it again", {
        FreshScene(),
        Do("save first", [](Ctx& c) { c.E.SaveScene(c.W, c.A); }), Wait(3),
        ExpectTrue("clean after the save", [](Ctx& c) { return !SL::Dirty(c.E); }),
        Rename("Alpha", "Alpha 2"), Wait(3),
        ExpectTrue("dirty after an edit", [](Ctx& c) { return SL::Dirty(c.E); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        Expect("clean again after undoing it", [](Ctx& c) {
            if (ByName(c.W, "Alpha") == entt::null) return std::string("undo did not run");
            return SL::Dirty(c.E) ? std::string("still dirty at the saved state") : std::string();
        }),
        Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("dirty after redo", [](Ctx& c) { return ByName(c.W, "Alpha 2") != entt::null && SL::Dirty(c.E); }),
        SelectByName("Beta"), Wait(3),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3), // undoes the selection
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3), // undoes the rename
        ExpectTrue("a selection step never dirties", [](Ctx& c) { return ByName(c.W, "Alpha") != entt::null && !SL::Dirty(c.E); }),
    });

    Add("saveload: Ctrl+N over unsaved changes asks; Cancel keeps the scene, Don't Save switches", {
        FreshScene(),
        Do("save first", [](Ctx& c) { c.E.SaveScene(c.W, c.A); }), Wait(3),
        Rename("Gamma", "Gamma Unsaved"), Wait(3),
        HoverPanel(kScene), Wait(2),
        Key(ImGuiKey_N, ImGuiMod_Ctrl),
        Until("the unsaved-changes prompt", [](Ctx& c) { return SL::SwitchPrompt(c.E); }),
        Until("its Cancel button", [](Ctx&) { return Find(ByExact("Cancel", kPrompt)) != nullptr; }),
        Shot("prompt", {kPrompt}),
        Click(ByExact("Cancel", kPrompt)),
        Until("prompt closed", [](Ctx& c) { return !SL::SwitchPrompt(c.E); }), Wait(3),
        ExpectTrue("still the same scene, still dirty, the edit kept", [](Ctx& c) {
            return X::ScenePath(c.E) == g_ScratchScene && SL::Dirty(c.E) && ByName(c.W, "Gamma Unsaved") != entt::null;
        }),
        Key(ImGuiKey_N, ImGuiMod_Ctrl),
        Until("the prompt again", [](Ctx&) { return Find(ByExact("Don't Save", kPrompt)) != nullptr; }),
        Click(ByExact("Don't Save", kPrompt)),
        Until("a new scene", [](Ctx& c) { return X::ScenePath(c.E) != g_ScratchScene; }),
        Do("note the new file", [](Ctx& c) { g_Path = X::ScenePath(c.E); }),
        Expect("an empty, clean Untitled scene", [](Ctx& c) {
            if (!Contains(fs::path(g_Path).filename().string(), "Untitled")) return "path is " + g_Path;
            if (SL::Title(c.E).rfind("Untitled", 0) != 0) return "title is " + SL::Title(c.E);
            if (SL::Dirty(c.E)) return std::string("dirty");
            return c.W.Registry.view<NameComponent>().empty() ? std::string() : std::string("objects in the new scene");
        }),
        Expect("the discarded edit never reached the scratch file", [](Ctx&) {
            return Contains(ReadFile(g_ScratchScene), "Gamma Unsaved") ? std::string("the edit was saved") : std::string();
        }),
        FreshScene(), Wait(10),
        DeleteUntitled(),
    });

    Add("saveload: Save in the New Scene prompt writes the edit, then switches", {
        FreshScene(),
        Do("save first", [](Ctx& c) { c.E.SaveScene(c.W, c.A); }), Wait(3),
        Rename("Beta", "Beta Kept"), Wait(3),
        HoverPanel(kScene), Wait(2),
        Key(ImGuiKey_N, ImGuiMod_Ctrl),
        Until("the prompt", [](Ctx&) { return Find(ByExact("Save", kPrompt)) != nullptr; }),
        Click(ByExact("Save", kPrompt)),
        Until("a new scene", [](Ctx& c) { return X::ScenePath(c.E) != g_ScratchScene; }),
        Do("note the new file", [](Ctx& c) { g_Path = X::ScenePath(c.E); }),
        Expect("the edit was saved first", [](Ctx&) {
            return Contains(ReadFile(g_ScratchScene), "Beta Kept") ? std::string() : std::string("Beta Kept is not in the scratch file");
        }),
        FreshScene(), Wait(10),
        DeleteUntitled(),
    });

    // Load -> save -> load of every project scene into scratch copies (never the originals), in a
    // World of its own so the editor's scene is untouched. The second save must equal the first.
    Add("saveload: every project scene round-trips through save and load unchanged", {
        FreshScene(), Wait(2),
        Do("allow errors naming the external files missing here", [](Ctx&) {
            for (const std::string& name : smoke_scenes::MissingExternalFiles()) g_AllowLog.push_back(name);
        }),
        Expect("round trips", [](Ctx& c) {
            const fs::path dir = fs::path(UserPaths::Root()) / "editor-tests-roundtrip";
            std::error_code ec;
            fs::create_directories(dir, ec);
            std::string problems;
            int n = 0;
            for (const fs::path& scene : ProjectScenes()) {
                const std::string label = scene.filename().string();
                const std::string before = ReadFile(scene.string());
                World first;
                if (!SceneSerializer::Load(first, c.A, scene.generic_string(), /*persistMigration=*/false)) {
                    problems += label + ": failed to load; ";
                    continue;
                }
                const std::string copyA = (dir / ("a-" + label)).generic_string();
                const std::string copyB = (dir / ("b-" + label)).generic_string();
                if (!SceneSerializer::Save(first, c.A, copyA)) { problems += label + ": first save failed; "; continue; }
                World second;
                if (!SceneSerializer::Load(second, c.A, copyA, false)) { problems += label + ": the saved copy failed to load; "; continue; }
                if (!SceneSerializer::Save(second, c.A, copyB)) { problems += label + ": second save failed; "; continue; }
                const std::string a = ReadFile(copyA), b = ReadFile(copyB);
                if (a != b) {
                    std::error_code fe;
                    problems += label + " " + FirstDifference(a, b) + " (sizes on disk " + std::to_string(fs::file_size(copyA, fe)) +
                                "/" + std::to_string(fs::file_size(copyB, fe)) + ", exists " + std::to_string(fs::exists(copyA, fe)) + "); ";
                }
                if (ReadFile(scene.string()) != before) problems += label + ": the original file changed; ";
                ++n;
            }
            fs::remove_all(dir, ec);
            if (n == 0) return std::string("no scenes found");
            return problems;
        }),
        Do("let the asset loads finish", [](Ctx&) {}),
        Until("assets settled", [](Ctx& c) { return c.A.AsyncInFlight() == 0; }, 60 * 120),
        FreshScene(), Wait(3),
    });
}
