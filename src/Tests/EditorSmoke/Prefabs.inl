// Editor smoke suite: Prefabs. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// Save as Prefab (SceneSerializer::SavePrefab, what both Save as Prefab... menu items call after
// their native file dialog, which a test must never open), placing an instance (what the Asset
// Browser's Place Instance does), Prefab Mode (Open Prefab in the Inspector banner: the prefab
// mode bar takes the action bar's place; edit, Ctrl+S / Save, Back to Scene; every instance picks
// the edits up), the Inspector's per-field overrides (right-click an overridden label: Revert to
// Prefab / Apply to Prefab) and the Hierarchy's Select Prefab Root / Unpack Prefab Instance. Each
// undoable step is undone and the scene compared against a full SceneSerializer dump. The .prefab
// files live in project/editor-smoke-scratch/ and every test deletes that folder at its end.

} // namespace
struct EditorSmokeAccess_Prefabs {
    static bool Dirty(EditorLayer& e) { return e.m_Dirty; }
    // A failed test can leave Prefab Mode open; leave it without saving so the next test starts clean.
    static void Leave(EditorLayer& e, World& w, AssetLibrary& a) { if (e.InPrefabMode()) e.ExitPrefabMode(w, a, /*save=*/false); }
};
namespace {
namespace smoke_prefabs {
namespace fs = std::filesystem;
using PF = EditorSmokeAccess_Prefabs;
using PA = EditorSmokeAccess_PlayMode;
const char* kModeBar = "##PrefabModeBar";
const char* kActionBar = "##Toolbar";

std::string ScratchDir() { return ProjectPaths::Resolve("editor-smoke-scratch"); }
std::string PrefabPath() { return (fs::path(ScratchDir()) / "SmokeCrate.prefab").generic_string(); }
// The scratch folder made / deleted outside AtomicFile: noted as the editor's own, or the project
// watcher reports it as an external change, which closes the undo transaction of whatever edit is
// under way (EditorLayer::SyncProjectChanges) and the edit never becomes an undo step.
void NoteScratch() {
    for (const std::string& p : {ScratchDir(), PrefabPath(), PrefabPath() + ".meta"}) AtomicFile::NoteSelfWrite(fs::path(p));
}
void MakeScratchDir() {
    NoteScratch();
    std::error_code ec;
    fs::create_directories(ScratchDir(), ec);
}
void RemoveScratchDir() {
    NoteScratch();
    std::error_code ec;
    fs::remove_all(ScratchDir(), ec);
}

std::string g_Before;   // a scene dump to come back to
std::string g_After;    // a second one
std::string g_Undo;     // undo labels to compare against

std::string FirstDifference(const std::string& a, const std::string& b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    const size_t from = i > 80 ? i - 80 : 0;
    return "differs at byte " + std::to_string(i) + " of " + std::to_string(a.size()) + "/" + std::to_string(b.size()) +
           ": '" + a.substr(from, 160) + "' vs '" + b.substr(from, 160) + "'";
}
// The scene dump minus the asset library's lists ("assetFolders", "assetMeta"...): the Asset
// Browser adds the scratch folder to them whenever its rescan notices the new .prefab.
std::string Dump(Ctx& c) {
    nlohmann::json j = nlohmann::json::parse(SceneSerializer::SaveToString(c.W, c.A), nullptr, false);
    if (j.is_discarded()) return "(unparsable dump)";
    for (auto it = j.begin(); it != j.end();) it = it.key().rfind("asset", 0) == 0 ? j.erase(it) : std::next(it);
    return j.dump();
}
Step Remember(std::string* into) {
    return Do("dump the scene", [into](Ctx& c) { *into = Dump(c); });
}
Step ExpectDump(std::string what, const std::string* want) {
    return Expect(std::move(what), [want](Ctx& c) {
        const std::string now = Dump(c);
        return now == *want ? std::string() : FirstDifference(*want, now) + " | history: " + PA::UndoLabels(c.E);
    });
}
std::string ReadFile(const std::string& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Starts every test: out of Prefab Mode, the fixture scene, no scratch prefab, Hierarchy unfiltered.
Step Fresh() {
    return Do("leave Prefab Mode, clear the scratch folder", [](Ctx& c) {
        PF::Leave(c.E, c.W, c.A);
        RemoveScratchDir();
        SceneSerializer::ClearPrefabPristineCache();
        HX::ClearFilter(c.E);
    });
}
Step Cleanup() {
    return Do("delete the scratch prefab", [](Ctx& c) {
        PF::Leave(c.E, c.W, c.A);
        c.A.RemovePrefab(PrefabPath());
        RemoveScratchDir();
        SceneSerializer::ClearPrefabPristineCache();
    });
}
// The test prefab, built in a World of its own: "Crate" (root) with a child "Crate Lamp", a
// Light of intensity 2 one metre up.
Step MakePrefab() {
    return Do("write SmokeCrate.prefab", [](Ctx&) {
        MakeScratchDir();
        World w;
        const glm::vec3 zero(0.0f), one(1.0f);
        const entt::entity root = w.CreateEmptyEntity(zero, zero, one, "Crate");
        const entt::entity lamp = w.CreateEmptyEntity(glm::vec3(0, 1, 0), zero, one, "Crate Lamp");
        w.Registry.emplace<LightComponent>(lamp).Intensity = 2.0f;
        if (!w.SetParent(lamp, root)) Fail("could not parent the lamp");
        if (!SceneSerializer::SavePrefab(w, root, PrefabPath())) Fail("SavePrefab failed");
    });
}
// What the Asset Browser's Place Instance does (an undo step, then instantiate and select),
// with the instance renamed to `name` and moved to `pos`.
Step Place(const char* name, glm::vec3 pos) {
    return Do(std::string("place instance ") + name, [name, pos](Ctx& c) {
        X::PushUndo(c.E, c.W, "Place Prefab Instance");
        const entt::entity e = SceneSerializer::InstantiatePrefab(c.W, c.A, PrefabPath());
        if (e == entt::null) { Fail("InstantiatePrefab failed"); return; }
        c.W.Registry.get<NameComponent>(e).Name = name;
        c.W.Registry.get<TransformComponent>(e).Position = pos;
        X::Select(c.E, e);
    });
}
entt::entity ChildOf(World& w, const char* root, const char* child) {
    const entt::entity r = ByName(w, root);
    if (r == entt::null) return entt::null;
    const auto* h = w.Registry.try_get<HierarchyComponent>(r);
    if (!h) return entt::null;
    for (entt::entity e : h->Children)
        if (NameOf(w, e) == child) return e;
    return entt::null;
}
// The Light intensity of `root`'s "Crate Lamp" child (NaN if it's missing).
float LampIntensity(World& w, const char* root) {
    const entt::entity l = ChildOf(w, root, "Crate Lamp");
    const auto* lc = l == entt::null ? nullptr : w.Registry.try_get<LightComponent>(l);
    return lc ? lc->Intensity : NAN;
}
// The lamp's intensity as stored in the .prefab file (instantiated into a scratch World).
float FileIntensity(Ctx& c) {
    World w;
    const entt::entity r = SceneSerializer::InstantiatePrefab(w, c.A, PrefabPath());
    if (r == entt::null) return NAN;
    w.Registry.get<NameComponent>(r).Name = "file";
    return LampIntensity(w, "file");
}
std::string ExpectLamp(Ctx& c, const char* root, float want) {
    const float v = LampIntensity(c.W, root);
    return std::fabs(v - want) < 1e-4f ? std::string()
        : std::string(root) + "'s lamp is " + std::to_string(v) + ", expected " + std::to_string(want);
}
bool Overridden(Ctx& c, const char* root) {
    return SceneSerializer::IsPrefabFieldOverridden(c.W, ChildOf(c.W, root, "Crate Lamp"), "Light", "Intensity");
}
Step SelectLamp(const char* root) {
    return Do(std::string("select ") + root + "'s lamp", [root](Ctx& c) {
        const entt::entity l = ChildOf(c.W, root, "Crate Lamp");
        if (l == entt::null) Fail("no lamp under " + std::string(root));
        else X::Select(c.E, l);
    });
}
// Right-clicks the label of the Inspector field tagged `tag` (the tag sits on the value widget;
// its label is a bare Text item to its left, which the item registry never sees).
Step RightClickLabel(std::string tag) {
    auto phase = std::make_shared<int>(0);
    return {"right-click the label of " + tag, [tag, phase](Ctx&) {
        ImGuiIO& io = ImGui::GetIO();
        if (*phase == 0) {
            const UiItem* it = Find(ByTag(tag));
            if (!it) return false;
            // The label is the row's first thing: just inside the left edge of the field's window.
            const ImGuiWindow* w = ImGui::FindWindowByName(it->Window.c_str());
            if (!w) return false;
            MouseTo(ImVec2(w->InnerRect.Min.x + ImGui::GetFontSize() * 1.5f, it->Rect.GetCenter().y));
            *phase = 1;
            return false;
        }
        io.AddMouseButtonEvent(1, *phase == 1);
        return ++*phase > 2;
    }, 120};
}
bool ModeBarShown() { return Find(ByLabel("Scene", kModeBar)) != nullptr; }
// The Play transport (in the title bar since the chrome moved out of the viewport) steps aside
// for the prefab-mode bar.
bool ActionBarShown() { return Find(ByLabel("Play", kActionBar)) != nullptr; }
} // namespace smoke_prefabs

void RegisterPrefabsTests() {
    using namespace smoke_prefabs;

    Add("prefabs: Save as Prefab writes the object and its children; placing an instance links it; undo, redo", {
        Fresh(), FreshScene(), HierFresh(), Wait(2),
        Do("Gamma under Beta", [](Ctx& c) {
            if (!c.W.SetParent(ByName(c.W, "Gamma"), ByName(c.W, "Beta"))) Fail("could not parent Gamma");
            PA::ClearUndo(c.E);
        }),
        Remember(&g_Before),
        Do("Save as Prefab (Beta)", [](Ctx& c) {
            MakeScratchDir();
            if (!SceneSerializer::SavePrefab(c.W, ByName(c.W, "Beta"), PrefabPath())) Fail("SavePrefab failed");
        }),
        Expect("the file holds Beta and Gamma, not Alpha", [](Ctx&) {
            const std::string s = ReadFile(PrefabPath());
            if (s.empty()) return std::string("no file written");
            return Contains(s, "\"Beta\"") && Contains(s, "\"Gamma\"") && !Contains(s, "\"Alpha\"") ? std::string() : "file: " + s.substr(0, 300);
        }),
        ExpectDump("saving left the scene alone", &g_Before),
        ExpectTrue("saving made no undo step", [](Ctx& c) { return X::UndoDepth(c.E) == 0; }),
        ExpectTrue("Beta itself did not become an instance", [](Ctx& c) {
            return !c.W.Registry.all_of<PrefabInstanceComponent>(ByName(c.W, "Beta"));
        }),
        Place("Beta Copy", glm::vec3(0, 0, 6)), Wait(3),
        Expect("a linked instance with both objects and their components", [](Ctx& c) {
            const entt::entity r = ByName(c.W, "Beta Copy");
            const auto* pi = c.W.Registry.try_get<PrefabInstanceComponent>(r);
            if (!pi) return std::string("no PrefabInstanceComponent");
            if (pi->SourcePath != PrefabPath()) return "linked to " + pi->SourcePath;
            if (pi->InstanceEntities.size() != 2) return "instance has " + std::to_string(pi->InstanceEntities.size()) + " objects";
            const entt::entity g = ChildOf(c.W, "Beta Copy", "Gamma");
            if (g == entt::null) return std::string("no Gamma child");
            if (!c.W.Registry.all_of<LightComponent, AudioSourceComponent>(r)) return std::string("root lost its components");
            const auto& l = c.W.Registry.get<LightComponent>(g);
            return l.Intensity == 1.0f ? std::string() : "child light " + std::to_string(l.Intensity);
        }),
        Until("its Hierarchy row", [](Ctx&) { return Find(ByTag("row:Beta Copy")) != nullptr; }),
        Until("the Inspector banner", [](Ctx&) { return Find(ByLabel("Open Prefab", kInspector)) != nullptr; }),
        Shot("instance", {kInspector, kHierarchy}),
        Remember(&g_After),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3), // the selection, then the placement
        ExpectDump("undo removes the instance", &g_Before),
        Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3),
        ExpectDump("redo puts it back", &g_After),
        ExpectTrue("still a linked instance after redo", [](Ctx& c) {
            const entt::entity r = ByName(c.W, "Beta Copy");
            return r != entt::null && c.W.Registry.all_of<PrefabInstanceComponent>(r);
        }),
        Cleanup(),
    });

