// Editor smoke suite: Panels. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder). Uses the ChromeUi helpers from Menus.inl (included before this).
//
// Every dockable panel (EditorPanels.h) and the other editor windows, opened the way a user opens
// them (Window menu, shortcut, the title bar's toggles, the status bar), shot non-blank, their main
// controls used lightly, closed with their own x, and checked gone. Also the title bar's Play
// transport, the maximized-play bar and the window-wide status bar.

void RegisterPanelsTests() {
    using namespace ChromeUi;
    const char* kBar = "##Toolbar";               // the title bar: transport + panel toggles
    const char* kMaxBar = "##ViewportActionBar";  // the slim transport over maximized play
    const char* kStatus = "##StatusBar";

    // ---------------------------------------------------------------------------------- chrome
    Add("panels: Scene, title bar, viewport header and status bar are drawn", {
        FreshScene(), Wait(3),
        UntilShown(EditorPanels::Scene),
        Shot("editor", {"###Scene", "###Hierarchy", "###Inspector", "###Assets", "##Toolbar", kStatus}),
        Expect("the title bar's controls", [kBar](Ctx&) {
            for (const char* icon : {EDITOR_ICON_STATISTICS, EDITOR_ICON_CONSOLE, EDITOR_ICON_HISTORY})
                if (!Find(ByExact(icon, kBar))) return std::string("missing an icon button");
            return Find(ByLabel("Play", kBar)) ? std::string() : std::string("no Play");
        }),
        ExpectTrue("nothing floats over the scene (no action bar while windowed)", [kMaxBar](Ctx&) {
            ImGuiWindow* w = ImGui::FindWindowByName(kMaxBar);
            return !w || !w->WasActive || w->Hidden;
        }),
        ExpectTrue("the viewport header's tools", [](Ctx&) {
            for (const char* icon : {EDITOR_ICON_HAND_TOOL, EDITOR_ICON_TRANSLATE, EDITOR_ICON_ROTATE, EDITOR_ICON_SCALE})
                if (!Find(ByExact(icon, "###Scene"))) return false;
            return true;
        }),
        ExpectTrue("the status bar's segments", [kStatus](Ctx&) {
            return FindAll(ByExact("##seg", kStatus)).size() >= 5 && Find(ByExact("##lastlog", kStatus)) != nullptr;
        }),
        ExpectTrue("the menu bar", [](Ctx&) {
            for (const char* m : {"File", "Edit", "Create", "View", "Window", "Help"}) if (!Find(TopMenu(m))) return false;
            return true;
        }),
    });

    // ---------------------------------------------------------------------------------- core panels
    {
        struct Core { const char* Item; const char* Window; bool& (*Flag)(EditorLayer&); };
        static const Core kCore[] = {
            {"Hierarchy", EditorPanels::Hierarchy, &A::ShowHierarchy},
            {"Inspector", EditorPanels::Inspector, &A::ShowInspector},
            {"Assets", EditorPanels::Assets, &A::ShowAssets},
        };
        Steps s = {FreshScene(), Wait(2), SelectByName("Beta"), Wait(3)};
        for (const Core& k : kCore) {
            const Core core = k;
            s.push_back(BringToFront(core.Window));
            s.push_back(UntilShown(core.Window));
            s.push_back(Shot(core.Item, {core.Window}));
            s.push_back(CloseByX(core.Window));
            s.push_back(Until(std::string(core.Item) + " closed by its x", [core](Ctx& c) { return !core.Flag(c.E) && Gone(core.Window); }));
            for (Step& st : MenuClick("Window", "Hierarchy", core.Item)) s.push_back(st);
            s.push_back(Until(std::string(core.Item) + " back from the Window menu", [core](Ctx& c) { return core.Flag(c.E) && Open(core.Window); }));
            s.push_back(BringToFront(core.Window));
            s.push_back(UntilShown(core.Window));
        }
        Add("panels: Hierarchy, Inspector and Assets close with their x and come back", s);
    }

    Add("panels: the viewport's Game switch shows the game view in the same rect; Scene comes back", {
        FreshScene(), Wait(2),
        Do("note the viewport size", [](Ctx& c) { g_Number = c.E.ViewportSize().x; }),
        Click(ByLabel("Game", "###Scene")),
        Until("the game is showing", [](Ctx& c) {
            return c.E.GetViewportMode() == EditorLayer::ViewportMode::Game && !c.E.ViewFadeActive();
        }),
        Wait(5),
        Shot("game", {"###Scene"}),
        ExpectTrue("no separate Game window", [](Ctx&) { return !Shown(EditorPanels::Game); }),
        ExpectTrue("Play stays in the title bar over the Game view", [kBar](Ctx&) { return Find(ByLabel("Play", kBar)) != nullptr; }),
        Click(ByLabel("Scene", "###Scene")),
        Until("the scene is back, the same size", [](Ctx& c) {
            return c.E.GetViewportMode() == EditorLayer::ViewportMode::Scene && !c.E.ViewFadeActive() &&
                   std::abs(c.E.ViewportSize().x - (float)g_Number) < 1.0f;
        }),
    });

    // ---------------------------------------------------------------------------------- Console
    {
        auto consoleState = []() -> EditorConsoleState& { return EditorModuleHost::ConsoleState(); };
        auto rows = []() { return (int)FindAll(ByExact("##row", "###Console")).size(); };
        auto levelButton = [](const char* tip) {
            Query q;
            q.Root = "###Console";
            const std::string suffix = std::string("##") + tip;
            q.Pred = [suffix](const UiItem& it) { return Contains(it.Label, suffix); };
            return q;
        };
        Add("panels: Console opens from the action bar; level toggles, text filter, clear", {
            FreshScene(), Wait(2),
            Do("log the marker lines", [](Ctx&) {
                Log::Info("smoke-console-marker one");
                Log::Info("smoke-console-marker two");
                Log::Warn("smoke-console-marker three");
            }),
            Click(ByExact(EDITOR_ICON_CONSOLE, kBar)),
            Until("console open", [consoleState](Ctx&) { return consoleState().Visible && Open(EditorPanels::Console); }),
            BringToFront(EditorPanels::Console), UntilShown(EditorPanels::Console),
            Shot("console", {"###Console"}),
            Click(ByExact("##search", "###Console")), Wait(2),
            TypeText("smoke-console-marker"), Wait(3),
            Until("the filter keeps the three marker lines", [rows](Ctx&) { return rows() == 3; }),
            Click(levelButton("Show / hide warnings")), Wait(3),
            ExpectTrue("warnings hidden", [consoleState](Ctx&) { return !consoleState().ShowWarning; }),
            Until("two rows", [rows](Ctx&) { return rows() == 2; }),
            Click(levelButton("Show / hide messages")), Wait(3),
            Until("no rows", [rows](Ctx&) { return rows() == 0; }),
            Shot("console-filtered", {"###Console"}),
            Click(levelButton("Show / hide messages")), Wait(2),
            Click(levelButton("Show / hide warnings")), Wait(2),
            Click(levelButton("Show / hide errors")), Wait(2),
            ExpectTrue("errors hidden", [consoleState](Ctx&) { return !consoleState().ShowError; }),
            Click(levelButton("Show / hide errors")), Wait(2),
            ExpectTrue("every level shown", [consoleState](Ctx&) { return consoleState().ShowInfo && consoleState().ShowWarning && consoleState().ShowError; }),
            Until("three rows again", [rows](Ctx&) { return rows() == 3; }),
            Click(RightEnd(ByExact("##search", "###Console"))), Wait(3), // the x inside the field
            ExpectTrue("filter cleared", [consoleState](Ctx&) { return consoleState().Filter[0] == '\0'; }),
            Click(ByExact(ICON_FA_TRASH_CAN, "###Console")), Wait(3),
            ExpectTrue("cleared", [](Ctx&) {
                for (const LogEntry& e : Log::Entries()) if (Contains(e.Message, "smoke-console-marker")) return false;
                return true;
            }),
            Shot("console-cleared", {"###Console"}),
            Key(ImGuiKey_C, ImGuiMod_Ctrl | ImGuiMod_Shift), // console.toggle: closes the front Console
            Until("Ctrl+Shift+C closed it", [consoleState](Ctx&) { return !consoleState().Visible && Gone(EditorPanels::Console); }),
        });

        Add("panels: Console options popup and a row's context menu", {
            FreshScene(), Wait(2),
            Do("log a marker", [](Ctx&) { Log::Info("smoke-console-context"); }),
            Key(ImGuiKey_C, ImGuiMod_Ctrl | ImGuiMod_Shift),
            Until("console open", [consoleState](Ctx&) { return consoleState().Visible && Open(EditorPanels::Console); }),
            BringToFront(EditorPanels::Console), UntilShown(EditorPanels::Console),
            Do("note timestamps", [consoleState](Ctx&) { g_Number = consoleState().ShowTimestamps ? 1.0 : 0.0; }),
            Click(ByExact(ICON_FA_SLIDERS, "###Console")),
            Until("options open", [](Ctx&) { return Find(MenuItem("Timestamps")) != nullptr; }),
            Shot("options", {"###Console"}),
            Expect("options listed", [](Ctx&) {
                for (const char* o : {"Auto-scroll", "Timestamps", "Collapse", "Clear on Play", "Error Pause"})
                    if (!Find(MenuItem(o))) return std::string("missing ") + o;
                return std::string();
            }),
            Click(MenuItem("Timestamps")), Wait(3),
            ExpectTrue("timestamps toggled", [consoleState](Ctx&) { return consoleState().ShowTimestamps != Noted(); }),
            Click(ByExact(ICON_FA_SLIDERS, "###Console")),
            Click(MenuItem("Timestamps")), Wait(3),
            ExpectTrue("timestamps restored", [consoleState](Ctx&) { return consoleState().ShowTimestamps == Noted(); }),
            Click(ByExact("##search", "###Console")), Wait(2),
            TypeText("smoke-console-context"), Wait(3),
            Until("one row", [rows](Ctx&) { return rows() == 1; }),
            RightClick(ByExact("##row", "###Console")),
            Until("the row menu", [](Ctx&) { return Find(MenuItem("Copy message")) != nullptr; }),
            ExpectTrue("Copy all shown / Clear console listed", [](Ctx&) { return Find(MenuItem("Copy all shown")) && Find(MenuItem("Clear console")); }),
            Click(MenuItem("Copy message")), Wait(3),
            ExpectTrue("copied", [](Ctx&) { const char* t = ImGui::GetClipboardText(); return t && Contains(t, "smoke-console-context"); }),
            Click(RightEnd(ByExact("##search", "###Console"))), Wait(2),
            CloseByX(EditorPanels::Console),
            Until("closed by its x", [consoleState](Ctx&) { return !consoleState().Visible && Gone(EditorPanels::Console); }),
        });
    }

    // ---------------------------------------------------------------------------------- Statistics
    {
        // The draws / triangles segment: second from the right of the status bar.
        Query drawsSegment;
        drawsSegment.Root = kStatus;
        drawsSegment.Pred = [kStatus](const UiItem& it) {
            if (it.Label != "##seg") return false;
            std::vector<float> xs;
            for (const UiItem& o : g_Reg.Last) if (o.Label == "##seg" && Contains(o.Root, kStatus)) xs.push_back(o.Rect.Min.x);
            std::sort(xs.begin(), xs.end());
            return xs.size() >= 2 && it.Rect.Min.x == xs[xs.size() - 2];
        };
        Add("panels: Statistics opens from the status bar, closes with its x", {
            FreshScene(), Wait(2),
            Do("stats hidden", [](Ctx&) { EditorSettings::Get().SceneShowStats = false; }), Wait(3),
            Click(drawsSegment),
            Until("Statistics open", [](Ctx&) { return EditorSettings::Get().SceneShowStats && Open(EditorPanels::Statistics); }),
            BringToFront(EditorPanels::Statistics), UntilShown(EditorPanels::Statistics), Wait(5),
            Shot("statistics", {"###Statistics"}),
            CloseByX(EditorPanels::Statistics),
            Until("closed", [](Ctx&) { return !EditorSettings::Get().SceneShowStats && Gone(EditorPanels::Statistics); }),
            UntilShown(EditorPanels::Inspector), // its dock-mate is the front tab again
        });
    }

    // ---------------------------------------------------------------------------------- History
    Add("panels: History (Ctrl+Shift+H) lists an edit; its rows jump undo / redo", Seq({
        {FreshScene(), Wait(2)},
        MenuClick("Create", "Cube", "Cube"),
        {Until("a cube", [](Ctx& c) { return ByName(c.W, "Cube") != entt::null; }),
         Key(ImGuiKey_H, ImGuiMod_Ctrl | ImGuiMod_Shift),
         Until("History open", [](Ctx& c) { return A::ShowHistory(c.E) && Open(EditorPanels::History); }),
         BringToFront(EditorPanels::History), UntilShown(EditorPanels::History),
         Until("the edit is listed", [](Ctx&) { return Find(ByLabel("Create Cube", "###History")) && Find(ByLabel("Current", "###History")); }),
         Shot("history", {"###History"}),
         Click(ByLabel("Create Cube", "###History")),
         Until("jumped back before the cube", [](Ctx& c) { return ByName(c.W, "Cube") == entt::null && A::RedoDepth(c.E) >= 1; }),
         Until("the redo row", [](Ctx&) { return Find(ByLabel("Create Cube", "###History")) != nullptr; }),
         Click(ByLabel("Create Cube", "###History")),
         Until("jumped forward to the cube", [](Ctx& c) { return ByName(c.W, "Cube") != entt::null; }),
         Key(ImGuiKey_H, ImGuiMod_Ctrl | ImGuiMod_Shift),
         Until("Ctrl+Shift+H closed it", [](Ctx& c) { return !A::ShowHistory(c.E) && Gone(EditorPanels::History); })},
    }));

    // ---------------------------------------------------------------------------------- Lighting
    Add("panels: Lighting (Ctrl+Shift+L): sections, Solo, picking a light, its x", {
        FreshScene(), Wait(2),
        Do("no solo / mute", [](Ctx& c) { A::ClearSoloMute(c.E); }),
        Key(ImGuiKey_L, ImGuiMod_Ctrl | ImGuiMod_Shift),
        Until("Lighting open", [](Ctx& c) { return A::ShowLighting(c.E) && Open(EditorPanels::Lighting); }),
        BringToFront(EditorPanels::Lighting), UntilShown(EditorPanels::Lighting), Wait(3),
        Shot("lighting", {"###Lighting"}),
        Do("scroll to the Lights section", [](Ctx&) {
            if (ImGuiWindow* w = Win(EditorPanels::Lighting)) ImGui::SetScrollY(w, w->ScrollMax.y);
        }), Wait(4),
        Until("both lights listed", [](Ctx&) { return Find(ByLabel("Beta", "###Lighting")) && Find(ByLabel("Gamma", "###Lighting")); }),
        Shot("lighting-lights", {"###Lighting"}),
        Click(ByExact("S", "###Lighting")), Wait(3),
        ExpectTrue("one light soloed", [](Ctx& c) { return A::SoloCount(c.E) == 1; }),
        Click(ByExact("S", "###Lighting")), Wait(3),
        ExpectTrue("solo off", [](Ctx& c) { return A::SoloCount(c.E) == 0; }),
        Click(ByLabel("Gamma", "###Lighting")), Wait(3),
        Expect("the row selected Gamma", [](Ctx& c) { return ExpectSelected(c, "Gamma"); }),
        CloseByX(EditorPanels::Lighting),
        Until("closed", [](Ctx& c) { return !A::ShowLighting(c.E) && Gone(EditorPanels::Lighting); }),
        Do("no solo / mute", [](Ctx& c) { A::ClearSoloMute(c.E); }),
    });

    // The Volumetric Fog section: its checkbox switches the froxel passes on for the Scene view (Beta's and Gamma's
    // lights scatter into it), its controls show, and undo switches it off again.
    Add("panels: Lighting > Volumetric Fog: the checkbox runs the froxel fog in the Scene view; undo", {
        FreshScene(), Wait(2),
        Do("a floor under the lights", [](Ctx& c) {
            c.W.CreateBox(glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(20.0f, 1.0f, 20.0f), glm::vec3(0.5f), glm::vec3(0.0f), "Floor");
        }),
        Key(ImGuiKey_L, ImGuiMod_Ctrl | ImGuiMod_Shift),
        Until("Lighting open", [](Ctx& c) { return A::ShowLighting(c.E) && Open(EditorPanels::Lighting); }),
        BringToFront(EditorPanels::Lighting), UntilShown(EditorPanels::Lighting), Wait(3),
        ExpectTrue("off in a new scene", [](Ctx& c) { return !c.W.VolumetricEnabled; }),
        InspReveal(ByLabel("Volumetric fog", "###Lighting")),
        Do("note the froxel passes run so far", [](Ctx&) { g_Number = (double)VolumetricFog::RenderCount(); }),
        Click(ByLabel("Volumetric fog", "###Lighting")), Wait(3),
        ExpectTrue("on", [](Ctx& c) { return c.W.VolumetricEnabled; }),
        Wait(8),
        ExpectTrue("the Scene view ran the froxel passes", [](Ctx&) { return (double)VolumetricFog::RenderCount() > g_Number; }),
        Shot("volumetric fog", {"###Scene", "###Lighting"}),
        // Its last control, below the fold of the panel (a clipped widget isn't registered until it's scrolled to).
        InspReveal(ByExact("Quality", "###Lighting")),
        Click(ByExact("Quality", "###Lighting")), Wait(2),
        Click(ByLabel("High", "##Combo")), Wait(4),
        ExpectTrue("Quality = High", [](Ctx& c) { return c.W.VolumetricQuality == 2; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> Medium", [](Ctx& c) { return c.W.VolumetricQuality == 1 && c.W.VolumetricEnabled; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> off", [](Ctx& c) { return !c.W.VolumetricEnabled; }),
        CloseByX(EditorPanels::Lighting),
        Until("closed", [](Ctx& c) { return !A::ShowLighting(c.E) && Gone(EditorPanels::Lighting); }),
    });

    // ---------------------------------------------------------------------------------- Physics Debug
    Add("panels: Physics Debug: draw channels and collider toggles persist; its x", Seq({
        {FreshScene(), Wait(2),
         Do("note", [](Ctx&) {
             g_Number = EditorSettings::Get().ShowColliders ? 1.0 : 0.0;
             g_Vec.x = (float)EditorSettings::Get().PhysicsDebugDrawFlags; // small bit set: exact in a float
         })},
        MenuClick("Window", "Hierarchy", "Physics Debug"),
        {UntilShown(EditorPanels::PhysicsDebug), Wait(3),
         Shot("physics", {"###PhysicsDebug"}),
         ExpectTrue("Enter Play offered outside Play", [](Ctx&) { return Find(ByLabel("Enter Play", "###PhysicsDebug")) != nullptr; }),
         Click(ByLabel("Contacts & impacts", "###PhysicsDebug")), Wait(3),
         ExpectTrue("contacts channel toggled", [](Ctx&) { return EditorSettings::Get().PhysicsDebugDrawFlags != (unsigned)g_Vec.x; }),
         Until("persisted", [](Ctx&) { return Prefs().value("physicsDebugDrawFlags", 0u) == EditorSettings::Get().PhysicsDebugDrawFlags; }),
         Click(ByLabel("Contacts & impacts", "###PhysicsDebug")), Wait(3),
         ExpectTrue("contacts channel restored", [](Ctx&) { return EditorSettings::Get().PhysicsDebugDrawFlags == (unsigned)g_Vec.x; }),
         Click(ByLabel("Collider shapes", "###PhysicsDebug")), Wait(3),
         ExpectTrue("colliders toggled", [](Ctx&) { return EditorSettings::Get().ShowColliders != Noted(); }),
         UntilPrefBool("showColliders", [] { return !Noted(); }),
         Click(ByLabel("Collider shapes", "###PhysicsDebug")), Wait(3),
         ExpectTrue("colliders restored", [](Ctx&) { return EditorSettings::Get().ShowColliders == Noted(); }),
         Do("scroll to the time-scale row", [](Ctx&) { // below the fold; clipped widgets carry no label
             if (ImGuiWindow* w = Win(EditorPanels::PhysicsDebug)) ImGui::SetScrollY(w, w->ScrollMax.y);
         }), Wait(3),
         Click(ByExact("1x", "###PhysicsDebug")), Wait(2),
         ExpectTrue("1x", [](Ctx&) { return EditorSettings::Get().PhysicsSimTimeScale == 1.0f; }),
         ExpectTrue("Step disabled outside Play", [](Ctx&) {
             const UiItem* it = Find(ByExact("Step", "###PhysicsDebug"));
             return it && (it->ItemFlags & ImGuiItemFlags_Disabled);
         }),
         CloseByX(EditorPanels::PhysicsDebug),
         Until("closed", [](Ctx&) { return !EditorSettings::Get().ShowPhysicsPanel && Gone(EditorPanels::PhysicsDebug); })},
    }));

    // ---------------------------------------------------------------------------------- other panels
    {
        struct Other { const char* Item; const char* Window; };
        static const Other kOther[] = {
            {"Audio", EditorPanels::Audio}, {"Animator", EditorPanels::Animator}, {"Asset Library", EditorPanels::AssetLibrary},
        };
        Steps s = {FreshScene(), Wait(2)};
        for (const Other& o : kOther) {
            const Other other = o;
            for (Step& st : MenuClick("Window", "Hierarchy", other.Item)) s.push_back(st);
            s.push_back(Until(std::string(other.Item) + " open", [other](Ctx&) { return Open(other.Window); }));
            s.push_back(BringToFront(other.Window));
            s.push_back(UntilShown(other.Window));
            s.push_back(Wait(3));
            s.push_back(Shot(other.Item, {other.Window}));
            if (std::string(other.Item) == "Asset Library") {
                // With no library folder set it offers a folder picker (a native dialog): not clicked.
                s.push_back(ExpectTrue("Choose Library Folder... offered", [](Ctx&) {
                    const UiItem* it = Find(ByLabel("Choose Library Folder...", "###AssetLibrary"));
                    return it && !(it->ItemFlags & ImGuiItemFlags_Disabled);
                }));
            }
            s.push_back(CloseByX(other.Window));
            s.push_back(UntilGone(other.Window));
        }
        s.push_back(ExpectTrue("their flags cleared", [](Ctx& c) {
            return !A::ShowAudio(c.E) && !A::ShowAnimator(c.E) && !EditorSettings::Get().ShowAssetLibrary;
        }));
        Add("panels: Audio, Animator and Asset Library open from the Window menu, close with their x", s);
    }

    Add("panels: Script IDE opens from the Window menu, closes with its x", Seq({
        {FreshScene(), Wait(2)},
        MenuClick("Window", "Hierarchy", "Script IDE"),
        {UntilShown("Script IDE"), Wait(3),
         Shot("script-ide", {"Script IDE"}),
         ExpectTrue("its own menu bar", [](Ctx&) { return Find(ByExact("File", "Script IDE")) && Find(ByExact("Edit", "Script IDE")); }),
         CloseByX("Script IDE"), UntilGone("Script IDE"),
         BringToFront(EditorPanels::Scene), UntilShown(EditorPanels::Scene)},
    }));

    Add("panels: Settings (Ctrl+,) and Build Settings (Ctrl+Shift+B) open and close", {
        FreshScene(), Wait(2),
        Key(ImGuiKey_Comma, ImGuiMod_Ctrl),
        UntilShown(EditorPanels::Settings), Wait(2),
        ExpectTrue("the THIS MACHINE group", [](Ctx& c) { return A::ShowSettings(c.E); }),
        Shot("settings", {"###Settings"}),
        CloseByX(EditorPanels::Settings),
        Until("closed", [](Ctx& c) { return !A::ShowSettings(c.E) && Gone(EditorPanels::Settings); }),
        Key(ImGuiKey_B, ImGuiMod_Ctrl | ImGuiMod_Shift),
        UntilShown(EditorPanels::Settings), Wait(3),
        ExpectTrue("on Project > Build", [](Ctx& c) { return A::SettingsIsProject(c.E) && A::ProjectCategory(c.E) == 2; }),
        Shot("build", {"###Settings"}),
        Do("scroll to the Build buttons", [](Ctx&) { // the page's bottom; clipped widgets carry no label
            for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
                if (w->WasActive && Contains(w->Name, "###Settings") && w->ScrollMax.y > 0.0f) ImGui::SetScrollY(w, w->ScrollMax.y);
        }), Wait(3),
        // Build / Build and Run start a real build: only checked to be there.
        ExpectTrue("Build and Build and Run offered", [](Ctx&) {
            return Find(ByLabel("Build and Run", "###Settings")) && Find(ByExact(ICON_FA_HAMMER "  Build", "###Settings"));
        }),
        CloseByX(EditorPanels::Settings),
        UntilGone(EditorPanels::Settings),
    });

    // ---------------------------------------------------------------------------------- title bar
    // (Undo / Redo / Save left the bar for the Edit and File menus: Menus.inl covers them.)
    Add("panels: title bar Play, Pause, Step, Fullscreen, Stop", {
        FreshScene(), Wait(2),
        Click(ByLabel("Play", kBar)),
        Until("playing", [](Ctx& c) { return X::InPlay(c.E); }, 300), Wait(5),
        Until("Stop and Pause shown", [kBar](Ctx&) { return Find(ByLabel("Stop", kBar)) && Find(ByLabel("Pause", kBar)); }),
        ExpectTrue("Step disabled while running", [kBar](Ctx&) {
            const UiItem* it = Find(ByLabel("Step", kBar));
            return it && (it->ItemFlags & ImGuiItemFlags_Disabled);
        }),
        Shot("playing", {kBar}),
        Click(ByLabel("Pause", kBar)),
        Until("paused (Resume shown)", [kBar](Ctx&) { return Find(ByLabel("Resume", kBar)) != nullptr; }),
        ExpectTrue("Step enabled while paused", [kBar](Ctx&) {
            const UiItem* it = Find(ByLabel("Step", kBar));
            return it && !(it->ItemFlags & ImGuiItemFlags_Disabled);
        }),
        Click(ByLabel("Step", kBar)), Wait(5),
        ExpectTrue("still paused after a step", [kBar](Ctx&) { return Find(ByLabel("Resume", kBar)) != nullptr; }),
        Click(ByLabel("Resume", kBar)),
        Until("running again", [kBar](Ctx&) { return Find(ByLabel("Pause", kBar)) != nullptr; }),
        Click(ByLabel("Fullscreen", kBar)),
        Until("maximized (Restore shown over the game)", [kMaxBar](Ctx&) { return Find(ByLabel("Restore", kMaxBar)) != nullptr; }), Wait(5),
        Shot("maximized", {kMaxBar}),
        // Maximized play gives the mouse to the game (ImGui runs with NoMouse) until Esc, which is
        // read from GLFW and can't be sent from here: restore the way F4 / Esc-then-click does.
        Do("restore", [](Ctx& c) { A::RequestMaximizeToggle(c.E); }),
        Until("windowed (Fullscreen shown)", [kBar](Ctx&) { return Find(ByLabel("Fullscreen", kBar)) != nullptr; }), Wait(3),
        Click(ByLabel("Stop", kBar)),
        Until("stopped", [](Ctx& c) { return !X::InPlay(c.E); }, 300),
        Until("Play shown again", [kBar](Ctx&) { return Find(ByLabel("Play", kBar)) != nullptr; }),
        BringToFront(EditorPanels::Scene), UntilShown(EditorPanels::Scene),
    });

    Add("panels: title bar toggles - stats overlay, Console, History", {
        FreshScene(), Wait(2),
        Do("note", [](Ctx&) { g_Number = EditorSettings::Get().GameViewShowStats ? 1.0 : 0.0; }),
        Click(ByExact(EDITOR_ICON_STATISTICS, kBar)), Wait(3),
        ExpectTrue("stats overlay toggled", [](Ctx&) { return EditorSettings::Get().GameViewShowStats != Noted(); }),
        UntilPrefBool("gameViewShowStats", [] { return !Noted(); }),
        Shot("stats-overlay", {"###Scene"}),
        Click(ByExact(EDITOR_ICON_STATISTICS, kBar)), Wait(3),
        ExpectTrue("stats overlay restored", [](Ctx&) { return EditorSettings::Get().GameViewShowStats == Noted(); }),
        Click(ByExact(EDITOR_ICON_HISTORY, kBar)),
        Until("History open", [](Ctx& c) { return A::ShowHistory(c.E) && Open(EditorPanels::History); }),
        BringToFront(EditorPanels::History), UntilShown(EditorPanels::History),
        Shot("history-toggle", {"###History"}),
        Click(ByExact(EDITOR_ICON_HISTORY, kBar)),
        Until("History closed", [](Ctx& c) { return !A::ShowHistory(c.E) && Gone(EditorPanels::History); }),
        Click(ByExact(EDITOR_ICON_CONSOLE, kBar)),
        Until("Console open", [](Ctx&) { return EditorModuleHost::ConsoleState().Visible && Open(EditorPanels::Console); }),
        BringToFront(EditorPanels::Console), UntilShown(EditorPanels::Console),
        Click(ByExact(EDITOR_ICON_CONSOLE, kBar)),
        Until("Console closed", [](Ctx&) { return !EditorModuleHost::ConsoleState().Visible && Gone(EditorPanels::Console); }),
        UntilShown(EditorPanels::Assets),
    });

    // ---------------------------------------------------------------------------------- status bar
    {
        // Segments are laid out right to left: the leftmost is the active tool, then the selection.
        auto nthSegment = [kStatus](int n) {
            Query q;
            q.Root = kStatus;
            q.Pred = [kStatus, n](const UiItem& it) {
                if (it.Label != "##seg") return false;
                std::vector<float> xs;
                for (const UiItem& o : g_Reg.Last) if (o.Label == "##seg" && Contains(o.Root, kStatus)) xs.push_back(o.Rect.Min.x);
                std::sort(xs.begin(), xs.end());
                return (int)xs.size() > n && it.Rect.Min.x == xs[(size_t)n];
            };
            return q;
        };
        Add("panels: status bar - tool, selection and last-log segments", {
            FreshScene(), Wait(2),
            Do("the move tool", [](Ctx& c) { c.E.SetHandToolActive(false); }), Wait(2),
            Click(nthSegment(0)), Wait(3),
            ExpectTrue("the tool segment picked the Hand tool", [](Ctx& c) { return A::HandTool(c.E); }),
            Click(nthSegment(0)), Wait(3),
            ExpectTrue("and back", [](Ctx& c) { return !A::HandTool(c.E); }),
            ResetCamera(), SelectByName("Gamma"), Wait(3), NoteCamera(),
            Click(nthSegment(1)),
            Until("the selection segment framed Gamma", CameraMoved, 240), Wait(20),
            Do("log a line", [](Ctx&) { Log::Info("smoke-status-bar-line"); }), Wait(3),
            Shot("status-bar", {kStatus}),
            Click(ByExact("##lastlog", kStatus)),
            Until("the last-log segment opened the Console", [](Ctx&) { return EditorModuleHost::ConsoleState().Visible && Shown(EditorPanels::Console); }),
            CloseByX(EditorPanels::Console),
            Until("Console closed", [](Ctx&) { return !EditorModuleHost::ConsoleState().Visible; }),
            ResetCamera(),
        });
    }
}
