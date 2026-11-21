// Editor smoke suite: HotReload. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The module hosts (HotReloadEditorModule / HotReloadGameModule) poll their DLL's write time every
// 0.35 s and swap in a fresh copy when it changes, so the cheapest real reload is to bump that
// time on the already-built DLL next to the exe. The slow test runs the real "Rebuild Game &
// Editor Modules" (cmake --build of both module targets, GameModuleBuild.cpp) when the editor
// runs from its CMake build tree.

} // namespace
#include "Core/Paths.h"
#include "GameModuleBuild.h"
#include "EditorRestart.h"
#include "RestartState.h"
struct EditorSmokeAccess_HotReload {
    // Whether the Asset Browser's Scenes listing (refreshed from disk when invalidated) has `path`.
    static bool SceneListed(EditorLayer& e, const std::string& path) {
        e.RefreshScenesListingIfNeeded();
        const std::string want = std::filesystem::path(path).generic_string();
        for (const std::string& p : e.m_ScenesListingCache.paths) {
            if (p.size() != want.size()) continue;
            bool same = true;
            for (size_t i = 0; i < p.size() && same; ++i) same = std::tolower((unsigned char)p[i]) == std::tolower((unsigned char)want[i]);
            if (same) return true;
        }
        return false;
    }
};
namespace {
namespace smoke_hotreload {
namespace fs = std::filesystem;
using HA = EditorSmokeAccess_HotReload;

#ifdef TARTARUS_EDITOR_MODULE_FILENAME
constexpr const char* kEditorDll = TARTARUS_EDITOR_MODULE_FILENAME;
#else
constexpr const char* kEditorDll = "TartarusEditor.dll";
#endif
#ifdef TARTARUS_GAME_MODULE_FILENAME
constexpr const char* kGameDll = TARTARUS_GAME_MODULE_FILENAME;
#else
constexpr const char* kGameDll = "TartarusGame.dll";
#endif
constexpr const char* kEditorReloaded = "Editor hot reload: TartarusEditor module reloaded.";
constexpr const char* kGameReloaded = "Hot reload: TartarusGame module reloaded.";
constexpr const char* kRebuildItem = "Rebuild Game & Editor Modules";

// Marks a file as just written (what the module watchers look at); false when it doesn't exist.
bool Touch(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
    return !ec;
}
fs::path ExeFile(const char* name) { return fs::path(EnginePaths::ExeDir()) / name; }
bool ItemDisabled(const UiItem* it) { return it && (it->ItemFlags & ImGuiItemFlags_Disabled) != 0; }
ImGuiWindow* Shown(const char* name) {
    ImGuiWindow* w = ImGui::FindWindowByName(name);
    return w && w->WasActive && !w->Hidden ? w : nullptr;
}
// The scene FreshScene() builds plus the rename the swap tests make, still there.
std::string SceneIntact(Ctx& c, const char* third) {
    for (const char* n : {"Alpha", "Beta", third})
        if (ByName(c.W, n) == entt::null) return std::string("object ") + n + " is gone";
    if (X::ScenePath(c.E) != g_ScratchScene) return "scene path changed to " + X::ScenePath(c.E);
    return std::string();
}
} // namespace smoke_hotreload

void RegisterHotReloadTests() {
    using namespace smoke_hotreload;

    // ------------------------------------------------------------------ status UI
    Add("hotreload: File > Rebuild Game & Editor Modules reflects the build state; IDE shows module status", {
        FreshScene(), Wait(2),
        Click(ByExact("File", "##Toolbar")), Wait(2),
        Until("the rebuild item is listed", [](Ctx&) { return Find(MenuItem(kRebuildItem)) != nullptr; }),
        Expect("enabled exactly when a build can start", [](Ctx&) {
            const bool want = GameModuleBuild::Available() && !GameModuleBuild::Running();
            const bool enabled = !ItemDisabled(Find(MenuItem(kRebuildItem)));
            return enabled == want ? std::string() : std::string(enabled ? "enabled" : "disabled") + " but a build " +
                (want ? "can start" : "can't start (no build tree, or one is running)");
        }),
        MoveToItem(MenuItem(kRebuildItem)), WaitSeconds(0.8),
        Shot("file-menu-rebuild"),
        CloseMenus(), Wait(2),
        Do("show the Script IDE", [](Ctx& c) { c.E.OpenScriptIDE(); }),
        UntilSeconds("the IDE is shown", [](Ctx&) { return Shown("Script IDE") != nullptr; }, 5),
        Wait(3),
        Shot("ide-module-status", {"Script IDE"}),
        Do("bring Scene to the front", [](Ctx&) { ImGui::FocusWindow(ImGui::FindWindowByName(EditorPanels::Scene)); }),
        Wait(3),
        Do("drop keyboard focus", [](Ctx&) { ImGui::FocusWindow(nullptr); }),
    });

    // ------------------------------------------------------------------ editor module swap
    Add("hotreload: the editor module swap keeps the scene, selection and undo; its panels draw again", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        Do("rename Gamma with undo", [](Ctx& c) {
            X::PushUndo(c.E, c.W, "Rename");
            c.W.Registry.get<NameComponent>(ByName(c.W, "Gamma")).Name = "Gamma2";
        }), Wait(3),
        Do("note undo depth and reload count, then touch the editor DLL", [](Ctx& c) {
            g_Number = (double)X::UndoDepth(c.E);
            g_Vec.x = (float)LogCount(kEditorReloaded);
            if (!Touch(ExeFile(kEditorDll))) Fail("no editor module at " + ExeFile(kEditorDll).string());
        }),
        UntilSeconds("the editor module reloaded", [](Ctx&) { return (float)LogCount(kEditorReloaded) > g_Vec.x; }, 15),
        Wait(10),
        Expect("scene intact", [](Ctx& c) { return SceneIntact(c, "Gamma2"); }),
        Expect("Beta still selected", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
        ExpectTrue("undo history kept", [](Ctx& c) { return (double)X::UndoDepth(c.E) == g_Number; }),
        UntilSeconds("module-drawn panels and the menu bar are back", [](Ctx&) {
            return Find(ByTag("row:Beta")) && Find(ByExact("File", "##Toolbar")) && Find(Header("Light"));
        }, 5),
        Shot("after-editor-swap", {kHierarchy, kInspector, kScene}),
        HoverPanel(kHierarchy), Wait(2),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo after the swap restores Gamma", [](Ctx& c) { return ByName(c.W, "Gamma") != entt::null; }),
    });

    // ------------------------------------------------------------------ game module swap
    Add("hotreload: the game module swap keeps the scene; Play still runs", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(2),
        Do("note the reload count, then touch the game DLL", [](Ctx&) {
            g_Vec.x = (float)LogCount(kGameReloaded);
            if (!Touch(ExeFile(kGameDll))) Fail("no game module at " + ExeFile(kGameDll).string());
        }),
        UntilSeconds("the game module reloaded", [](Ctx&) { return (float)LogCount(kGameReloaded) > g_Vec.x; }, 15),
        Wait(5),
        Expect("scene intact", [](Ctx& c) { return SceneIntact(c, "Gamma"); }),
        Expect("Alpha still selected", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
        Do("play", [](Ctx& c) { X::RequestPlayStop(c.E); }),
        UntilSeconds("in play", [](Ctx& c) { return X::InPlay(c.E); }, 20),
        WaitSeconds(1.0),
        Shot("play-after-game-swap", {kHierarchy}),
        Do("stop", [](Ctx& c) { X::RequestPlayStop(c.E); }),
        UntilSeconds("stopped", [](Ctx& c) { return !X::InPlay(c.E); }, 20), Wait(3),
        Expect("scene intact after Play", [](Ctx& c) { return SceneIntact(c, "Gamma"); }),
    });

    // ------------------------------------------------------------------ project watcher (ProjectSync)
    {
        auto path = [] { return fs::path(ProjectPaths::Resolve("scenes")) / "EditorSmokeWatcher.json"; };
        auto cleanup = [path](Ctx&) {
            std::error_code ec;
            fs::remove(path(), ec);
            fs::remove(fs::path(path().string() + ".meta"), ec);
        };
        Add("hotreload: a scene file added outside the editor gets listed (and a GUID); removing it drops both", {
            FreshScene(), Wait(2),
            Do("no leftover from an earlier run", cleanup), WaitSeconds(1.0),
            ExpectTrue("not listed yet", [path](Ctx& c) { return !HA::SceneListed(c.E, path().string()); }),
            Do("write a scene file from outside", [path](Ctx&) {
                if (!WriteFile(path(), "{\n  \"formatVersion\": 4\n}\n")) Fail("couldn't write " + path().string());
            }),
            UntilSeconds("the Scenes listing picks it up", [path](Ctx& c) { return HA::SceneListed(c.E, path().string()); }, 15),
            UntilSeconds("it got a .meta sidecar", [path](Ctx&) {
                std::error_code ec;
                return fs::exists(fs::path(path().string() + ".meta"), ec);
            }, 10),
            Do("delete it from outside", [path](Ctx&) { std::error_code ec; fs::remove(path(), ec); }),
            UntilSeconds("the listing drops it", [path](Ctx& c) { return !HA::SceneListed(c.E, path().string()); }, 15),
            UntilSeconds("its orphaned .meta is removed", [path](Ctx&) {
                std::error_code ec;
                return !fs::exists(fs::path(path().string() + ".meta"), ec);
            }, 10),
        }, cleanup); // pass or fail: a scene left in project/scenes would become a scenes: test of its own
    }

    // ------------------------------------------------------------------ Rebuild & Restart (fake build)
    // TARTARUS_RESTART_FAKE_BUILD stands in for CMake and stops the relaunch, so the hand-over runs end to end in
    // this process: a failed build keeps the editor as it was; a good one writes the recovery snapshot and the state
    // file, which a "new editor" (ApplyRestartState here) reads back into the same selection.
    Add("hotreload: Rebuild & Restart: a failed build keeps editing; a good one hands over scene, edits, selection", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        Do("rename Gamma with undo (unsaved edit)", [](Ctx& c) {
            X::PushUndo(c.E, c.W, "Rename");
            c.W.Registry.get<NameComponent>(ByName(c.W, "Gamma")).Name = "Gamma2";
            X::MarkDirty(c.E);
        }), Wait(3),
        AllowLogError("Rebuild & Restart: the build failed"),
        Do("fake build: fail", [](Ctx& c) {
            if (!EditorRestart::Available()) return; // packaged editor: the menu item is disabled, nothing to run
            Env::Set(Env::Var::RestartFakeBuild, "fail");
            c.E.RequestRebuildRestart();
        }),
        UntilSeconds("the failed build is reaped", [](Ctx&) { return !EditorRestart::Building(); }, 10),
        Wait(3),
        ExpectTrue("no relaunch after a failed build", [](Ctx&) { return !EditorRestart::ReadyToRelaunch(); }),
        Expect("scene intact", [](Ctx& c) { return SceneIntact(c, "Gamma2"); }),
        Do("fake build: ok", [](Ctx& c) {
            g_Vec.x = (float)LogCount("fake build, no relaunch");
            if (!EditorRestart::Available()) return;
            Env::Set(Env::Var::RestartFakeBuild, "ok");
            c.E.RequestRebuildRestart();
        }),
        UntilSeconds("the hand-over ran", [](Ctx&) {
            return !EditorRestart::Available() || (float)LogCount("fake build, no relaunch") > g_Vec.x;
        }, 10),
        Do("clear the fake", [](Ctx&) { Env::Set(Env::Var::RestartFakeBuild, ""); }),
        Expect("the state file names the scene, its edits and the selection", [](Ctx& c) {
            if (!EditorRestart::Available()) return std::string();
            std::string why;
            const auto s = RestartState::Load(EditorRestart::StateFile(), RestartState::NowUnix(), &why);
            if (!s) return "no state file: " + why;
            if (s->ScenePath != X::ScenePath(c.E)) return "scene " + s->ScenePath;
            if (!s->RestoreRecovery) return std::string("unsaved edits not handed over");
            if (s->Selection.size() != 1 || s->Selection[0].Name != "Beta") return std::string("selection not Beta");
            return std::string();
        }),
        Expect("still editing (no relaunch happened)", [](Ctx& c) { return SceneIntact(c, "Gamma2"); }),
        // The "new editor" starts on the saved scene. This process never raised the startup recovery prompt, the
        // same as a restart whose snapshot ties with the scene file's time: the edits must come back anyway.
        Do("back to the saved scene's content", [](Ctx& c) {
            c.W.Registry.get<NameComponent>(ByName(c.W, "Gamma2")).Name = "Gamma";
            g_Vec.z = (float)LogCount("with its unsaved edits");
        }),
        SelectByName("Alpha"), Wait(3),
        Do("the new editor's side: apply the state", [](Ctx& c) {
            if (EditorRestart::Available()) c.E.ApplyRestartState(c.W, c.A, EditorRestart::StateFile());
        }), Wait(3),
        Expect("Beta selected again", [](Ctx& c) {
            return EditorRestart::Available() ? ExpectSelected(c, "Beta") : std::string();
        }),
        Expect("the unsaved edit is back, and the log says so", [](Ctx& c) {
            if (!EditorRestart::Available()) return std::string();
            if (ByName(c.W, "Gamma2") == entt::null) return std::string("Gamma2 not restored");
            return (float)LogCount("with its unsaved edits") > g_Vec.z ? std::string() : std::string("no 'with its unsaved edits' line");
        }),
        ExpectTrue("the state file is consumed", [](Ctx&) {
            std::error_code ec;
            return !std::filesystem::exists(EditorRestart::StateFile(), ec);
        }),
    }, [](Ctx&) { EditorRestart::Cancel(); Env::Set(Env::Var::RestartFakeBuild, ""); std::error_code ec; std::filesystem::remove(EditorRestart::StateFile(), ec); });

