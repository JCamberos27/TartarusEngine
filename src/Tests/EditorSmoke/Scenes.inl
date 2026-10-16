// Editor smoke suite: Scenes. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// Every scene under project/scenes/ (and project/scenes/OutfitTest/) is opened through the
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
};
namespace {
namespace smoke_scenes {
namespace fs = std::filesystem;
using SA = EditorSmokeAccess_Scenes;

Step UntilSeconds(std::string name, std::function<bool(Ctx&)> pred, double seconds) {
    auto start = std::make_shared<double>(-1.0);
    return {name, [name, pred, seconds, start](Ctx& c) {
        const double now = ImGui::GetTime();
        if (*start < 0.0) *start = now;
        if (pred(c)) return true;
        if (now - *start > seconds) {
            Fail("step '" + name + "' timed out after " + std::to_string((int)seconds) + " s");
            return true;
        }
        return false;
    }, 100000000};
}
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
bool GameShown() {
    ImGuiWindow* w = ImGui::FindWindowByName(EditorPanels::Game);
    return w && w->WasActive && !w->Hidden;
}
} // namespace smoke_scenes

void RegisterScenesTests() {
    using namespace smoke_scenes;

    // Found when the tests are registered (the first editor frame), in a stable order.
    std::vector<fs::path> scenes;
    for (const char* dir : {"scenes", "scenes/OutfitTest"}) {
        std::vector<fs::path> here;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(fs::path(ProjectPaths::Resolve(dir)), ec)) {
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
        std::string label = fs::path(ProjectPaths::Relativize(scene.string())).generic_string();
        if (label.rfind("scenes/", 0) == 0) label = label.substr(7);
        if (label.size() > 5 && label.compare(label.size() - 5, 5, ".json") == 0) label.resize(label.size() - 5);

        Add("scenes: " + label + " opens, settles, inspects and plays", {
            // Allowed first: the previous scene's async loads can still log while FreshScene runs.
            Do("allow errors naming the external files missing here", [](Ctx&) {
                for (const std::string& name : MissingExternalFiles()) g_AllowLog.push_back(name);
                // Logged without a file name when the soldier's (external) body meshes are missing.
                if (!MissingExternalFiles().empty())
                    g_AllowLog.push_back("Enemy AI: the soldier's body has no animated skeleton");
            }),
            FreshScene(), Wait(2),
            Do("Scene tab in front", [](Ctx& c) { c.E.RequestSceneTabFocus(); }), Wait(2),
            Do("open " + label, [path](Ctx& c) { SA::Open(c.E, c.W, c.A, path); }),
            Expect("it is the open scene", [path](Ctx& c) {
                return X::ScenePath(c.E) == path ? std::string() : "open scene is '" + X::ScenePath(c.E) + "'";
            }),
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
            UntilSeconds("the Game view is shown", [](Ctx&) { return GameShown(); }, 10),
            Shot("game", {"###Game"}),
            Do("stop", [](Ctx& c) { X::RequestPlayStop(c.E); }),
            UntilSeconds("stopped", [](Ctx& c) { return !X::InPlay(c.E); }, 60),
            Wait(3),
            Expect("still the same scene after Stop", [path](Ctx& c) {
                return X::ScenePath(c.E) == path ? std::string() : "open scene is '" + X::ScenePath(c.E) + "'";
            }),
            Do("Scene tab in front", [](Ctx& c) { c.E.RequestSceneTabFocus(); }),
            FreshScene(), Wait(3), // back on the scratch scene: nothing can save over the real one
        });
    }
}
