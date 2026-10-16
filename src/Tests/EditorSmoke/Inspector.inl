// Editor smoke suite: Inspector. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// Covers the Inspector (EditorLayer_Inspector.cpp): Add Component (search + click) for every
// component ComponentRegistry offers, each section expanded and shot, removed from its menu,
// undone and redone; editing each field kind through the UI (float / int drags with Ctrl+click
// typing, Transform position / rotation / scale, checkbox, enum combo, colour hex, string);
// the header card (name, active, Static, Tag); the object actions menu; the component menus
// (Reset, Copy / Paste Component, Save / Apply Preset); multi-selection editing; the lock.
// Enhancer-specific section behaviour (hover keys, picking, pinned windows, Reset to Default on
// a field, Keep Changes After Play) is covered by the tests in EditorUiTests.cpp.

} // namespace
struct EditorSmokeAccess_Inspector {
    static void SelectMany(EditorLayer& e, const std::vector<entt::entity>& v) {
        e.ClearAssetSelection();
        e.ClearSelection();
        for (size_t i = 0; i < v.size(); ++i) e.SelectItem(v[i], i > 0);
    }
    static const std::string& ClipKind(EditorLayer& e) { return e.m_ComponentClipKind; }
};
namespace {
using IX = EditorSmokeAccess_Inspector;

// An item in an open popup (or one of its submenus) whose raw label is exactly `raw`.
Query InspPopup(std::string raw) {
    Query q;
    q.Exact = std::move(raw);
    q.Pred = [](const UiItem& it) { return Contains(it.Root, "##Popup") || Contains(it.Root, "##Menu"); };
    return q;
}
// An entry of an open combo box (visible label equal to `label`).
Query InspComboItem(std::string label) {
    Query q;
    q.Pred = [label](const UiItem& it) { return Contains(it.Root, "##Combo") && Visible(it.Label) == label; };
    return q;
}
// A button in the Inspector's header card (name row: padlock, active toggle...).
Query InspCard(std::string raw) {
    Query q;
    q.Exact = std::move(raw);
    q.Root = kInspector;
    q.Pred = [](const UiItem& it) { return Contains(it.Window, "entityHeaderCard"); };
    return q;
}
// The `axis`-th drag field (0 = X) of the `row`-th X/Y/Z row in the Inspector, top to bottom
// (with Transform open and first: 0 = Location, 1 = Rotation, 2 = Scale).
Query InspAxisField(int row, int axis) {
    Query q;
    q.Root = kInspector;
    q.Pred = [row, axis](const UiItem& it) {
        if (it.Label != "##v") return false;
        std::vector<const UiItem*> xs;
        for (const UiItem& b : g_Reg.Last)
            if (b.Label == "X" && Contains(b.Root, kInspector) && b.Rect.GetHeight() > 0.0f) xs.push_back(&b);
        std::sort(xs.begin(), xs.end(), [](const UiItem* a, const UiItem* b) { return a->Rect.Min.y < b->Rect.Min.y; });
        if (row >= (int)xs.size()) return false;
        const UiItem* xb = xs[(size_t)row];
        std::vector<const UiItem*> fields;
        for (const UiItem& f : g_Reg.Last)
            if (f.Label == "##v" && Contains(f.Root, kInspector) && f.Rect.Min.x > xb->Rect.Min.x &&
                f.Rect.GetCenter().y > xb->Rect.Min.y && f.Rect.GetCenter().y < xb->Rect.Max.y)
                fields.push_back(&f);
        std::sort(fields.begin(), fields.end(), [](const UiItem* a, const UiItem* b) { return a->Rect.Min.x < b->Rect.Min.x; });
        return axis < (int)fields.size() && fields[(size_t)axis]->Id == it.Id;
    };
    return q;
}
// The `row`-th X axis button (click = zero that axis).
Query InspAxisButton(int row) {
    Query q = ByExact("X", kInspector, row);
    return q;
}
// Scrolls the Inspector (or whatever scrolls the item) until `q` is fully on screen.
Step InspReveal(Query q) {
    auto tries = std::make_shared<int>(0);
    return {"reveal", [q, tries](Ctx&) {
        const UiItem* it = Find(q);
        if (!it) {
            // A clipped widget is registered without its label (ImGui returns before reporting
            // it), so a label query can't see it: page the panel down until it shows up.
            if (++*tries > 30) return false;
            for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
                if (w->WasActive && !w->Hidden && w->ScrollMax.y > 0.0f && Contains(w->Name, q.Root.empty() ? kInspector : q.Root)) {
                    ImGui::SetScrollY(w, std::min(w->Scroll.y + w->InnerRect.GetHeight() * 0.5f, w->ScrollMax.y));
                    break;
                }
            return false;
        }
        if (it->Rect.Min.y >= it->Clip.Min.y && it->Rect.Max.y <= it->Clip.Max.y) return true;
        if (++*tries > 30) return true; // can't scroll further: let the click report it
        ImGuiWindow* w = ImGui::FindWindowByName(it->Window.c_str());
        while (w && w->ScrollMax.y <= 0.0f) w = w->ParentWindow;
        if (w) ImGui::SetScrollFromPosY(w, it->Rect.GetCenter().y - w->Pos.y, 0.5f);
        return false;
    }, 120};
}
// Clicks a component's "..." button until its menu shows `item`: now and then the first click
// opens nothing (seen at both window sizes, a different component each run).
Step InspOpenMenu(std::string component, const char* item) {
    auto click = std::make_shared<Step>(Click(MoreButton(component.c_str())));
    auto state = std::make_shared<int>(0); // 0 = clicking, > 0 = frames waited for the menu
    auto tries = std::make_shared<int>(0);
    return {"open the " + component + " menu", [component, item, click, state, tries](Ctx& c) {
        if (*state == 0) {
            if (click->Fn(c)) *state = 1;
            return false;
        }
        if (Find(MenuItem(item))) return true;
        if (++*state < 10) return false;
        if (++*tries >= 3) return true; // let the next step report it
        ImGui::ClosePopupsExceptModals();
        *click = Click(MoreButton(component.c_str()));
        *state = 0;
        return false;
    }, 240};
}
// Opens a component section unless it is already open (open state is per object order and
// survives between tests).
Step InspOpen(std::string component) {
    Step click = Click(Header(component.c_str()));
    auto state = std::make_shared<int>(-1);
    click.Name = "open section " + component;
    click.Fn = [inner = click.Fn, component, state](Ctx& c) {
        if (*state < 0) {
            const int o = SectionOpen(component.c_str());
            if (o < 0) return false; // header not drawn yet
            *state = o == 1 ? 1 : 0;
        }
        return *state == 1 || inner(c);
    };
    return click;
}
Step InspUntilOpen(std::string component) {
    return Until("section " + component + " open", [component](Ctx&) { return SectionOpen(component.c_str()) == 1; });
}
// Ctrl+click a drag / slider to type into it, type `text`, Enter.
std::vector<Step> InspTypeInto(Query q, std::string text) {
    return {InspReveal(q), Click(q, ImGuiMod_Ctrl), Wait(3), TypeText(std::move(text)), Wait(2), Key(ImGuiKey_Enter), Wait(3)};
}
std::vector<Step> InspCat(std::vector<std::vector<Step>> parts) {
    std::vector<Step> out;
    for (auto& p : parts) for (auto& s : p) out.push_back(std::move(s));
    return out;
}
bool InspNear(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
std::string InspPresetDir() { return ProjectPaths::Resolve("presets"); }
// A throwaway folder inside the project for files a test needs the editor to find; tests delete it.
std::string InspScratchDir() { return ProjectPaths::Resolve("editor-smoke-scratch"); }
std::set<std::string> InspListDir(const std::string& dir) {
    std::set<std::string> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return out;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) out.insert(e.path().string());
    return out;
}

void RegisterInspectorTests() {
    // ------------------------------------------------------------------ every Add Component entry
    for (const RegisteredComponent& rc : ComponentRegistry::All()) {
        if (!rc.Meta.GenericInspector || !rc.Meta.Name) continue; // Mesh Renderer: its own test below
        const std::string name = rc.Meta.Name;
        const std::string entry = std::string(rc.Meta.Icon ? rc.Meta.Icon : "") + "  " + name;
        const RegisteredComponent* rcp = &rc; // registry entries are stable for the run
        Add("inspector: Add Component > " + name + ": added, section draws, Remove, undo, redo", {
            FreshScene(), Wait(2),
            SelectByName("Alpha"), Wait(3),
            InspReveal(ByLabel("Add Component", kInspector)),
            Click(ByLabel("Add Component", kInspector)), Wait(3),
            TypeText(name), Wait(3),
            Click(InspPopup(entry)), Wait(3),
            ExpectTrue("added to Alpha", [rcp](Ctx& c) { return rcp->Has(c.W.Registry, ByName(c.W, "Alpha")); }),
            InspOpen(name), InspUntilOpen(name), WaitSeconds(0.3),
            Shot("section", {kInspector}),
            InspOpenMenu(name, "Remove Component"),
            Click(MenuItem("Remove Component")), WaitSeconds(0.5),
            ExpectTrue("removed", [rcp](Ctx& c) { return !rcp->Has(c.W.Registry, ByName(c.W, "Alpha")); }),
            Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
            ExpectTrue("undo puts it back", [rcp](Ctx& c) { return rcp->Has(c.W.Registry, ByName(c.W, "Alpha")); }),
            Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3),
            ExpectTrue("redo removes it again", [rcp](Ctx& c) { return !rcp->Has(c.W.Registry, ByName(c.W, "Alpha")); }),
        });
    }
    Add("inspector: Add Component > Mesh Renderer: mesh, Lighting fields, Material; remove keeps the mesh; undo, redo", {
        FreshScene(), Wait(2),
        SelectByName("Alpha"), Wait(3),
        InspReveal(ByLabel("Add Component", kInspector)),
        Click(ByLabel("Add Component", kInspector)), Wait(3),
        TypeText("Mesh Renderer"), Wait(3),
        Click(InspPopup(ICON_FA_DRAW_POLYGON "  Mesh Renderer")), Wait(3),
        ExpectTrue("added", [](Ctx& c) { return c.W.Registry.all_of<RenderableComponent>(ByName(c.W, "Alpha")); }),
        InspOpen("Mesh Renderer"), InspUntilOpen("Mesh Renderer"), WaitSeconds(0.3),
        Until("a Material section too", [](Ctx&) { return Find(Header("Material")) != nullptr; }),
        Shot("mesh renderer", {kInspector}),
        // Mesh: the button opens the primitive list.
        Click(ByExact("Cube", kInspector)), Wait(2),
        Click(InspPopup(ICON_FA_CIRCLE "  Sphere")), Wait(3),
        Until("mesh is now a Sphere", [](Ctx&) { return Find(ByExact("Sphere", kInspector)) != nullptr; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        Until("undo -> Cube", [](Ctx&) { return Find(ByExact("Cube", kInspector)) != nullptr; }),
        // Cast Shadows (combo) and Receive Shadows (checkbox).
        InspReveal(ByExact("##CastShadows", kInspector)),
        Click(ByExact("##CastShadows", kInspector)), Wait(2),
        Click(InspComboItem("Two Sided")), Wait(3),
        ExpectTrue("Cast Shadows = Two Sided", [](Ctx& c) {
            return (int)c.W.Registry.get<RenderableComponent>(ByName(c.W, "Alpha")).CastShadows == 2;
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> On", [](Ctx& c) { return (int)c.W.Registry.get<RenderableComponent>(ByName(c.W, "Alpha")).CastShadows == 1; }),
        Do("note Receive Shadows", [](Ctx& c) { g_Number = c.W.Registry.get<RenderableComponent>(ByName(c.W, "Alpha")).ReceiveShadows ? 1.0 : 0.0; }),
        InspReveal(ByExact("##ReceiveShadows", kInspector)),
        Click(ByExact("##ReceiveShadows", kInspector)), Wait(3),
        ExpectTrue("Receive Shadows toggled", [](Ctx& c) {
            return (c.W.Registry.get<RenderableComponent>(ByName(c.W, "Alpha")).ReceiveShadows ? 1.0 : 0.0) != g_Number;
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo", [](Ctx& c) { return (c.W.Registry.get<RenderableComponent>(ByName(c.W, "Alpha")).ReceiveShadows ? 1.0 : 0.0) == g_Number; }),
        // Remove from the section menu: the mesh is parked, not lost.
        Click(MoreButton("Mesh Renderer")), Wait(2),
        Click(MenuItem("Remove Component")), WaitSeconds(0.5),
        ExpectTrue("removed; mesh kept detached", [](Ctx& c) {
            const entt::entity a = ByName(c.W, "Alpha");
            return !c.W.Registry.all_of<RenderableComponent>(a) && c.W.Registry.all_of<DetachedMeshComponent>(a);
        }),
        ExpectTrue("Material section gone", [](Ctx&) { return Find(Header("Material")) == nullptr; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo", [](Ctx& c) { return c.W.Registry.all_of<RenderableComponent>(ByName(c.W, "Alpha")); }),
        Key(ImGuiKey_Y, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("redo", [](Ctx& c) { return !c.W.Registry.all_of<RenderableComponent>(ByName(c.W, "Alpha")); }),
    });
    Add("inspector: Add Component search narrows the list; a present component is greyed out", {
        FreshScene(), Wait(2),
        SelectByName("Beta"), Wait(3),
        InspReveal(ByLabel("Add Component", kInspector)), // section open states carry over between tests
        Click(ByLabel("Add Component", kInspector)), Wait(3),
        Shot("unfiltered"),
        TypeText("light"), Wait(3),
        ExpectTrue("Light listed but disabled (Beta has one)", [](Ctx&) {
            const UiItem* it = Find(InspPopup(ICON_FA_LIGHTBULB "  Light"));
            return it && (it->ItemFlags & ImGuiItemFlags_Disabled);
        }),
        ExpectTrue("Audio Source filtered out", [](Ctx&) { return Find(InspPopup(ICON_FA_VOLUME_HIGH "  Audio Source")) == nullptr; }),
        Shot("filtered"),
        Key(ImGuiKey_Escape), Wait(2), Key(ImGuiKey_Escape), Wait(2),
    });

    // ------------------------------------------------------------------ field kinds
    Add("inspector: Transform rows: type Location X, Rotation Y, Scale Z; axis button zeroes; undo each", InspCat({
        {FreshScene(), Wait(2), SelectByName("Alpha"), Wait(3), InspOpen("Transform"), InspUntilOpen("Transform"), Wait(3)},
        InspTypeInto(InspAxisField(0, 0), "4.5"),
        {ExpectTrue("Location X = 4.5", [](Ctx& c) { return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Position.x, 4.5f); }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> -3", [](Ctx& c) { return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Position.x, -3.0f); })},
        InspTypeInto(InspAxisField(1, 1), "45"),
        {ExpectTrue("Rotation Y = 45", [](Ctx& c) { return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).EulerDegrees().y, 45.0f, 0.01f); }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> 0", [](Ctx& c) { return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).EulerDegrees().y, 0.0f, 0.01f); })},
        InspTypeInto(InspAxisField(2, 2), "2"),
        {ExpectTrue("Scale Z = 2", [](Ctx& c) { return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Scale.z, 2.0f); }),
         Shot("edited", {kInspector}),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> 1", [](Ctx& c) { return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Scale.z, 1.0f); }),
         Click(InspAxisButton(0)), Wait(3),
         ExpectTrue("X button zeroed Location X", [](Ctx& c) { return c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Position.x == 0.0f; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> -3", [](Ctx& c) { return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Position.x, -3.0f); })},
    }));
    Add("inspector: Audio Source float (typed), checkbox and enum combo; undo each", InspCat({
        {FreshScene(), Wait(2), SelectByName("Beta"), Wait(3), InspOpen("Audio Source"), InspUntilOpen("Audio Source"), Wait(3)},
        InspTypeInto(ByTag("field:Audio Source/Max Distance"), "25"),
        {ExpectTrue("Max Distance = 25", [](Ctx& c) { return c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).MaxDistance == 25.0f; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> 500", [](Ctx& c) { return c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).MaxDistance == AudioSourceComponent{}.MaxDistance; }),
         InspReveal(ByTag("field:Audio Source/Loop")),
         Click(ByTag("field:Audio Source/Loop")), Wait(3),
         ExpectTrue("Loop on", [](Ctx& c) { return c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).Loop; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> off", [](Ctx& c) { return !c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).Loop; }),
         InspReveal(ByTag("field:Audio Source/Output")),
         Click(ByTag("field:Audio Source/Output")), Wait(2),
         Shot("output combo"),
         Click(InspComboItem("Music")), Wait(3),
         ExpectTrue("Output = Music", [](Ctx& c) { return c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).Output == 1; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> SFX", [](Ctx& c) { return c.W.Registry.get<AudioSourceComponent>(ByName(c.W, "Beta")).Output == 0; })},
    }));
    Add("inspector: Transform Controller vec3 (typed Z) and string field; undo each", InspCat({
        {FreshScene(),
         Do("add a Transform Controller", [](Ctx& c) {
             X::PushUndo(c.E, c.W, "Add Transform Controller");
             c.W.Registry.emplace<TransformControllerComponent>(ByName(c.W, "Alpha"));
         }), Wait(2),
         SelectByName("Alpha"), Wait(3), InspOpen("Transform Controller"), InspUntilOpen("Transform Controller"), Wait(3)},
        InspTypeInto(ByTag("field:Transform Controller/Rotation Deg/Sec"), "90"),
        {ExpectTrue("Rotation Deg/Sec Z = 90", [](Ctx& c) {
             return c.W.Registry.get<TransformControllerComponent>(ByName(c.W, "Alpha")).RotationDegPerSec.z == 90.0f;
         }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> default", [](Ctx& c) {
             return c.W.Registry.get<TransformControllerComponent>(ByName(c.W, "Alpha")).RotationDegPerSec.z ==
                    TransformControllerComponent{}.RotationDegPerSec.z;
         }),
         // Script Path is an asset picker (a searchable dropdown of the project's .tescript files),
         // not a text field. The project ships none, so make one in a scratch folder (deleted at
         // the end) and wait out the picker's 4 s project-scan cache.
         Do("make a scratch script", [](Ctx&) {
             std::error_code ec;
             std::filesystem::create_directories(InspScratchDir(), ec);
             std::ofstream(InspScratchDir() + "/spin.tescript") << "// editor smoke test\n";
         }),
         // The field's tag sits on its last item, the browse button (a native dialog: never
         // clicked unattended); the dropdown is the item to its left on the same row.
         InspReveal(ByTag("field:Transform Controller/Script Path")),
         Click([] {
             Query q;
             q.Root = kInspector;
             q.Pred = [](const UiItem& it) {
                 const UiItem* browse = Find(ByTag("field:Transform Controller/Script Path"));
                 return browse && it.Id != browse->Id && it.Rect.Max.x <= browse->Rect.Min.x &&
                        it.Rect.Max.x > browse->Rect.Min.x - 40.0f &&
                        it.Rect.GetCenter().y > browse->Rect.Min.y && it.Rect.GetCenter().y < browse->Rect.Max.y;
             };
             return q;
         }()), Wait(3),
         TypeText("spin"),
         // The picker re-walks the project at most every 4 s, and only while open: an earlier
         // scan this run may still be cached, so keep it open until the new file is listed.
         Until("the scratch script is listed", [](Ctx&) {
             for (const UiItem& it : g_Reg.Last) if (it.Label == "##row" && Contains(it.Root, "##Combo")) return true;
             return false;
         }, 360),
         Shot("picker", {kInspector}),
         Click([] {
             Query q;
             q.Exact = "##row";
             q.Pred = [](const UiItem& it) { return Contains(it.Root, "##Combo"); };
             return q;
         }()), Wait(3),
         Expect("Script Path picked", [](Ctx& c) {
             const std::string p = c.W.Registry.get<TransformControllerComponent>(ByName(c.W, "Alpha")).ScriptPath;
             return p.find("spin.tescript") != std::string::npos ? std::string() : "Script Path is '" + p + "'";
         }),
         Shot("edited", {kInspector}),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> default", [](Ctx& c) {
             return c.W.Registry.get<TransformControllerComponent>(ByName(c.W, "Alpha")).ScriptPath ==
                    TransformControllerComponent{}.ScriptPath;
         }),
         Do("delete the scratch script", [](Ctx&) { std::error_code ec; std::filesystem::remove_all(InspScratchDir(), ec); })},
    }));
    Add("inspector: Particle System int (typed) and colour (hex); undo each", InspCat({
        {FreshScene(),
         Do("add a Particle System", [](Ctx& c) {
             X::PushUndo(c.E, c.W, "Add Particle System");
             c.W.Registry.emplace<ParticleSystemComponent>(ByName(c.W, "Alpha"));
         }), Wait(2),
         SelectByName("Alpha"), Wait(3), InspOpen("Particle System"), InspUntilOpen("Particle System"), Wait(3),
         Shot("groups", {kInspector}),
         Expect("each field group has one header", [](Ctx&) {
             std::set<std::string> seen;
             for (const UiItem& it : g_Reg.Last)
                 if (Contains(it.Window, "Particle System") && (it.Status & ImGuiItemStatusFlags_Openable) &&
                     !seen.insert(Visible(it.Label)).second)
                     return "two '" + Visible(it.Label) + "' headers";
             return std::string();
         }),
         InspReveal(ByExact("Emission", kInspector)), Click(ByExact("Emission", kInspector)), Wait(3),
         Until("Max Particles shown", [](Ctx&) { return Find(ByTag("field:Particle System/Max Particles")) != nullptr; })},
        InspTypeInto(ByTag("field:Particle System/Max Particles"), "64"),
        {ExpectTrue("Max Particles = 64", [](Ctx& c) { return c.W.Registry.get<ParticleSystemComponent>(ByName(c.W, "Alpha")).MaxParticles == 64; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> default", [](Ctx& c) {
             return c.W.Registry.get<ParticleSystemComponent>(ByName(c.W, "Alpha")).MaxParticles == ParticleSystemComponent{}.MaxParticles;
         })},
        [] {
            // Start Color's hex field: the first ColorEdit hex input in the Particle System section.
            // Off screen it has no label (clipped widgets aren't reported); InspReveal pages to it.
            Query hex;
            hex.Root = kInspector;
            hex.Pred = [](const UiItem& it) { return it.Label == "##Text" && Contains(it.Window, "Particle System"); };
            return std::vector<Step>{
                InspReveal(ByExact("Lifetime", kInspector)), Click(ByExact("Lifetime", kInspector)), Wait(3),
                InspReveal(hex), Click(hex), Wait(3),
                Key(ImGuiKey_A, ImGuiMod_Ctrl), TypeText("FF0000"), Wait(2), Key(ImGuiKey_Enter), Wait(3),
                ExpectTrue("Start Color = red", [](Ctx& c) {
                    const glm::vec3 v = c.W.Registry.get<ParticleSystemComponent>(ByName(c.W, "Alpha")).StartColor;
                    return InspNear(v.x, 1.0f) && InspNear(v.y, 0.0f) && InspNear(v.z, 0.0f);
                }),
                Shot("red", {kInspector}),
                Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
                ExpectTrue("undo -> default colour", [](Ctx& c) {
                    const glm::vec3 v = c.W.Registry.get<ParticleSystemComponent>(ByName(c.W, "Alpha")).StartColor;
                    return glm::length(v - ParticleSystemComponent{}.StartColor) < 1e-3f;
                }),
            };
        }(),
    }));

    // ------------------------------------------------------------------ header card
    Add("inspector: header card: rename in the name field; undo", {
        FreshScene(), Wait(2), SelectByName("Alpha"), Wait(3),
        Click(ByExact("##Name", kInspector)), Wait(3),
        Key(ImGuiKey_A, ImGuiMod_Ctrl), TypeText("Renamed"), Wait(2), Key(ImGuiKey_Enter), Wait(3),
        ExpectTrue("renamed", [](Ctx& c) { return ByName(c.W, "Renamed") != entt::null && ByName(c.W, "Alpha") == entt::null; }),
        Until("the Hierarchy row follows", [](Ctx&) { return Find(ByTag("row:Renamed")) != nullptr; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> Alpha", [](Ctx& c) { return ByName(c.W, "Alpha") != entt::null && ByName(c.W, "Renamed") == entt::null; }),
    });
    Add("inspector: header card: active toggle, Static checkbox, New tag; undo each", {
        FreshScene(), Wait(2), SelectByName("Alpha"), Wait(3),
        Click(InspCard("##active")), Wait(3),
        ExpectTrue("inactive", [](Ctx& c) { return c.W.Registry.all_of<DeactivatedTag>(ByName(c.W, "Alpha")); }),
        Shot("inactive", {kInspector}),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> active", [](Ctx& c) { return !c.W.Registry.all_of<DeactivatedTag>(ByName(c.W, "Alpha")); }),
        Click(ByExact("##Static", kInspector)), Wait(3),
        ExpectTrue("Static", [](Ctx& c) { return c.W.Registry.all_of<StaticTag>(ByName(c.W, "Alpha")); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> not static", [](Ctx& c) { return !c.W.Registry.all_of<StaticTag>(ByName(c.W, "Alpha")); }),
        Click(ByExact("##Tag", kInspector)), Wait(2),
        Click(InspComboItem(ICON_FA_PLUS "  New tag...")), Wait(3),
        TypeText("Enemy"), Wait(2), Key(ImGuiKey_Enter), Wait(3),
        ExpectTrue("tagged Enemy", [](Ctx& c) {
            const auto* t = c.W.Registry.try_get<TagComponent>(ByName(c.W, "Alpha"));
            return t && t->Tag == "Enemy";
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> untagged", [](Ctx& c) { return !c.W.Registry.all_of<TagComponent>(ByName(c.W, "Alpha")); }),
    });
    Add("inspector: object actions: Duplicate, Delete; Save as Prefab offered; undo each", {
        FreshScene(), Wait(2), SelectByName("Alpha"), Wait(3),
        Click(ObjectMenu()), Wait(2),
        Until("Save as Prefab... listed and enabled", [](Ctx&) {
            const UiItem* it = Find(InspPopup(ICON_FA_BOX_ARCHIVE "  Save as Prefab..."));
            return it && !(it->ItemFlags & ImGuiItemFlags_Disabled);
        }),
        Shot("object actions"),
        Click(InspPopup(ICON_FA_CLONE "  Duplicate")), Wait(3),
        Expect("Alpha (1) created and shown", [](Ctx& c) {
            if (ByName(c.W, "Alpha (1)") == entt::null) return std::string("no Alpha (1)");
            return ExpectSelected(c, "Alpha (1)");
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo removes the copy", [](Ctx& c) { return ByName(c.W, "Alpha (1)") == entt::null; }),
        SelectByName("Alpha"), Wait(3),
        Click(ObjectMenu()), Wait(2),
        Click(InspPopup(ICON_FA_TRASH "  Delete")), Wait(3),
        ExpectTrue("Alpha deleted, Inspector empty", [](Ctx& c) { return ByName(c.W, "Alpha") == entt::null && X::Selection(c.E).empty(); }),
        Shot("nothing selected", {kInspector}),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo restores Alpha", [](Ctx& c) { return ByName(c.W, "Alpha") != entt::null; }),
    });
    Add("inspector: object actions: Copy All Components, Paste Components as New; undo", {
        FreshScene(), Wait(2), SelectByName("Beta"), Wait(3),
        Click(ObjectMenu()), Wait(2),
        Click(InspPopup(ICON_FA_COPY "  Copy All Components")), Wait(2),
        ExpectTrue("clipboard holds Beta's components", [](Ctx& c) { return X::Clipboard(c.E).size() >= 2; }),
        SelectByName("Alpha"), Wait(3),
        Click(ObjectMenu()), Wait(2),
        Click(MenuItem("Paste Components as New")), Wait(3),
        Expect("Alpha got the Light and the Audio Source", [](Ctx& c) {
            const entt::entity a = ByName(c.W, "Alpha");
            const auto* l = c.W.Registry.try_get<LightComponent>(a);
            const auto* s = c.W.Registry.try_get<AudioSourceComponent>(a);
            if (!l || !s) return std::string("missing a component");
            return l->Intensity == 9.0f && s->Volume == 0.3f ? std::string() : std::string("values not copied");
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo removes both", [](Ctx& c) {
            const entt::entity a = ByName(c.W, "Alpha");
            return !c.W.Registry.all_of<LightComponent>(a) && !c.W.Registry.all_of<AudioSourceComponent>(a);
        }),
    });

    // ------------------------------------------------------------------ component menus
    Add("inspector: component menu Copy Component / Paste Component Values; undo", {
        FreshScene(), Wait(2), SelectByName("Beta"), Wait(3),
        Click(MoreButton("Light")), Wait(2),
        Shot("light menu"),
        Click(InspPopup(ICON_FA_COPY "  Copy Component")), Wait(2),
        ExpectTrue("Light on the component clipboard", [](Ctx& c) { return IX::ClipKind(c.E) == "Light"; }),
        SelectByName("Gamma"), Wait(3),
        Click(MoreButton("Light")), Wait(2),
        Click(InspPopup(ICON_FA_PASTE "  Paste Component Values")), Wait(3),
        ExpectTrue("Gamma's intensity = 9", [](Ctx& c) { return c.W.Registry.get<LightComponent>(ByName(c.W, "Gamma")).Intensity == 9.0f; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> 1", [](Ctx& c) { return c.W.Registry.get<LightComponent>(ByName(c.W, "Gamma")).Intensity == 1.0f; }),
        ExpectTrue("Gamma shows no Audio Source section", [](Ctx&) { return Find(Header("Audio Source")) == nullptr; }),
    });
    Add("inspector: Transform menu Reset, Copy Component and Paste onto another object; undo", {
        FreshScene(), Wait(2), SelectByName("Alpha"), Wait(3),
        Click(MoreButton("Transform")), Wait(2),
        Click(InspPopup(ICON_FA_COPY "  Copy Component")), Wait(2),
        SelectByName("Gamma"), Wait(3),
        Click(MoreButton("Transform")), Wait(2),
        Click(InspPopup(ICON_FA_PASTE "  Paste Component Values")), Wait(3),
        ExpectTrue("Gamma moved to Alpha's position", [](Ctx& c) {
            return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Gamma")).Position.x, -3.0f);
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> 3", [](Ctx& c) { return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Gamma")).Position.x, 3.0f); }),
        Click(MoreButton("Transform")), Wait(2),
        Click(InspPopup(ICON_FA_ROTATE_LEFT "  Reset")), Wait(3),
        ExpectTrue("reset to the origin", [](Ctx& c) { return c.W.Registry.get<TransformComponent>(ByName(c.W, "Gamma")).Position == glm::vec3(0.0f); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> 3", [](Ctx& c) { return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Gamma")).Position.x, 3.0f); }),
    });
    Add("inspector: component menu Reset to defaults; undo", {
        FreshScene(), Wait(2), SelectByName("Beta"), Wait(3),
        Click(MoreButton("Light")), Wait(2),
        Click(InspPopup(ICON_FA_ROTATE_LEFT "  Reset")), Wait(3),
        ExpectTrue("Light back to defaults", [](Ctx& c) { return c.W.Registry.get<LightComponent>(ByName(c.W, "Beta")).Intensity == LightComponent{}.Intensity; }),
        ExpectTrue("still has its Audio Source", [](Ctx& c) { return c.W.Registry.all_of<AudioSourceComponent>(ByName(c.W, "Beta")); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> 9", [](Ctx& c) { return c.W.Registry.get<LightComponent>(ByName(c.W, "Beta")).Intensity == 9.0f; }),
    });
    {
        auto before = std::make_shared<std::set<std::string>>();
        Query presetItem;
        presetItem.Pred = [](const UiItem& it) { return Contains(it.Root, "##Menu") && !g_Text.empty() && Visible(it.Label) == g_Text; };
        Add("inspector: component menu Save Preset, then Apply Preset restores the values; undo", {
            FreshScene(), Wait(2), SelectByName("Beta"), Wait(3),
            Do("list the presets folder", [before](Ctx&) { *before = InspListDir(InspPresetDir()); g_Text.clear(); }),
            Click(MoreButton("Light")), Wait(2),
            Click(InspPopup(ICON_FA_FLOPPY_DISK "  Save Preset")), Wait(3),
            Expect("a new Light preset file", [before](Ctx&) {
                for (const std::string& p : InspListDir(InspPresetDir()))
                    if (!before->count(p) && std::filesystem::path(p).extension() == ".preset") {
                        g_Text = std::filesystem::path(p).stem().string();
                        return std::string();
                    }
                return std::string("no new .preset in ") + InspPresetDir();
            }),
            Do("change the intensity", [](Ctx& c) {
                X::PushUndo(c.E, c.W, "Change Intensity");
                c.W.Registry.get<LightComponent>(ByName(c.W, "Beta")).Intensity = 42.0f;
            }), Wait(2),
            Click(MoreButton("Light")), Wait(2),
            Click(InspPopup(ICON_FA_SLIDERS "  Apply Preset")), Wait(3),
            Click(presetItem), Wait(3),
            ExpectTrue("preset applied: intensity 9 again", [](Ctx& c) { return c.W.Registry.get<LightComponent>(ByName(c.W, "Beta")).Intensity == 9.0f; }),
            Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
            ExpectTrue("undo -> 42", [](Ctx& c) { return c.W.Registry.get<LightComponent>(ByName(c.W, "Beta")).Intensity == 42.0f; }),
            Do("delete the new preset files", [before](Ctx&) {
                std::error_code ec;
                for (const std::string& p : InspListDir(InspPresetDir()))
                    if (!before->count(p)) std::filesystem::remove(p, ec);
            }),
        });
    }

    // ------------------------------------------------------------------ multi-selection
    Add("inspector: multi-selection: shared Transform X typed, Static checkbox, Duplicate / Delete buttons; undo each", InspCat({
        {FreshScene(), Wait(2),
         Do("select Alpha and Gamma", [](Ctx& c) { IX::SelectMany(c.E, {ByName(c.W, "Alpha"), ByName(c.W, "Gamma")}); }), Wait(3),
         Until("the multi-object Inspector", [](Ctx&) { return Find(Header("Object")) != nullptr; }),
         InspOpen("Transform"), InspUntilOpen("Transform"), Wait(3),
         Shot("two selected", {kInspector})},
        InspTypeInto(InspAxisField(0, 0), "7"),
        {ExpectTrue("both at x = 7", [](Ctx& c) {
             return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Position.x, 7.0f) &&
                    InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Gamma")).Position.x, 7.0f);
         }),
         ExpectTrue("untouched axes kept", [](Ctx& c) { return c.W.Registry.get<TransformComponent>(ByName(c.W, "Gamma")).Position.y == 0.0f; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> -3 and 3", [](Ctx& c) {
             return InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Position.x, -3.0f) &&
                    InspNear(c.W.Registry.get<TransformComponent>(ByName(c.W, "Gamma")).Position.x, 3.0f);
         }),
         ExpectTrue("selection survived the undo", [](Ctx& c) { return X::Selection(c.E).size() == 2; }),
         InspOpen("Object"), InspUntilOpen("Object"), Wait(3),
         InspReveal(ByExact("Static", kInspector)),
         Click(ByExact("Static", kInspector)), Wait(3),
         ExpectTrue("both Static", [](Ctx& c) {
             return c.W.Registry.all_of<StaticTag>(ByName(c.W, "Alpha")) && c.W.Registry.all_of<StaticTag>(ByName(c.W, "Gamma"));
         }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo -> neither", [](Ctx& c) {
             return !c.W.Registry.all_of<StaticTag>(ByName(c.W, "Alpha")) && !c.W.Registry.all_of<StaticTag>(ByName(c.W, "Gamma"));
         }),
         Do("note the object count", [](Ctx& c) { g_Number = (double)c.W.Registry.view<const NameComponent>().size(); }),
         InspReveal(ByExact(ICON_FA_CLONE, kInspector)),
         Click(ByExact(ICON_FA_CLONE, kInspector)), Wait(3),
         ExpectTrue("both duplicated", [](Ctx& c) { return (double)c.W.Registry.view<const NameComponent>().size() == g_Number + 2.0; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo removes the copies", [](Ctx& c) { return (double)c.W.Registry.view<const NameComponent>().size() == g_Number; }),
         Do("select Alpha and Gamma again", [](Ctx& c) { IX::SelectMany(c.E, {ByName(c.W, "Alpha"), ByName(c.W, "Gamma")}); }), Wait(3),
         InspReveal(ByExact(ICON_FA_TRASH, kInspector)),
         Click(ByExact(ICON_FA_TRASH, kInspector)), Wait(3),
         ExpectTrue("both deleted", [](Ctx& c) { return ByName(c.W, "Alpha") == entt::null && ByName(c.W, "Gamma") == entt::null; }),
         Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
         ExpectTrue("undo restores them", [](Ctx& c) { return ByName(c.W, "Alpha") != entt::null && ByName(c.W, "Gamma") != entt::null; })},
    }));
    Add("inspector: multi-selection shows only shared components (Light on Beta + Gamma)", {
        FreshScene(), Wait(2),
        Do("select Beta and Gamma", [](Ctx& c) { IX::SelectMany(c.E, {ByName(c.W, "Beta"), ByName(c.W, "Gamma")}); }), Wait(3),
        Until("a shared Light section", [](Ctx&) { return Find(Header("Light")) != nullptr; }),
        ExpectTrue("no Audio Source (only Beta has one)", [](Ctx&) { return Find(Header("Audio Source")) == nullptr; }),
        InspOpen("Light"), InspUntilOpen("Light"), WaitSeconds(0.3),
        Shot("shared light", {kInspector}),
    });

    // ------------------------------------------------------------------ lock
    Add("inspector: the padlock keeps showing the locked object while the selection moves", {
        FreshScene(), Wait(2), SelectByName("Alpha"), Wait(3),
        Click(InspCard(ICON_FA_LOCK_OPEN)), Wait(3),
        ExpectTrue("locked", [](Ctx& c) { return X::InspectorLocked(c.E); }),
        SelectByName("Beta"), Wait(3),
        Expect("selection moved to Beta", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
        ExpectTrue("Inspector still on Alpha (no Light section)", [](Ctx&) { return Find(Header("Light")) == nullptr; }),
        Shot("locked", {kInspector}),
        Click(InspCard(ICON_FA_LOCK)), Wait(3),
        ExpectTrue("unlocked", [](Ctx& c) { return !X::InspectorLocked(c.E); }),
        Until("Inspector follows the selection: Beta's Light", [](Ctx&) { return Find(Header("Light")) != nullptr; }),
    });
}
