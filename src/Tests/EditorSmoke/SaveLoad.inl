// Editor smoke suite: SaveLoad. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The scene file life cycle: an edit dirties the scene and Ctrl+S cleans it; what was saved
// reopens identical; New Scene over unsaved work asks first (Cancel keeps everything, Don't Save
// drops it, Save writes it then switches). File > Save / Revert / New themselves are in the Menus
// suite. Last, every scene under project/scenes/ and tests/scenes/ goes through a load -> save -> load round trip
// into a scratch copy and must come back byte-identical. Nothing under project/ is written, except
// the Untitled*.json New Scene creates, which each test deletes again.

} // namespace
struct EditorSmokeAccess_SaveLoad {
    static std::string Title(EditorLayer& e) { return e.SceneTitle(); }
    static bool SwitchPrompt(EditorLayer& e) { return e.m_ScenePromptPending; }
    static void Open(EditorLayer& e, World& w, AssetLibrary& a, const std::string& path) { e.RequestOpenScene(w, a, path); }
    // The auto-save timer fires on the next Draw.
    static void ForceAutoSave(EditorLayer& e) { e.m_AutoSaveTimer = 1.0e9f; }
    static bool RecoveryWritePending(EditorLayer& e) { return e.m_RecoveryWrite.valid(); }
    static std::string RecoveryPath(EditorLayer& e) { return EditorLayer::RecoveryPathFor(e.m_CurrentScenePath); }
    static bool BaselineCurrent(EditorLayer& e) { return e.UndoBaselineIsCurrent(); }
    // A scene change no undo step records (as an external rename rewriting references does).
    static void ChangeOutsideUndo(EditorLayer& e) { e.m_Dirty = true; e.NoteChangeOutsideUndo(); }
};
namespace {
namespace smoke_saveload {
namespace fs = std::filesystem;
using SL = EditorSmokeAccess_SaveLoad;
const char* kPrompt = "Save changes?##SceneSwitch";

std::string g_Dump;  // a scene dump to compare against
std::string g_Path;  // a scene path to come back to / clean up

Step Rename(const char* from, const char* to) {
    return Do(std::string("rename ") + from + " to " + to, [from, to](Ctx& c) {
        X::PushUndo(c.E, c.W, "Rename");
        c.W.Registry.get<NameComponent>(ByName(c.W, from)).Name = to;
    });
}
// New Scene writes project/scenes/Untitled*.json: delete it (and any .meta the asset scan made). The
// tests also run this from their Finally, so a failed test leaves no scene for the "scenes:" suite to pick up.
void RemoveUntitled() {
    std::error_code ec;
    if (!Contains(fs::path(g_Path).filename().string(), "Untitled")) return;
    fs::remove(g_Path, ec);
    fs::remove(g_Path + ".meta", ec);
}
Step DeleteUntitled() {
    return Do("delete the Untitled scene New Scene wrote", [](Ctx&) { RemoveUntitled(); });
}
std::vector<fs::path> ProjectScenes() {
    std::vector<fs::path> scenes;
    for (const std::string& dir : {ProjectPaths::Resolve("scenes"), EnginePaths::Resolve("tests/scenes")}) {
        std::vector<fs::path> here;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(fs::path(dir), ec)) {
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
        ExpectTrue("dirty after the edits", [](Ctx& c) { return X::Dirty(c.E); }),
        HoverPanel(kScene), Wait(2),
        Key(ImGuiKey_S, ImGuiMod_Ctrl),
        Until("saved (clean, file written)", [](Ctx& c) { return !X::Dirty(c.E) && fs::exists(g_ScratchScene); }),
        Expect("the file holds the edits", [](Ctx&) {
            const std::string s = ReadFileText(g_ScratchScene);
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
        ExpectTrue("clean after the open", [](Ctx& c) { return !X::Dirty(c.E); }),
        Expect("no undo history after the open", [](Ctx& c) {
            return X::UndoDepth(c.E) == 0 ? std::string() : "history: " + EditorSmokeAccess_PlayMode::UndoLabels(c.E);
        }),
    });

    // The timer's recovery snapshot is read on the UI thread and written on a worker; a Save right
    // after it must still leave no snapshot behind (it waits for the write before clearing it).
    Add("saveload: the auto-save timer writes the recovery snapshot in the background; a Save right after clears it", Cat({
        {FreshScene(),
         Do("save first, auto-save on", [](Ctx& c) {
             c.E.SaveScene(c.W, c.A);
             g_Dump = EditorSettings::Get().AutoSaveEnabled ? "1" : "0";
             EditorSettings::Get().AutoSaveEnabled = true;
             std::error_code ec; fs::remove(fs::u8path(SL::RecoveryPath(c.E)), ec);
         }), Wait(3)},
        {Rename("Alpha", "Alpha Autosaved"), Wait(3),
         ExpectTrue("dirty after the edit", [](Ctx& c) { return X::Dirty(c.E); }),
         Do("let the timer fire", [](Ctx& c) { SL::ForceAutoSave(c.E); }), Wait(1),
         Until("the snapshot is written and the write is collected", [](Ctx& c) {
             return !SL::RecoveryWritePending(c.E) && fs::exists(fs::u8path(SL::RecoveryPath(c.E)));
         }, 600),
         ExpectTrue("it holds the edit", [](Ctx& c) { return Contains(ReadFileText(SL::RecoveryPath(c.E)), "Alpha Autosaved"); }),
         ExpectTrue("still dirty: the snapshot never stands in for a save", [](Ctx& c) { return X::Dirty(c.E); }),
         Rename("Alpha Autosaved", "Alpha Autosaved 2"), Wait(3),
         Do("let the timer fire again", [](Ctx& c) { SL::ForceAutoSave(c.E); }), Wait(1),
         Do("save while that write may still run", [](Ctx& c) { c.E.SaveScene(c.W, c.A); }), Wait(3),
         ExpectTrue("clean", [](Ctx& c) { return !X::Dirty(c.E); }),
         ExpectTrue("no recovery snapshot left behind", [](Ctx& c) { return !fs::exists(fs::u8path(SL::RecoveryPath(c.E))); })},
    }), [](Ctx& c) {
        EditorSettings::Get().AutoSaveEnabled = g_Dump != "0";
        std::error_code ec; fs::remove(fs::u8path(SL::RecoveryPath(c.E)), ec);
    });

    // The timer writes the undo baseline when it still is the World: the same bytes Save writes.
    // A change no undo step recorded makes it read the World instead, so the change isn't lost.
    Add("saveload: the auto-save writes the undo baseline as Save would; a change outside undo still reaches it", Cat({
        {FreshScene(),
         Do("save first, auto-save on", [](Ctx& c) {
             c.E.SaveScene(c.W, c.A);
             g_Dump = EditorSettings::Get().AutoSaveEnabled ? "1" : "0";
             EditorSettings::Get().AutoSaveEnabled = true;
             std::error_code ec; fs::remove(fs::u8path(SL::RecoveryPath(c.E)), ec);
         }), Wait(3)},
        {Rename("Alpha", "Alpha Baseline"), Wait(3),
         ExpectTrue("the baseline is current after a committed edit", [](Ctx& c) { return SL::BaselineCurrent(c.E); }),
         Do("let the timer fire", [](Ctx& c) { SL::ForceAutoSave(c.E); }), Wait(1),
         Until("the snapshot is written", [](Ctx& c) {
             return !SL::RecoveryWritePending(c.E) && fs::exists(fs::u8path(SL::RecoveryPath(c.E)));
         }, 600),
         Expect("byte for byte what Save writes", [](Ctx& c) {
             const std::string ref = SL::RecoveryPath(c.E) + ".ref.json";
             if (!SceneSerializer::Save(c.W, c.A, ref)) return std::string("Save failed");
             const std::string want = ReadFileText(ref), got = ReadFileText(SL::RecoveryPath(c.E));
             std::error_code ec; fs::remove(fs::u8path(ref), ec);
             return got == want ? std::string() : FirstDifference(want, got);
         }),
         Do("change Beta's name without an undo step", [](Ctx& c) {
             std::error_code ec; fs::remove(fs::u8path(SL::RecoveryPath(c.E)), ec);
             c.W.Registry.get<NameComponent>(ByName(c.W, "Beta")).Name = "Beta Outside Undo";
             SL::ChangeOutsideUndo(c.E);
         }), Wait(2),
         ExpectTrue("the baseline no longer counts as current", [](Ctx& c) { return !SL::BaselineCurrent(c.E); }),
         Do("let the timer fire again", [](Ctx& c) { SL::ForceAutoSave(c.E); }), Wait(1),
         Until("the snapshot is written again", [](Ctx& c) {
             return !SL::RecoveryWritePending(c.E) && fs::exists(fs::u8path(SL::RecoveryPath(c.E)));
         }, 600),
         ExpectTrue("it holds the change made outside undo", [](Ctx& c) {
             const std::string t = ReadFileText(SL::RecoveryPath(c.E));
             return Contains(t, "Beta Outside Undo") && Contains(t, "Alpha Baseline");
         })},
    }), [](Ctx& c) {
        EditorSettings::Get().AutoSaveEnabled = g_Dump != "0";
        std::error_code ec; fs::remove(fs::u8path(SL::RecoveryPath(c.E)), ec);
    });

    Add("saveload: undo back to the saved state clears the dirty flag; redo sets it again", {
        FreshScene(),
        Do("save first", [](Ctx& c) { c.E.SaveScene(c.W, c.A); }), Wait(3),
        ExpectTrue("clean after the save", [](Ctx& c) { return !X::Dirty(c.E); }),
        Rename("Alpha", "Alpha 2"), Wait(3),
        ExpectTrue("dirty after an edit", [](Ctx& c) { return X::Dirty(c.E); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        Expect("clean again after undoing it", [](Ctx& c) {
            if (ByName(c.W, "Alpha") == entt::null) return std::string("undo did not run");
            return X::Dirty(c.E) ? std::string("still dirty at the saved state") : std::string();
        }),
        Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("dirty after redo", [](Ctx& c) { return ByName(c.W, "Alpha 2") != entt::null && X::Dirty(c.E); }),
        SelectByName("Beta"), Wait(3),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3), // undoes the selection
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3), // undoes the rename
        ExpectTrue("a selection step never dirties", [](Ctx& c) { return ByName(c.W, "Alpha") != entt::null && !X::Dirty(c.E); }),
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
            return X::ScenePath(c.E) == g_ScratchScene && X::Dirty(c.E) && ByName(c.W, "Gamma Unsaved") != entt::null;
        }),
        Key(ImGuiKey_N, ImGuiMod_Ctrl),
        Until("the prompt again", [](Ctx&) { return Find(ByExact("Don't Save", kPrompt)) != nullptr; }),
        Click(ByExact("Don't Save", kPrompt)),
        Until("a new scene", [](Ctx& c) { return X::ScenePath(c.E) != g_ScratchScene; }),
        Do("note the new file", [](Ctx& c) { g_Path = X::ScenePath(c.E); }),
        Expect("an empty, clean Untitled scene", [](Ctx& c) {
            if (!Contains(fs::path(g_Path).filename().string(), "Untitled")) return "path is " + g_Path;
            if (SL::Title(c.E).rfind("Untitled", 0) != 0) return "title is " + SL::Title(c.E);
            if (X::Dirty(c.E)) return std::string("dirty");
            return c.W.Registry.view<NameComponent>().empty() ? std::string() : std::string("objects in the new scene");
        }),
        Expect("the discarded edit never reached the scratch file", [](Ctx&) {
            return Contains(ReadFileText(g_ScratchScene), "Gamma Unsaved") ? std::string("the edit was saved") : std::string();
        }),
        FreshScene(), Wait(10),
        DeleteUntitled(),
    }, [](Ctx&) { RemoveUntitled(); });

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
            return Contains(ReadFileText(g_ScratchScene), "Beta Kept") ? std::string() : std::string("Beta Kept is not in the scratch file");
        }),
        FreshScene(), Wait(10),
        DeleteUntitled(),
    }, [](Ctx&) { RemoveUntitled(); });

    // Load -> save -> load of every project scene into scratch copies (never the originals), in a
    // World of its own so the editor's scene is untouched. The second save must equal the first.
    // One scene per step: with the Drive assets present a big scene's first load imports its models
    // synchronously (minutes for the Forest), and one frame holding every scene tripped the watchdog.
    {
        std::vector<Step> steps = {
            FreshScene(), Wait(2),
            smoke_scenes::AllowMissingExternal(),
        };
        const auto scenes = ProjectScenes();
        if (scenes.empty()) steps.push_back(Expect("project scenes", [](Ctx&) { return std::string("no scenes found"); }));
        for (const fs::path& scene : scenes) {
            const std::string label = scene.filename().string();
            steps.push_back(Expect(label + " round-trips", [scene, label](Ctx& c) {
                const fs::path dir = fs::path(UserPaths::Root()) / "editor-tests-roundtrip";
                std::error_code ec;
                fs::create_directories(dir, ec);
                const std::string before = ReadFileText(scene.string());
                const std::string copyA = (dir / ("a-" + label)).generic_string();
                const std::string copyB = (dir / ("b-" + label)).generic_string();
                std::string problem;
                World first, second;
                if (!SceneSerializer::Load(first, c.A, scene.generic_string(), /*persistMigration=*/false)) problem = "failed to load";
                else if (!SceneSerializer::Save(first, c.A, copyA)) problem = "first save failed";
                else if (!SceneSerializer::Load(second, c.A, copyA, false)) problem = "the saved copy failed to load";
                else if (!SceneSerializer::Save(second, c.A, copyB)) problem = "second save failed";
                else if (const std::string a = ReadFileText(copyA), b = ReadFileText(copyB); a != b) {
                    std::error_code fe;
                    problem = FirstDifference(a, b) + " (sizes on disk " + std::to_string(fs::file_size(copyA, fe)) + "/" +
                              std::to_string(fs::file_size(copyB, fe)) + ")";
                } else if (ReadFileText(scene.string()) != before) problem = "the original file changed";
                fs::remove(copyA, ec);
                fs::remove(copyB, ec);
                return problem.empty() ? std::string() : label + ": " + problem;
            }));
            steps.push_back(Wait(1));
        }
        steps.push_back(Do("remove the scratch folder", [](Ctx&) {
            std::error_code ec;
            fs::remove_all(fs::path(UserPaths::Root()) / "editor-tests-roundtrip", ec);
        }));
        steps.push_back(Until("assets settled", [](Ctx& c) { return c.A.AsyncInFlight() == 0; }, 60 * 120));
        steps.push_back(FreshScene());
        steps.push_back(Wait(3));
        Add("saveload: every project scene round-trips through save and load unchanged", std::move(steps));
    }
}
