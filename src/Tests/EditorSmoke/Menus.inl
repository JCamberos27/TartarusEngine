// Editor smoke suite: Menus. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The top menu bar (File / Edit / Create / View / Window / Help, drawn by EditorModuleToolbar.cpp
// into "##Toolbar"; the bodies are EditorLayer::Draw*MenuBody in EditorLayer_Toolbar.cpp). Every
// menu is opened by clicking it, shot, its items checked, and every item that is safe unattended
// clicked. Items that open a native dialog, a browser or Explorer, rebuild the modules or quit
// (Open..., Save As..., Import > Model/Texture/Sound..., Rebuild..., Exit, Documentation, Open Log
// Folder, Report a Bug...) are only checked to be there and enabled.
//
// This file also holds the helpers the Panels and Settings suites share (namespace ChromeUi): it
// is included before both.

} // namespace
#include "HotReloadEditorModule.h" // EditorModuleHost::ConsoleState()
#include "EditorModuleAPI.h"       // EditorConsoleState
#include "EditorIcons.h"           // EDITOR_ICON_* (action bar buttons)

// Editor internals the menus / panels / settings suites read and restore.
struct EditorSmokeAccess_Menus {
    static bool& ShowGizmos(EditorLayer& e) { return e.m_ShowGizmos; }
    static bool& ShowGrid(EditorLayer& e) { return e.m_ShowGrid; }
    static bool& MeasureTool(EditorLayer& e) { return e.m_MeasureTool; }
    static bool& FrameOnSelect(EditorLayer& e) { return e.m_FrameOnSelect; }
    static bool& ShowHierarchy(EditorLayer& e) { return e.m_ShowHierarchy; }
    static bool& ShowInspector(EditorLayer& e) { return e.m_ShowInspector; }
    static bool& ShowAssets(EditorLayer& e) { return e.m_ShowAssetBrowser; }
    static bool& ShowAnimator(EditorLayer& e) { return e.m_ShowAnimator; }
    static bool& ShowLighting(EditorLayer& e) { return e.m_ShowLighting; }
    static bool& ShowAudio(EditorLayer& e) { return e.m_ShowAudioDebug; }
    static bool& ShowHistory(EditorLayer& e) { return e.m_ShowHistory; }
    static void RequestMaximizeToggle(EditorLayer& e) { e.m_MaximizeToggleRequested = true; }
    static bool& ShowSettings(EditorLayer& e) { return e.m_ShowPreferences; }
    static bool& SettingsIsProject(EditorLayer& e) { return e.m_SettingsGroupIsProject; }
    static int& PrefsCategory(EditorLayer& e) { return e.m_PrefsCategory; }
    static int& ProjectCategory(EditorLayer& e) { return e.m_ProjSettingsCategory; }
    static void ClearSettingsSearch(EditorLayer& e) { e.m_SettingsSearch[0] = '\0'; }
    static std::string SettingsSearch(EditorLayer& e) { return e.m_SettingsSearch; }
    static std::string& ShortcutFilter(EditorLayer& e) { return e.m_PrefsShortcutFilter; }
    static std::string& CapturingShortcut(EditorLayer& e) { return e.m_PrefsCapturingId; }
    static bool GameOpen(EditorLayer& e) { return e.m_GameViewOpenCached; }
    static bool Dirty(EditorLayer& e) { return e.m_Dirty; }
    static size_t RedoDepth(EditorLayer& e) { return e.m_RedoStack.size(); }
    static bool HandTool(EditorLayer& e) { return e.m_HandTool; }
    static bool LayoutPending(EditorLayer& e) { return e.m_ResetLayoutRequested || !e.m_PendingLayoutIni.empty(); }
    static bool& ShowSaveLayout(EditorLayer& e) { return e.m_ShowSaveLayout; }
    static size_t SoloCount(EditorLayer& e) { return e.m_SoloLights.size(); }
    static void ClearSoloMute(EditorLayer& e) { e.m_SoloLights.clear(); e.m_MutedLights.clear(); }
    static bool RevertPrompt(EditorLayer& e) { return e.m_RevertPromptPending; }
    static bool CanBack(EditorLayer& e) { return e.CanSelectionHistoryBack(); }
};

