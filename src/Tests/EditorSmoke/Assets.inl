// Editor smoke suite: Assets. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// The Asset Browser (EditorModuleAssetBrowser.cpp draws the toolbar, folder tree and grid scaffold;
// EditorLayer_AssetBrowser.cpp the tiles, their context menus and the grid's background menu), the
// asset Inspectors and the Asset Library panel. Everything happens inside a scratch project folder,
// project/editor-smoke-scratch-assets, that the first test writes (a cube .obj, two .png images in
// it and in Sub/Deeper, a one-object .prefab) and the last test deletes through the browser's own
// Delete Folder. CI has none of the project's external models, so the suite makes its own.
//
// Tiles and tree rows are label-less ("##tile" / "##node" under PushID(key)), so they are found by
// the ImGui ID they hash to. Nothing here opens a native dialog (Import Asset..., Show in folder,
// Open Externally and the Asset Library's Choose Library Folder... / Import are never clicked).

} // namespace
#include "AssetDatabase.h"
struct EditorSmokeAccess_Assets {
    static const std::vector<AssetGridCell>& Cells(EditorLayer& e) { return e.m_AssetGridCells; }
    static void Refresh(EditorLayer& e) { e.RefreshAssetBrowser(); }
    static float& IconSize(EditorLayer& e) { return e.m_AssetIconSize; }
    static int ViewMode(EditorLayer& e) { return e.GetAssetViewMode(); }
    static std::string& Search(EditorLayer& e) { return e.m_AssetSearchFilter; }
    static bool& FavoritesOnly(EditorLayer& e) { return e.m_AssetFavoritesOnly; }
    static bool Favorite(EditorLayer& e, const std::string& key) { return e.IsAssetFavorite(key); }
    static const std::string& Renaming(EditorLayer& e) { return e.m_RenamingAssetKey; }
    static unsigned ModelThumb(EditorLayer& e, Model& m) { return e.ModelThumbnail(m); }
    static bool& ShowAnimator(EditorLayer& e) { return e.m_ShowAnimator; }
    static void ClearSelection(EditorLayer& e) { e.ClearAssetSelection(); }
    // Whether the undo step `fromTop` below the newest one only changed the selection.
    static bool SelectionStep(EditorLayer& e, size_t fromTop) {
        return fromTop < e.m_UndoStack.size() && e.m_UndoStack[e.m_UndoStack.size() - 1 - fromTop].SelectionOnly;
    }
    static float RefreshFlash(EditorLayer& e) { return e.m_AssetRefreshFlash; }
};
namespace {
namespace smoke_assets {
namespace fs = std::filesystem;
using AX = EditorSmokeAccess_Assets;
using Steps = std::vector<Step>;

EditorLayer* g_Ed = nullptr; // for item queries (they get no Ctx); set by EnsureScratch / Navigate

const char* kFolder = "editor-smoke-scratch-assets"; // the browser's folder for project/<kFolder>
const char* kSub = "editor-smoke-scratch-assets/Sub";
const char* kDeeper = "editor-smoke-scratch-assets/Sub/Deeper";
const char* kLibraryPanel = "###AssetLibrary";

std::string Dir() { return ProjectPaths::Resolve(kFolder); }
std::string DirFile(const std::string& name) { return (fs::path(Dir()) / name).generic_string(); }
std::string LibraryDir() { return (fs::path(UserPaths::Root()) / "editor-smoke-asset-library").generic_string(); }

Steps Cat(std::vector<Steps> parts) {
    Steps out;
    for (auto& p : parts) for (auto& s : p) out.push_back(std::move(s));
    return out;
}
bool HasFolder(Ctx& c, const std::string& f) {
    const auto& v = c.A.Folders();
    return std::find(v.begin(), v.end(), f) != v.end();
}

// --- scratch files ---------------------------------------------------------------------------
void WriteCubeObj(const std::string& path) {
    std::ofstream f(fs::u8path(path), std::ios::binary);
    f << "# editor smoke test cube\n";
    const float v[8][3] = {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
                           {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f}};
    for (const auto& p : v) f << "v " << p[0] << " " << p[1] << " " << p[2] << "\n";
    f << "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n";
}
void WritePng(const std::string& path, unsigned char r, unsigned char g, unsigned char b) {
    unsigned char px[16 * 16 * 4];
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            unsigned char* p = &px[(y * 16 + x) * 4];
            const bool on = ((x / 4) + (y / 4)) % 2 == 0;
            p[0] = on ? r : 30; p[1] = on ? g : 30; p[2] = on ? b : 30; p[3] = 255;
        }
    stbi_write_png(path.c_str(), 16, 16, 4, px, 16 * 4);
}
// Writes the scratch folder (unless it is already there) and waits until the browser lists it.
Steps EnsureScratch() {
    return {
        Do("write the scratch folder", [](Ctx& c) {
            g_Ed = &c.E;
            std::error_code ec;
            if (fs::exists(fs::u8path(DirFile("Sub/Deeper/Deep Tex.png")), ec) && HasFolder(c, kDeeper)) return;
            fs::remove_all(fs::u8path(Dir()), ec);
            fs::create_directories(fs::u8path(DirFile("Sub/Deeper")), ec);
            WriteCubeObj(DirFile("Smoke Cube.obj"));
            WritePng(DirFile("Smoke Tex.png"), 230, 120, 40);
            WritePng(DirFile("Sub/Sub Tex.png"), 60, 160, 230);
            WritePng(DirFile("Sub/Deeper/Deep Tex.png"), 90, 220, 90);
            // A one-object prefab (an empty named Smoke Prefab Root).
            World w;
            const entt::entity e = w.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Smoke Prefab Root");
            if (!SceneSerializer::SavePrefab(w, e, DirFile("Smoke Prefab.prefab"))) Fail("couldn't write the scratch prefab");
            AX::Refresh(c.E);
        }),
        Until("the browser lists the scratch folders", [](Ctx& c) { return HasFolder(c, kFolder) && HasFolder(c, kSub) && HasFolder(c, kDeeper); }, 900),
        Do("grid view, 64 px tiles, no search / favorites view / asset selection", [](Ctx& c) {
            EditorSettings::Get().AssetDetailsMode = false;
            AX::IconSize(c.E) = 64.0f;
            AX::Search(c.E).clear();
            AX::FavoritesOnly(c.E) = false;
            AX::ClearSelection(c.E);
        }),
        // An undo refreshes the browser, and its "Assets refreshed" note wraps the toolbar for a moment.
        Until("the Assets refreshed note is gone", [](Ctx& c) { return AX::RefreshFlash(c.E) <= 0.0f; }, 240),
    };
}

// --- browser state -----------------------------------------------------------------------------
std::string Leaf(const std::string& key) { return fs::path(key).filename().string(); }
// The grid cell whose file (or folder) is named `name` (-1 when it isn't listed).
int CellIndex(Ctx& c, const std::string& name) {
    const auto& cells = AX::Cells(c.E);
    for (int i = 0; i < (int)cells.size(); ++i)
        if (Leaf(cells[i].key) == name) return i;
    return -1;
}
std::string CellKey(Ctx& c, const std::string& name) {
    const int i = CellIndex(c, name);
    return i < 0 ? std::string() : AX::Cells(c.E)[(size_t)i].key;
}
std::string ListedNames(Ctx& c) {
    std::string out;
    for (const auto& cell : AX::Cells(c.E)) out += " '" + Leaf(cell.key) + "'";
    return out.empty() ? std::string(" (nothing)") : out;
}
Step Navigate(std::string folder) {
    return Do("browse " + (folder.empty() ? std::string("the root") : folder), [folder](Ctx& c) { g_Ed = &c.E; X::NavigateFolder(c.E, folder); });
}
Step UntilFolder(std::string folder) {
    return Until("the browser is in '" + folder + "'", [folder](Ctx& c) { return X::CurrentFolder(c.E) == folder; }, 60);
}
Step UntilListed(std::string name, int timeout = 240) {
    return Until(name + " is listed", [name](Ctx& c) { return CellIndex(c, name) >= 0; }, timeout);
}
// The grid / list child window (its tiles live in it).
ImGuiWindow* ListWindow() {
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        if (w->Active && Contains(w->Name, "##AssetList") && w->RootWindow && Contains(w->RootWindow->Name, kAssets)) return w;
    return nullptr;
}
ImGuiWindow* TreeWindow() {
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        if (w->Active && Contains(w->Name, "##AssetTree") && w->RootWindow && Contains(w->RootWindow->Name, kAssets)) return w;
    return nullptr;
}
// A grid tile: Selectable("##tile") under PushID(cell key), straight in the list window.
Query Tile(std::string name) {
    Query q;
    q.Root = kAssets;
    q.Pred = [name](const UiItem& it) {
        const ImGuiWindow* w = ListWindow();
        if (!w) return false;
        if (!g_Ed) return false;
        for (const auto& cell : AX::Cells(*g_Ed))
            if (Leaf(cell.key) == name) return it.Id == ImHashStr("##tile", 0, ImHashStr(cell.key.c_str(), 0, w->ID));
        return false;
    };
    return q;
}
// A folder tree row: TreeNodeEx("##node") under PushID(path). The row's PopID pops the node's own
// TreePush, so a child row sits under its parents' PushID(path)s alone (the root "Assets" row is
// PushID("") and always open): "a/b" is ##node in PushID("a/b") in PushID("a") in PushID("").
Query TreeRow(std::string folder) {
    Query q;
    q.Root = kAssets;
    // Clicked at the centre of its visible part: on the name, clear of the expand arrow (the row
    // spans past the tree's edge, so an AnchorX across the whole row can land in the grid).
    q.Pred = [folder](const UiItem& it) {
        const ImGuiWindow* w = TreeWindow();
        if (!w) return false;
        ImGuiID seed = ImHashStr("", 0, w->ID);
        for (size_t s = 0; s <= folder.size();) {
            const size_t slash = folder.find('/', s);
            seed = ImHashStr(folder.substr(0, slash).c_str(), 0, seed);
            if (slash == std::string::npos) break;
            s = slash + 1;
        }
        return it.Id == ImHashStr("##node", 0, seed);
    };
    return q;
}
// Scrolls `w` until the item `q` is mostly in view (an item scrolled out registers its rect but no
// usable click point). `where` estimates how far down (0..1) the item sits when it can't be found.
bool RevealIn(ImGuiWindow* w, const Query& q, float where, int& tries) {
    if (!w) return false;
    if (const UiItem* it = Find(q)) {
        ImRect vis = it->Rect;
        vis.ClipWithFull(it->Clip);
        if (vis.GetHeight() >= std::min(it->Rect.GetHeight(), it->Clip.GetHeight()) * 0.8f) return true;
        if (++tries > 60) return true; // let the click report it
        ImGui::SetScrollY(w, std::clamp(w->Scroll.y + (it->Rect.GetCenter().y - w->InnerRect.GetCenter().y), 0.0f, w->ScrollMax.y));
        return false;
    }
    if (++tries > 60) return true;
    ImGui::SetScrollY(w, std::clamp(where * (w->ScrollMax.y + w->InnerRect.GetHeight()) - w->InnerRect.GetHeight() * 0.3f,
                                    0.0f, w->ScrollMax.y));
    return false;
}
Step RevealTile(std::string name) {
    auto tries = std::make_shared<int>(0);
    return {"reveal tile " + name, [name, tries](Ctx& c) {
        const int index = CellIndex(c, name);
        if (index < 0) return false;
        const float frac = (float)index / (float)std::max<size_t>(1, AX::Cells(c.E).size());
        return RevealIn(ListWindow(), Tile(name), frac, *tries);
    }, 120};
}
Step RevealTreeRow(std::string folder) {
    auto tries = std::make_shared<int>(0);
    return {"reveal tree row " + folder, [folder, tries](Ctx& c) {
        const auto& f = c.A.Folders();
        const auto at = std::find(f.begin(), f.end(), folder);
        const float frac = at == f.end() ? 0.0f : (float)(at - f.begin()) / (float)std::max<size_t>(1, f.size());
        return RevealIn(TreeWindow(), TreeRow(folder), frac, *tries);
    }, 120};
}
// A spot on the grid's background: inside the list window (scrolled to its end), under no tile.
bool GridBackground(Ctx&, ImVec2& out) {
    ImGuiWindow* w = ListWindow();
    if (!w) return false;
    if (w->Scroll.y < w->ScrollMax.y) { ImGui::SetScrollY(w, w->ScrollMax.y); return false; }
    const ImRect r = w->InnerRect;
    for (float y = r.Max.y - 6.0f; y > r.Min.y + 6.0f; y -= 8.0f)
        for (float x = r.Max.x - 20.0f; x > r.Min.x + 6.0f; x -= 16.0f) {
            bool onItem = false;
            for (const UiItem& it : g_Reg.Last)
                if (it.Window == w->Name && it.Label != w->Name && it.Rect.GetWidth() > 20.0f && it.Rect.Contains(ImVec2(x, y))) {
                    onItem = true;
                    break;
                }
            if (!onItem) { out = ImVec2(x, y); return true; }
        }
    return false;
}
Step RightClickAt(std::string what, std::function<bool(Ctx&, ImVec2&)> where) {
    auto phase = std::make_shared<int>(0);
    return {"right-click " + what, [where, phase](Ctx& c) {
        ImGuiIO& io = ImGui::GetIO();
        switch (*phase) {
            case 0: { ImVec2 p; if (!where(c, p)) return false; MouseTo(p); *phase = 1; return false; }
            case 1: io.AddMouseButtonEvent(1, true); *phase = 2; return false;
            default: io.AddMouseButtonEvent(1, false); return true;
        }
    }, 120};
}
// GridBackground, making room when there is none. In a short panel (1920x1080 at the desktop's
// 150%) the tiles can fill every row even scrolled to the end: they shrink a step a frame until a
// spot opens up, staying in grid view. It runs as the right-click's own search, not a step before
// it, since a tile the importer adds late can fill a spot found earlier. The tiles keep that size (putting it back would reflow the
// grid under the rename field a new folder opens); every test here starts at 64 px (EnsureScratch).
bool RoomyGridBackground(Ctx& c, ImVec2& out) {
    if (GridBackground(c, out)) return true;
    ImGuiWindow* w = ListWindow();
    if (!w || w->Scroll.y < w->ScrollMax.y) return false; // GridBackground is scrolling it to the end
    AX::IconSize(c.E) -= 4.0f;
    if (AX::ViewMode(c.E) != 0) AX::IconSize(c.E) += 4.0f; // any smaller is the list view: as small as it goes
    return false;
}
// Right-click on the grid's background, then an entry of its menu.
Steps BackgroundMenu(const char* item) {
    return {RightClickAt("the grid background", RoomyGridBackground), Wait(3),
            Click(MenuItem(item)), Wait(4)};
}
// Right-click a tile, then an entry of its context menu.
Steps TileMenu(std::string name, const char* item) {
    return {RevealTile(name), RightClick(Tile(name)), Wait(3), Click(MenuItem(item)), Wait(4)};
}
// The inline rename field the browser opens (focused a frame later): type the new name, Enter.
Steps RenameTo(std::string name) {
    return {Until("the rename field is up", [](Ctx& c) { return !AX::Renaming(c.E).empty() && Find(ByExact("##rename", kAssets)); }, 60),
            Wait(3), Key(ImGuiKey_A, ImGuiMod_Ctrl), TypeText(std::move(name)), Wait(2), Key(ImGuiKey_Enter), Wait(4),
            Until("the rename is done", [](Ctx& c) { return AX::Renaming(c.E).empty(); }, 60)};
}
// The toolbar's / footer's icon buttons (ActionButton: the glyph is the label).
Query Icon(const char* glyph) { return ByExact(glyph, kAssets); }

// --- log ---------------------------------------------------------------------------------------
unsigned long long g_LogMark = 0;
Step MarkLog() {
    return Do("mark the log", [](Ctx&) { for (const LogEntry& e : Log::Entries()) g_LogMark = std::max(g_LogMark, e.Seq); });
}
Step ExpectLogged(std::string fragment) {
    return Until("logged '" + fragment + "'", [fragment](Ctx&) {
        for (const LogEntry& e : Log::Entries())
            if (e.Seq > g_LogMark && Contains(e.Message, fragment)) return true;
        return false;
    }, 120);
}
bool FileExists(const std::string& path) { std::error_code ec; return fs::exists(fs::u8path(path), ec); }
std::string ReadAll(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace smoke_assets

void RegisterAssetsTests() {
    using namespace smoke_assets;

    // ------------------------------------------------------------------ tree, breadcrumbs, history
    Add("assets: folder tree, tiles, breadcrumbs, back / forward / up", Cat({
        {FreshScene(), Wait(2)},
        EnsureScratch(),
        {Navigate(""), Wait(4),
         RevealTreeRow(kFolder), Click(TreeRow(kFolder)), Wait(4), UntilFolder(kFolder),
         UntilListed("Sub"), UntilListed("Smoke Cube.obj"), UntilListed("Smoke Tex.png"), UntilListed("Smoke Prefab.prefab"),
         Shot("scratch-folder", {kAssets}),
         RevealTile("Sub"), DoubleClick(Tile("Sub")), Wait(4), UntilFolder(kSub),
         UntilListed("Deeper"), UntilListed("Sub Tex.png"),
         Expect("only Sub's own files", [](Ctx& c) {
             return CellIndex(c, "Smoke Cube.obj") < 0 ? std::string() : "listed:" + ListedNames(c);
         }),
         // Deeper's tile is where Sub's was: past the double-click time, or ImGui counts a triple click.
         WaitSeconds(0.5), RevealTile("Deeper"), DoubleClick(Tile("Deeper")), Wait(4), UntilFolder(kDeeper),
         UntilListed("Deep Tex.png"),
         RevealTreeRow(kSub), Click(TreeRow(kSub)), Wait(4), UntilFolder(kSub),
         RevealTreeRow(kDeeper), Click(TreeRow(kDeeper)), Wait(4), UntilFolder(kDeeper),
         Shot("deeper", {kAssets}),
         // Breadcrumbs: Assets > editor-smoke-scratch-assets > Sub > Deeper.
         Click(ByExact("Sub", kAssets)), Wait(4), UntilFolder(kSub),
         Click(Icon(ICON_FA_ARROW_LEFT)), Wait(4), UntilFolder(kDeeper),
         Click(Icon(ICON_FA_ARROW_RIGHT)), Wait(4), UntilFolder(kSub),
         Click(Icon(ICON_FA_ARROW_UP)), Wait(4), UntilFolder(kFolder),
         ExpectTrue("the current folder is plain text, not a link", [](Ctx&) { return Find(ByExact(kFolder, kAssets)) == nullptr; }),
         Click(ByExact("Assets", kAssets)), Wait(4), UntilFolder(""),
         Click(Icon(ICON_FA_ARROW_LEFT)), Wait(4), UntilFolder(kFolder),
         Shot("breadcrumbs", {kAssets})},
    }));

    // ------------------------------------------------------------------ search, filters, sort
    Add("assets: search, t: filters, the filter and sort menus", Cat({
        {FreshScene(), Wait(2)},
        EnsureScratch(),
        {Navigate(kFolder), Wait(4), UntilListed("Smoke Cube.obj"),
         Click(ByExact("##search", kAssets)), Wait(3), TypeText("Smoke"), Wait(4),
         Until("search = Smoke", [](Ctx& c) { return AX::Search(c.E) == "Smoke"; }, 30),
         Expect("Smoke matches the cube, the image and the prefab, not the others", [](Ctx& c) {
             const bool ok = CellIndex(c, "Smoke Cube.obj") >= 0 && CellIndex(c, "Smoke Tex.png") >= 0 &&
                             CellIndex(c, "Smoke Prefab.prefab") >= 0 && CellIndex(c, "Sub Tex.png") < 0 && CellIndex(c, "Sub") < 0;
             return ok ? std::string() : "listed:" + ListedNames(c);
         }),
         TypeText(" t:texture"), Wait(4),
         Expect("t:texture keeps only the image", [](Ctx& c) {
             return CellIndex(c, "Smoke Tex.png") >= 0 && CellIndex(c, "Smoke Cube.obj") < 0 ? std::string() : "listed:" + ListedNames(c);
         }),
         Shot("search", {kAssets}),
         Key(ImGuiKey_A, ImGuiMod_Ctrl), Key(ImGuiKey_Backspace), Wait(3), Key(ImGuiKey_Escape), Wait(3),
         Do("clear the search", [](Ctx& c) { AX::Search(c.E).clear(); }), Wait(3),
         ExpectTrue("browsing again (Sub listed)", [](Ctx& c) { return CellIndex(c, "Sub") >= 0; }),
         // The Filter menu: Model, then Clear all filters.
         Click(Icon(ICON_FA_FILTER)), Wait(3),
         Click(MenuItem("Model")), Wait(4),
         Until("t:model in the search", [](Ctx& c) { return Contains(AX::Search(c.E), "t:model"); }, 30),
         Expect("only models (the cube) under the scratch folder", [](Ctx& c) {
             return CellIndex(c, "Smoke Cube.obj") >= 0 && CellIndex(c, "Smoke Tex.png") < 0 && CellIndex(c, "Deep Tex.png") < 0
                        ? std::string() : "listed:" + ListedNames(c);
         }),
         Click(Icon(ICON_FA_FILTER)), Wait(3),
         Shot("filter-menu", {kAssets}),
         Click(MenuItem("Clear all filters")), Wait(4),
         ExpectTrue("filters cleared", [](Ctx& c) { return AX::Search(c.E).empty(); }),
         // The Sort menu: Size, Descending; back to Name, Ascending.
         Click(Icon(ICON_FA_ARROW_DOWN_SHORT_WIDE)), Wait(3), Click(MenuItem("Size")), Wait(3),
         ExpectTrue("sorted by size", [](Ctx&) { return EditorSettings::Get().AssetSortMode == 3; }),
         Click(Icon(ICON_FA_ARROW_DOWN_SHORT_WIDE)), Wait(3), Click(MenuItem("Descending")), Wait(3),
         ExpectTrue("descending", [](Ctx&) { return EditorSettings::Get().AssetSortDesc; }),
         Expect("folders still come first", [](Ctx& c) {
             const auto& cells = AX::Cells(c.E);
             return !cells.empty() && cells.front().kind == AssetGridCell::Kind::Folder ? std::string() : "listed:" + ListedNames(c);
         }),
         Shot("sorted", {kAssets}),
         Click(Icon(ICON_FA_ARROW_DOWN_SHORT_WIDE)), Wait(3), Click(MenuItem("Name")), Wait(3),
         Click(Icon(ICON_FA_ARROW_DOWN_SHORT_WIDE)), Wait(3), Click(MenuItem("Ascending")), Wait(3),
         ExpectTrue("Name, ascending again", [](Ctx&) { return EditorSettings::Get().AssetSortMode == 0 && !EditorSettings::Get().AssetSortDesc; })},
    }));

    // ------------------------------------------------------------------ views and zoom
    {
        auto size = std::make_shared<float>(0.0f);
        Add("assets: grid, list and details views; the zoom slider", Cat({
            {FreshScene(), Wait(2)},
            EnsureScratch(),
            {Navigate(kFolder), Wait(4), UntilListed("Smoke Tex.png"),
             Do("grid view to start", [size](Ctx& c) {
                 EditorSettings::Get().AssetDetailsMode = false;
                 if (AX::IconSize(c.E) < 48.0f) AX::IconSize(c.E) = 64.0f;
                 *size = AX::IconSize(c.E);
             }), Wait(3),
             ExpectTrue("grid", [](Ctx& c) { return AX::ViewMode(c.E) == 0; }),
             Click(Icon(ICON_FA_LIST)), Wait(4),
             ExpectTrue("list view", [](Ctx& c) { return AX::ViewMode(c.E) == 1; }),
             // At CI size the panel shows three rows and the image is the fourth: scroll it in first.
             Step{"scroll the list to the image", [tries = std::make_shared<int>(0)](Ctx& c) {
                 const int index = CellIndex(c, "Smoke Tex.png");
                 if (index < 0) return false;
                 const float frac = (float)index / (float)std::max<size_t>(1, AX::Cells(c.E).size());
                 return RevealIn(ListWindow(), ByLabel("Smoke Tex", kAssets), frac, *tries);
             }, 120},
             Until("list rows (the image by name)", [](Ctx&) { return Find(ByLabel("Smoke Tex", kAssets)) != nullptr; }, 30),
             Shot("list", {kAssets}),
             Click(Icon(ICON_FA_TABLE_LIST)), Wait(4),
             ExpectTrue("details view", [](Ctx& c) { return AX::ViewMode(c.E) == 2; }),
             Until("the details header", [](Ctx&) { return Find(ByExact("TYPE", kAssets)) && Find(ByExact("MODIFIED", kAssets)); }, 30),
             Shot("details", {kAssets}),
             Click(Icon(ICON_FA_TABLE_CELLS_LARGE)), Wait(4),
             ExpectTrue("grid view again", [](Ctx& c) { return AX::ViewMode(c.E) == 0; }),
             ExpectTrue("at the zoom it had", [size](Ctx& c) { return std::fabs(AX::IconSize(c.E) - *size) < 0.5f; }),
             // Zoom: type into the slider.
             Click(ByExact("##IconSize", kAssets), ImGuiMod_Ctrl), Wait(3),
             Key(ImGuiKey_A, ImGuiMod_Ctrl), TypeText("100"), Key(ImGuiKey_Enter), Wait(4),
             ExpectTrue("tiles at 100 px", [](Ctx& c) { return std::fabs(AX::IconSize(c.E) - 100.0f) < 0.5f; }),
             Shot("zoom-100", {kAssets}),
             Click(ByExact("##IconSize", kAssets), ImGuiMod_Ctrl), Wait(3),
             Key(ImGuiKey_A, ImGuiMod_Ctrl), TypeText("48"), Key(ImGuiKey_Enter), Wait(4),
             ExpectTrue("tiles at 48 px", [](Ctx& c) { return std::fabs(AX::IconSize(c.E) - 48.0f) < 0.5f; }),
             Shot("zoom-48", {kAssets}),
             Do("back to the size it had", [size](Ctx& c) { AX::IconSize(c.E) = *size; }), Wait(2)},
        }));
    }

    // ------------------------------------------------------------------ the grid's background menu
    Add("assets: New Folder + rename, Create Material / Animator Controller / Recoil / Camera Shake / Body Locomotion", Cat({
        {FreshScene(), Wait(2)},
        EnsureScratch(),
        {Navigate(kFolder), Wait(4), UntilListed("Smoke Tex.png"),
         Do("note the undo depth", [](Ctx& c) { g_Number = (double)X::UndoDepth(c.E); })},
        BackgroundMenu("New Folder"),
        {ExpectTrue("New Folder created", [](Ctx& c) { return HasFolder(c, std::string(kFolder) + "/New Folder"); })},
        RenameTo("Smoke Folder"),
        {ExpectTrue("renamed to Smoke Folder", [](Ctx& c) {
             return HasFolder(c, std::string(kFolder) + "/Smoke Folder") && !HasFolder(c, std::string(kFolder) + "/New Folder");
         }),
         UntilListed("Smoke Folder"),
         ExpectTrue("two undo entries (create, rename)", [](Ctx& c) { return (double)X::UndoDepth(c.E) >= g_Number + 2; }),
         // Asset undo / redo re-lists the browser.
         Expect("the rename is the newest undo step", [](Ctx& c) {
             return Contains(X::UndoTop(c.E), "Rename") ? std::string() : "top '" + X::UndoTop(c.E) + "'";
         }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(4),
         Until("undo lists New Folder again", [](Ctx& c) {
             return HasFolder(c, std::string(kFolder) + "/New Folder") && CellIndex(c, "New Folder") >= 0 && CellIndex(c, "Smoke Folder") < 0;
         }, 120),
         Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(4),
         Until("redo lists Smoke Folder again", [](Ctx& c) {
             return HasFolder(c, std::string(kFolder) + "/Smoke Folder") && CellIndex(c, "Smoke Folder") >= 0 && CellIndex(c, "New Folder") < 0;
         }, 120),
         ExpectTrue("no Assets refreshed note for an undo", [](Ctx& c) { return AX::RefreshFlash(c.E) <= 0.0f; })},
        BackgroundMenu("Create Material"),
        {Until("New Material.mat written", [](Ctx&) { return FileExists(DirFile("New Material.mat")); }, 60)},
        RenameTo("Smoke Mat"),
        {Until("renamed on disk to Smoke Mat.mat", [](Ctx&) {
             return FileExists(DirFile("Smoke Mat.mat")) && !FileExists(DirFile("New Material.mat"));
         }, 60),
         UntilListed("Smoke Mat.mat")},
        BackgroundMenu("Create Animator Controller"),
        {Until("New Animator Controller.controller written", [](Ctx&) { return FileExists(DirFile("New Animator Controller.controller")); }, 60),
         ExpectTrue("it opened in the Animator", [](Ctx& c) { return AX::ShowAnimator(c.E); }),
         Do("close the Animator", [](Ctx& c) { AX::ShowAnimator(c.E) = false; }), Wait(3)},
        BackgroundMenu("Create Recoil Profile"),
        {Until("New Recoil.recoil written", [](Ctx&) { return FileExists(DirFile("New Recoil.recoil")); }, 60),
         ExpectTrue("and selected", [](Ctx& c) { return Leaf(X::AssetKey(c.E)) == "New Recoil.recoil"; })},
        BackgroundMenu("Create Camera Shake"),
        {Until("New Camera Shake.camerashake written", [](Ctx&) { return FileExists(DirFile("New Camera Shake.camerashake")); }, 60),
         ExpectTrue("and selected", [](Ctx& c) { return Leaf(X::AssetKey(c.E)) == "New Camera Shake.camerashake"; })},
        BackgroundMenu("Create Body Locomotion Controller"),
        {Until("Body Locomotion.controller written", [](Ctx&) { return FileExists(DirFile("Body Locomotion.controller")); }, 60),
         Do("close the Animator", [](Ctx& c) { AX::ShowAnimator(c.E) = false; }), Wait(3),
         // The new files show up in the folder without a manual refresh.
         UntilListed("New Animator Controller.controller", 300), UntilListed("New Recoil.recoil", 300),
         UntilListed("New Camera Shake.camerashake", 300), UntilListed("Body Locomotion.controller", 300),
         Shot("created", {kAssets})},
    }));

    // ------------------------------------------------------------------ a tile's context menu
    {
        auto clip = std::make_shared<std::string>();
        Add("assets: Edit Labels, favorites, Copy Path, Find References, Reimport, Place Instance", Cat({
            {FreshScene(), Wait(2)},
            EnsureScratch(),
            {Navigate(kFolder), Wait(4), UntilListed("Smoke Cube.obj"), UntilListed("Smoke Prefab.prefab")},
            // Labels on the image, then l: finds it.
            TileMenu("Smoke Tex.png", "Edit Labels..."),
            {Click(ByExact("##LabelsBuf")), Wait(3), Key(ImGuiKey_A, ImGuiMod_Ctrl), TypeText("smokelabel"), Wait(2),
             Click(PopupItem("Apply")), Wait(4),
             Expect("labelled", [](Ctx& c) {
                 const auto& l = c.A.Labels(CellKey(c, "Smoke Tex.png"));
                 return l.count("smokelabel") ? std::string() : std::string("no smokelabel label");
             }),
             Do("search l:smokelabel", [](Ctx& c) { AX::Search(c.E) = "l:smokelabel"; }), Wait(4),
             Expect("l:smokelabel finds just the image", [](Ctx& c) {
                 return AX::Cells(c.E).size() == 1 && CellIndex(c, "Smoke Tex.png") == 0 ? std::string() : "listed:" + ListedNames(c);
             }),
             Do("clear the search", [](Ctx& c) { AX::Search(c.E).clear(); }), Wait(4),
             // Undo takes the label off again.
             Expect("Edit Labels is the newest undo step", [](Ctx& c) {
                 return Contains(X::UndoTop(c.E), "Label") ? std::string() : "top '" + X::UndoTop(c.E) + "'";
             }),
             Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(4),
             Until("undo removes the label", [](Ctx& c) { return c.A.Labels(CellKey(c, "Smoke Tex.png")).count("smokelabel") == 0; }, 60),
             Do("search l:smokelabel", [](Ctx& c) { AX::Search(c.E) = "l:smokelabel"; }), Wait(4),
             Expect("l:smokelabel finds nothing", [](Ctx& c) { return AX::Cells(c.E).empty() ? std::string() : "listed:" + ListedNames(c); }),
             Do("clear the search", [](Ctx& c) { AX::Search(c.E).clear(); }), Wait(4)},
            // Favorites: add, favorites-only view, remove.
            TileMenu("Smoke Cube.obj", "Add to Favorites"),
            {ExpectTrue("a favorite", [](Ctx& c) { return AX::Favorite(c.E, CellKey(c, "Smoke Cube.obj")); }),
             Click(Icon(ICON_FA_STAR)), Wait(4),
             ExpectTrue("favorites only", [](Ctx& c) { return AX::FavoritesOnly(c.E); }),
             Expect("the cube is listed among the favorites", [](Ctx& c) {
                 return CellIndex(c, "Smoke Cube.obj") >= 0 && CellIndex(c, "Smoke Tex.png") < 0 ? std::string() : "listed:" + ListedNames(c);
             }),
             Shot("favorites", {kAssets}),
             Click(Icon(ICON_FA_STAR)), Wait(4),
             ExpectTrue("all assets again", [](Ctx& c) { return !AX::FavoritesOnly(c.E); })},
            TileMenu("Smoke Cube.obj", "Remove from Favorites"),
            {ExpectTrue("not a favorite", [](Ctx& c) { return !AX::Favorite(c.E, CellKey(c, "Smoke Cube.obj")); }),
             // Copy Path (the system clipboard is put back afterwards).
             Do("keep the clipboard", [clip](Ctx&) { const char* t = ImGui::GetClipboardText(); *clip = t ? t : ""; })},
            TileMenu("Smoke Cube.obj", "Copy Path"),
            {Expect("the path is on the clipboard", [](Ctx& c) {
                 const char* t = ImGui::GetClipboardText();
                 return t && std::string(t) == CellKey(c, "Smoke Cube.obj") ? std::string() : "clipboard: " + std::string(t ? t : "");
             }),
             Do("put the clipboard back", [clip](Ctx&) { ImGui::SetClipboardText(clip->c_str()); }),
             MarkLog()},
            TileMenu("Smoke Cube.obj", "Find References in Scene"),
            {ExpectLogged("No objects in this scene use"), MarkLog()},
            TileMenu("Smoke Tex.png", "Find References in Project"),
            {ExpectLogged("No scene, prefab, material or controller in the project references"), MarkLog()},
            TileMenu("Smoke Cube.obj", "Reimport"),
            {ExpectLogged("Reimported model")},
            // Place Instance from the prefab's menu.
            TileMenu("Smoke Prefab.prefab", "Place Instance"),
            {Until("an instance is placed and selected", [](Ctx& c) {
                 const entt::entity e = X::Selected(c.E);
                 return e != entt::null && c.W.Registry.all_of<PrefabInstanceComponent>(e) && Contains(NameOf(c.W, e), "Smoke Prefab Root");
             }, 60),
             MarkLog()},
            // Now the scene uses the prefab: Find References in Scene selects it.
            {Do("deselect", [](Ctx& c) { X::Deselect(c.E); }), Wait(2)},
            TileMenu("Smoke Prefab.prefab", "Find References in Scene"),
            {ExpectLogged("Selected 1 object(s) using"),
             ExpectTrue("the instance is selected", [](Ctx& c) { return Contains(NameOf(c.W, X::Selected(c.E)), "Smoke Prefab Root"); }),
             // Selection changes are undo steps: Find References' select and the deselect come first.
             ExpectTrue("the two newest undo steps are selections", [](Ctx& c) { return AX::SelectionStep(c.E, 0) && AX::SelectionStep(c.E, 1); }),
             Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(4), Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(4),
             ExpectTrue("the instance is still there", [](Ctx& c) { return ByName(c.W, "Smoke Prefab Root") != entt::null; }),
             Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(4),
             Until("the third undo removes the instance", [](Ctx& c) { return ByName(c.W, "Smoke Prefab Root") == entt::null; }, 60)},
        }));
    }

    // ------------------------------------------------------------------ drag a model in
    Add("assets: drag a model into the Scene and into the Hierarchy", Cat({
        {FreshScene(), Wait(2),
         Do("Scene tab in front", [](Ctx& c) { c.E.RequestSceneTabFocus(); }), Wait(3)},
        EnsureScratch(),
        {Navigate(kFolder), Wait(4), UntilListed("Smoke Cube.obj"),
         RevealTile("Smoke Cube.obj"),
         Click(Tile("Smoke Cube.obj")), Wait(4), // loads it (an unloaded file is loaded on its first click)
         Until("the cube is loaded", [](Ctx& c) {
             const int i = CellIndex(c, "Smoke Cube.obj");
             return i >= 0 && AX::Cells(c.E)[(size_t)i].model != nullptr;
         }, 300),
         RevealTile("Smoke Cube.obj"),
         Drag(Tile("Smoke Cube.obj"), AtItem(ByExact("##viewport_drop_zone"))), Wait(5),
         Until("Smoke Cube placed in the scene", [](Ctx& c) {
             const entt::entity e = ByName(c.W, "Smoke Cube");
             return e != entt::null && c.W.Registry.all_of<RenderableComponent>(e) && X::Selected(c.E) == e;
         }, 120),
         HoverPanel(kScene), WaitSeconds(0.3),
         Shot("in-scene", {kScene, kHierarchy}),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(4),
         Until("undo removes it", [](Ctx& c) { return ByName(c.W, "Smoke Cube") == entt::null; }, 60),
         RevealTile("Smoke Cube.obj"),
         Drag(Tile("Smoke Cube.obj"), [](Ctx&, ImVec2& out) {
             // Below the Hierarchy's rows: its empty drop zone (instantiates at the root).
             for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
                 if (w->Active && !w->Hidden && Contains(w->Name, kHierarchy) && !Contains(w->Name, "/")) {
                     out = ImVec2(w->InnerRect.GetCenter().x, w->InnerRect.Max.y - 30.0f);
                     return true;
                 }
             return false;
         }), Wait(5),
         Until("Smoke Cube created from the Hierarchy drop", [](Ctx& c) {
             const entt::entity e = ByName(c.W, "Smoke Cube");
             return e != entt::null && c.W.Registry.all_of<RenderableComponent>(e);
         }, 120),
         Expect("at the root", [](Ctx& c) {
             const auto* h = c.W.Registry.try_get<HierarchyComponent>(ByName(c.W, "Smoke Cube"));
             return !h || h->Parent == entt::null ? std::string() : std::string("it was parented");
         }),
         Until("listed in the Hierarchy", [](Ctx&) { return Find(ByTag("row:Smoke Cube")) != nullptr; }, 60),
         Shot("in-hierarchy", {kScene, kHierarchy}),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(4),
         Until("undo removes it", [](Ctx& c) { return ByName(c.W, "Smoke Cube") == entt::null; }, 60)},
    }));

    // ------------------------------------------------------------------ thumbnails and inspectors
    {
        auto before = std::make_shared<std::string>();
        Add("assets: thumbnails, asset inspectors, a material edit saved and undone", Cat({
            {FreshScene(), Wait(2)},
            EnsureScratch(),
            {Navigate(kFolder), Wait(4), UntilListed("Smoke Cube.obj"),
             Do("load the cube", [](Ctx& c) { c.A.LoadModel(CellKey(c, "Smoke Cube.obj")); }),
             Until("the cube's tile has a rendered thumbnail", [](Ctx& c) {
                 const int i = CellIndex(c, "Smoke Cube.obj");
                 if (i < 0) return false;
                 const auto& cell = AX::Cells(c.E)[(size_t)i];
                 return cell.model && AX::ModelThumb(c.E, *cell.model) != 0;
             }, 300),
             Until("the image's tile shows the image", [](Ctx& c) {
                 const int i = CellIndex(c, "Smoke Tex.png");
                 return i >= 0 && (AX::Cells(c.E)[(size_t)i].texture || AX::Cells(c.E)[(size_t)i].unloaded);
             }, 120),
             WaitSeconds(0.5),
             Shot("thumbnails", {kAssets}),
             // An inspector per kind.
             RevealTile("Smoke Cube.obj"), Click(Tile("Smoke Cube.obj")), Wait(5),
             ExpectTrue("the cube is the selected asset", [](Ctx& c) { return Leaf(X::AssetKey(c.E)) == "Smoke Cube.obj"; }),
             Shot("model-inspector", {kInspector}),
             RevealTile("Smoke Tex.png"), Click(Tile("Smoke Tex.png")), Wait(5),
             Shot("texture-inspector", {kInspector}),
             RevealTile("Smoke Prefab.prefab"), Click(Tile("Smoke Prefab.prefab")), Wait(5),
             Shot("prefab-inspector", {kInspector})},
            // A material: write one (the background menu's own path), edit it in its inspector.
            BackgroundMenu("Create Material"),
            {Key(ImGuiKey_Escape), Wait(3), // leave the rename
             Until("a new material file", [](Ctx& c) { return Contains(Leaf(X::AssetKey(c.E)), ".mat") && FileExists(X::AssetKey(c.E)); }, 60),
             Do("note it", [](Ctx& c) { g_Text = X::AssetKey(c.E); }),
             Do("select it", [](Ctx& c) { X::SelectAsset(c.E, g_Text); }), Wait(5),
             Until("its inspector", [](Ctx&) { return Find(ByExact("##f", kInspector)) != nullptr; }, 60),
             Shot("material-inspector", {kInspector}),
             InspReveal(ByExact("##f", kInspector)),
             Do("keep the file as it is before the edit", [before](Ctx&) { *before = ReadAll(g_Text); }),
             Drag(ByExact("##f", kInspector), [](Ctx&, ImVec2& out) {
                 const UiItem* it = Find(ByExact("##f", kInspector));
                 if (!it) return false;
                 out = ImVec2(it->Rect.Min.x + it->Rect.GetWidth() * 0.85f, it->Rect.GetCenter().y);
                 return true;
             }), Wait(6),
             Until("the edit is saved to the .mat", [before](Ctx&) { return ReadAll(g_Text) != *before; }, 60),
             Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(6),
             Until("undo puts the file back", [before](Ctx&) { return ReadAll(g_Text) == *before; }, 60),
             Do("deselect", [](Ctx& c) { X::Deselect(c.E); }), Wait(2)},
        }));
    }

    // ------------------------------------------------------------------ Asset Library panel
    Add("assets: the Asset Library panel browses and searches a library folder", {
        FreshScene(), Wait(2),
        Do("write a small library and point the panel at it", [](Ctx&) {
            std::error_code ec;
            fs::remove_all(fs::u8path(LibraryDir()), ec);
            const fs::path crate = fs::u8path(LibraryDir()) / "Props" / "Crate";
            const fs::path barrel = fs::u8path(LibraryDir()) / "Props" / "Barrel";
            fs::create_directories(crate, ec);
            fs::create_directories(barrel, ec);
            WriteCubeObj((crate / "crate.obj").generic_string());
            WritePng((crate / "crate_preview.png").generic_string(), 200, 160, 90);
            WriteCubeObj((barrel / "barrel.obj").generic_string());
            EditorSettings::Get().AssetLibraryPath = LibraryDir();
            EditorSettings::Get().ShowAssetLibrary = true;
        }),
        Until("the Asset Library is shown", [](Ctx&) { return ChromeUi::Shown(EditorPanels::AssetLibrary); }, 120), Wait(6),
        Until("it lists Props", [](Ctx&) { return Find(ByLabel("Props", kLibraryPanel)) != nullptr; }, 60),
        Shot("library", {kLibraryPanel}),
        Click(ByExact("##libsearch", kLibraryPanel)), Wait(3), TypeText("crate"), Wait(4),
        Click(ByLabel("Props/Crate", kLibraryPanel)), Wait(4),
        // The summary (a preview image, the counts) runs below the panel: its Import button is
        // under the image, so scroll the details pane to its end.
        Until("Crate's summary, scrolled to its Import button", [](Ctx&) {
            for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
                if (w->Active && Contains(w->Name, "##libdetail") && w->Scroll.y < w->ScrollMax.y) ImGui::SetScrollY(w, w->ScrollMax.y);
            return Find(ByLabel("Import 'Crate'", kLibraryPanel)) != nullptr;
        }, 60),
        Shot("crate", {kLibraryPanel}), // the Import button isn't clicked: it copies into the project
        Do("close the panel and delete the library", [](Ctx&) {
            EditorSettings::Get().ShowAssetLibrary = false;
            EditorSettings::Get().AssetLibraryPath.clear();
            std::error_code ec;
            fs::remove_all(fs::u8path(LibraryDir()), ec);
        }),
        Until("the Asset Library is gone", [](Ctx&) { return !ChromeUi::Shown(EditorPanels::AssetLibrary); }, 60),
    });

    // ------------------------------------------------------------------ cleanup: Delete Folder
    Add("assets: Delete Folder removes the scratch folder (cleanup)", {
        FreshScene(), Wait(2),
        Do("browser state back to normal", [](Ctx& c) {
            AX::Search(c.E).clear();
            AX::FavoritesOnly(c.E) = false;
            AX::ShowAnimator(c.E) = false;
        }),
        Navigate(""), Wait(4),
        Do("find the folder (search for it)", [](Ctx& c) { AX::Search(c.E) = kFolder; }), Wait(4),
        UntilListed(kFolder, 120),
        RevealTile(kFolder), RightClick(Tile(kFolder)), Wait(3),
        Click(MenuItem("Delete Folder")), Wait(4),
        Until("the Delete Asset? prompt", [](Ctx&) { return Find(ByExact("Delete", "Delete Asset?")) != nullptr; }, 60),
        Shot("prompt"),
        Click(ByExact("Delete", "Delete Asset?")), Wait(4),
        Until("the folder left the browser", [](Ctx& c) { return !HasFolder(c, kFolder) && !HasFolder(c, kSub); }, 60),
        Do("delete it from disk and rescan", [](Ctx& c) {
            AX::Search(c.E).clear();
            std::error_code ec;
            fs::remove_all(fs::u8path(Dir()), ec);
            AX::Refresh(c.E);
        }), WaitSeconds(1.0),
        ExpectTrue("gone from disk", [](Ctx&) { return !FileExists(Dir()); }),
    });
}