    Add("prefabs: Open Prefab shows the prefab mode bar; edit, Ctrl+S, Save, Back to Scene; every instance updates", {
        Fresh(), FreshScene(), HierFresh(), MakePrefab(), Wait(2),
        Place("Crate A", glm::vec3(-4, 0, 0)), Place("Crate B", glm::vec3(4, 0, 0)),
        Do("a clean history", [](Ctx& c) { PA::ClearUndo(c.E); }),
        SelectByName("Crate A"), Wait(3),
        Do("note the history", [](Ctx& c) { g_Undo = PA::UndoLabels(c.E); }),
        ExpectTrue("the action bar is up", [](Ctx&) { return ActionBarShown() && !ModeBarShown(); }),
        Click(ByLabel("Open Prefab", kInspector)),
        Until("in Prefab Mode", [](Ctx& c) { return c.E.InPrefabMode(); }),
        Until("the prefab mode bar", [](Ctx&) { return ModeBarShown(); }), Wait(3),
        ExpectTrue("it replaced the action bar", [](Ctx&) { return !ActionBarShown(); }),
        Expect("the world holds just the prefab, as plain objects", [](Ctx& c) {
            if (ByName(c.W, "Alpha") != entt::null || ByName(c.W, "Crate A") != entt::null) return std::string("scene objects still there");
            const entt::entity r = ByName(c.W, "Crate");
            if (r == entt::null || ChildOf(c.W, "Crate", "Crate Lamp") == entt::null) return std::string("no Crate / Crate Lamp");
            if (c.W.Registry.all_of<PrefabInstanceComponent>(r)) return std::string("the root is an instance of itself");
            return X::UndoDepth(c.E) == 0 ? std::string() : "history: " + PA::UndoLabels(c.E);
        }),
        Shot("prefab mode", {kScene}),
        // Edit 1, saved with Ctrl+S.
        SelectLamp("Crate"), Wait(3),
        InspOpen("Light"), InspUntilOpen("Light"),
        InspReveal(ByTag("field:Light/Intensity")), Click(ByTag("field:Light/Intensity"), ImGuiMod_Ctrl), Wait(3),
        TypeText("7"), Wait(2), Key(ImGuiKey_Enter), Wait(3),
        ExpectTrue("intensity 7, unsaved", [](Ctx& c) { return std::fabs(LampIntensity(c.W, "Crate") - 7.0f) < 1e-4f && PF::Dirty(c.E); }),
        Shot("prefab mode dirty", {kScene, kInspector}),
        HoverPanel(kScene), Wait(2),
        Key(ImGuiKey_S, ImGuiMod_Ctrl),
        Until("saved", [](Ctx& c) { return !PF::Dirty(c.E); }),
        Expect("the file has 7", [](Ctx& c) {
            const float v = FileIntensity(c);
            return std::fabs(v - 7.0f) < 1e-4f ? std::string() : "file intensity " + std::to_string(v);
        }),
        ExpectTrue("Ctrl+S kept Prefab Mode", [](Ctx& c) { return c.E.InPrefabMode(); }),
        // Edit 2, saved with the bar's Save button.
        Do("move the lamp", [](Ctx& c) {
            X::PushUndo(c.E, c.W, "Move");
            c.W.Registry.get<TransformComponent>(ChildOf(c.W, "Crate", "Crate Lamp")).Position = glm::vec3(0, 2.5f, 0);
        }), Wait(3),
        ExpectTrue("dirty again", [](Ctx& c) { return PF::Dirty(c.E); }),
        Click(ByLabel("Save", kModeBar)),
        Until("saved by the button", [](Ctx& c) { return !PF::Dirty(c.E); }),
        // Edit 3, left unsaved: Back to Scene saves it.
        Do("rename the lamp", [](Ctx& c) {
            X::PushUndo(c.E, c.W, "Rename");
            c.W.Registry.get<NameComponent>(ChildOf(c.W, "Crate", "Crate Lamp")).Name = "Crate Bulb";
        }), Wait(3),
        Click(ByLabel("Scene", kModeBar)),
        Until("back in the scene", [](Ctx& c) { return !c.E.InPrefabMode(); }),
        Until("the action bar is back", [](Ctx&) { return ActionBarShown() && !ModeBarShown(); }), Wait(3),
        Expect("both instances picked up every edit and kept their own placement", [](Ctx& c) {
            std::string out;
            for (const char* name : {"Crate A", "Crate B"}) {
                const entt::entity r = ByName(c.W, name);
                if (r == entt::null) { out += std::string(name) + " is gone; "; continue; }
                const entt::entity b = ChildOf(c.W, name, "Crate Bulb");
                if (b == entt::null) { out += std::string(name) + " has no Crate Bulb (rename not applied); "; continue; }
                const float v = c.W.Registry.get<LightComponent>(b).Intensity;
                if (std::fabs(v - 7.0f) > 1e-4f) out += std::string(name) + " intensity " + std::to_string(v) + "; ";
                const glm::vec3 lp = c.W.Registry.get<TransformComponent>(b).Position;
                if (std::fabs(lp.y - 2.5f) > 1e-4f) out += std::string(name) + " bulb at y=" + std::to_string(lp.y) + "; ";
                if (SceneSerializer::IsPrefabFieldOverridden(c.W, b, "Light", "Intensity")) out += std::string(name) + " shows an override; ";
            }
            const float ax = c.W.Registry.get<TransformComponent>(ByName(c.W, "Crate A")).Position.x;
            const float bx = c.W.Registry.get<TransformComponent>(ByName(c.W, "Crate B")).Position.x;
            if (ax != -4.0f || bx != 4.0f) out += "placements moved (" + std::to_string(ax) + ", " + std::to_string(bx) + ")";
            return out;
        }),
        ExpectTrue("the rest of the scene is back", [](Ctx& c) {
            return ByName(c.W, "Alpha") != entt::null && ByName(c.W, "Beta") != entt::null && ByName(c.W, "Gamma") != entt::null;
        }),
        Expect("the scene's history is as it was", [](Ctx& c) {
            return PA::UndoLabels(c.E) == g_Undo ? std::string() : "was [" + g_Undo + "], now [" + PA::UndoLabels(c.E) + "]";
        }),
        Shot("back in the scene", {kScene, kHierarchy}),
        Cleanup(),
    });

