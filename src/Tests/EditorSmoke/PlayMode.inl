// Editor smoke suite: PlayMode. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// Play / Pause / Step / Maximize through their F1..F4 shortcuts (the action bar buttons are
// covered by "panels: action bar Play, Pause, Step, Fullscreen, Stop"). Those shortcuts are read
// by main.cpp from GLFW, not from ImGui, so they are pressed with Input::SimulateKeyTap. A falling
// rigidbody crate is the clock: it moves only while the simulation runs. Stop has to put the scene
// back exactly as it was (a full SceneSerializer dump compared before and after), keep the
// selection, and leave the undo history alone.

} // namespace
struct EditorSmokeAccess_PlayMode {
    static void ClearUndo(EditorLayer& e) { e.m_UndoStack.clear(); e.m_CurrentUndoScene.clear(); }
    static std::string UndoLabels(EditorLayer& e) {
        std::string s;
        for (const auto& u : e.m_UndoStack) {
            s += (s.empty() ? "" : " | ") + u.Label + (u.SelectionOnly ? " (sel)" : "");
            for (const auto& f : u.Files) s += " [" + f.Path.generic_string() + "]";
        }
        return s;
    }
};
namespace {
namespace smoke_play {
using PA = EditorSmokeAccess_PlayMode;
const char* kBar = "##MainToolbar";           // the main toolbar row's transport
const char* kMaxBar = "##ViewportActionBar";  // the slim transport over maximized play

Step Tap(int glfwKey, const char* what) {
    return Do(std::string("press ") + what, [glfwKey](Ctx&) { Input::SimulateKeyTap(glfwKey); });
}
// The crate the tests watch fall: a 1 m cube with a box collider and a rigidbody, 10 m up.
// Plus a camera looking at it so the Game view has something to show.
// A sun, so the Game view shots show the scene instead of an unlit black frame.
void AddSun(Ctx& c) {
    const entt::entity sun = c.W.CreateEmptyEntity(glm::vec3(0, 8, 0), glm::vec3(-40, 50, 0), glm::vec3(1.0f), "Sun");
    auto& l = c.W.Registry.emplace<LightComponent>(sun);
    l.Kind = LightComponent::Type::Directional;
    l.Intensity = 6.0f;
}
Step CrateScene() {
    return Do("add a falling crate and a camera", [](Ctx& c) {
        const glm::vec3 zero(0.0f), one(1.0f);
        const entt::entity crate = c.W.CreateModelEntity(c.A.CreatePrimitive("cube"), glm::vec3(0, 10, 0), zero, one, "Crate");
        c.W.Registry.emplace<ColliderComponent>(crate).Kind = ColliderComponent::Shape::Box;
        c.W.Registry.emplace<RigidbodyComponent>(crate);
        const entt::entity cam = c.W.CreateEmptyEntity(glm::vec3(0, 5, 14), glm::vec3(12, 0, 0), one, "Main Camera");
        c.W.Registry.emplace<CameraComponent>(cam);
        AddSun(c);
        PA::ClearUndo(c.E);
    });
}
float CrateY(Ctx& c) {
    const entt::entity e = ByName(c.W, "Crate");
    return e == entt::null ? NAN : c.W.Registry.get<TransformComponent>(e).Position.y;
}
bool Paused() { return Find(ByLabel("Resume", kBar)) != nullptr; }
// The unified viewport showing the game (the Play dissolve finished).
bool GameShown(Ctx& c) {
    return c.E.GetViewportMode() == EditorLayer::ViewportMode::Game && !c.E.ViewFadeActive() && ChromeUi::Shown(EditorPanels::Scene);
}
std::string g_Before;     // the scene dump taken before Play
size_t g_UndoBefore = 0;  // undo depth before Play
float g_Y = 0.0f;         // a remembered crate height
} // namespace smoke_play

void RegisterPlayModeTests() {
    using namespace smoke_play;

    Add("playmode: F1 plays, F2 pauses (time stops), F3 steps one frame, F2 resumes, F1 stops", {
        FreshScene(), CrateScene(), Wait(3),
        Tap(GLFW_KEY_F1, "F1"),
        Until("in play", [](Ctx& c) { return X::InPlay(c.E); }, 300),
        Until("the Game view is shown", [](Ctx& c) { return GameShown(c); }),
        Until("the crate falls", [](Ctx& c) { return CrateY(c) < 9.5f; }, 300),
        Shot("game", {"###Game"}),
        Tap(GLFW_KEY_F2, "F2"),
        Until("paused", [](Ctx&) { return Paused(); }), Wait(3),
        Do("note the height", [](Ctx& c) { g_Y = CrateY(c); }), Wait(20),
        Expect("the crate is frozen while paused", [](Ctx& c) {
            return CrateY(c) == g_Y ? std::string() : "moved from " + std::to_string(g_Y) + " to " + std::to_string(CrateY(c));
        }),
        Tap(GLFW_KEY_F3, "F3"), Wait(3),
        Expect("one step moved it a little", [](Ctx& c) {
            const float d = g_Y - CrateY(c);
            return d > 0.0f && d < 0.5f ? std::string() : "a step moved it by " + std::to_string(d);
        }),
        ExpectTrue("still paused after the step", [](Ctx&) { return Paused(); }),
        Do("note the height", [](Ctx& c) { g_Y = CrateY(c); }), Wait(10),
        ExpectTrue("frozen again after the step", [](Ctx& c) { return CrateY(c) == g_Y; }),
        Tap(GLFW_KEY_F2, "F2"),
        Until("running again", [](Ctx&) { return Find(ByLabel("Pause", kBar)) != nullptr; }),
        Until("the crate falls again", [](Ctx& c) { return CrateY(c) < g_Y - 0.2f; }, 300),
        Tap(GLFW_KEY_F1, "F1"),
        Until("stopped", [](Ctx& c) { return !X::InPlay(c.E); }, 300), Wait(3),
        Expect("the crate is back at 10 m", [](Ctx& c) {
            return std::fabs(CrateY(c) - 10.0f) < 1e-4f ? std::string() : "crate at " + std::to_string(CrateY(c));
        }),
        ChromeUi::BringToFront(EditorPanels::Scene),
    });

    Add("playmode: F3 does nothing while running; edit-mode time does not move the crate", {
        FreshScene(), CrateScene(), Wait(3),
        Do("note the height", [](Ctx& c) { g_Y = CrateY(c); }), Wait(30),
        ExpectTrue("no physics in edit mode", [](Ctx& c) { return CrateY(c) == g_Y; }),
        Tap(GLFW_KEY_F3, "F3 (not playing)"), Wait(5),
        ExpectTrue("still in edit mode", [](Ctx& c) { return !X::InPlay(c.E) && CrateY(c) == g_Y; }),
        Tap(GLFW_KEY_F1, "F1"),
        Until("in play", [](Ctx& c) { return X::InPlay(c.E); }, 300), Wait(5),
        Tap(GLFW_KEY_F3, "F3 (running)"), Wait(5),
        ExpectTrue("F3 did not pause", [](Ctx&) { return !Paused(); }),
        Tap(GLFW_KEY_F1, "F1"),
        Until("stopped", [](Ctx& c) { return !X::InPlay(c.E); }, 300), Wait(3),
        ChromeUi::BringToFront(EditorPanels::Scene),
    });

    Add("playmode: Stop restores the scene exactly, keeps the selection and the undo history", {
        FreshScene(), CrateScene(), Wait(3),
        SelectByName("Beta"), Wait(3),
        Do("one undoable edit", [](Ctx& c) {
            X::PushUndo(c.E, c.W, "Rename Gamma");
            c.W.Registry.get<NameComponent>(ByName(c.W, "Gamma")).Name = "Gamma Edited";
        }), Wait(3),
        Do("dump the scene", [](Ctx& c) {
            g_Before = SceneSerializer::SaveToString(c.W, c.A);
            g_UndoBefore = X::UndoDepth(c.E);
        }),
        Tap(GLFW_KEY_F1, "F1"),
        Until("in play", [](Ctx& c) { return X::InPlay(c.E); }, 300), Wait(10),
        Do("gameplay changes everything", [](Ctx& c) {
            const entt::entity a = ByName(c.W, "Alpha");
            c.W.Registry.get<TransformComponent>(a).Position = glm::vec3(50, 50, 50);
            c.W.Registry.get<LightComponent>(ByName(c.W, "Beta")).Intensity = 0.01f;
            c.W.Registry.get<NameComponent>(ByName(c.W, "Gamma Edited")).Name = "Renamed In Play";
            c.W.CreateEmptyEntity(glm::vec3(1), glm::vec3(0), glm::vec3(1), "Spawned In Play");
        }), Wait(10),
        Until("the crate fell", [](Ctx& c) { return CrateY(c) < 9.0f; }, 300),
        Tap(GLFW_KEY_F1, "F1"),
        Until("stopped", [](Ctx& c) { return !X::InPlay(c.E); }, 300), Wait(3),
        Expect("the scene dump is identical", [](Ctx& c) {
            const std::string now = SceneSerializer::SaveToString(c.W, c.A);
            if (now == g_Before) return std::string();
            size_t i = 0;
            while (i < now.size() && i < g_Before.size() && now[i] == g_Before[i]) ++i;
            const size_t from = i > 80 ? i - 80 : 0;
            return "differs at byte " + std::to_string(i) + ": before '" + g_Before.substr(from, 160) + "' after '" + now.substr(from, 160) + "'";
        }),
        ExpectTrue("nothing spawned in Play survived", [](Ctx& c) { return ByName(c.W, "Spawned In Play") == entt::null; }),
        Expect("Beta still selected", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
        Expect("undo history unchanged", [](Ctx& c) {
            return X::UndoDepth(c.E) == g_UndoBefore ? std::string()
                : "undo depth " + std::to_string(g_UndoBefore) + " -> " + std::to_string(X::UndoDepth(c.E)) + ": " + PA::UndoLabels(c.E);
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("the edit before Play still undoes", [](Ctx& c) { return ByName(c.W, "Gamma") != entt::null; }),
        ChromeUi::BringToFront(EditorPanels::Scene),
    });

    Add("playmode: F4 maximizes the Game view and restores it", {
        FreshScene(), CrateScene(), Wait(3),
        Tap(GLFW_KEY_F1, "F1"),
        Until("in play", [](Ctx& c) { return X::InPlay(c.E); }, 300), Wait(5),
        Tap(GLFW_KEY_F4, "F4"),
        Until("maximized (Restore shown)", [](Ctx&) { return Find(ByLabel("Restore", kMaxBar)) != nullptr; }), Wait(10),
        ExpectTrue("the editor panels are hidden", [](Ctx&) {
            ImGuiWindow* w = ImGui::FindWindowByName(kHierarchy);
            return !w || !w->WasActive || w->Hidden;
        }),
        Shot("maximized"),
        Tap(GLFW_KEY_F4, "F4"),
        Until("windowed (Fullscreen shown)", [](Ctx&) { return Find(ByLabel("Fullscreen", kBar)) != nullptr; }), Wait(5),
        Until("the Hierarchy is back", [](Ctx&) { return Find(ByTag("row:Alpha")) != nullptr; }),
        Tap(GLFW_KEY_F1, "F1"),
        Until("stopped", [](Ctx& c) { return !X::InPlay(c.E); }, 300), Wait(3),
        ChromeUi::BringToFront(EditorPanels::Scene),
    });

    Add("playmode: a First Person Player on a floor plays and stops cleanly", {
        FreshScene(), Wait(2),
        // Project .wav files are gitignored (a fresh checkout, CI, has none): the landing foley can't load.
        AllowLogError("Audio: failed to load sound"),
        Do("floor and sun", [](Ctx& c) {
            const glm::vec3 zero(0.0f);
            const entt::entity floor = c.W.CreateModelEntity(c.A.CreatePrimitive("cube"), glm::vec3(0, -0.5f, 0), zero, glm::vec3(40, 1, 40), "Floor");
            c.W.Registry.emplace<ColliderComponent>(floor).Kind = ColliderComponent::Shape::Box;
            AddSun(c);
            PA::ClearUndo(c.E);
        }),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Click(HierMenu(ICON_FA_PLUS "  Create")), Wait(2),
        Click(HierMenu(ICON_FA_PERSON "  First Person Player")), Wait(3),
        ExpectTrue("a Player Spawn exists", [](Ctx& c) { return ByName(c.W, "Player Spawn") != entt::null; }),
        Do("put it on the floor", [](Ctx& c) {
            X::PushUndo(c.E, c.W, "Move Player");
            const entt::entity p = ByName(c.W, "Player Spawn");
            auto& t = c.W.Registry.get<TransformComponent>(p);
            t.Position = glm::vec3(0, 1, 6);
        }),
        Do("dump the scene", [](Ctx& c) { g_Before = SceneSerializer::SaveToString(c.W, c.A); }),
        Tap(GLFW_KEY_F1, "F1"),
        Until("in play", [](Ctx& c) { return X::InPlay(c.E); }, 300),
        WaitSeconds(2.0),
        Until("the Game view is shown", [](Ctx& c) { return GameShown(c); }),
        Expect("the player stands on the floor", [](Ctx& c) {
            const entt::entity p = ByName(c.W, "Player Spawn");
            if (p == entt::null) return std::string("the Player Spawn is gone in Play");
            const float y = c.W.Registry.get<TransformComponent>(p).Position.y;
            return y > -0.5f && y < 3.0f ? std::string() : "player at y=" + std::to_string(y) + " (fell through or flew off)";
        }),
        Shot("game", {"###Game"}),
        Tap(GLFW_KEY_F1, "F1"),
        Until("stopped", [](Ctx& c) { return !X::InPlay(c.E); }, 300), Wait(3),
        ExpectTrue("scene restored", [](Ctx& c) { return SceneSerializer::SaveToString(c.W, c.A) == g_Before; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        Expect("undo moves the player back", [](Ctx& c) {
            const entt::entity p = ByName(c.W, "Player Spawn");
            if (p == entt::null) return "the player is gone; history: " + PA::UndoLabels(c.E);
            return c.W.Registry.get<TransformComponent>(p).Position != glm::vec3(0, 1, 6) ? std::string()
                : "still at the moved spot; history: " + PA::UndoLabels(c.E);
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        Expect("undo removes the player", [](Ctx& c) {
            return ByName(c.W, "Player Spawn") == entt::null ? std::string() : "history: " + PA::UndoLabels(c.E);
        }),
        ChromeUi::BringToFront(EditorPanels::Scene),
    });
}
