// Editor smoke suite: Settings. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The Settings window (EditorLayer::DrawSettingsWindow, EditorLayer.cpp): every category of both
// groups is opened from its sidebar entry and shot, and one setting on each is changed through
// its widget, checked to persist, and changed back. THIS MACHINE writes editor_prefs.json (in the
// scratch user folder); THIS PROJECT writes the repo's project/settings.json and layers.json, so
// those tests also put the files back byte for byte when they finish. Build is only shot: its
// Build / Build and Run buttons are never clicked. Opening the window from the menus and
// shortcuts is covered by the Menus and Panels suites.

} // namespace
#include "ProjectSettings.h"
#include "LayerRegistry.h"

struct EditorSmokeAccess_Settings {
    static float UiScale(EditorLayer& e) { return e.m_UIScale; }
    static float UiScaleAtStartup(EditorLayer& e) { return e.m_UIScaleOverrideAtStartup; }
    static int PendingTheme(EditorLayer& e) { return e.m_PendingThemeId; }
};
namespace {
namespace smoke_settings {
using namespace ChromeUi;
using SA = EditorSmokeAccess_Settings;

// The sidebar entries, in m_PrefsCategory / m_ProjSettingsCategory order.
const char* const kMachineCats[] = {
    ICON_FA_UNIVERSAL_ACCESS "  General", ICON_FA_CAMERA "  Viewport", ICON_FA_TABLE_CELLS "  Grid & Snapping",
    ICON_FA_CLOCK "  Auto-Save", ICON_FA_GAUGE_HIGH "  Performance", ICON_FA_KEYBOARD "  Shortcuts",
    ICON_FA_WAND_MAGIC_SPARKLES "  Editor Enhancers", ICON_FA_CIRCLE_INFO "  About",
};
const char* const kProjectCats[] = {ICON_FA_PERSON_FALLING_BURST "  Physics", ICON_FA_TAGS "  Tags & Layers", ICON_FA_HAMMER "  Build"};

bool OnCategory(Ctx& c, bool project, int i) {
    return A::ShowSettings(c.E) && A::SettingsIsProject(c.E) == project &&
           (project ? A::ProjectCategory(c.E) : A::PrefsCategory(c.E)) == i;
}
// Opens the window on the other group's first category, then clicks the sidebar entry, so every
// test reaches its page the way a user does.
Steps OpenCategory(bool project, int i) {
    const char* label = project ? kProjectCats[i] : kMachineCats[i];
    return {
        Do("open Settings", [project](Ctx& c) { A::ClearSettingsSearch(c.E); c.E.OpenSettingsCategory(!project, 0); }),
        UntilShown(EditorPanels::Settings), Wait(2),
        Click(ByExact(label, "###Settings")),
        Until(std::string("on ") + label, [project, i](Ctx& c) { return OnCategory(c, project, i); }),
        Do("scroll the page to the top", [](Ctx&) {
            for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
                if (Contains(w->Name, "SettingsBody")) ImGui::SetScrollY(w, 0.0f);
        }),
        Wait(3),
    };
}

// An item of the Settings body whose ID is `id(window ID)` (for widgets the hooks give no label).
Query BodyItem(std::function<ImGuiID(ImGuiID)> id) {
    Query q;
    q.Root = "###Settings";
    q.Pred = [id](const UiItem& it) {
        if (!Contains(it.Window, "SettingsBody")) return false;
        const ImGuiWindow* w = ImGui::FindWindowByName(it.Window.c_str());
        return w && it.Id == id(w->ID);
    };
    return q;
}
// SettingsCheckbox(label): the box is "##cb" under PushID(label).
Query SettingsCheck(const char* label) {
    return BodyItem([label](ImGuiID win) { return ImHashStr("##cb", 0, ImHashStr(label, 0, win)); });
}
// A tag row's x: ActionButton(icon, tooltip) is `icon` under PushID(tooltip), under PushID(tag).
Query RemoveTagButton(std::string tag) {
    return BodyItem([tag](ImGuiID win) {
        return ImHashStr(ICON_FA_XMARK "##deltag", 0, ImHashStr("Remove this tag", 0, ImHashStr(tag.c_str(), 0, win)));
    });
}
// The i-th component of a DragFloat3 (each is "" under PushID(label), PushID(i)).
Query Vec3Part(const char* label, int i) {
    return BodyItem([label, i](ImGuiID win) { return ImHashStr("", 0, ImHashData(&i, sizeof(i), ImHashStr(label, 0, win))); });
}
// Scrolls the Settings body until the item is inside its visible part (long pages: Physics'
// collision matrix, the 32 layer slots).
Step Reveal(Query q) {
    return {"scroll into view", [q](Ctx&) {
        const UiItem* it = Find(q);
        if (!it) return false;
        ImGuiWindow* w = ImGui::FindWindowByName(it->Window.c_str());
        if (!w) return false;
        const float top = it->Clip.Min.y, bottom = it->Clip.Max.y;
        if (it->Rect.Min.y >= top && it->Rect.Max.y <= bottom) return true;
        ImGui::SetScrollY(w, w->Scroll.y + (it->Rect.GetCenter().y - (top + bottom) * 0.5f));
        return false;
    }, 120};
}
// A combo's popup entry.
Query ComboItem(const char* label) { return ByExact(label, "##Combo"); }

// A prefs checkbox: click it, the live value flips and editor_prefs.json `key` follows; click it
// again, both are back.
using LiveBool = bool (*)(Ctx&);
Steps TogglePref(const char* label, const char* key, LiveBool live) {
    const std::string l(label);
    return {
        Do(std::string("note ") + label, [live](Ctx& c) { g_Number = live(c) ? 1.0 : 0.0; }),
        Click(SettingsCheck(label)), Wait(2),
        ExpectTrue(l + " toggled", [live](Ctx& c) { return live(c) != Noted(); }),
        UntilPrefBool(key, [] { return !Noted(); }),
        Shot(std::string(key) + "-toggled", {"###Settings"}),
        Click(SettingsCheck(label)), Wait(2),
        ExpectTrue(l + " back", [live](Ctx& c) { return live(c) == Noted(); }),
        UntilPrefBool(key, [] { return Noted(); }),
    };
}

// --- Project files ------------------------------------------------------------------------------
std::string SettingsPath() { return ProjectPaths::Resolve("settings.json"); }
std::string LayersPath() { return ProjectPaths::Resolve("layers.json"); }
struct Saved { bool Existed = false; std::string Bytes; };
Saved g_SettingsFile, g_LayersFile;
Saved Snapshot(const std::string& path) {
    Saved s;
    s.Existed = std::filesystem::exists(path);
    if (s.Existed) s.Bytes = ReadText(path);
    return s;
}
void Restore(const std::string& path, const Saved& s) {
    std::error_code ec;
    if (!s.Existed) { std::filesystem::remove(path, ec); return; }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << s.Bytes;
}
Step SnapshotProjectFiles() {
    return Do("snapshot project/settings.json and layers.json", [](Ctx&) {
        g_SettingsFile = Snapshot(SettingsPath());
        g_LayersFile = Snapshot(LayersPath());
    });
}
Step RestoreProjectFiles() {
    return Do("put project/settings.json and layers.json back", [](Ctx&) {
        Restore(SettingsPath(), g_SettingsFile);
        Restore(LayersPath(), g_LayersFile);
    });
}
nlohmann::json ProjectJson() { return ReadJson(SettingsPath()); }
// The first .meta under project/assets written after `since`, or "" when none was.
std::filesystem::file_time_type g_MetaSince;
const char* const kGuessedTexture = "assets/Materials/Prototype/proto_grid_orange.png"; // its .meta has no importer
Saved g_UndoMeta;
ModelImportSettings g_UndoModel;
std::string MetaWrittenSince(std::filesystem::file_time_type since) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(ProjectPaths::Resolve("assets"), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".meta") continue;
        std::error_code tec;
        if (fs::last_write_time(it->path(), tec) > since && !tec) return ProjectPaths::Relativize(it->path().string());
    }
    return {};
}
float FileGravityY() {
    const nlohmann::json j = ProjectJson();
    try { return j.at("physics").at("gravity").at(1).get<float>(); } catch (...) { return NAN; }
}
bool FileHasTag(const std::string& tag) {
    const nlohmann::json j = ProjectJson();
    auto it = j.find("tags");
    if (it == j.end() || !it->is_array()) return false;
    for (const auto& t : *it) if (t.is_string() && t.get<std::string>() == tag) return true;
    return false;
}
std::string FileLayerName(int slot) {
    const nlohmann::json j = ReadJson(LayersPath());
    try { return j.at("names").at(slot).get<std::string>(); } catch (...) { return "<none>"; }
}
bool HasTag(const std::string& tag) {
    const auto& tags = ProjectSettings::Tags();
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

// Types into the text box a Ctrl+click opened (it starts with everything selected) and commits.
Steps CtrlClickType(Query q, const char* text) {
    return {Click(std::move(q), ImGuiMod_Ctrl), Wait(2), TypeText(text), Wait(1), Key(ImGuiKey_Enter), Wait(3)};
}

const char* const kThemes[] = {"Tartarus", "Black Glass"};
Steps PickTheme(int id) {
    return {
        Click(ByExact("##Editor theme", "###Settings")),
        Until("theme list open", [](Ctx&) { return Find(ComboItem("Black Glass")) != nullptr; }), Wait(1),
        Click(ComboItem(kThemes[id])),
        Until(std::string(kThemes[id]) + " applied", [id](Ctx& c) {
            return SA::PendingTheme(c.E) < 0 && EditorSettings::Get().EditorThemeId == id;
        }),
        Wait(5),
    };
}

int g_Slot = -1; // the unnamed layer slot the Tags & Layers test names
// That slot's name field ("##lname" under PushID(slot)); the slot is only known when the test runs.
Query SlotName() {
    Query q = BodyItem([](ImGuiID win) { const int n = g_Slot; return ImHashStr("##lname", 0, ImHashData(&n, sizeof(n), win)); });
    return q;
}
} // namespace smoke_settings

