// Editor smoke suite: ScriptIDE. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The Script IDE (src/Editor/ScriptIDE.cpp) keeps its documents behind a pimpl, so these tests
// check what a user can see from outside: the IDE window and its tabs (ImGui's tab bars carry the
// unsaved marker), EditorLayer::HasUnsavedScripts(), the files on disk and the IDE's
// session.json. Every file a test types into is a scratch copy under project/work/ (gitignored);
// the one real engine shader touched (the staged copy next to the exe) is restored byte for byte.

} // namespace
#include "GameModuleBuild.h"
#include "ShaderLibrary.h"
#include "ScriptIDEThemes.h"
namespace {
namespace smoke_ide {
namespace fs = std::filesystem;

constexpr const char* kIde = "Script IDE";

std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
bool WriteText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return static_cast<bool>(f);
}
fs::path Scratch(const std::string& name) { return fs::path(ProjectPaths::Resolve("work/editor-smoke-ide")) / name; }
fs::path SessionFile() { return fs::path(ProjectPaths::Resolve("Library/IDE")) / "session.json"; }
int SessionTheme() {
    const nlohmann::json j = nlohmann::json::parse(ReadText(SessionFile()), nullptr, false);
    return j.is_object() ? j.value("theme", -1) : -1;
}
bool Under(const fs::path& path, const fs::path& root) {
    if (root.empty()) return false;
    std::error_code ec;
    const std::string p = fs::weakly_canonical(path, ec).generic_string();
    const std::string r = fs::weakly_canonical(root, ec).generic_string();
    if (r.empty() || p.size() <= r.size()) return false;
    auto lower = [](std::string s) { for (char& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; };
    return lower(p).rfind(lower(r) + "/", 0) == 0;
}
// Log lines containing `fragment`, counting collapsed repeats.
int LogCount(const std::string& fragment) {
    int n = 0;
    for (const LogEntry& e : Log::Entries()) if (Contains(e.Message, fragment)) n += e.Count;
    return n;
}
// Until, with a real-time limit (frame rates vary a lot between a desktop GPU and llvmpipe).
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

ImGuiWindow* Shown(const char* name) {
    ImGuiWindow* w = ImGui::FindWindowByName(name);
    return w && w->WasActive && !w->Hidden ? w : nullptr;
}
ImGuiWindow* IdeWindow() { return Shown(kIde); }
// The ImGuiColorTextEdit child of the active tab ("Script IDE/EditorArea_../CodePane_../Code_..").
ImGuiWindow* IdeCode() {
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        if (w->WasActive && !w->Hidden && Contains(w->Name, "Script IDE/") && Contains(w->Name, "/Code_")) return w;
    return nullptr;
}
// A document tab: "<file name>###<absolute path>".
Query IdeTab(const std::string& file) {
    Query q;
    q.Root = kIde;
    q.Pred = [file](const UiItem& it) { return Contains(it.Label, "###") && Visible(it.Label) == file; };
    return q;
}
Query IdeTabEndingWith(const std::string& suffix) {
    Query q;
    q.Root = kIde;
    q.Pred = [suffix](const UiItem& it) {
        const std::string v = Visible(it.Label);
        return Contains(it.Label, "###") && v.size() >= suffix.size() && v.compare(v.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return q;
}
// A row of the SOURCES list (a Selectable labelled with the file's project-relative path).
Query SourceRow(const std::string& file) {
    Query q;
    q.Root = kIde;
    q.Pred = [file](const UiItem& it) { return !Contains(it.Label, "###") && Contains(Visible(it.Label), file); };
    return q;
}
// An item in an open menu or popup, by its exact label.
Query PopupExact(const std::string& label) {
    Query q;
    q.Exact = label;
    q.Pred = [](const UiItem& it) { return Contains(it.Root, "##Popup") || Contains(it.Root, "##Menu"); };
    return q;
}
Query PopupPrefix(const std::string& prefix) {
    Query q;
    q.Pred = [prefix](const UiItem& it) {
        return (Contains(it.Root, "##Popup") || Contains(it.Root, "##Menu")) && Visible(it.Label).rfind(prefix, 0) == 0;
    };
    return q;
}
Query Ide(const std::string& exact) { return ByExact(exact, kIde); }
Query Prompt(const std::string& exact) { return ByExact(exact, "Unsaved script"); }

// The tab item ImGui keeps for a document (its flags carry the unsaved-document marker).
const ImGuiTabItem* TabItemFor(const std::string& file) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int i = 0; i < g.TabBars.GetMapSize(); ++i) {
        ImGuiTabBar* bar = g.TabBars.TryGetMapData(i);
        if (!bar) continue;
        for (ImGuiTabItem& tab : bar->Tabs) {
            const char* name = ImGui::TabBarGetTabName(bar, &tab);
            if (name && std::string(name).rfind(file + "###", 0) == 0) return &tab;
        }
    }
    return nullptr;
}
bool TabDirty(const std::string& file) {
    const ImGuiTabItem* t = TabItemFor(file);
    return t && (t->Flags & ImGuiTabItemFlags_UnsavedDocument) != 0;
}
Step ExpectClean(const std::string& file) {
    return Expect("'" + file + "' has no unsaved marker", [file](Ctx& c) {
        if (c.E.HasUnsavedScripts()) return std::string("the IDE reports unsaved files");
        if (TabDirty(file)) return std::string("the tab still shows the unsaved marker");
        return std::string();
    });
}
Step ExpectDirty(const std::string& file) {
    return Expect("'" + file + "' shows the unsaved marker", [file](Ctx& c) {
        if (!c.E.HasUnsavedScripts()) return std::string("the IDE reports no unsaved files");
        if (!TabDirty(file)) return std::string("the tab has no unsaved marker");
        return std::string();
    });
}
Step ExpectDisk(const fs::path& p, std::string want) {
    return Expect(p.filename().string() + " on disk", [p, want](Ctx&) {
        const std::string got = ReadText(p);
        return got == want ? std::string() : "file holds '" + Trim(got, 200) + "', expected '" + Trim(want, 200) + "'";
    });
}

// Clicks the middle of the active tab's code area: focuses the editor widget and drops the caret.
Step ClickCode() {
    auto phase = std::make_shared<int>(0);
    return {"click the code editor", [phase](Ctx&) {
        ImGuiIO& io = ImGui::GetIO();
        if (*phase == 0) {
            ImGuiWindow* w = IdeCode();
            if (!w) return false;
            MouseTo(w->InnerRect.GetCenter());
            *phase = 1;
            return false;
        }
        if (*phase == 1) { io.AddMouseButtonEvent(0, true); *phase = 2; return false; }
        io.AddMouseButtonEvent(0, false);
        return true;
    }, 120};
}
// Gives the code widget keyboard focus without moving its caret (after a popup or button).
std::vector<Step> FocusCode() {
    return {
        Until("the code editor is shown", [](Ctx&) { return IdeCode() != nullptr; }),
        Do("focus the code editor", [](Ctx&) { ImGui::FocusWindow(IdeCode()); }),
        Wait(2),
    };
}
// Caret to the end of the document, ready to type.
std::vector<Step> CaretToEnd() { return {ClickCode(), Wait(2), Key(ImGuiKey_End, ImGuiMod_Ctrl), Wait(2)}; }

std::vector<Step> OpenDoc(fs::path path) {
    const std::string file = path.filename().string();
    return {
        Do("open " + file + " in the Script IDE", [path](Ctx& c) { c.E.OpenScriptIDE(path.string()); }),
        UntilSeconds("the IDE shows " + file, [file](Ctx&) { return IdeWindow() && Find(IdeTab(file)); }, 10),
        Wait(3),
    };
}
std::vector<Step> WriteDoc(fs::path path, std::string text) {
    return {Do("write " + path.filename().string(), [path, text](Ctx&) {
        if (!WriteText(path, text)) Fail("couldn't write " + path.string());
    })};
}
// Middle-click closes a clean tab (a modified one asks first).
std::vector<Step> CloseTab(const std::string& file) {
    return {
        Click(IdeTab(file), 0, 2), Wait(3),
        ExpectTrue(file + " tab closed", [file](Ctx&) { return Find(IdeTab(file)) == nullptr; }),
    };
}
std::vector<Step> RemoveDoc(fs::path path) {
    return {Do("remove " + path.filename().string(), [path](Ctx&) { std::error_code ec; fs::remove(path, ec); })};
}
// The IDE docks over Scene; put Scene back in front so later tests see the viewport.
std::vector<Step> HideIde() {
    return {
        Do("close menus", [](Ctx&) { ImGui::ClosePopupsExceptModals(); }),
        Do("bring Scene to the front", [](Ctx&) { ImGui::FocusWindow(ImGui::FindWindowByName(EditorPanels::Scene)); }),
        Wait(3),
        Do("drop keyboard focus", [](Ctx&) { ImGui::FocusWindow(nullptr); }),
        Wait(2),
    };
}
Step CloseMenus() { return Do("close menus", [](Ctx&) { ImGui::ClosePopupsExceptModals(); }); }
std::vector<Step> IdeMenu(const std::string& menu, const std::string& item) {
    return {Click(Ide(menu)), Wait(2), Click(PopupExact(item)), Wait(3)};
}
// Clicks `button` every `every` seconds until `done` holds (a background scan the click depends on).
Step ClickUntil(Query button, std::function<bool()> done, double every, double seconds, std::string name) {
    auto start = std::make_shared<double>(-1.0), last = std::make_shared<double>(-1e9);
    auto phase = std::make_shared<int>(0);
    return {name, [button, done, every, seconds, name, start, last, phase](Ctx&) {
        const double now = ImGui::GetTime();
        if (*start < 0.0) *start = now;
        ImGuiIO& io = ImGui::GetIO();
        if (*phase == 1) { io.AddMouseButtonEvent(0, true); *phase = 2; return false; }
        if (*phase == 2) { io.AddMouseButtonEvent(0, false); *phase = 0; return false; }
        if (done()) return true;
        if (now - *start > seconds) { Fail("step '" + name + "' timed out"); return true; }
        if (now - *last >= every) {
            const UiItem* it = Find(button);
            ImVec2 p;
            if (it && ClickPoint(*it, p)) { MouseTo(p); *phase = 1; *last = now; }
        }
        return false;
    }, 100000000};
}
// Clicks `q` if it is on screen now; otherwise does nothing.
Step ClickIfShown(Query q) {
    Step click = Click(q);
    auto skip = std::make_shared<int>(-1);
    click.Name += " (if shown)";
    click.Fn = [inner = click.Fn, q, skip](Ctx& c) {
        if (*skip < 0) *skip = Find(q) ? 0 : 1;
        return *skip == 1 || inner(c);
    };
    return click;
}
std::vector<Step> Join(std::vector<std::vector<Step>> parts) {
    std::vector<Step> out;
    for (auto& part : parts) for (auto& s : part) out.push_back(std::move(s));
    return out;
}
} // namespace smoke_ide

void RegisterScriptIDETests() {
    using namespace smoke_ide;

    // ------------------------------------------------------------------ opening it
    {
        std::vector<Step> steps = {
            FreshScene(), Wait(2),
            Click(ByExact("Window", "##Toolbar")), Wait(2),
            Click(MenuItem("Script IDE")),
            UntilSeconds("the IDE is shown", [](Ctx&) { return IdeWindow() != nullptr; }, 5),
            Wait(3),
            Expect("docked in the Scene / Game node", [](Ctx&) {
                ImGuiWindow* ide = IdeWindow();
                ImGuiWindow* scene = ImGui::FindWindowByName(EditorPanels::Scene);
                if (!ide || !scene) return std::string("window missing");
                return ide->DockId != 0 && ide->DockId == scene->DockId ? std::string()
                    : "IDE dock " + std::to_string(ide->DockId) + ", Scene dock " + std::to_string(scene->DockId);
            }),
            ExpectTrue("the IDE has the keyboard", [](Ctx& c) { return c.E.ScriptIDEHasKeyboardFocus(); }),
            Shot("ide", {kIde}),
        };
        const std::pair<const char*, const char*> menus[] = {
            {"File", "Save All"}, {"Edit", "Find / Replace"}, {"Navigate", "Go to line"},
            {"Build", "Save All & Rebuild Modules"}, {"Appearance", "Tartarus Dark"},
        };
        for (const auto& [menu, item] : menus) {
            const std::string m = menu, i = item;
            steps.push_back(Click(Ide(m)));
            steps.push_back(Wait(3));
            steps.push_back(ExpectTrue(m + " menu lists " + i, [i](Ctx&) { return Find(PopupExact(i)) != nullptr; }));
            steps.push_back(Shot("menu-" + m, {kIde}));
            steps.push_back(CloseMenus());
            steps.push_back(Wait(2));
        }
        // Build: enabled exactly when the source checkout and CMake build tree are there.
        steps.push_back(Click(Ide("Build")));
        steps.push_back(Wait(3));
        steps.push_back(Expect("Save All & Rebuild Modules enabled iff a build tree exists", [](Ctx&) {
            const UiItem* it = Find(PopupExact("Save All & Rebuild Modules"));
            if (!it) return std::string("item missing");
            const bool enabled = !(it->ItemFlags & ImGuiItemFlags_Disabled);
            return enabled == GameModuleBuild::Available() ? std::string()
                : std::string(enabled ? "enabled" : "disabled") + " but Available() is " + (GameModuleBuild::Available() ? "true" : "false");
        }));
        steps.push_back(CloseMenus());
        for (Step& s : HideIde()) steps.push_back(std::move(s));
        Add("scriptide: Window > Script IDE opens it docked with Scene; File/Edit/Navigate/Build/Appearance menus", std::move(steps));
    }

    Add("scriptide: Open in Script IDE from a shader's Asset Browser menu", Join({
        {FreshScene(), Wait(2),
         Do("browse the Shaders folder", [](Ctx& c) { g_Text = X::CurrentFolder(c.E); X::NavigateFolder(c.E, "Shaders"); }),
         Wait(5),
         RightClick([] {
             Query q;
             q.Root = kAssets;
             q.Pred = [](const UiItem& it) { return it.Label == "##tile" || Contains(Visible(it.Label), "(built-in)"); };
             return q;
         }()),
         Wait(2),
         Click(MenuItem("Open in Script IDE")),
         UntilSeconds("the IDE shows the shader", [](Ctx&) { return IdeWindow() && Find(IdeTabEndingWith(".shader")); }, 10),
         Wait(3),
         Shot("shader-descriptor", {kIde}),
         Click(IdeTabEndingWith(".shader"), 0, 2), Wait(3),
         ExpectTrue("its tab closed", [](Ctx&) { return Find(IdeTabEndingWith(".shader")) == nullptr; }),
         Do("back to the previous folder", [](Ctx& c) { X::NavigateFolder(c.E, g_Text); })},
        HideIde(),
    }));

    // ------------------------------------------------------------------ editing
    {
        const fs::path p = Scratch("SmokeType.cpp");
        const std::string f = p.filename().string(), orig = "int SmokeValue = 1;\n";
        Add("scriptide: type in a C++ file, undo / redo by keys and Edit menu, dirty marker, Ctrl+S saves", Join({
            {FreshScene()}, WriteDoc(p, orig), OpenDoc(p),
            {ExpectClean(f)},
            CaretToEnd(),
            {ExpectTrue("the code editor has the keyboard", [](Ctx& c) { return c.E.ScriptIDEHasKeyboardFocus(); }),
             TypeText("7"), Wait(3),
             ExpectDirty(f),
             Shot("dirty", {kIde}),
             Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3), ExpectClean(f),
             Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3), ExpectDirty(f)},
            IdeMenu("Edit", "Undo"), {ExpectClean(f)},
            IdeMenu("Edit", "Redo"), {ExpectDirty(f)},
            {ExpectDisk(p, orig)},
            FocusCode(),
            {Key(ImGuiKey_S, ImGuiMod_Ctrl), Wait(3),
             ExpectDisk(p, orig + "7"),
             ExpectClean(f)},
            CloseTab(f), RemoveDoc(p), HideIde(),
        }));
    }
    {
        const fs::path p = Scratch("SmokeFind.cpp");
        const std::string f = p.filename().string();
        Add("scriptide: Edit > Find / Replace: Next, Replace match, Replace all, undo, File > Save", Join({
            {FreshScene()}, WriteDoc(p, "int SmokeValue = 1;\nint Other = SmokeValue;\n"), OpenDoc(p),
            IdeMenu("Edit", "Find / Replace"),
            {Click(Ide("Find")), Wait(2), Key(ImGuiKey_A, ImGuiMod_Ctrl), TypeText("SmokeValue"), Wait(2),
             Click(Ide("Next")), Wait(2),
             ClickUnlessShown(Ide("Replace"), Ide("With")), Wait(2),
             Click(Ide("With")), Wait(2), Key(ImGuiKey_A, ImGuiMod_Ctrl), TypeText("Renamed"), Wait(2),
             Click(Ide("Replace match")), Wait(3),
             ExpectDirty(f)},
            IdeMenu("File", "Save"),
            {ExpectDisk(p, "int Renamed = 1;\nint Other = SmokeValue;\n"),
             Click(Ide("Replace all")), Wait(3), ExpectDirty(f),
             Shot("find-replace", {kIde})},
            IdeMenu("File", "Save"),
            {ExpectDisk(p, "int Renamed = 1;\nint Other = Renamed;\n"), ExpectClean(f)},
            IdeMenu("Edit", "Undo"), {ExpectDirty(f)},
            IdeMenu("Edit", "Redo"), {ExpectClean(f)},
            {Click(Ide("Close find")), Wait(3),
             ExpectTrue("the find bar closed", [](Ctx&) { return Find(Ide("Find")) == nullptr; })},
            CloseTab(f), RemoveDoc(p), HideIde(),
        }));
    }
    {
        const fs::path p = Scratch("SmokeGoto.cpp");
        const std::string f = p.filename().string();
        Add("scriptide: Navigate > Go to line moves the caret; Ctrl+G and Ctrl+F open their tools", Join({
            {FreshScene()}, WriteDoc(p, "int A = 1;\nint B = 2;\nint C = 3;\n"), OpenDoc(p),
            {ClickIfShown(Ide("Close find")), Wait(2)}, // start with the find bar closed
            IdeMenu("Navigate", "Go to line"),
            {Click(PopupExact("Line")), Wait(2), Key(ImGuiKey_A, ImGuiMod_Ctrl), TypeText("2"), Wait(2),
             Click(PopupExact("Go")), Wait(3)},
            FocusCode(),
            {TypeText("#"), Wait(2), Key(ImGuiKey_S, ImGuiMod_Ctrl), Wait(3),
             ExpectDisk(p, "int A = 1;\n#int B = 2;\nint C = 3;\n")},
            FocusCode(),
            {Key(ImGuiKey_G, ImGuiMod_Ctrl), Wait(3),
             ExpectTrue("Ctrl+G opened Go to line", [](Ctx&) { return Find(PopupExact("Go")) != nullptr; }),
             Shot("go-to-line", {kIde}),
             CloseMenus(), Wait(2)},
            FocusCode(),
            {Key(ImGuiKey_F, ImGuiMod_Ctrl), Wait(3),
             ExpectTrue("Ctrl+F opened the find bar", [](Ctx&) { return Find(Ide("Find")) != nullptr; }),
             Click(Ide("Close find")), Wait(3),
             ExpectTrue("Close find hid it", [](Ctx&) { return Find(Ide("Find")) == nullptr; })},
            CloseTab(f), RemoveDoc(p), HideIde(),
        }));
    }
    {
        const fs::path p = Scratch("SmokeShader.frag.glsl");
        const std::string f = p.filename().string();
        const std::string orig = "#version 460 core\n// SmokeShaderMarker\nout vec4 FragColor;\nvoid main() { FragColor = vec4(1.0); }\n";
        Add("scriptide: a project GLSL file from the SOURCES list and from Find in project; edit and save", Join({
            {FreshScene()}, WriteDoc(p, orig),
            {Do("show the IDE", [](Ctx& c) { c.E.OpenScriptIDE(); }),
             UntilSeconds("the IDE is shown", [](Ctx&) { return IdeWindow() != nullptr; }, 5),
             Click(Ide("##ScriptSearch")), Wait(2), Key(ImGuiKey_A, ImGuiMod_Ctrl), TypeText("SmokeShader.frag"), Wait(2),
             UntilSeconds("the source scan lists the shader", [f](Ctx&) { return Find(SourceRow(f)) != nullptr; }, 20),
             Click(SourceRow(f)),
             UntilSeconds("its tab opened", [f](Ctx&) { return Find(IdeTab(f)) != nullptr; }, 5),
             Wait(3),
             Shot("glsl", {kIde})},
            CloseTab(f),
            {Click(Ide("##ScriptSearch")), Wait(2), Key(ImGuiKey_A, ImGuiMod_Ctrl), Key(ImGuiKey_Backspace), Wait(2)},
            IdeMenu("Navigate", "Find in project"),
            {Click(PopupExact("Text")), Wait(2), Key(ImGuiKey_A, ImGuiMod_Ctrl), TypeText("SmokeShaderMarker"), Wait(2),
             ClickUntil(PopupExact("Search"), [f] { return Find(PopupPrefix(f + ":")) != nullptr; }, 1.0, 20.0,
                        "Search until the shader is found"),
             Shot("find-in-project", {kIde}),
             Click(PopupPrefix(f + ":2")),
             UntilSeconds("the match opened its file", [f](Ctx&) { return Find(IdeTab(f)) != nullptr; }, 5),
             CloseMenus(), Wait(3)},
            FocusCode(),
            {Key(ImGuiKey_F, ImGuiMod_Ctrl | ImGuiMod_Shift), Wait(3),
             ExpectTrue("Ctrl+Shift+F opened Find in project", [](Ctx&) { return Find(PopupExact("Search")) != nullptr; }),
             CloseMenus(), Wait(2)},
            CaretToEnd(),
            {TypeText("// saved by the smoke test"), Wait(2), ExpectDirty(f),
             Key(ImGuiKey_S, ImGuiMod_Ctrl), Wait(3),
             ExpectDisk(p, orig + "// saved by the smoke test"), ExpectClean(f)},
            CloseTab(f), RemoveDoc(p), HideIde(),
        }));
    }
    {
        const fs::path pa = Scratch("SmokeCloseA.cpp"), pb = Scratch("SmokeCloseB.cpp");
        const std::string fa = pa.filename().string(), fb = pb.filename().string();
        Add("scriptide: closing tabs: clean closes; modified asks (Cancel keeps, Save writes, Discard drops)", Join({
            {FreshScene()}, WriteDoc(pa, "int A = 1;\n"), WriteDoc(pb, "int B = 2;\n"), OpenDoc(pa), OpenDoc(pb),
            CloseTab(fa),
            {ExpectTrue(fb + " still open", [fb](Ctx&) { return Find(IdeTab(fb)) != nullptr; })},
            CaretToEnd(),
            {TypeText("x"), Wait(2), ExpectDirty(fb),
             Click(IdeTab(fb), 0, 2),
             UntilSeconds("the unsaved prompt", [](Ctx&) { return Find(Prompt("Cancel")) != nullptr; }, 5),
             Shot("unsaved-prompt", {kIde}),
             Click(Prompt("Cancel")), Wait(3),
             ExpectTrue("Cancel kept the tab", [fb](Ctx&) { return Find(IdeTab(fb)) != nullptr && Find(Prompt("Cancel")) == nullptr; }),
             ExpectDirty(fb),
             Click(IdeTab(fb), 0, 2),
             UntilSeconds("the unsaved prompt again", [](Ctx&) { return Find(Prompt("Save")) != nullptr; }, 5),
             Click(Prompt("Save")), Wait(3),
             ExpectTrue("Save closed the tab", [fb](Ctx&) { return Find(IdeTab(fb)) == nullptr; }),
             ExpectDisk(pb, "int B = 2;\nx"),
             ExpectTrue("nothing unsaved", [](Ctx& c) { return !c.E.HasUnsavedScripts(); })},
            OpenDoc(pb), CaretToEnd(),
            {TypeText("y"), Wait(2), ExpectDirty(fb),
             Click(IdeTab(fb), 0, 2),
             UntilSeconds("the unsaved prompt a third time", [](Ctx&) { return Find(Prompt("Discard")) != nullptr; }, 5),
             Click(Prompt("Discard")), Wait(3),
             ExpectTrue("Discard closed the tab", [fb](Ctx&) { return Find(IdeTab(fb)) == nullptr; }),
             ExpectDisk(pb, "int B = 2;\nx"),
             ExpectTrue("nothing unsaved", [](Ctx& c) { return !c.E.HasUnsavedScripts(); })},
            RemoveDoc(pa), RemoveDoc(pb), HideIde(),
        }));
    }
    {
        const fs::path pa = Scratch("SmokeAllA.cpp"), pb = Scratch("SmokeAllB.cpp");
        const std::string fa = pa.filename().string(), fb = pb.filename().string();
        Add("scriptide: File > Save All writes every modified tab", Join({
            {FreshScene()}, WriteDoc(pa, "int A = 1;\n"), WriteDoc(pb, "int B = 2;\n"), OpenDoc(pa),
            CaretToEnd(), {TypeText("a"), Wait(2)},
            OpenDoc(pb),
            CaretToEnd(), {TypeText("b"), Wait(2),
                           ExpectTrue("both tabs modified", [fa, fb](Ctx&) { return TabDirty(fa) && TabDirty(fb); })},
            IdeMenu("File", "Save All"),
            {ExpectDisk(pa, "int A = 1;\na"), ExpectDisk(pb, "int B = 2;\nb"),
             ExpectTrue("nothing unsaved", [](Ctx& c) { return !c.E.HasUnsavedScripts(); })},
            CloseTab(fa), CloseTab(fb), RemoveDoc(pa), RemoveDoc(pb), HideIde(),
        }));
    }
    {
        const fs::path p = Scratch("SmokeExternal.cpp");
        const std::string f = p.filename().string();
        Add("scriptide: edits outside the IDE: a clean tab reloads; a modified one offers Reload / Overwrite; Reload from disk", Join({
            {FreshScene()}, WriteDoc(p, "int V = 1;\n"), OpenDoc(p),
            WriteDoc(p, "int V = 2;\n"),
            {WaitSeconds(2.0),
             ExpectClean(f),
             ExpectTrue("no conflict bar on a clean tab", [](Ctx&) { return Find(Ide("Reload disk version")) == nullptr; })},
            CaretToEnd(), {TypeText("a"), Wait(2), Key(ImGuiKey_S, ImGuiMod_Ctrl), Wait(3),
                           ExpectDisk(p, "int V = 2;\na")}, // the buffer had picked up the outside edit
            CaretToEnd(), {TypeText("b"), Wait(2)},
            WriteDoc(p, "int V = 3;\n"),
            {UntilSeconds("the conflict bar", [](Ctx&) { return Find(Ide("Overwrite disk with buffer")) != nullptr; }, 5),
             Shot("conflict", {kIde}),
             Click(Ide("Overwrite disk with buffer")), Wait(3),
             ExpectDisk(p, "int V = 2;\nab"), ExpectClean(f)},
            CaretToEnd(), {TypeText("c"), Wait(2)},
            WriteDoc(p, "int V = 4;\n"),
            {UntilSeconds("the conflict bar again", [](Ctx&) { return Find(Ide("Reload disk version")) != nullptr; }, 5),
             Click(Ide("Reload disk version")),
             UntilSeconds("the unsaved prompt", [](Ctx&) { return Find(Prompt("Discard")) != nullptr; }, 5),
             Click(Prompt("Discard")), Wait(3),
             ExpectClean(f),
             ExpectTrue("conflict bar gone", [](Ctx&) { return Find(Ide("Reload disk version")) == nullptr; }),
             ExpectDisk(p, "int V = 4;\n")},
            IdeMenu("File", "Reload from disk"),
            {ExpectTrue("a clean reload asks nothing", [](Ctx&) { return Find(Prompt("Discard")) == nullptr; }), ExpectClean(f)},
            CaretToEnd(), {TypeText("d"), Wait(2), ExpectDirty(f)},
            IdeMenu("File", "Reload from disk"),
            {UntilSeconds("the unsaved prompt", [](Ctx&) { return Find(Prompt("Cancel")) != nullptr; }, 5),
             Click(Prompt("Cancel")), Wait(3), ExpectDirty(f)},
            IdeMenu("File", "Reload from disk"),
            {UntilSeconds("the unsaved prompt", [](Ctx&) { return Find(Prompt("Discard")) != nullptr; }, 5),
             Click(Prompt("Discard")), Wait(3), ExpectClean(f), ExpectDisk(p, "int V = 4;\n")},
            CloseTab(f), RemoveDoc(p), HideIde(),
        }));
    }
    {
        const fs::path p = Scratch("SmokeComplete.cpp");
        const std::string f = p.filename().string();
        Add("scriptide: Complete word (Edit menu and Ctrl+Space) inserts an identifier from the open files", Join({
            {FreshScene()}, WriteDoc(p, "int SmokeCompletionTarget = 1;\n"), OpenDoc(p),
            CaretToEnd(), {TypeText("SmokeCompl"), Wait(2)},
            IdeMenu("Edit", "Complete word"),
            {UntilSeconds("the completion list", [](Ctx&) { return Find(PopupExact("SmokeCompletionTarget")) != nullptr; }, 5),
             Shot("completion", {kIde}),
             Click(PopupExact("SmokeCompletionTarget")), Wait(3)},
            FocusCode(),
            {Key(ImGuiKey_Enter), Wait(2), TypeText("SmokeCompl"), Wait(2),
             Key(ImGuiKey_Space, ImGuiMod_Ctrl), Wait(3),
             UntilSeconds("Ctrl+Space lists it", [](Ctx&) { return Find(PopupExact("SmokeCompletionTarget")) != nullptr; }, 5),
             Click(PopupExact("SmokeCompletionTarget")), Wait(3)},
            FocusCode(),
            {Key(ImGuiKey_S, ImGuiMod_Ctrl), Wait(3),
             ExpectDisk(p, "int SmokeCompletionTarget = 1;\nSmokeCompletionTarget\nSmokeCompletionTarget")},
            CloseTab(f), RemoveDoc(p), HideIde(),
        }));
    }

    // ------------------------------------------------------------------ appearance
    {
        const fs::path p = Scratch("SmokeTheme.frag.glsl");
        const std::string f = p.filename().string();
        auto restore = std::make_shared<int>(0);
        std::vector<Step> steps = Join({
            {FreshScene()},
            WriteDoc(p, "#version 460 core\n// themes\nout vec4 FragColor;\nvoid main() { FragColor = vec4(0.5, 0.25, 1.0, 1.0); }\n"),
            OpenDoc(p),
            {Do("note the current theme", [restore](Ctx&) { *restore = std::max(0, SessionTheme()); })},
        });
        for (int i = 0; i < (int)ScriptIDEThemes::Presets.size(); ++i) {
            const std::string name = ScriptIDEThemes::Presets[(size_t)i].Name;
            for (Step& s : IdeMenu("Appearance", name)) steps.push_back(std::move(s));
            steps.push_back(Expect(name + " saved with the session", [i](Ctx&) {
                const int t = SessionTheme();
                return t == i ? std::string() : "session.json theme is " + std::to_string(t);
            }));
            if (name == "Dracula" || name == "GitHub Light" || name == "High Contrast") steps.push_back(Shot("theme-" + name, {kIde}));
        }
        steps.push_back(Click(Ide("Appearance")));
        steps.push_back(Wait(2));
        {
            Query q;
            q.Pred = [restore](const UiItem& it) {
                return (Contains(it.Root, "##Popup") || Contains(it.Root, "##Menu")) &&
                       it.Label == ScriptIDEThemes::Presets[(size_t)std::clamp(*restore, 0, (int)ScriptIDEThemes::Presets.size() - 1)].Name;
            };
            steps.push_back(Click(q));
        }
        steps.push_back(Wait(3));
        steps.push_back(ExpectTrue("theme restored", [restore](Ctx&) { return SessionTheme() == *restore; }));
        for (Step& s : IdeMenu("Appearance", "Dock alongside Scene / Game")) steps.push_back(std::move(s));
        steps.push_back(Wait(3));
        steps.push_back(ExpectTrue("still docked with Scene", [](Ctx&) {
            ImGuiWindow* ide = ImGui::FindWindowByName(kIde);
            ImGuiWindow* scene = ImGui::FindWindowByName(EditorPanels::Scene);
            return ide && scene && ide->DockId != 0 && ide->DockId == scene->DockId;
        }));
        for (Step& s : Join({CloseTab(f), RemoveDoc(p), HideIde()})) steps.push_back(std::move(s));
        Add("scriptide: Appearance switches all twelve themes (saved with the session); Dock alongside Scene / Game", std::move(steps));
    }

    // ------------------------------------------------------------------ panel close keeps buffers
    {
        const fs::path p = Scratch("SmokePanel.cpp");
        const std::string f = p.filename().string();
        Query dockTab;
        dockTab.Exact = kIde;
        dockTab.Pred = [](const UiItem& it) { return it.Root != kIde && !Contains(it.Root, "##Menu") && !Contains(it.Root, "##Popup"); };
        Add("scriptide: closing the IDE panel hides it and keeps a modified buffer; Window menu brings it back", Join({
            {FreshScene()}, WriteDoc(p, "int P = 1;\n"), OpenDoc(p),
            CaretToEnd(), {TypeText("q"), Wait(2), ExpectDirty(f),
                           Click(dockTab, 0, 2), Wait(3),
                           ExpectTrue("the IDE panel closed", [](Ctx&) { return IdeWindow() == nullptr; }),
                           ExpectTrue("the buffer is still unsaved", [](Ctx& c) { return c.E.HasUnsavedScripts(); }),
                           ExpectDisk(p, "int P = 1;\n"),
                           Click(ByExact("Window", "##Toolbar")), Wait(2),
                           Click(MenuItem("Script IDE")),
                           UntilSeconds("the IDE is back", [f](Ctx&) { return IdeWindow() && Find(IdeTab(f)); }, 5),
                           Wait(3),
                           ExpectDirty(f),
                           Do("discard the edit", [](Ctx& c) { c.E.DiscardScripts(); }), Wait(3),
                           ExpectClean(f)},
            CloseTab(f), RemoveDoc(p), HideIde(),
        }));
    }

    // ------------------------------------------------------------------ engine shaders
    {
        const std::string f = "Outline.frag.glsl";
        auto original = std::make_shared<std::string>();
        auto editable = std::make_shared<bool>(false);
        auto base = std::make_shared<int>(0);
        const std::string reloaded = "Shader hot reload: recompiled OutlineModel.vert.glsl + Outline.frag.glsl.";
        auto path = [] { return fs::path(ShaderLibrary::Dir()) / "Outline.frag.glsl"; };
        Add("scriptide: a staged engine shader opens read-only; editing it on disk hot reloads, then it is restored", Join({
            {FreshScene(),
             Do("read the staged shader", [path, original, editable](Ctx&) {
                 *original = ReadText(path());
                 if (original->empty()) Fail("no staged shader at " + path().string());
                 // Editable only from a source checkout (src/) or the project; the staged copy is neither.
                 *editable = (GameModuleBuild::Available() && Under(path(), GameModuleBuild::SourceRoot() / "src")) ||
                             Under(path(), fs::path(ProjectPaths::Root()));
             }),
             Do("open it in the Script IDE", [path](Ctx& c) { c.E.OpenScriptIDE(path().string()); }),
             UntilSeconds("the IDE shows " + f, [f](Ctx&) { return IdeWindow() && Find(IdeTab(f)); }, 10),
             Wait(3),
             Shot("engine-shader", {kIde})},
            CaretToEnd(),
            {TypeText("x"), Wait(3),
             ExpectTrue("typing doesn't change a read-only file", [editable](Ctx& c) { return *editable || !c.E.HasUnsavedScripts(); }),
             Do("drop any edit", [](Ctx& c) { if (c.E.HasUnsavedScripts()) c.E.DiscardScripts(); }), Wait(2)},
            CloseTab(f),
            {Do("append a comment to the staged shader", [path, original, base, reloaded](Ctx&) {
                 *base = LogCount(reloaded);
                 if (!WriteText(path(), *original + "\n// editor smoke test: hot reload probe\n")) Fail("couldn't write " + path().string());
             }),
             UntilSeconds("the outline program recompiled", [base, reloaded](Ctx&) { return LogCount(reloaded) > *base; }, 20),
             Do("restore the original text", [path, original](Ctx&) {
                 if (!WriteText(path(), *original)) Fail("couldn't restore " + path().string());
             }),
             UntilSeconds("recompiled again from the original", [base, reloaded](Ctx&) { return LogCount(reloaded) > *base + 1; }, 20),
             ExpectTrue("the staged shader is byte-for-byte the original", [path, original](Ctx&) { return ReadText(path()) == *original; })},
            HideIde(),
            {Shot("after-reload", {kScene})},
        }));
    }
}