    Add("prefabs: an Inspector override shows; Revert to Prefab restores it; undo, undo", {
        Fresh(), FreshScene(), HierFresh(), MakePrefab(), Wait(2),
        Place("Crate A", glm::vec3(0, 0, 0)),
        Do("a clean history", [](Ctx& c) { PA::ClearUndo(c.E); }),
        SelectLamp("Crate A"), Wait(3),
        Until("the part-of-instance banner", [](Ctx&) { return Find(ByLabel("Open Prefab", kInspector)) != nullptr; }),
        ExpectTrue("no override yet", [](Ctx& c) { return !Overridden(c, "Crate A"); }),
        Remember(&g_Before),
        InspOpen("Light"), InspUntilOpen("Light"),
        InspReveal(ByTag("field:Light/Intensity")), Click(ByTag("field:Light/Intensity"), ImGuiMod_Ctrl), Wait(3),
        TypeText("5"), Wait(2), Key(ImGuiKey_Enter), Wait(3),
        Expect("intensity 5, an override", [](Ctx& c) {
            if (std::fabs(LampIntensity(c.W, "Crate A") - 5.0f) > 1e-4f) return "intensity " + std::to_string(LampIntensity(c.W, "Crate A"));
            return Overridden(c, "Crate A") ? std::string() : std::string("not reported as overridden");
        }),
        Remember(&g_After),
        Expect("the edit is an undo step and the scene stores the override", [](Ctx& c) {
            if (!Contains(PA::UndoLabels(c.E), "Edit Light")) return "history: " + PA::UndoLabels(c.E);
            return Contains(g_After, "\"overrides\"") ? std::string() : std::string("no overrides in the dump");
        }),
        Shot("override", {kInspector}),
        RightClickLabel("field:Light/Intensity"), Wait(2),
        Until("the override menu", [](Ctx&) { return Find(MenuItem("Revert to Prefab")) != nullptr; }),
        Shot("override menu", {kInspector}),
        Click(MenuItem("Revert to Prefab")), Wait(3),
        Expect("back to the prefab's 2, no override", [](Ctx& c) {
            const std::string why = ExpectLamp(c, "Crate A", 2.0f);
            if (!why.empty()) return why;
            return Overridden(c, "Crate A") ? std::string("still overridden") : std::string();
        }),
        ExpectDump("the scene matches the one before the edit", &g_Before),
        ExpectTrue("Revert is an undo step", [](Ctx& c) { return Contains(PA::UndoLabels(c.E), "Revert to Prefab"); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectDump("undo brings the override back", &g_After),
        ExpectTrue("and it shows as one", [](Ctx& c) { return Overridden(c, "Crate A"); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectDump("undo again removes the edit", &g_Before),
        Cleanup(),
    });

    Add("prefabs: Apply to Prefab writes the file, clears the override, and other instances pick it up", {
        Fresh(), FreshScene(), HierFresh(), MakePrefab(), Wait(2),
        Place("Crate A", glm::vec3(-4, 0, 0)), Place("Crate B", glm::vec3(4, 0, 0)),
        Do("a clean history", [](Ctx& c) { PA::ClearUndo(c.E); }),
        SelectLamp("Crate A"), Wait(3),
        InspOpen("Light"), InspUntilOpen("Light"),
        InspReveal(ByTag("field:Light/Intensity")), Click(ByTag("field:Light/Intensity"), ImGuiMod_Ctrl), Wait(3),
        TypeText("5"), Wait(2), Key(ImGuiKey_Enter), Wait(3),
        ExpectTrue("an override, recorded as an edit", [](Ctx& c) { return Overridden(c, "Crate A") && Contains(PA::UndoLabels(c.E), "Edit Light"); }),
        Do("note the history", [](Ctx& c) { g_Undo = PA::UndoLabels(c.E); }),
        RightClickLabel("field:Light/Intensity"), Wait(2),
        Click(MenuItem("Apply to Prefab")), Wait(3),
        Expect("the file now has 5", [](Ctx& c) {
            const float v = FileIntensity(c);
            return std::fabs(v - 5.0f) < 1e-4f ? std::string() : "file intensity " + std::to_string(v);
        }),
        ExpectTrue("Crate A keeps 5 and no longer shows an override", [](Ctx& c) {
            return ExpectLamp(c, "Crate A", 5.0f).empty() && !Overridden(c, "Crate A");
        }),
        Expect("Apply is one undo step, holding the .prefab file", [](Ctx& c) {
            const std::string now = PA::UndoLabels(c.E);
            return now == g_Undo + " | Apply to Prefab [" + PrefabPath() + "]" ? std::string() : "was [" + g_Undo + "], now [" + now + "]";
        }),
        Expect("Crate B (never overridden) tracks the prefab: 5, no override", [](Ctx& c) {
            const std::string why = ExpectLamp(c, "Crate B", 5.0f);
            if (!why.empty()) return why;
            return Overridden(c, "Crate B") ? std::string("Crate B shows an override") : std::string();
        }),
        Expect("a save and reload gives both 5", [](Ctx& c) {
            World w;
            if (!SceneSerializer::LoadFromString(w, c.A, Dump(c))) return std::string("reload failed");
            std::string out;
            for (const char* name : {"Crate A", "Crate B"}) {
                const float v = LampIntensity(w, name);
                if (std::fabs(v - 5.0f) > 1e-4f) out += std::string(name) + " reloads with " + std::to_string(v) + "; ";
            }
            return out;
        }),
        Shot("applied", {kInspector}),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        Expect("undo puts the file back to 2", [](Ctx& c) {
            const float v = FileIntensity(c);
            return std::fabs(v - 2.0f) < 1e-4f ? std::string() : "file intensity " + std::to_string(v) + " | history: " + PA::UndoLabels(c.E);
        }),
        Expect("Crate A keeps its 5, an override again; Crate B is 2", [](Ctx& c) {
            std::string why = ExpectLamp(c, "Crate A", 5.0f) + ExpectLamp(c, "Crate B", 2.0f);
            if (why.empty() && !Overridden(c, "Crate A")) why = "Crate A is not shown as overridden";
            return why;
        }),
        Expect("the history is back to before Apply", [](Ctx& c) {
            return PA::UndoLabels(c.E) == g_Undo ? std::string() : "was [" + g_Undo + "], now [" + PA::UndoLabels(c.E) + "]";
        }),
        Cleanup(),
    });

    Add("prefabs: Select Prefab Root and Unpack Prefab Instance from the Hierarchy menu; undo each", {
        Fresh(), FreshScene(), HierFresh(), MakePrefab(), Wait(2),
        Place("Crate A", glm::vec3(0, 0, 0)),
        Do("a clean history", [](Ctx& c) { PA::ClearUndo(c.E); }),
        Wait(3), HierExpandAll(), Wait(3),
        Click(ByTag("row:Crate Lamp")), Wait(3),
        Expect("the lamp is selected", [](Ctx& c) { return ExpectSelected(c, "Crate Lamp"); }),
        Remember(&g_Before),
        RightClick(ByTag("row:Crate Lamp")), Wait(3),
        Click(HierMenu(ICON_FA_CUBES "  Select Prefab Root")), Wait(3),
        Expect("the instance root is selected", [](Ctx& c) { return ExpectSelected(c, "Crate A"); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        Expect("undo selects the lamp again", [](Ctx& c) {
            const std::string why = ExpectSelected(c, "Crate Lamp");
            return why.empty() ? why : why + " | history: " + PA::UndoLabels(c.E);
        }),
        ExpectDump("the scene never changed", &g_Before),
        Click(ByTag("row:Crate A")), Wait(3),
        Remember(&g_Before),
        RightClick(ByTag("row:Crate A")), Wait(3),
        Shot("menu", {kHierarchy}),
        Click(HierMenu(ICON_FA_LINK_SLASH "  Unpack Prefab Instance")), Wait(3),
        Expect("plain objects now: no link, both still there", [](Ctx& c) {
            const entt::entity r = ByName(c.W, "Crate A");
            if (r == entt::null || ChildOf(c.W, "Crate A", "Crate Lamp") == entt::null) return std::string("objects gone");
            return c.W.Registry.all_of<PrefabInstanceComponent>(r) ? std::string("still an instance") : std::string();
        }),
        ExpectTrue("the Inspector banner is gone", [](Ctx&) { return Find(ByLabel("Open Prefab", kInspector)) == nullptr; }),
        Remember(&g_After),
        Expect("the scene stores the objects in full, not a link", [](Ctx&) {
            return Contains(g_After, "SmokeCrate.prefab") ? std::string("the dump still names the prefab") : std::string();
        }),
        Shot("unpacked", {kInspector, kHierarchy}),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectDump("undo relinks it", &g_Before),
        ExpectTrue("an instance again", [](Ctx& c) { return c.W.Registry.all_of<PrefabInstanceComponent>(ByName(c.W, "Crate A")); }),
        Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3),
        ExpectDump("redo unpacks again", &g_After),
        Cleanup(),
    });
}