void RegisterSettingsTests() {
    using namespace smoke_settings;

    // ------------------------------------------------------------------------- THIS MACHINE
    Add("settings: General - tooltips checkbox persists to editor_prefs.json", Seq({
        {FreshScene(), Wait(2)},
        OpenCategory(false, 0),
        {Shot("general", {"###Settings"})},
        TogglePref("Show editor tooltips", "showTooltips", [](Ctx&) { return EditorSettings::Get().ShowTooltips; }),
        {CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    Add("settings: Viewport - Show grid persists", Seq({
        {FreshScene(), Wait(2)},
        OpenCategory(false, 1),
        {Shot("viewport", {"###Settings"})},
        TogglePref("Show grid", "viewShowGrid", [](Ctx& c) { return A::ShowGrid(c.E); }),
        {CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    Add("settings: Grid & Snapping - Show axis lines persists", Seq({
        {FreshScene(), Wait(2)},
        OpenCategory(false, 2),
        {Shot("grid", {"###Settings"})},
        TogglePref("Show axis lines", "gridShowAxisLines", [](Ctx&) { return EditorSettings::Get().GridShowAxisLines; }),
        {CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    Add("settings: Auto-Save - Enable auto-save persists", Seq({
        {FreshScene(), Wait(2)},
        OpenCategory(false, 3),
        {Shot("autosave", {"###Settings"})},
        TogglePref("Enable auto-save", "autoSaveEnabled", [](Ctx&) { return EditorSettings::Get().AutoSaveEnabled; }),
        {CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    // Performance has no prefs checkbox: its first checkbox (Auto exposure) is a scene setting,
    // so it is an undo step; the render resolution combo is the prefs setting here.
    Add("settings: Performance - Auto exposure undoes; render resolution persists", Seq({
        // A material whose folder comes from disk (its .meta has none), loaded before the scene
        // so the undo snapshot carries that folder - which the undo must not write to the .meta.
        {Do("load a material", [](Ctx& c) {
             c.A.LoadMaterial(ProjectPaths::Resolve("assets/Characters/Quantum/Materials/Clothing/Tops/TShirt/M_TShirt_Gray.mat"));
         }),
         FreshScene(), Wait(2)},
        OpenCategory(false, 4),
        {Shot("performance", {"###Settings"}),
         Do("note", [](Ctx& c) {
             g_Number = c.W.AutoExposure ? 1.0 : 0.0; g_Vec.x = (float)X::UndoDepth(c.E);
             g_MetaSince = std::filesystem::file_time_type::clock::now();
         }),
         Click(ByExact("Auto exposure", "###Settings")), Wait(3),
         ExpectTrue("auto exposure toggled", [](Ctx& c) { return c.W.AutoExposure != Noted(); }),
         ExpectTrue("one undo step", [](Ctx& c) { return (float)X::UndoDepth(c.E) == g_Vec.x + 1.0f; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         Until("undo restores it", [](Ctx& c) { return c.W.AutoExposure == Noted(); }),
         // An undo re-applies every asset's metadata; that used to rewrite ~90 tracked .meta files.
         Expect("the undo wrote no .meta", [](Ctx&) {
             const std::string m = MetaWrittenSince(g_MetaSince);
             return m.empty() ? std::string() : "wrote " + m;
         }),
         Do("note the resolution", [](Ctx&) { g_Vec.y = (float)EditorSettings::Get().RenderHeight; }),
         Click(ByExact("##Game render resolution", "###Settings")),
         Until("resolution list open", [](Ctx&) { return Find(ComboItem("720p")) != nullptr; }), Wait(1),
         Do("pick the one that isn't current", [](Ctx&) { g_Text = g_Vec.y == 720.0f ? "1080p" : "720p"; }),
         Click([] { Query q; q.Root = "##Combo"; q.Pred = [](const UiItem& it) { return it.Label == g_Text; }; return q; }()),
         Wait(2),
         ExpectTrue("render height changed", [](Ctx&) { return std::to_string(EditorSettings::Get().RenderHeight) + "p" == g_Text; }),
         Until("editor_prefs.json renderHeight", [](Ctx&) {
             return Prefs().value("renderHeight", -1) == EditorSettings::Get().RenderHeight;
         }),
         Click(ByExact("##Game render resolution", "###Settings")),
         Until("resolution list open", [](Ctx&) { return Find(ComboItem("Native")) != nullptr; }), Wait(1),
         Do("pick the original", [](Ctx&) {
             const int h = (int)g_Vec.y;
             g_Text = h == 0 ? "Native" : std::to_string(h) + "p";
         }),
         Click([] { Query q; q.Root = "##Combo"; q.Pred = [](const UiItem& it) { return it.Label == g_Text; }; return q; }()),
         Wait(2),
         ExpectTrue("render height back", [](Ctx&) { return (float)EditorSettings::Get().RenderHeight == g_Vec.y; }),
         Until("editor_prefs.json renderHeight back", [](Ctx&) { return (float)Prefs().value("renderHeight", -1) == g_Vec.y; }),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    // An undo restores every asset's import settings too. A guessed normal-map import lives in
    // memory only (it used to land in the texture's .meta), and a model's clip trims must survive
    // (the undo snapshot used to drop them, deleting them from the .meta).
    Add("settings: an undo keeps a guessed texture import in memory and a model's clip trims", Seq({
        {Do("load a guessed normal map and a trimmed model", [](Ctx& c) {
             g_Text = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
             g_UndoMeta = Snapshot(g_Text + ".meta");
             c.A.LoadTexture(ProjectPaths::Resolve(kGuessedTexture), AssetLibrary::TextureUse::Normal);
             c.A.LoadModel(g_Text);
             g_UndoModel = c.A.GetModelSettings(g_Text);
             ModelImportSettings trimmed = g_UndoModel;
             trimmed.ClipTrims.push_back({"SmokeTrim", 0.1f, 0.5f});
             c.A.SetModelSettings(g_Text, trimmed);
             g_MetaSince = std::filesystem::file_time_type::clock::now();
         }),
         FreshScene(), Wait(2)},
        OpenCategory(false, 4),
        {Do("note", [](Ctx& c) { g_Number = c.W.AutoExposure ? 1.0 : 0.0; }),
         Click(ByExact("Auto exposure", "###Settings")), Wait(3),
         ExpectTrue("auto exposure toggled", [](Ctx& c) { return c.W.AutoExposure != Noted(); }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         Until("undo restores it", [](Ctx& c) { return c.W.AutoExposure == Noted(); }),
         Expect("the undo wrote no .meta", [](Ctx&) {
             const std::string m = MetaWrittenSince(g_MetaSince);
             return m.empty() ? std::string() : "wrote " + m;
         }),
         ExpectTrue("the guessed import is still in memory", [](Ctx& c) {
             return !c.A.GetTextureSettings(ProjectPaths::Resolve(kGuessedTexture)).IsSRGB;
         }),
         ExpectTrue("the clip trim survived", [](Ctx& c) { return c.A.GetModelSettings(g_Text).ClipTrims.size() == g_UndoModel.ClipTrims.size() + 1; }),
         Do("put the texture and the model back", [](Ctx& c) {
             const std::string tex = ProjectPaths::Resolve(kGuessedTexture);
             c.A.SetTextureSettings(tex, TextureImportSettings{}, /*writeMeta=*/false);
             c.A.ReimportTexture(tex);
             c.A.SetModelSettings(g_Text, g_UndoModel, /*writeMeta=*/false);
             Restore(g_Text + ".meta", g_UndoMeta);
         }),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    // Shortcuts has no checkbox either: rebind one (press-to-bind), then reset it from its row.
    Add("settings: Shortcuts - rebind Invert Selection, then reset it", Seq({
        {FreshScene(), Wait(2)},
        OpenCategory(false, 5),
        {Shot("shortcuts", {"###Settings"}),
         Click(ByExact("##scfilter", "###Settings")), Wait(1),
         TypeText("Invert Selection"), Wait(3),
         ExpectTrue("one row left", [](Ctx&) {
             Query q; q.Root = "###Settings"; q.Pred = [](const UiItem& it) { return Contains(it.Label, "###bind"); };
             return FindAll(q).size() == 1;
         }),
         Shot("filtered", {"###Settings"}),
         Click([] { Query q; q.Root = "###Settings"; q.Pred = [](const UiItem& it) { return Contains(it.Label, "###bind"); }; return q; }()),
         Until("listening", [](Ctx& c) { return A::CapturingShortcut(c.E) == "edit.invertSelection"; }), Wait(2),
         Key(ImGuiKey_K), Wait(2), Key(ImGuiKey_Enter), Wait(3),
         Expect("bound to K", [](Ctx& c) {
             const Shortcuts::Shortcut* s = Shortcuts::Find("edit.invertSelection");
             if (!s) return std::string("no edit.invertSelection");
             if (!A::CapturingShortcut(c.E).empty()) return std::string("still capturing");
             return s->Current.Key == ImGuiKey_K && !s->Current.Ctrl && s->Overridden ? std::string()
                 : "bound to " + Shortcuts::ToString(s->Current);
         }),
         Until("shortcuts.json has it", [](Ctx&) { return Contains(ReadText(UserPaths::Resolve("shortcuts.json")), "edit.invertSelection"); }),
         Shot("rebound", {"###Settings"}),
         // The row's reset arrow (the toolbar's restore-all arrow is the first one).
         Click(ByExact(ICON_FA_ARROW_ROTATE_LEFT, "###Settings", 1)), Wait(3),
         Expect("back to Ctrl+I", [](Ctx&) {
             const Shortcuts::Shortcut* s = Shortcuts::Find("edit.invertSelection");
             return s && !s->Overridden && s->Current == s->Default ? std::string() : "now " + Shortcuts::ToString(s->Current);
         }),
         Until("shortcuts.json no longer has it", [](Ctx&) { return !Contains(ReadText(UserPaths::Resolve("shortcuts.json")), "edit.invertSelection"); }),
         Do("clear the filter", [](Ctx& c) { A::ShortcutFilter(c.E).clear(); ImGui::ClearActiveID(); }),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    Add("settings: Editor Enhancers - Hover keys persists", Seq({
        {FreshScene(), Wait(2)},
        OpenCategory(false, 6),
        {Shot("enhancers", {"###Settings"})},
        TogglePref("Hover keys", "enhancerHoverKeys", [](Ctx&) { return EditorSettings::Get().EnhancerHoverKeys; }),
        {CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    // About has nothing to change (Copy report would overwrite the user's clipboard).
    Add("settings: About shows the version and the system report", Seq({
        {FreshScene(), Wait(2)},
        OpenCategory(false, 7),
        {ExpectTrue("Copy report is there", [](Ctx&) { return Find(ByLabel("Copy report", "###Settings")) != nullptr; }),
         Shot("about", {"###Settings"}),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    // ------------------------------------------------------------------------------ Themes
    Add("settings: every theme - Black Glass and its options, then back to Tartarus", Seq({
        {FreshScene(), Wait(2)},
        OpenCategory(false, 0),
        {ExpectTrue("starts on Tartarus", [](Ctx&) { return EditorSettings::Get().EditorThemeId == 0; }),
         ExpectTrue("no glass options on Tartarus", [](Ctx&) { return Find(SettingsCheck("Frosted blur")) == nullptr; })},
        PickTheme(1),
        {Until("the glass options appear", [](Ctx&) { return Find(SettingsCheck("Frosted blur")) != nullptr; }),
         Wait(10),
         Shot("black-glass")},
        TogglePref("See-through window", "glassSeeThroughWindow", [](Ctx&) { return EditorSettings::Get().GlassSeeThroughWindow; }),
        TogglePref("Frosted blur", "glassFrostedBlur", [](Ctx&) { return EditorSettings::Get().GlassFrostedBlur; }),
        TogglePref("Viewport HUDs", "glassViewportHuds", [](Ctx&) { return EditorSettings::Get().GlassViewportHuds; }),
        {Do("frosted blur off", [](Ctx&) { EditorSettings::Get().GlassFrostedBlur = false; }), Wait(10),
         Shot("black-glass-no-blur"),
         Do("frosted blur on", [](Ctx&) { EditorSettings::Get().GlassFrostedBlur = true; EditorSettings::Save(); }), Wait(5)},
        PickTheme(0),
        {Until("the glass options are gone", [](Ctx&) { return Find(SettingsCheck("Frosted blur")) == nullptr; }),
         Wait(5),
         Shot("tartarus"),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    // ---------------------------------------------------------------------------- UI scale
    Add("settings: UI scale override by typing, restart warning, Auto resets it", Seq({
        // TARTARUS_EDITOR_TEST_SCALE starts the run on an override, so note it and put it back.
        {FreshScene(), Wait(2),
         Do("note the starting override", [](Ctx&) { g_Vec.x = EditorSettings::Get().UiScaleOverride; }),
         ExpectTrue("doesn't start on 1.25", [](Ctx&) { return std::fabs(g_Vec.x - 1.25f) > 1e-4f; })},
        OpenCategory(false, 0),
        {Do("note the live scale", [](Ctx& c) { g_Number = SA::UiScale(c.E); })},
        CtrlClickType(ByExact("##UI scale", "###Settings"), "1.25"),
        {Expect("override is 1.25", [](Ctx&) {
             const float v = EditorSettings::Get().UiScaleOverride;
             return std::fabs(v - 1.25f) < 1e-4f ? std::string() : "override " + std::to_string(v);
         }),
         Until("editor_prefs.json uiScaleOverride", [](Ctx&) { return std::fabs(Prefs().value("uiScaleOverride", 0.0f) - 1.25f) < 1e-4f; }),
         ExpectTrue("the live scale waits for a restart", [](Ctx& c) { return SA::UiScale(c.E) == (float)g_Number; }),
         ExpectTrue("restart warning (override differs from startup)", [](Ctx& c) {
             return EditorSettings::Get().UiScaleOverride != SA::UiScaleAtStartup(c.E);
         }),
         Shot("override", {"###Settings"}),
         Click(ByExact("Auto##uiscale", "###Settings")), Wait(3),
         ExpectTrue("back on Auto", [](Ctx&) { return EditorSettings::Get().UiScaleOverride == 0.0f; }),
         Until("editor_prefs.json uiScaleOverride 0", [](Ctx&) { return Prefs().value("uiScaleOverride", -1.0f) == 0.0f; }),
         Until("the Auto button is gone", [](Ctx&) { return Find(ByExact("Auto##uiscale", "###Settings")) == nullptr; }),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings),
         Do("restore the starting override", [](Ctx&) { EditorSettings::Get().UiScaleOverride = g_Vec.x; EditorSettings::Save(); })},
    }));

    // ------------------------------------------------------------------------------ Search
    Add("settings: search 'vsync' jumps to Performance; clearing it shows every category", Seq({
        {FreshScene(), Wait(2)},
        OpenCategory(false, 0),
        {Click(ByExact("##search", "###Settings")), Wait(1),
         TypeText("vsync"),
         Until("on Performance", [](Ctx& c) { return OnCategory(c, false, 4); }), Wait(3),
         ExpectTrue("search text kept", [](Ctx& c) { return A::SettingsSearch(c.E) == "vsync"; }),
         ExpectTrue("General is filtered out", [](Ctx&) { return Find(ByExact(kMachineCats[0], "###Settings")) == nullptr; }),
         ExpectTrue("Performance is listed", [](Ctx&) { return Find(ByExact(kMachineCats[4], "###Settings")) != nullptr; }),
         ExpectTrue("Build is listed too (its own VSync)", [](Ctx&) { return Find(ByExact(kProjectCats[2], "###Settings")) != nullptr; }),
         Shot("vsync", {"###Settings"}),
         Key(ImGuiKey_A, ImGuiMod_Ctrl), Key(ImGuiKey_Backspace), Wait(1),
         TypeText("gravity"),
         Until("on Physics", [](Ctx& c) { return OnCategory(c, true, 0); }), Wait(3),
         Shot("gravity", {"###Settings"}),
         Click(RightEnd(ByExact("##search", "###Settings"))), Wait(3),
         ExpectTrue("search cleared", [](Ctx& c) { return A::SettingsSearch(c.E).empty(); }),
         Expect("every category listed", [](Ctx&) {
             std::string missing;
             for (const char* l : kMachineCats) if (!Find(ByExact(l, "###Settings"))) missing += std::string(" ") + l;
             for (const char* l : kProjectCats) if (!Find(ByExact(l, "###Settings"))) missing += std::string(" ") + l;
             return missing.empty() ? std::string() : "missing:" + missing;
         }),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    // ------------------------------------------------------------------------- THIS PROJECT
    Add("settings: Physics - gravity and a collision-matrix cell persist to project/settings.json", Seq({
        {FreshScene(), Wait(2), SnapshotProjectFiles()},
        OpenCategory(true, 0),
        {Shot("physics", {"###Settings"}),
         Do("note gravity", [](Ctx&) { g_Vec = ProjectSettings::Physics().Gravity; })},
        CtrlClickType(Vec3Part("##grav", 1), "-3.5"),
        {Expect("gravity y is -3.5", [](Ctx&) {
             const glm::vec3 g = ProjectSettings::Physics().Gravity;
             return g.y == -3.5f && g.x == g_Vec.x && g.z == g_Vec.z ? std::string()
                 : "gravity " + std::to_string(g.x) + ", " + std::to_string(g.y) + ", " + std::to_string(g.z);
         }),
         Until("settings.json gravity -3.5", [](Ctx&) { return FileGravityY() == -3.5f; })},
        CtrlClickType(Vec3Part("##grav", 1), "-9.81"),
        {ExpectTrue("gravity back", [](Ctx&) { return ProjectSettings::Physics().Gravity == g_Vec; }),
         Until("settings.json gravity back", [](Ctx&) { return FileGravityY() == g_Vec.y; }),
         // The first checkbox: Default x Default in the collision matrix.
         Do("note", [](Ctx&) { g_Number = ProjectSettings::Physics().LayersCollide(0, 0) ? 1.0 : 0.0; }),
         Reveal(ByExact("##m0_0", "###Settings")), Wait(2),
         Click(ByExact("##m0_0", "###Settings")), Wait(3),
         ExpectTrue("Default/Default toggled", [](Ctx&) { return ProjectSettings::Physics().LayersCollide(0, 0) != Noted(); }),
         Until("settings.json layerCollision follows", [](Ctx&) {
             try { return ((ProjectJson().at("physics").at("layerCollision").at(0).get<std::uint32_t>() & 1u) != 0) != Noted(); }
             catch (...) { return false; }
         }),
         Shot("matrix-toggled", {"###Settings"}),
         Click(ByExact("##m0_0", "###Settings")), Wait(3),
         ExpectTrue("Default/Default back", [](Ctx&) { return ProjectSettings::Physics().LayersCollide(0, 0) == Noted(); }),
         Until("settings.json layerCollision back", [](Ctx&) {
             try { return ((ProjectJson().at("physics").at("layerCollision").at(0).get<std::uint32_t>() & 1u) != 0) == Noted(); }
             catch (...) { return false; }
         }),
         RestoreProjectFiles(),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    Add("settings: Tags & Layers - add and remove a tag, name and unname a layer", Seq({
        {FreshScene(), Wait(2), SnapshotProjectFiles(),
         ExpectTrue("no SmokeTag yet", [](Ctx&) { return !HasTag("SmokeTag"); }),
         Do("pick an unnamed layer slot", [](Ctx&) {
             g_Slot = -1;
             for (int i = LayerRegistry::kCount - 1; i >= 1 && g_Slot < 0; --i)
                 if (LayerRegistry::IsRenamable(i) && LayerRegistry::Name(i).empty()) g_Slot = i;
             if (g_Slot < 0) Fail("every layer slot is named");
         })},
        OpenCategory(true, 1),
        {Shot("tags-layers", {"###Settings"}),
         Click(ByExact("##newtag", "###Settings")), Wait(1),
         TypeText("SmokeTag"), Key(ImGuiKey_Enter), Wait(3),
         ExpectTrue("tag added", [](Ctx&) { return HasTag("SmokeTag"); }),
         Until("settings.json has the tag", [](Ctx&) { return FileHasTag("SmokeTag"); }),
         Shot("tag-added", {"###Settings"}),
         Click(RemoveTagButton("SmokeTag")), Wait(3),
         ExpectTrue("tag removed", [](Ctx&) { return !HasTag("SmokeTag"); }),
         Until("settings.json lost the tag", [](Ctx&) { return !FileHasTag("SmokeTag"); }),
         // Layers: the list runs past the window, so scroll the slot into view first.
         Reveal(SlotName()), Wait(2),
         Click(SlotName()), Wait(1),
         TypeText("SmokeLayer"), Key(ImGuiKey_Enter), Wait(3),
         ExpectTrue("layer named", [](Ctx&) { return LayerRegistry::Name(g_Slot) == "SmokeLayer"; }),
         Until("layers.json has the name", [](Ctx&) { return FileLayerName(g_Slot) == "SmokeLayer"; }),
         Shot("layer-named", {"###Settings"}),
         Click(SlotName()), Wait(1),
         Key(ImGuiKey_A, ImGuiMod_Ctrl), Key(ImGuiKey_Backspace), Key(ImGuiKey_Enter), Wait(3),
         ExpectTrue("layer unnamed", [](Ctx&) { return LayerRegistry::Name(g_Slot).empty(); }),
         Until("layers.json unnamed", [](Ctx&) { return FileLayerName(g_Slot).empty(); }),
         RestoreProjectFiles(),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));

    // Shot only: Build and Build and Run are never clicked.
    Add("settings: Build page (shot only)", Seq({
        {FreshScene(), Wait(2)},
        OpenCategory(true, 2),
        {Shot("build", {"###Settings"}),
         CloseByX(EditorPanels::Settings), UntilGone(EditorPanels::Settings)},
    }));
}