    // Rebuild & Restart from Prefab Mode: no silent prefab writes. A clean prefab mode is left without saving and
    // the hand-over goes on; unsaved prefab edits disable the command, and edits made while the build runs cancel
    // the restart (the editor stays in Prefab Mode with them).
    {
        namespace SP = smoke_prefabs;
        static std::string s_PrefabBytes;
        auto prefabBytes = [] {
            std::ifstream f(std::filesystem::u8path(SP::PrefabPath()), std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        auto enter = [](const char* what) {
            return Do(what, [](Ctx& c) {
                c.E.EnterPrefabMode(c.W, c.A, SP::PrefabPath());
                if (!c.E.InPrefabMode()) Fail("not in Prefab Mode");
            });
        };
        auto cleanup = [](Ctx& c) {
            EditorRestart::Cancel();
            Env::Set(Env::Var::RestartFakeBuild, "");
            std::error_code ec;
            std::filesystem::remove(EditorRestart::StateFile(), ec);
            SP::Cleanup(c);
        };
        Add("hotreload: Rebuild & Restart in Prefab Mode: clean leaves it unsaved and hands over; dirty is blocked or cancels", {
            SP::Fresh(), FreshScene(), SP::MakePrefab(), Wait(2),
            Do("note the prefab file", [prefabBytes](Ctx&) { s_PrefabBytes = prefabBytes(); }),
            enter("open the prefab"), Wait(3),
            // Dirty: the command is disabled and says why.
            Do("rename the lamp (unsaved prefab edit)", [](Ctx& c) {
                X::PushUndo(c.E, c.W, "Rename");
                c.W.Registry.get<NameComponent>(SP::ChildOf(c.W, "Crate", "Crate Lamp")).Name = "Crate Bulb";
                X::MarkDirty(c.E);
            }), Wait(3),
            Expect("Rebuild & Restart is disabled", [](Ctx& c) {
                if (!EditorRestart::Available()) return std::string();
                const std::string why = c.E.RestartBlockedReason();
                return why.find("Prefab Mode") != std::string::npos ? std::string() : "blocked reason: '" + why + "'";
            }),
            Do("ask anyway", [](Ctx& c) { c.E.RequestRebuildRestart(); }), Wait(3),
            ExpectTrue("no build started", [](Ctx&) { return !EditorRestart::Building() && !EditorRestart::ReadyToRelaunch(); }),
            // Dirty after the build started: the hand-over cancels and keeps the edits.
            Do("leave without saving, reopen clean", [](Ctx& c) { c.E.ExitPrefabMode(c.W, c.A, /*save=*/false); }), Wait(3),
            enter("reopen the prefab"), Wait(3),
            Do("fake build: ok, then edit the prefab while it builds", [](Ctx& c) {
                g_Vec.x = (float)LogCount("the open prefab has unsaved edits");
                if (!EditorRestart::Available()) return;
                Env::Set(Env::Var::RestartFakeBuild, "ok");
                c.E.RequestRebuildRestart();
                if (!EditorRestart::Building()) { Fail("the clean prefab mode blocked the restart"); return; }
                X::PushUndo(c.E, c.W, "Rename");
                c.W.Registry.get<NameComponent>(SP::ChildOf(c.W, "Crate", "Crate Lamp")).Name = "Crate Bulb";
                X::MarkDirty(c.E);
            }),
            UntilSeconds("the hand-over cancelled", [](Ctx&) {
                return !EditorRestart::Available() || (float)LogCount("the open prefab has unsaved edits") > g_Vec.x;
            }, 10),
            Wait(3),
            Expect("still in Prefab Mode with the edit, nothing written, no restart pending", [prefabBytes](Ctx& c) {
                if (!c.E.InPrefabMode()) return std::string("left Prefab Mode");
                if (SP::ChildOf(c.W, "Crate", "Crate Bulb") == entt::null) return std::string("the edit is gone");
                if (!X::Dirty(c.E)) return std::string("not dirty");
                if (prefabBytes() != s_PrefabBytes) return std::string("the prefab file was written");
                if (EditorRestart::ReadyToRelaunch() || EditorRestart::Building()) return std::string("restart still pending");
                std::error_code ec;
                return std::filesystem::exists(EditorRestart::StateFile(), ec) ? std::string("a state file was written") : std::string();
            }),
            // Clean: the hand-over leaves Prefab Mode without saving and goes on.
            Do("leave without saving, reopen clean", [](Ctx& c) { c.E.ExitPrefabMode(c.W, c.A, /*save=*/false); }), Wait(3),
            enter("reopen the prefab"), Wait(3),
            Do("fake build: ok", [](Ctx& c) {
                g_Vec.y = (float)LogCount("fake build, no relaunch");
                if (EditorRestart::Available()) c.E.RequestRebuildRestart();
            }),
            UntilSeconds("the hand-over ran", [](Ctx&) {
                return !EditorRestart::Available() || (float)LogCount("fake build, no relaunch") > g_Vec.y;
            }, 10),
            Do("clear the fake", [](Ctx&) { Env::Set(Env::Var::RestartFakeBuild, ""); }),
            Expect("out of Prefab Mode, the prefab file untouched", [prefabBytes](Ctx& c) {
                if (!EditorRestart::Available()) return std::string();
                if (c.E.InPrefabMode()) return std::string("still in Prefab Mode");
                return prefabBytes() == s_PrefabBytes ? std::string() : std::string("the prefab file was written");
            }),
        }, cleanup);
    }

    // ------------------------------------------------------------------ the real rebuild
    // cmake --build <build> --config <cfg> --target TartarusGame TartarusEditor, as the menu item
    // runs it. Touching one source of each module makes the build compile one file per module and
    // relink both DLLs, which the watchers then swap in. Without a build tree (a packaged editor)
    // the item must be disabled and the test checks only that.
    {
        auto clickRebuild = [] {
            Step click = Click(MenuItem(kRebuildItem));
            click.Name += " (when a build tree exists)";
            click.Fn = [inner = click.Fn](Ctx& c) {
                if (!GameModuleBuild::Available()) { ImGui::ClosePopupsExceptModals(); return true; }
                return inner(c);
            };
            return click;
        };
        Add("hotreload: rebuild modules (slow)", {
            FreshScene(), Wait(2),
            SelectByName("Beta"), Wait(3),
            Do("note reload counts; touch one source of each module", [](Ctx&) {
                g_Vec = glm::vec3((float)LogCount(kGameReloaded), (float)LogCount(kEditorReloaded), 0.0f);
                if (!GameModuleBuild::Available()) {
                    std::printf("    [hotreload] no CMake build tree: only the disabled menu item is checked\n");
                    return;
                }
                const fs::path src = GameModuleBuild::SourceRoot() / "src";
                if (!Touch(src / "Game" / "LightFlickerSystem.cpp")) Fail("no src/Game/LightFlickerSystem.cpp");
                if (!Touch(src / "Editor" / "EditorModuleStats.cpp")) Fail("no src/Editor/EditorModuleStats.cpp");
            }),
            // Reported by the explicit check below, with the compiler output.
            AllowLogError("Module rebuild failed"),
            Click(ByExact("File", "##Toolbar")), Wait(2),
            Expect("the item is enabled exactly when a build tree exists", [](Ctx&) {
                const UiItem* it = Find(MenuItem(kRebuildItem));
                if (!it) return std::string("item missing");
                return ItemDisabled(it) == !GameModuleBuild::Available() ? std::string() : std::string("wrong enabled state");
            }),
            clickRebuild(), Wait(3),
            ExpectTrue("a module build is running", [](Ctx&) { return !GameModuleBuild::Available() || GameModuleBuild::Running(); }),
            Click(ByExact("File", "##Toolbar")), Wait(3),
            ExpectTrue("the item is disabled while it builds", [](Ctx&) {
                return !GameModuleBuild::Available() || !GameModuleBuild::Running() || ItemDisabled(Find(MenuItem(kRebuildItem)));
            }),
            Shot("building"),
            CloseMenus(), Wait(2),
            UntilSeconds("the build finished", [](Ctx&) { return !GameModuleBuild::Running(); }, 360),
            Expect("the build succeeded", [](Ctx&) {
                if (!GameModuleBuild::Available() || GameModuleBuild::LastSucceeded()) return std::string();
                const std::string out = GameModuleBuild::LastLog();
                return "cmake --build failed: ..." + Trim(out.size() > 1500 ? out.substr(out.size() - 1500) : out, 1600);
            }),
            UntilSeconds("both modules hot reloaded", [](Ctx&) {
                return !GameModuleBuild::Available() ||
                       ((float)LogCount(kGameReloaded) > g_Vec.x && (float)LogCount(kEditorReloaded) > g_Vec.y);
            }, 30),
            Wait(10),
            Expect("scene intact", [](Ctx& c) { return SceneIntact(c, "Gamma"); }),
            Expect("Beta still selected", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
            UntilSeconds("panels draw after the rebuild", [](Ctx&) { return Find(ByTag("row:Beta")) && Find(ByExact("File", "##Toolbar")); }, 5),
            Shot("after-rebuild", {kHierarchy, kInspector, kScene}),
        });
    }
}
