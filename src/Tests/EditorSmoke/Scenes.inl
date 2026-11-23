// Editor smoke suite: Scenes. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// Every scene under project/scenes/ (the Forest) and tests/scenes/ (the test scenes) is opened through the
// editor's own open path (RequestOpenScene, what the Asset Browser's double-click and File > Open
// Recent use), left to settle, looked at through the Hierarchy and Inspector, then played for a
// couple of seconds. Nothing is saved: each test ends back on the scratch scene.
//
// CI has none of the files project/external_assets.csv lists (Mixamo characters, the sky HDRIs,
// the furniture packs...). The loaders mostly downgrade an unfetched external file to the asset
// scan's warning (AssetDatabase::IsUnfetchedExternal), but a few still log an error naming the
// file - "Model: async import of '<path>' produced nothing." (AssetLibraryAsync.cpp),
// "Audio: failed to load sound '<path>'." (AudioEngine.cpp), "Cubemap: failed to load HDR image
// '<path>'" (Cubemap.cpp) - so each test allows errors naming one of the external files that are
// actually missing on this machine, and nothing else.

} // namespace
struct EditorSmokeAccess_Scenes {
    // The scratch scene FreshScene() builds is never worth keeping, so skip the unsaved-changes
    // prompt a stray dirty flag would raise (it would block the open behind a modal).
    static void Open(EditorLayer& e, World& w, AssetLibrary& a, const std::string& path) {
        e.m_Dirty = false;
        e.RequestOpenScene(w, a, path);
    }
    // An open is two steps since the async preload (#938): `path` is open once nothing is preloading and it is the
    // current scene. With the Drive assets linked the preload can take seconds, far past a fixed frame wait.
    static bool IsOpen(EditorLayer& e, const std::string& path) { return !e.m_ScenePreload && e.m_CurrentScenePath == path; }
    // For a timeout message: what the open is still waiting on.
    static std::string OpenState(EditorLayer& e, AssetLibrary& a) {
        return std::string(e.m_ScenePreload ? "a preload is still pending" : "no preload pending") +
               ", " + std::to_string(a.AsyncInFlight()) + " asset load(s) in flight, open scene '" + e.m_CurrentScenePath + "'";
    }
    // The undo stack's labels that aren't selection steps (opening a scene and playing it is none).
    static std::string EditSteps(EditorLayer& e) {
        std::string s;
        for (const auto& u : e.m_UndoStack)
            if (!u.SelectionOnly) s += (s.empty() ? "" : ", ") + u.Label;
        return s;
    }
    // Undo / Redo keep the snapshot they just loaded as the new baseline instead of serializing the
    // scene again, which needs a snapshot to load back to the same bytes. Empty when it does; else
    // where the two first differ (and whether only the AssetLibrary half does).
    static std::string RoundTripDifference(World& w, AssetLibrary& a) {
        const std::string before = SceneSerializer::SaveToString(w, a);
        if (!SceneSerializer::LoadFromString(w, a, before)) return "the snapshot didn't load";
        const std::string after = SceneSerializer::SaveToString(w, a);
        if (after == before) return {};
        size_t i = 0;
        while (i < before.size() && i < after.size() && before[i] == after[i]) ++i;
        const size_t from = i > 80 ? i - 80 : 0;
        return std::string(SceneSerializer::SameSceneContent(before, after) ? "library half only" : "scene differs") +
               " at byte " + std::to_string(i) + ": was '" + before.substr(from, 200) + "' now '" + after.substr(from, 200) + "'";
    }
    // A root object with a transform, for the undo timing below (null when there is none).
    static entt::entity MovableRoot(World& w) {
        for (auto [ent, t, n] : w.Registry.view<TransformComponent, NameComponent>().each()) {
            const auto* h = w.Registry.try_get<HierarchyComponent>(ent);
            if (!h || h->Parent == entt::null) return ent;
        }
        return entt::null;
    }
    // Times one Undo / Redo call. The step's in-place check (X-33 C) runs on a background thread
    // from the moment the step is recorded; a person takes longer than that to press Ctrl+Z, so it
    // is waited for first, outside the timing (`jobMs` gets how long that took). The verify mode
    // is off for the timed call (it saves the whole scene) and done after it instead: the scene
    // must save as the new baseline.
    static double TimedHistoryStep(EditorLayer& e, World& w, AssetLibrary& a, bool redo, double* jobMs = nullptr,
                                   std::string* mismatch = nullptr) {
        const auto& stack = redo ? e.m_RedoStack : e.m_UndoStack;
        const auto w0 = std::chrono::steady_clock::now();
        if (!stack.empty()) stack.back().Step.Wait();
        if (jobMs) *jobMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
        const bool verify = e.m_VerifyInPlaceUndo;
        e.m_VerifyInPlaceUndo = false;
        const auto t0 = std::chrono::steady_clock::now();
        redo ? e.Redo(w, a) : e.Undo(w, a);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        e.m_VerifyInPlaceUndo = verify;
        if (mismatch && SceneSerializer::SaveToString(w, a) != e.m_CurrentUndoScene)
            *mismatch += std::string(redo ? " redo" : " undo") + " left a scene that doesn't save as its baseline;";
        return ms;
    }
    static void SetInPlaceUndo(EditorLayer& e, bool on) { e.m_InPlaceUndo = on; }
    static int InPlaceSteps(const EditorLayer& e) { return e.m_InPlaceHistorySteps; }
    // Moves `ent` 1 m along x as one committed undo step.
    static void MoveOneMetre(EditorLayer& e, World& w, entt::entity ent) {
        e.PushUndo(w, "Move");
        w.Registry.get<TransformComponent>(ent).Position.x += 1.0f;
        e.FinishGlobalUndo(w, /*force=*/true);
    }
};
namespace {
namespace smoke_scenes {
namespace fs = std::filesystem;
using SA = EditorSmokeAccess_Scenes;

// File names of the external assets (project/external_assets.csv) missing on this machine.
const std::vector<std::string>& MissingExternalFiles() {
    static const std::vector<std::string> names = [] {
        std::set<std::string> found;
        std::ifstream in(fs::path(ProjectPaths::Resolve("external_assets.csv")), std::ios::binary);
        std::string line;
        bool header = true;
        while (std::getline(in, line)) {
            if (header) { header = false; continue; }
            const std::string rel = line.substr(0, line.find(','));
            if (rel.empty()) continue;
            std::error_code ec;
            if (fs::exists(fs::path(ProjectPaths::Resolve(rel)), ec)) continue;
            const std::string name = fs::path(rel).filename().string();
            if (name.size() > 4) found.insert(name);
        }
        return std::vector<std::string>(found.begin(), found.end());
    }();
    return names;
}
// A step allowing the log errors that name those files (and the one the Dummy logs without a
// name when its external body meshes are missing).
Step AllowMissingExternal() {
    return Do("allow errors naming the external files missing here", [](Ctx&) {
        for (const std::string& name : MissingExternalFiles()) g_AllowLog.push_back(name);
        if (!MissingExternalFiles().empty()) g_AllowLog.push_back("Dummies: the body has no animated skeleton");
    });
}
Query Row(int nth) {
    Query q = ByTagPrefix("row:");
    q.Root = kHierarchy;
    q.Nth = nth;
    q.AnchorX = 0.25f; // the name side, like ByTag("row:..."): a narrow row's centre is its icon strip
    return q;
}
int RowCount() {
    Query q = ByTagPrefix("row:");
    q.Root = kHierarchy;
    return (int)FindAll(q).size();
}
// Clicks the Hierarchy's nth row when the scene has that many shown; otherwise nothing.
Step ClickRowIfAny(int nth) {
    Step click = Click(Row(nth));
    auto skip = std::make_shared<int>(-1);
    click.Name = "select Hierarchy row " + std::to_string(nth + 1) + " (if there is one)";
    click.Fn = [inner = click.Fn, skip, nth](Ctx& c) {
        if (*skip < 0) *skip = RowCount() > nth ? 0 : 1;
        return *skip == 1 || inner(c);
    };
    return click;
}
// The unified viewport showing the game (the Play dissolve finished).
bool GameShown(Ctx& c) {
    return c.E.GetViewportMode() == EditorLayer::ViewportMode::Game && !c.E.ViewFadeActive() && ChromeUi::Shown(EditorPanels::Scene);
}
} // namespace smoke_scenes

void RegisterScenesTests() {
    using namespace smoke_scenes;

    // The same round trip for every component Add Component offers, one at a time on the scratch
    // scene's Alpha (the shipped scenes don't carry them all).
    Add("scenes: every component round-trips through an undo snapshot", {
        FreshScene(), Wait(2),
        Expect("each component loads back to the same bytes", [](Ctx& c) {
            std::string bad, prev;
            for (const RegisteredComponent& rc : ComponentRegistry::All()) {
                if (!rc.Add || !rc.Has || !rc.Remove || !rc.Meta.Name) continue;
                entt::entity alpha = ByName(c.W, "Alpha");
                if (alpha == entt::null) return bad + " || no Alpha after " + prev + ", before " + rc.Meta.Name;
                const bool had = rc.Has(c.W.Registry, alpha);
                if (!had) rc.Add(c.W.Registry, alpha);
                const std::string diff = SA::RoundTripDifference(c.W, c.A);
                if (!diff.empty()) bad += std::string(bad.empty() ? "" : " || ") + rc.Meta.Name + ": " + diff;
                alpha = ByName(c.W, "Alpha"); // the round trip rebuilt the registry
                if (!had && alpha != entt::null && rc.Has(c.W.Registry, alpha)) rc.Remove(c.W.Registry, alpha);
                prev = rc.Meta.Name;
            }
            return bad;
        }),
        FreshScene(), Wait(3),
    });

    // Found when the tests are registered (the first editor frame), in a stable order.
    std::vector<fs::path> scenes;
    for (const std::string& dir : {ProjectPaths::Resolve("scenes"), EnginePaths::Resolve("tests/scenes")}) {
        std::vector<fs::path> here;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(fs::path(dir), ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            const std::string name = entry.path().filename().string();
            if (Contains(name, ".recovery.json") || Contains(name, ".backup.json")) continue;
            here.push_back(entry.path());
        }
        std::sort(here.begin(), here.end());
        scenes.insert(scenes.end(), here.begin(), here.end());
    }

    for (const fs::path& scene : scenes) {
        const std::string path = scene.generic_string(); // the Asset Browser's spelling of a scene key
        const std::string label = scene.stem().string();

        Add("scenes: " + label + " opens, settles, inspects and plays", {
            // Allowed first: the previous scene's async loads can still log while FreshScene runs.
            AllowMissingExternal(),
            FreshScene(), Wait(2),
            Do("Scene tab in front", [](Ctx& c) { c.E.RequestSceneTabFocus(); }), Wait(2),
            Do("open " + label, [path](Ctx& c) { SA::Open(c.E, c.W, c.A, path); }),
            // The open waits for the scene's models and materials to load on the asset workers first.
            UntilSeconds("it is the open scene", [path](Ctx& c) { return X::ScenePath(c.E) == path; }, 600),
            UntilSeconds("assets streamed in", [](Ctx& c) { return c.A.AsyncInFlight() == 0; }, 120),
            WaitSeconds(1.5),
            UntilSeconds("assets still settled", [](Ctx& c) { return c.A.AsyncInFlight() == 0; }, 60),
            Expect("objects loaded", [](Ctx& c) {
                int n = 0;
                for (auto e : c.W.Registry.view<NameComponent>()) { (void)e; ++n; }
                return n > 0 ? std::string() : std::string("the world has no named objects");
            }),
            UntilSeconds("the Hierarchy lists them", [](Ctx&) { return RowCount() > 0; }, 10),
            Shot("editor", {kScene, kHierarchy, kInspector}),
            ClickRowIfAny(0), Wait(4),
            ExpectTrue("a click selected the first row", [](Ctx& c) { return !X::Selection(c.E).empty(); }),
            ClickRowIfAny(1), Wait(4),
            ClickRowIfAny(2), Wait(4),
            ExpectTrue("still something selected", [](Ctx& c) { return !X::Selection(c.E).empty(); }),
            Shot("inspector", {kInspector}),
            Do("play", [](Ctx& c) { X::RequestPlayStop(c.E); }),
            UntilSeconds("in play", [](Ctx& c) { return X::InPlay(c.E); }, 60),
            WaitSeconds(2.0),
            UntilSeconds("the Game view is shown", [](Ctx& c) { return GameShown(c); }, 10),
            Shot("game", {"###Scene"}),
            Do("stop", [](Ctx& c) { X::RequestPlayStop(c.E); }),
            UntilSeconds("stopped", [](Ctx& c) { return !X::InPlay(c.E); }, 60),
            Wait(3),
            Expect("still the same scene after Stop", [path](Ctx& c) {
                return X::ScenePath(c.E) == path ? std::string() : "open scene is '" + X::ScenePath(c.E) + "'";
            }),
            // #15: what the editor does to a scene it just opened (an outfit rebuilding its body) is
            // no "Unrecorded Change" for Play to journal.
            Expect("opening, clicking and playing made no undo step and left it clean", [](Ctx& c) {
                const std::string steps = SA::EditSteps(c.E);
                if (!steps.empty()) return "undo steps: " + steps;
                return c.E.IsDirty() ? std::string("the scene is dirty") : std::string();
            }),
            Expect("a snapshot loads back to the same bytes", [](Ctx& c) { return SA::RoundTripDifference(c.W, c.A); }),
            Expect("a 1 m move undoes, redoes and undoes again (timed)", [label](Ctx& c) {
                const entt::entity ent = SA::MovableRoot(c.W);
                if (ent == entt::null || !c.W.Registry.all_of<OrderComponent>(ent)) return std::string(); // nothing to move
                const int order = c.W.Registry.get<OrderComponent>(ent).Value;
                const float x0 = c.W.Registry.get<TransformComponent>(ent).Position.x;
                auto xNow = [&c, order]() -> float {
                    for (auto [e, o, t] : c.W.Registry.view<OrderComponent, TransformComponent>().each())
                        if (o.Value == order) return t.Position.x;
                    return -1e9f;
                };
                // The same three steps with the in-place path off first: the full reload's times, from
                // the same run (X-33 C A/B). The first move also warms the caches the timed ones use.
                {
                    SA::SetInPlaceUndo(c.E, false);
                    SA::MoveOneMetre(c.E, c.W, ent);
                    std::string fullMismatch;
                    const double u1 = SA::TimedHistoryStep(c.E, c.W, c.A, false, nullptr, &fullMismatch);
                    const double r1 = SA::TimedHistoryStep(c.E, c.W, c.A, true, nullptr, &fullMismatch);
                    const double u2 = SA::TimedHistoryStep(c.E, c.W, c.A, false, nullptr, &fullMismatch);
                    SA::SetInPlaceUndo(c.E, true);
                    Log::Info("UNDO_TIMING_FULL " + label + " undo=" + std::to_string(u1) + "ms redo=" + std::to_string(r1) +
                              "ms undo2=" + std::to_string(u2) + "ms");
                    if (!fullMismatch.empty()) return "full reload:" + fullMismatch;
                    if (xNow() != x0) return std::string("full reload: the move didn't undo");
                }
                entt::entity live = entt::null; // the reload made new entities: the same one by order
                for (auto [e, o] : c.W.Registry.view<OrderComponent>().each())
                    if (o.Value == order) live = e;
                if (live == entt::null) return std::string("the moved object is gone after the full reload");
                SA::MoveOneMetre(c.E, c.W, live);
                const int inPlace0 = SA::InPlaceSteps(c.E);
                double job = 0.0;
                std::string mismatch;
                const double undo1 = SA::TimedHistoryStep(c.E, c.W, c.A, false, &job, &mismatch);
                const float afterUndo = xNow();
                const double redo = SA::TimedHistoryStep(c.E, c.W, c.A, true, nullptr, &mismatch);
                const float afterRedo = xNow();
                const double undo2 = SA::TimedHistoryStep(c.E, c.W, c.A, false, nullptr, &mismatch);
                const float afterUndo2 = xNow();
                Log::Info("UNDO_TIMING " + label + " undo=" + std::to_string(undo1) + "ms redo=" + std::to_string(redo) +
                          "ms undo2=" + std::to_string(undo2) + "ms inplace=" + std::to_string(SA::InPlaceSteps(c.E) - inPlace0) +
                          "/3 stepjob=" + std::to_string(job) + "ms");
                if (!mismatch.empty()) return mismatch;
                if (SA::InPlaceSteps(c.E) - inPlace0 != 3) return std::string("a 1 m move didn't undo / redo in place");
                if (afterUndo != x0 || afterRedo != x0 + 1.0f || afterUndo2 != x0)
                    return "x went " + std::to_string(x0) + " -> undo " + std::to_string(afterUndo) + " -> redo " +
                           std::to_string(afterRedo) + " -> undo " + std::to_string(afterUndo2);
                const std::string steps = SA::EditSteps(c.E);
                return steps.empty() ? std::string() : "undo steps left: " + steps; // no "Unrecorded Change"
            }),
            Do("Scene tab in front", [](Ctx& c) { c.E.RequestSceneTabFocus(); }),
            FreshScene(), Wait(3), // back on the scratch scene: nothing can save over the real one
        });
    }
}