namespace {
namespace ChromeUi {
using A = EditorSmokeAccess_Menus;
using Steps = std::vector<Step>;

Steps Seq(std::initializer_list<Steps> parts) {
    Steps out;
    for (const Steps& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// --- Windows -------------------------------------------------------------------------------------
// Read in BeforeFrame, before NewFrame: Active = the window was begun last frame.
ImGuiWindow* Win(const char* name) { return ImGui::FindWindowByName(name); }
bool Open(const char* name) { ImGuiWindow* w = Win(name); return w && w->Active; }
bool Shown(const char* name) { ImGuiWindow* w = Win(name); return w && w->Active && !w->Hidden; }
bool Gone(const char* name) { return !Open(name); }

// A docked window's tab in its dock node's tab bar.
Query TabOf(const char* name) {
    Query q;
    q.Pred = [name](const UiItem& it) { ImGuiWindow* w = Win(name); return w && it.Id == w->TabId; };
    return q;
}

// Clicks a window's own close button: the x on its title bar, or on its dock tab (shown while the
// tab is selected or hovered - the tab is hovered first when it isn't there yet).
Step CloseByX(const char* name) {
    auto phase = std::make_shared<int>(0);
    return {std::string("close ") + name + " with its x", [name, phase](Ctx&) {
        ImGuiIO& io = ImGui::GetIO();
        if (*phase >= 2) {
            io.AddMouseButtonEvent(0, *phase == 2);
            return ++*phase > 3;
        }
        ImGuiWindow* w = Win(name);
        if (!w) return false;
        const ImGuiID closeId = ImHashStr("#CLOSE", 0, w->ID);
        const UiItem* close = nullptr;
        const UiItem* tab = nullptr;
        for (const UiItem& it : g_Reg.Last) {
            if (it.Id == closeId) close = &it;
            else if (it.Id == w->TabId) tab = &it;
        }
        ImVec2 p;
        if (close && ClickPoint(*close, p)) { MouseTo(p); *phase = 2; return false; }
        if (tab && ClickPoint(*tab, p)) MouseTo(p);
        return false;
    }, 180};
}

// Clicks a docked window's tab unless the window is already the visible one.
Step BringToFront(const char* name) {
    Step click = Click(TabOf(name));
    auto skip = std::make_shared<int>(-1);
    click.Name = std::string("bring ") + name + " to the front";
    click.Fn = [inner = click.Fn, name, skip](Ctx& c) {
        if (*skip < 0) *skip = Shown(name) ? 1 : 0;
        return *skip == 1 || inner(c);
    };
    return click;
}

Step UntilShown(const char* name, int timeout = 180) {
    return Until(std::string(name) + " is shown", [name](Ctx&) { return Shown(name); }, timeout);
}
Step UntilGone(const char* name, int timeout = 180) {
    return Until(std::string(name) + " is gone", [name](Ctx&) { return Gone(name); }, timeout);
}

// --- Menus ---------------------------------------------------------------------------------------
Query TopMenu(const char* name) { return ByExact(name, "##Toolbar"); }
bool InMenu(const UiItem& it) { return Contains(it.Root, "##Menu") || Contains(it.Root, "##Popup"); }
// An item of an open menu whose raw label is exactly `raw` (labels that are a substring of another).
Query MenuExact(std::string raw) {
    Query q = ByExact(std::move(raw));
    q.Pred = InMenu;
    return q;
}
bool ItemEnabled(const Query& q) {
    const UiItem* it = Find(q);
    return it && !(it->ItemFlags & ImGuiItemFlags_Disabled);
}

// Opens a top-level menu by clicking it (closing whatever menu or popup is still open first), and
// waits until `firstItem` is listed in it.
Steps OpenMenu(const char* menu, const char* firstItem) {
    return {
        Do("close open menus", [](Ctx&) { ImGui::ClosePopupsExceptModals(); }), Wait(2),
        Click(TopMenu(menu)),
        Until(std::string(menu) + " menu open", [firstItem](Ctx&) { return Find(MenuItem(firstItem)) != nullptr; }),
        Wait(1),
    };
}
// Opens `menu` and clicks `item` (a MenuItem query).
Steps MenuClick(const char* menu, const char* firstItem, Query item) {
    Steps s = OpenMenu(menu, firstItem);
    s.push_back(Click(std::move(item)));
    s.push_back(Wait(3));
    return s;
}
Steps MenuClick(const char* menu, const char* firstItem, const char* item) { return MenuClick(menu, firstItem, MenuItem(item)); }

// Every label is listed by the open menu (and, for `enabled`, can be clicked).
Step ExpectListed(std::string what, std::vector<std::string> labels, bool enabled = false) {
    return Expect(std::move(what), [labels, enabled](Ctx&) {
        std::string missing;
        for (const std::string& l : labels) {
            const UiItem* it = Find(MenuItem(l.c_str()));
            if (!it) missing += " '" + l + "' missing;";
            else if (enabled && (it->ItemFlags & ImGuiItemFlags_Disabled)) missing += " '" + l + "' disabled;";
        }
        return missing;
    });
}
Step CloseMenus() { return Do("close menus", [](Ctx&) { ImGui::ClosePopupsExceptModals(); }); }

// --- Scene ---------------------------------------------------------------------------------------
int EntityCount(World& w) {
    int n = 0;
    for (auto e : w.Registry.view<const NameComponent>()) { (void)e; ++n; }
    return n;
}
bool CameraMoved(Ctx& c) { return glm::length(c.Cam.Position - g_Vec) > 1e-3f; }
Step NoteCamera() { return Do("note the camera", [](Ctx& c) { g_Vec = c.Cam.Position; }); }
// The editor camera back to a plain perspective view of the fixture.
Step ResetCamera() {
    return Do("reset the camera", [](Ctx& c) {
        c.Cam.Orthographic = false;
        c.Cam.Position = glm::vec3(0.0f, 2.0f, 10.0f);
        c.Cam.Yaw = -90.0f;
        c.Cam.Pitch = -10.0f;
    });
}

// --- Files ---------------------------------------------------------------------------------------
std::string ReadText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}
nlohmann::json ReadJson(const std::string& path) {
    try {
        return nlohmann::json::parse(ReadText(path));
    } catch (...) {
        return nlohmann::json();
    }
}
nlohmann::json Prefs() { return ReadJson(EditorSettings::PrefsFilePath()); }
// editor_prefs.json holds `key` == `value` (it is written once per frame, after the change).
template <class T>
Step UntilPref(const char* key, T value) {
    return Until(std::string("editor_prefs.json ") + key, [key, value](Ctx&) {
        const nlohmann::json j = Prefs();
        auto it = j.find(key);
        if (it == j.end()) return false;
        try { return it->get<T>() == value; } catch (...) { return false; }
    }, 120);
}
// The same for a bool whose expected value is only known when the step runs.
Step UntilPrefBool(const char* key, std::function<bool()> want) {
    return Until(std::string("editor_prefs.json ") + key, [key, want](Ctx&) {
        const nlohmann::json j = Prefs();
        auto it = j.find(key);
        return it != j.end() && it->is_boolean() && it->get<bool>() == want();
    }, 120);
}
bool Noted() { return g_Number != 0.0; } // a bool a test noted in g_Number
} // namespace ChromeUi

void RegisterMenusTests() {
    using namespace ChromeUi;

    // ---------------------------------------------------------------------------------- File
    Add("menus: File lists its items; Import's submenu too", Seq({
        {FreshScene(), Wait(2)},
        OpenMenu("File", "New Scene"),
        {Shot("file", {"##Menu_00"}),
         ExpectListed("File items", {"New Scene", "Open...", "Open Recent", "Save", "Save As...", "Revert Scene", "Import",
                                     "Rebuild Game & Editor Modules", "Exit"}),
         ExpectListed("enabled", {"New Scene", "Open...", "Save As...", "Revert Scene", "Import", "Exit"}, true),
         ExpectTrue("Save enabled", [](Ctx&) { return ItemEnabled(MenuExact(ICON_FA_FLOPPY_DISK "  Save")); }),
         Click(MenuItem("Import")),
         Until("Import submenu open", [](Ctx&) { return Find(MenuItem("Model...")) != nullptr; }), Wait(1),
         Shot("import", {"##Menu_01"}),
         // Each opens a native file dialog: listed, never clicked.
         ExpectListed("Import items", {"Model...", "Texture...", "Sound..."}, true),
         CloseMenus(), Wait(2)},
    }));

    Add("menus: File > Save writes the scratch scene; Revert Scene reloads it", Seq({
        {FreshScene(),
         Do("no scratch file yet", [](Ctx&) { std::error_code ec; std::filesystem::remove(g_ScratchScene, ec); }),
         Do("mark the scene edited", [](Ctx& c) { X::PushUndo(c.E, c.W, "Edit"); c.W.Registry.get<NameComponent>(ByName(c.W, "Gamma")).Name = "Gamma Saved"; }),
         Wait(3)},
        MenuClick("File", "New Scene", MenuExact(ICON_FA_FLOPPY_DISK "  Save")),
        {Until("saved", [](Ctx& c) { return std::filesystem::exists(g_ScratchScene) && !A::Dirty(c.E); }),
         Expect("the file holds the scene", [](Ctx&) {
             const std::string s = ReadText(g_ScratchScene);
             return Contains(s, "Gamma Saved") && Contains(s, "Alpha") ? std::string() : std::string("Gamma Saved / Alpha not in the file");
         })},
        {Do("edit after the save", [](Ctx& c) { X::PushUndo(c.E, c.W, "Rename"); c.W.Registry.get<NameComponent>(ByName(c.W, "Alpha")).Name = "Alpha Unsaved"; }),
         Until("dirty", [](Ctx& c) { return A::Dirty(c.E); })},
        MenuClick("File", "New Scene", "Revert Scene"),
        {Until("the revert prompt", [](Ctx& c) { return A::RevertPrompt(c.E); }),
         Shot("revert-prompt", {"Revert Scene?"}),
         Click(ByExact("Revert", "Revert Scene?")),
         Until("reloaded from disk", [](Ctx& c) { return ByName(c.W, "Alpha") != entt::null && ByName(c.W, "Alpha Unsaved") == entt::null; }),
         ExpectTrue("the saved edit is back", [](Ctx& c) { return ByName(c.W, "Gamma Saved") != entt::null; }),
         ExpectTrue("not dirty after the reload", [](Ctx& c) { return !A::Dirty(c.E); })},
    }));

    Add("menus: File > New Scene starts an empty scene file", Seq({
        {FreshScene(), Wait(2), Do("note the scene", [](Ctx& c) { g_Text = X::ScenePath(c.E); })},
        MenuClick("File", "New Scene", "New Scene"),
        {Until("a new scene", [](Ctx& c) { return X::ScenePath(c.E) != g_Text; }),
         Expect("an empty Untitled scene", [](Ctx& c) {
             const std::string p = X::ScenePath(c.E);
             if (!Contains(std::filesystem::path(p).filename().string(), "Untitled")) return "path is " + p;
             if (!std::filesystem::exists(p)) return "no file at " + p;
             return EntityCount(c.W) == 0 ? std::string() : std::to_string(EntityCount(c.W)) + " objects in the new scene";
         }),
         ExpectTrue("not dirty", [](Ctx& c) { return !A::Dirty(c.E); }),
         Shot("new-scene", {"###Hierarchy", "###Scene"}),
         Do("note the file", [](Ctx& c) { g_Text = X::ScenePath(c.E); }),
         FreshScene(), Wait(10),
         // New Scene writes project/scenes/Untitled*.json: remove it (and any .meta the scan made).
         Do("delete the new scene file", [](Ctx&) {
             std::error_code ec;
             if (Contains(g_Text, "Untitled")) {
                 std::filesystem::remove(g_Text, ec);
                 std::filesystem::remove(g_Text + ".meta", ec);
             }
         })},
    }));

    // ---------------------------------------------------------------------------------- Edit
    Add("menus: Edit menu - Duplicate, Undo, Redo, Copy, Paste, Delete, Select All", Seq({
        {FreshScene(), Wait(2), SelectByName("Beta"), Wait(3)},
        OpenMenu("Edit", "Undo"),
        {Shot("edit", {"##Menu_00"}),
         ExpectListed("Edit items", {"Undo", "Redo", "Copy", "Paste", "Duplicate", "Delete", "Select All", "Preferences...", "Project Settings..."}),
         ExpectListed("enabled with a selection", {"Copy", "Paste", "Duplicate", "Delete", "Select All", "Preferences...", "Project Settings..."}, true),
         CloseMenus(), Wait(2)},
        MenuClick("Edit", "Undo", "Duplicate"),
        {Until("duplicated", [](Ctx& c) { return EntityCount(c.W) == 4; }),
         Expect("the copy is selected", [](Ctx& c) {
             const std::string n = NameOf(c.W, X::Selected(c.E));
             return Contains(n, "Beta") && X::Selected(c.E) != ByName(c.W, "Beta") ? std::string() : "selected " + n;
         })},
        MenuClick("Edit", "Undo", "Undo"),
        {Until("undo removed the copy", [](Ctx& c) { return EntityCount(c.W) == 3; })},
        MenuClick("Edit", "Undo", "Redo"),
        {Until("redo brought it back", [](Ctx& c) { return EntityCount(c.W) == 4; })},
        MenuClick("Edit", "Undo", "Undo"),
        {Until("undone again", [](Ctx& c) { return EntityCount(c.W) == 3; }),
         SelectByName("Gamma"), Wait(3)},
        MenuClick("Edit", "Undo", "Copy"),
        MenuClick("Edit", "Undo", "Paste"),
        {Until("pasted", [](Ctx& c) { return EntityCount(c.W) == 4; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl),
         Until("paste undone", [](Ctx& c) { return EntityCount(c.W) == 3; }),
         SelectByName("Alpha"), Wait(3)},
        MenuClick("Edit", "Undo", "Delete"),
        {Until("Alpha deleted", [](Ctx& c) { return ByName(c.W, "Alpha") == entt::null; })},
        MenuClick("Edit", "Undo", "Undo"),
        {Until("Alpha restored", [](Ctx& c) { return ByName(c.W, "Alpha") != entt::null; })},
        MenuClick("Edit", "Undo", "Select All"),
        {Until("everything selected", [](Ctx& c) { return X::Selection(c.E).size() == 3; })},
    }));

    Add("menus: Edit > Preferences... and Project Settings... open the Settings window", Seq({
        {FreshScene(), Wait(2)},
        MenuClick("Edit", "Undo", "Preferences..."),
        {UntilShown(EditorPanels::Settings),
         ExpectTrue("the THIS MACHINE group", [](Ctx& c) { return !A::SettingsIsProject(c.E); }),
         Shot("preferences", {"###Settings"})},
        MenuClick("Edit", "Undo", "Project Settings..."),
        {Until("the THIS PROJECT group", [](Ctx& c) { return A::SettingsIsProject(c.E); }),
         Shot("project-settings", {"###Settings"}),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings),
         ExpectTrue("closed", [](Ctx& c) { return !A::ShowSettings(c.E); })},
    }));

    // ---------------------------------------------------------------------------------- Create
    {
        struct Kind { const char* Item; const char* Name; int Check; }; // Check: 0 model, 1 empty, 2 light, 3 player, 4 camera, 5 separator
        static const Kind kKinds[] = {
            {"Cube", "Cube", 0}, {"Sphere", "Sphere", 0}, {"Cylinder", "Cylinder", 0}, {"Capsule", "Capsule", 0},
            {"Cone", "Cone", 0}, {"Pyramid", "Pyramid", 0}, {"Donut", "Donut", 0}, {"Plane", "Plane", 0},
            {"Empty", "Empty", 1}, {"Separator", "Separator", 5}, {"Point Light", "Point Light", 2},
            {"Spot Light", "Spot Light", 2}, {"Directional Light", "Directional Light", 2},
            {"First Person Player", "Player Spawn", 3}, {"Camera", "Camera", 4},
        };
        Steps s = Seq({{FreshScene(), Wait(2)}, OpenMenu("Create", "Cube")});
        s.push_back(Shot("create", {"##Menu_00"}));
        {
            std::vector<std::string> labels;
            for (const Kind& k : kKinds) labels.push_back(k.Item);
            s.push_back(ExpectListed("every object type", labels, true));
        }
        s.push_back(CloseMenus());
        for (const Kind& k : kKinds) {
            const Kind kind = k;
            Query item = MenuItem(kind.Item);
            if (std::string(kind.Item) == "Empty") item = MenuExact(ICON_FA_DIAGRAM_PROJECT "  Empty");
            for (Step& st : MenuClick("Create", "Cube", item)) s.push_back(st);
            s.push_back(Until(std::string(kind.Name) + " created and selected", [kind](Ctx& c) {
                const entt::entity e = ByName(c.W, kind.Name);
                return e != entt::null && X::Selected(c.E) == e;
            }));
            s.push_back(Expect(std::string(kind.Name) + " has its components", [kind](Ctx& c) {
                const entt::entity e = ByName(c.W, kind.Name);
                auto& r = c.W.Registry;
                switch (kind.Check) {
                    case 0: return r.all_of<ColliderComponent>(e) ? std::string() : std::string("no collider on the primitive");
                    case 2: {
                        const auto* l = r.try_get<LightComponent>(e);
                        if (!l) return std::string("no light");
                        if (std::string(kind.Name) == "Spot Light" && l->Kind != LightComponent::Type::Spot) return std::string("not a spot light");
                        if (std::string(kind.Name) == "Directional Light" && l->Kind != LightComponent::Type::Directional) return std::string("not directional");
                        return std::string();
                    }
                    case 3: return r.all_of<FirstPersonControllerComponent, FirstPersonBodyComponent>(e) ? std::string() : std::string("no controller / body");
                    case 4: return r.all_of<CameraComponent>(e) ? std::string() : std::string("no camera component");
                    case 5: {
                        const auto* style = r.try_get<HierarchyStyleComponent>(e);
                        return style && style->Separator ? std::string() : std::string("not a separator row");
                    }
                    default: return EntityCount(c.W) == 4 ? std::string() : std::string("unexpected object count");
                }
            }));
            s.push_back(Key(ImGuiKey_Z, ImGuiMod_Ctrl));
            s.push_back(Until(std::string("undo removes the ") + kind.Name, [kind](Ctx& c) {
                return ByName(c.W, kind.Name) == entt::null && EntityCount(c.W) == 3;
            }));
        }
        Add("menus: Create adds every object type; each undoes", s);
    }

    // ---------------------------------------------------------------------------------- View
    Add("menus: View lists its items; Frame Selected, Frame All, Selection Back / Forward", Seq({
        {FreshScene(), ResetCamera(), Wait(2), SelectByName("Alpha"), Wait(3), SelectByName("Gamma"), Wait(3)},
        OpenMenu("View", "Frame Selected"),
        {Shot("view", {"##Menu_00"}),
         ExpectListed("View items", {"Frame Selected", "Frame All", "Selection Back", "Selection Forward", "Transform Gizmo",
                                     "Measure Tool", "Frame on Select", "Mute Audio", "Orthographic"}),
         Expect("snap-to-view items", [](Ctx&) {
             for (const char* v : {"  Iso", "  Front", "  Back", "  Right", "  Left", "  Top", "  Bottom"})
                 if (!Find(MenuExact(v))) return std::string("missing") + v;
             return std::string();
         }),
         ExpectListed("enabled", {"Frame Selected", "Frame All", "Selection Back"}, true),
         CloseMenus(), Wait(2), NoteCamera()},
        MenuClick("View", "Frame Selected", "Frame Selected"),
        {Until("the camera framed Gamma", CameraMoved, 240), Wait(30),
         Do("move the camera away", [](Ctx& c) { c.Cam.Position = glm::vec3(0.0f, 50.0f, 200.0f); g_Vec = c.Cam.Position; })},
        MenuClick("View", "Frame Selected", "Frame All"),
        {Until("the camera framed the scene", CameraMoved, 240), Wait(30)},
        MenuClick("View", "Frame Selected", "Selection Back"),
        {Until("back to Alpha", [](Ctx& c) { return ExpectSelected(c, "Alpha").empty(); })},
        MenuClick("View", "Frame Selected", "Selection Forward"),
        {Until("forward to Gamma", [](Ctx& c) { return ExpectSelected(c, "Gamma").empty(); }),
         ResetCamera()},
    }));

    Add("menus: View toggles - Transform Gizmo, Measure Tool, Frame on Select, Mute Audio", Seq({
        {FreshScene(), ResetCamera(), Wait(2), Do("note", [](Ctx& c) { g_Number = A::ShowGizmos(c.E) ? 1.0 : 0.0; })},
        MenuClick("View", "Frame Selected", "Transform Gizmo"),
        {ExpectTrue("gizmo toggled", [](Ctx& c) { return A::ShowGizmos(c.E) != Noted(); })},
        MenuClick("View", "Frame Selected", "Transform Gizmo"),
        {ExpectTrue("gizmo back", [](Ctx& c) { return A::ShowGizmos(c.E) == Noted(); })},
        MenuClick("View", "Frame Selected", "Measure Tool"),
        {ExpectTrue("measure tool on", [](Ctx& c) { return A::MeasureTool(c.E); }),
         Shot("measure", {"###Scene"})},
        MenuClick("View", "Frame Selected", "Measure Tool"),
        {ExpectTrue("measure tool off", [](Ctx& c) { return !A::MeasureTool(c.E); })},
        MenuClick("View", "Frame Selected", "Frame on Select"),
        {ExpectTrue("frame on select on", [](Ctx& c) { return A::FrameOnSelect(c.E); }),
         UntilPref("viewFrameOnSelect", true),
         NoteCamera(), SelectByName("Gamma"),
         Until("selecting framed Gamma", CameraMoved, 240), Wait(30)},
        MenuClick("View", "Frame Selected", "Frame on Select"),
        {ExpectTrue("frame on select off", [](Ctx& c) { return !A::FrameOnSelect(c.E); }),
         UntilPref("viewFrameOnSelect", false),
         Do("note mute", [](Ctx&) { g_Number = EditorSettings::Get().AudioMuted ? 1.0 : 0.0; })},
        MenuClick("View", "Frame Selected", "Mute Audio"),
        {ExpectTrue("mute toggled", [](Ctx&) { return EditorSettings::Get().AudioMuted != Noted(); }),
         UntilPrefBool("audioMuted", [] { return !Noted(); })},
        MenuClick("View", "Frame Selected", "Mute Audio"),
        {ExpectTrue("mute restored", [](Ctx&) { return EditorSettings::Get().AudioMuted == Noted(); }),
         UntilPrefBool("audioMuted", [] { return Noted(); }),
         ResetCamera()},
    }));

    {
        struct Preset { const char* Raw; float Yaw, Pitch; };
        static const Preset kPresets[] = {
            {"  Iso", -45.0f, -35.264f}, {"  Front", -90.0f, 0.0f}, {"  Back", 90.0f, 0.0f}, {"  Right", 180.0f, 0.0f},
            {"  Left", 0.0f, 0.0f}, {"  Top", -90.0f, -89.9f}, {"  Bottom", -90.0f, 89.9f},
        };
        Steps s = {FreshScene(), ResetCamera(), Wait(2)};
        for (const Preset& p : kPresets) {
            const Preset preset = p;
            for (Step& st : MenuClick("View", "Frame Selected", MenuExact(preset.Raw))) s.push_back(st);
            s.push_back(Until(std::string("camera looks") + preset.Raw, [preset](Ctx& c) {
                const float y = glm::radians(preset.Yaw), pt = glm::radians(preset.Pitch);
                const glm::vec3 want = glm::normalize(glm::vec3(std::cos(y) * std::cos(pt), std::sin(pt), std::sin(y) * std::cos(pt)));
                return glm::dot(c.Cam.Front(), want) > 0.999f && c.Cam.Orthographic;
            }, 240));
            if (std::string(preset.Raw) == "  Front" || std::string(preset.Raw) == "  Top") s.push_back(Shot(preset.Raw + 2, {"###Scene"}));
        }
        for (Step& st : MenuClick("View", "Frame Selected", "Orthographic")) s.push_back(st);
        s.push_back(Until("back to perspective", [](Ctx& c) { return !c.Cam.Orthographic; }, 240));
        s.push_back(Wait(30));
        s.push_back(ResetCamera());
        Add("menus: View > Snap to view presets and Orthographic", s);
    }

    // ---------------------------------------------------------------------------------- Window
    Add("menus: Window lists every panel", Seq({
        {FreshScene(), Wait(2)},
        OpenMenu("Window", "Hierarchy"),
        {Shot("window", {"##Menu_00"}),
         ExpectListed("Window items", {"Scene", "Game", "Hierarchy", "Inspector", "Assets", "Asset Library", "Animator", "Lighting",
                                       "Audio", "Console", "Statistics", "History", "Script IDE", "Physics Debug",
                                       "Physics HUD overlay", "Reset Layout", "Layout Presets"}),
         ExpectTrue("Scene can't be closed (disabled)", [](Ctx&) {
             const UiItem* it = Find(MenuItem("Scene"));
             return it && (it->ItemFlags & ImGuiItemFlags_Disabled);
         }),
         ExpectTrue("no Glass Workspace outside the Glass theme", [](Ctx&) { return Find(MenuItem("Glass Workspace")) == nullptr; }),
         CloseMenus(), Wait(2)},
    }));

    {
        // Core panels: open by default, the menu closes and reopens them.
        struct Core { const char* Item; const char* Window; bool& (*Flag)(EditorLayer&); };
        static const Core kCore[] = {
            {"Hierarchy", EditorPanels::Hierarchy, &A::ShowHierarchy},
            {"Inspector", EditorPanels::Inspector, &A::ShowInspector},
            {"Assets", EditorPanels::Assets, &A::ShowAssets},
        };
        Steps s = {FreshScene(), Wait(2)};
        for (const Core& k : kCore) {
            const Core core = k;
            for (Step& st : MenuClick("Window", "Hierarchy", core.Item)) s.push_back(st);
            s.push_back(Until(std::string(core.Item) + " closed", [core](Ctx& c) { return !core.Flag(c.E) && Gone(core.Window); }));
            for (Step& st : MenuClick("Window", "Hierarchy", core.Item)) s.push_back(st);
            s.push_back(Until(std::string(core.Item) + " back", [core](Ctx& c) { return core.Flag(c.E) && Open(core.Window); }));
            s.push_back(BringToFront(core.Window));
            s.push_back(UntilShown(core.Window));
        }
        // Game: open by default (a tab beside Scene); its toggle round-trips through main.cpp.
        for (Step& st : MenuClick("Window", "Hierarchy", "Game")) s.push_back(st);
        s.push_back(Until("Game closed", [](Ctx& c) { return !A::GameOpen(c.E) && Gone(EditorPanels::Game); }));
        for (Step& st : MenuClick("Window", "Hierarchy", "Game")) s.push_back(st);
        s.push_back(Until("Game back", [](Ctx& c) { return A::GameOpen(c.E) && Open(EditorPanels::Game); }));
        s.push_back(BringToFront(EditorPanels::Scene));
        s.push_back(UntilShown(EditorPanels::Scene));
        Add("menus: Window closes and reopens Hierarchy, Inspector, Assets and Game", s);
    }

    {
        // Optional panels: hidden by default, the menu shows and hides them.
        struct Opt { const char* Item; const char* Window; };
        static const Opt kOpt[] = {
            {"Asset Library", EditorPanels::AssetLibrary}, {"Animator", EditorPanels::Animator},
            {"Lighting", EditorPanels::Lighting}, {"Audio", EditorPanels::Audio}, {"Console", EditorPanels::Console},
            {"Statistics", EditorPanels::Statistics}, {"History", EditorPanels::History},
            {"Physics Debug", EditorPanels::PhysicsDebug},
        };
        Steps s = {FreshScene(), Wait(2)};
        for (const Opt& o : kOpt) {
            const Opt opt = o;
            for (Step& st : MenuClick("Window", "Hierarchy", MenuItem(opt.Item))) s.push_back(st);
            s.push_back(Until(std::string(opt.Item) + " opened", [opt](Ctx&) { return Open(opt.Window); }));
            s.push_back(BringToFront(opt.Window));
            s.push_back(UntilShown(opt.Window));
            s.push_back(Shot(opt.Item, {opt.Window}));
            for (Step& st : MenuClick("Window", "Hierarchy", MenuItem(opt.Item))) s.push_back(st);
            s.push_back(UntilGone(opt.Window));
        }
        s.push_back(Do("panels back to their defaults", [](Ctx&) {
            EditorSettings::Get().SceneShowStats = false;
            EditorSettings::Get().ShowPhysicsPanel = false;
            EditorSettings::Get().ShowAssetLibrary = false;
            EditorSettings::Save();
        }));
        Add("menus: Window opens and closes every optional panel", s);
    }

    Add("menus: Window > Script IDE and Physics HUD overlay", Seq({
        {FreshScene(), Wait(2)},
        MenuClick("Window", "Hierarchy", "Script IDE"),
        {UntilShown("Script IDE"), Wait(3),
         Shot("script-ide", {"Script IDE"}),
         CloseByX("Script IDE"), UntilGone("Script IDE"),
         BringToFront(EditorPanels::Scene), UntilShown(EditorPanels::Scene),
         Do("note", [](Ctx&) { g_Number = EditorSettings::Get().PhysicsHudOverlay ? 1.0 : 0.0; })},
        MenuClick("Window", "Hierarchy", "Physics HUD overlay"),
        {ExpectTrue("overlay toggled", [](Ctx&) { return EditorSettings::Get().PhysicsHudOverlay != Noted(); }),
         UntilPrefBool("physicsHudOverlay", [] { return !Noted(); })},
        MenuClick("Window", "Hierarchy", "Physics HUD overlay"),
        {ExpectTrue("overlay restored", [](Ctx&) { return EditorSettings::Get().PhysicsHudOverlay == Noted(); }),
         UntilPrefBool("physicsHudOverlay", [] { return Noted(); })},
    }));

    {
        // Window > Layout Presets > Default Layouts > <name>.
        auto preset = [](const char* name) {
            Steps s = OpenMenu("Window", "Hierarchy");
            s.push_back(Click(MenuItem("Layout Presets")));
            s.push_back(Until("Layout Presets open", [](Ctx&) { return Find(MenuItem("Default Layouts")) != nullptr; }));
            s.push_back(Click(MenuItem("Default Layouts")));
            s.push_back(Until("Default Layouts open", [](Ctx&) { return Find(MenuExact("Focus")) != nullptr; }));
            s.push_back(Click(MenuExact(name)));
            s.push_back(Until(std::string(name) + " layout applied", [](Ctx& c) { return !A::LayoutPending(c.E); }));
            s.push_back(Wait(8));
            return s;
        };
        // The Hierarchy's share of the window's width / the Assets panel's share of its height.
        auto widthOf = [](const char* name) -> double {
            ImGuiWindow* w = Win(name);
            const float total = ImGui::GetMainViewport()->Size.x;
            return w && total > 0.0f ? (double)(w->Size.x / total) : 0.0;
        };
        auto heightOf = [](const char* name) -> double {
            ImGuiWindow* w = Win(name);
            const float total = ImGui::GetMainViewport()->Size.y;
            return w && total > 0.0f ? (double)(w->Size.y / total) : 0.0;
        };
        static double s_DefaultW = 0.0, s_DefaultH = 0.0, s_WideW = 0.0;
        Add("menus: Window > Layout Presets: Default, Wide, Tall, Focus, Reset Layout", Seq({
            {FreshScene(), Wait(2)},
            preset("Default"),
            {UntilShown(EditorPanels::Hierarchy), UntilShown(EditorPanels::Inspector),
             Do("measure Default", [widthOf, heightOf](Ctx&) { s_DefaultW = widthOf(EditorPanels::Hierarchy); s_DefaultH = heightOf(EditorPanels::Assets); }),
             Shot("default", {"###Scene", "###Hierarchy", "###Inspector", "###Assets"})},
            preset("Wide"),
            {UntilShown(EditorPanels::Hierarchy),
             Expect("Wide: narrower side panels", [widthOf](Ctx&) {
                 s_WideW = widthOf(EditorPanels::Hierarchy);
                 return s_WideW > 0.0 && s_WideW < s_DefaultW - 0.01 ? std::string()
                     : "Hierarchy " + std::to_string(s_WideW) + " vs default " + std::to_string(s_DefaultW);
             }),
             Shot("wide", {"###Scene", "###Hierarchy"})},
            preset("Tall"),
            {UntilShown(EditorPanels::Assets),
             Expect("Tall: a taller bottom strip", [heightOf](Ctx&) {
                 const double h = heightOf(EditorPanels::Assets);
                 return h > s_DefaultH + 0.02 ? std::string() : "Assets " + std::to_string(h) + " vs default " + std::to_string(s_DefaultH);
             }),
             Shot("tall", {"###Scene", "###Assets"})},
            preset("Focus"),
            {Until("Focus hides Hierarchy and Inspector", [](Ctx& c) {
                 return !A::ShowHierarchy(c.E) && !A::ShowInspector(c.E) && Gone(EditorPanels::Hierarchy) && Gone(EditorPanels::Inspector);
             }),
             Shot("focus", {"###Scene"})},
            MenuClick("Window", "Hierarchy", "Reset Layout"),
            {Until("Reset Layout restores the panels", [](Ctx& c) {
                 return !A::LayoutPending(c.E) && Shown(EditorPanels::Hierarchy) && Shown(EditorPanels::Inspector) && Open(EditorPanels::Assets);
             }),
             Wait(5),
             Expect("default proportions again", [widthOf](Ctx&) {
                 const double w = widthOf(EditorPanels::Hierarchy);
                 return std::fabs(w - s_DefaultW) < 0.02 ? std::string() : "Hierarchy " + std::to_string(w) + " vs " + std::to_string(s_DefaultW);
             })},
        }));
    }

    {
        // Window > Layout Presets > Save Current Layout..., load it, delete it (two clicks).
        auto openPresets = []() {
            Steps s = OpenMenu("Window", "Hierarchy");
            s.push_back(Click(MenuItem("Layout Presets")));
            s.push_back(Until("Layout Presets open", [](Ctx&) { return Find(MenuItem("Save Current Layout")) != nullptr; }));
            s.push_back(Wait(1));
            return s;
        };
        static const std::string kFile = "Smoke Layout.ini";
        auto presetPath = []() { return (std::filesystem::path(UserPaths::Resolve("layouts")) / kFile).string(); };
        Add("menus: Window > Layout Presets: save, load and delete a preset", Seq({
            {FreshScene(), Wait(2), Do("no preset yet", [presetPath](Ctx&) { std::error_code ec; std::filesystem::remove(presetPath(), ec); })},
            openPresets(),
            {Click(MenuItem("Save Current Layout")),
             Until("the name prompt", [](Ctx& c) { return A::ShowSaveLayout(c.E) && Find(ByExact("##layoutname")) != nullptr; }), Wait(2),
             Shot("save-layout", {"Save Layout"}),
             TypeText("Smoke Layout"), Wait(2),
             Click(ByExact("Save", "Save Layout")),
             Until("saved", [presetPath](Ctx& c) { return !A::ShowSaveLayout(c.E) && std::filesystem::exists(presetPath()); })},
            openPresets(),
            {ExpectTrue("listed", [](Ctx&) { return Find(MenuExact("Smoke Layout")) != nullptr; }),
             Shot("presets", {"##Menu_01"}),
             Click(MenuExact("Smoke Layout")),
             Until("loaded", [](Ctx& c) { return !A::LayoutPending(c.E); }), Wait(5),
             UntilShown(EditorPanels::Hierarchy)},
            openPresets(),
            {Click(ByExact(ICON_FA_TRASH, "##Menu")),
             Until("armed", [](Ctx&) { return Find(ByLabel("Delete?", "##Menu")) != nullptr; }),
             ExpectTrue("not deleted on the first click", [presetPath](Ctx&) { return std::filesystem::exists(presetPath()); }),
             Click(ByLabel("Delete?", "##Menu")),
             Until("deleted", [presetPath](Ctx&) { return !std::filesystem::exists(presetPath()); }),
             CloseMenus(), Wait(2)},
        }));
    }

    // ---------------------------------------------------------------------------------- Help
    Add("menus: Help lists its items; Shortcuts and About open the Settings window", Seq({
        {FreshScene(), Wait(2)},
        OpenMenu("Help", "Shortcuts"),
        {Shot("help", {"##Menu_00"}),
         // Documentation / Open Log Folder / Report a Bug open the browser or Explorer: listed only.
         ExpectListed("Help items", {"Shortcuts", "Documentation", "Open Log Folder", "Report a Bug...", "About Tartarus Engine"}, true),
         CloseMenus(), Wait(2)},
        MenuClick("Help", "Shortcuts", "Shortcuts"),
        {UntilShown(EditorPanels::Settings),
         ExpectTrue("on Shortcuts", [](Ctx& c) { return !A::SettingsIsProject(c.E) && A::PrefsCategory(c.E) == 5; }),
         Shot("shortcuts", {"###Settings"})},
        MenuClick("Help", "Shortcuts", "About Tartarus Engine"),
        {Until("on About", [](Ctx& c) { return !A::SettingsIsProject(c.E) && A::PrefsCategory(c.E) == 8; }),
         Wait(2),
         Shot("about", {"###Settings"}),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));
}
