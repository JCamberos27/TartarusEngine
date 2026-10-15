// Editor smoke suite: Hierarchy. Included by src/Tests/EditorUiTests.cpp inside its anonymous namespace
// (see README.md in this folder).
//
// Covers the Scene Hierarchy panel (EditorModuleHierarchy.cpp + EditorLayer_Hierarchy.cpp): the
// row context menu's Create submenu (every entity type), row selection (click / Ctrl / Shift /
// Ctrl+A / arrows / type-ahead), rename (F2, menu, double-click), the clipboard, Duplicate,
// Delete, parenting (drag, menu, Group, Unparent, drop on empty space), sibling order, the
// selection menu items, active / scene visibility toggles, favorites, row styles, hover keys,
// the search box, the type filter and sort popups, and expand / collapse. Structural edits are
// undone and compared against the exact previous tree (names, parents, sibling order).

} // namespace
struct EditorSmokeAccess_Hierarchy {
    static const std::vector<entt::entity>& Visible(EditorLayer& e) { return e.m_HierarchyVisibleOrder; }
    static entt::entity Renaming(EditorLayer& e) { return e.m_RenamingEntity; }
    static bool ClipboardEmpty(EditorLayer& e) { return e.m_Clipboard.empty(); }
    static entt::entity DefaultParent(EditorLayer& e, World& w) { return e.ResolveDefaultParent(w); }
    static bool ArrayDupShown(EditorLayer& e) { return e.m_ShowArrayDuplicate; }
    static void ResetArrayDup(EditorLayer& e) {
        e.m_ArrayDupCount[0] = 3; e.m_ArrayDupCount[1] = 1; e.m_ArrayDupCount[2] = 1;
        e.m_ArrayDupStep[0] = 2.0f; e.m_ArrayDupStep[1] = 0.0f; e.m_ArrayDupStep[2] = 0.0f;
    }
    static void ClearFilter(EditorLayer& e) { e.m_HierarchyFilter.clear(); }
    static const std::string& Filter(EditorLayer& e) { return e.m_HierarchyFilter; }
};
namespace {
using HX = EditorSmokeAccess_Hierarchy;

// The whole tree as one string, in Hierarchy order (OrderComponent): "name<parent" per object.
// Equal strings = same names, same parents, same sibling order.
std::string HierSnap(World& w) {
    std::vector<std::pair<int, std::string>> rows;
    for (auto [e, n] : w.Registry.view<const NameComponent>().each()) {
        const auto* o = w.Registry.try_get<OrderComponent>(e);
        const auto* h = w.Registry.try_get<HierarchyComponent>(e);
        const std::string parent = (h && h->Parent != entt::null) ? NameOf(w, h->Parent) : std::string("-");
        rows.emplace_back(o ? o->Value : -1, n.Name + "<" + parent);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out;
    for (const auto& r : rows) out += (out.empty() ? "" : ", ") + r.second;
    return out;
}
// The root objects' names in sibling order (what Set as First / Last Sibling and drag reorder change).
std::string HierRootOrder(World& w) {
    std::vector<std::pair<int, std::string>> rows;
    for (auto [e, n] : w.Registry.view<const NameComponent>().each()) {
        const auto* h = w.Registry.try_get<HierarchyComponent>(e);
        if (h && h->Parent != entt::null) continue;
        const auto* o = w.Registry.try_get<OrderComponent>(e);
        rows.emplace_back(o ? o->Value : -1, n.Name);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out;
    for (const auto& r : rows) out += (out.empty() ? "" : ",") + r.second;
    return out;
}
std::string HierParentOf(World& w, const char* child) {
    const entt::entity e = ByName(w, child);
    if (e == entt::null) return "(missing)";
    const auto* h = w.Registry.try_get<HierarchyComponent>(e);
    return (h && h->Parent != entt::null) ? NameOf(w, h->Parent) : std::string("-");
}
int HierCount(World& w) {
    int n = 0;
    for (auto e : w.Registry.view<const NameComponent>()) { (void)e; ++n; }
    return n;
}
// Remembers the tree; HierExpectRestored compares against it after an undo.
Step HierRemember() {
    return Do("remember the tree", [](Ctx& c) { g_Text = HierSnap(c.W); });
}
Step HierExpectRestored(const char* what) {
    return Expect(what, [](Ctx& c) {
        const std::string now = HierSnap(c.W);
        return now == g_Text ? std::string() : "tree is [" + now + "], expected [" + g_Text + "]";
    });
}
// Every Hierarchy test starts here: the fixture scene, no search text / type filter / sort.
Step HierFresh() {
    return Do("hierarchy: clear search, type filter and sort", [](Ctx& c) {
        HX::ClearFilter(c.E);
        EditorSettings::Get().HierarchyTypeFilterMask = 0;
        EditorSettings::Get().HierarchySortMode = 0;
        EditorSettings::Get().HierarchySortDesc = false;
    });
}
// Parents `child` under `parent` in code (set-up only).
Step HierParent(const char* child, const char* parent) {
    return Do(std::string("parent ") + child + " under " + parent, [child, parent](Ctx& c) {
        if (!c.W.SetParent(ByName(c.W, child), ByName(c.W, parent))) Fail(std::string("could not parent ") + child);
    });
}
// The toolbar's Expand all button (row open state lives in the Hierarchy window's storage, so
// it can only be set from inside that window's frame).
Step HierExpandAll() { return Click(ByExact(ICON_FA_ANGLES_DOWN, kHierarchy)); }
// Objects whose name starts with `prefix` (a paste may reuse the source's name).
int HierCountNamed(World& w, const std::string& prefix) {
    int n = 0;
    for (auto [e, nm] : w.Registry.view<const NameComponent>().each()) { (void)e; if (nm.Name.rfind(prefix, 0) == 0) ++n; }
    return n;
}
// An item in the open popup / menu whose raw label is exactly `raw` (icon + "  " + words).
Query HierMenu(std::string raw) {
    Query q;
    q.Exact = std::move(raw);
    q.Pred = [](const UiItem& it) { return Contains(it.Root, "##Popup") || Contains(it.Root, "##Menu"); };
    return q;
}
// A widget (e.g. "##active", "##svis") drawn on the Hierarchy row of `row`; Nth picks left to right.
Query HierRowPart(std::string row, std::string label, int nth = 0) {
    Query q;
    q.Root = kHierarchy;
    q.Nth = nth;
    const std::string tag = "row:" + row;
    q.Pred = [tag, label](const UiItem& it) {
        if (it.Label != label) return false;
        for (const UiItem& r : g_Reg.Last)
            if (r.Tag == tag && it.Rect.GetCenter().y > r.Rect.Min.y && it.Rect.GetCenter().y < r.Rect.Max.y) return true;
        return false;
    };
    return q;
}
// A row clicked on its disclosure chevron (the leading slot).
Query HierChevron(std::string row) {
    Query q = ByTag("row:" + row);
    q.AnchorX = 0.015f;
    return q;
}
int HierVisible(Ctx& c) { return (int)HX::Visible(c.E).size(); }

// One Create-submenu entry: its menu label, the object name it makes, and what it must carry.
struct HierCreateCase {
    const char* Label;
    const char* Name;
    std::function<std::string(entt::registry&, entt::entity)> Check;
};
std::string HierPrimitive(entt::registry& r, entt::entity e, ColliderComponent::Shape shape) {
    if (!r.all_of<RenderableComponent>(e)) return "no Mesh Renderer";
    const auto* col = r.try_get<ColliderComponent>(e);
    if (!col) return "no Collider";
    return col->Kind == shape ? std::string() : "collider shape " + std::to_string((int)col->Kind);
}
std::string HierLight(entt::registry& r, entt::entity e, LightComponent::Type kind) {
    const auto* l = r.try_get<LightComponent>(e);
    if (!l) return "no Light";
    if (r.all_of<RenderableComponent>(e)) return "a light got a mesh";
    return l->Kind == kind ? std::string() : "light kind " + std::to_string((int)l->Kind);
}

void RegisterHierarchyTests() {
    // ------------------------------------------------------------------ Create submenu
    using Shape = ColliderComponent::Shape;
    const std::vector<HierCreateCase> creates = {
        {ICON_FA_CUBE "  Cube", "Cube", [](entt::registry& r, entt::entity e) { return HierPrimitive(r, e, Shape::Box); }},
        {ICON_FA_CIRCLE "  Sphere", "Sphere", [](entt::registry& r, entt::entity e) { return HierPrimitive(r, e, Shape::Sphere); }},
        {ICON_FA_DATABASE "  Cylinder", "Cylinder", [](entt::registry& r, entt::entity e) { return HierPrimitive(r, e, Shape::ConvexHull); }},
        {ICON_FA_CAPSULES "  Capsule", "Capsule", [](entt::registry& r, entt::entity e) { return HierPrimitive(r, e, Shape::Capsule); }},
        {ICON_FA_FILTER "  Cone", "Cone", [](entt::registry& r, entt::entity e) { return HierPrimitive(r, e, Shape::ConvexHull); }},
        {ICON_FA_MOUNTAIN "  Pyramid", "Pyramid", [](entt::registry& r, entt::entity e) { return HierPrimitive(r, e, Shape::ConvexHull); }},
        {ICON_FA_LIFE_RING "  Donut", "Donut", [](entt::registry& r, entt::entity e) { return HierPrimitive(r, e, Shape::Mesh); }},
        {ICON_FA_SQUARE "  Plane", "Plane", [](entt::registry& r, entt::entity e) { return HierPrimitive(r, e, Shape::Mesh); }},
        {ICON_FA_DIAGRAM_PROJECT "  Empty", "Empty", [](entt::registry& r, entt::entity e) {
            return r.all_of<RenderableComponent>(e) || r.all_of<LightComponent>(e) || r.all_of<CameraComponent>(e)
                ? std::string("the Empty carries a mesh, light or camera") : std::string();
        }},
        {ICON_FA_GRIP_LINES "  Separator", "Separator", [](entt::registry& r, entt::entity e) {
            const auto* s = r.try_get<HierarchyStyleComponent>(e);
            return s && s->Separator ? std::string() : std::string("not styled as a separator row");
        }},
        {ICON_FA_LIGHTBULB "  Point Light", "Point Light", [](entt::registry& r, entt::entity e) { return HierLight(r, e, LightComponent::Type::Point); }},
        {ICON_FA_BULLSEYE "  Spot Light", "Spot Light", [](entt::registry& r, entt::entity e) { return HierLight(r, e, LightComponent::Type::Spot); }},
        {ICON_FA_SUN "  Directional Light", "Directional Light", [](entt::registry& r, entt::entity e) {
            const std::string why = HierLight(r, e, LightComponent::Type::Directional);
            if (!why.empty()) return why;
            return r.get<LightComponent>(e).Shadow.Enabled ? std::string() : std::string("a new sun should cast shadows");
        }},
        {ICON_FA_PERSON "  First Person Player", "Player Spawn", [](entt::registry& r, entt::entity e) {
            return r.all_of<FirstPersonControllerComponent>(e) && r.all_of<FirstPersonBodyComponent>(e)
                ? std::string() : std::string("missing the First Person Controller or Body");
        }},
        {ICON_FA_VIDEO "  Camera", "Camera", [](entt::registry& r, entt::entity e) {
            return r.all_of<CameraComponent>(e) ? std::string() : std::string("no Camera component");
        }},
    };
    for (const HierCreateCase& cc : creates) {
        const std::string label = cc.Label, name = cc.Name;
        const auto check = cc.Check;
        Add(std::string("hierarchy: right-click > Create > ") + name + ", undo removes it", {
            FreshScene(), HierFresh(), Wait(2),
            HierRemember(),
            RightClick(ByTag("row:Alpha")), Wait(2),
            Click(HierMenu(ICON_FA_PLUS "  Create")), Wait(2),
            Click(HierMenu(label)), Wait(3),
            Expect("created with the right components", [name, check](Ctx& c) {
                const entt::entity e = ByName(c.W, name.c_str());
                if (e == entt::null) return "no object named " + name + " (tree: " + HierSnap(c.W) + ")";
                if (HierCount(c.W) != 4) return "expected 4 objects, have " + std::to_string(HierCount(c.W));
                if (!c.W.Registry.all_of<TransformComponent>(e)) return std::string("no Transform");
                if (HierParentOf(c.W, name.c_str()) != "-") return "created under " + HierParentOf(c.W, name.c_str());
                return check(c.W.Registry, e);
            }),
            Expect("the new object is selected", [name](Ctx& c) { return ExpectSelected(c, name.c_str()); }),
            Until("its row is listed", [name](Ctx&) { return Find(ByTag("row:" + name)) != nullptr; }),
            Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
            ExpectTrue("undo removed it", [name](Ctx& c) { return ByName(c.W, name.c_str()) == entt::null; }),
            HierExpectRestored("tree restored"),
        });
    }

    // ------------------------------------------------------------------ populated shot
    Add("hierarchy: populated tree (children, separator, styled and inactive rows)", {
        FreshScene(), HierFresh(),
        Do("build a tree", [](Ctx& c) {
            const glm::vec3 zero(0.0f), one(1.0f);
            const entt::entity sep = c.W.CreateEmptyEntity(zero, zero, one, "Lights");
            HierarchyStyleComponent st;
            st.Separator = true;
            st.Color = IM_COL32(230, 160, 60, 255);
            c.W.Registry.emplace<HierarchyStyleComponent>(sep, st);
            c.W.SetParent(ByName(c.W, "Beta"), sep);
            c.W.SetParent(ByName(c.W, "Gamma"), sep);
            const entt::entity cube = c.W.CreateModelEntity(c.A.CreatePrimitive("cube"), zero, zero, one, "Crate");
            c.W.SetParent(cube, ByName(c.W, "Alpha"));
            const entt::entity child = c.W.CreateEmptyEntity(zero, zero, one, "Crate Lid");
            c.W.SetParent(child, cube);
            HierarchyStyleComponent tint;
            tint.Color = IM_COL32(80, 160, 240, 255);
            tint.FillMode = HierarchyStyleComponent::Flat;
            c.W.Registry.emplace<HierarchyStyleComponent>(cube, tint);
            c.W.Registry.emplace<DeactivatedTag>(ByName(c.W, "Gamma"));
            const entt::entity cam = c.W.CreateEmptyEntity(glm::vec3(0, 2, 8), zero, one, "Main Camera");
            c.W.Registry.emplace<CameraComponent>(cam);
        }), Wait(3),
        HierExpandAll(), Wait(3),
        SelectByName("Crate"), Wait(3),
        Until("every row is drawn", [](Ctx& c) { return HierVisible(c) == 7; }),
        ExpectTrue("Crate Lid is listed under Crate", [](Ctx& c) { return HierParentOf(c.W, "Crate Lid") == "Crate"; }),
        HoverPanel(kScene), Wait(2),
        Shot("populated", {kHierarchy}),
    });

    // ------------------------------------------------------------------ selection
    Add("hierarchy: click, Ctrl+click, Shift+click and Ctrl+A select rows", {
        FreshScene(), HierFresh(), Wait(2),
        Click(ByTag("row:Alpha")), Wait(2),
        Expect("click selects Alpha", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
        Click(ByTag("row:Gamma"), ImGuiMod_Ctrl), Wait(2),
        ExpectTrue("Ctrl+click adds Gamma", [](Ctx& c) {
            const auto s = X::Selection(c.E);
            return s.size() == 2 && std::find(s.begin(), s.end(), ByName(c.W, "Gamma")) != s.end();
        }),
        Click(ByTag("row:Alpha"), ImGuiMod_Ctrl), Wait(2),
        Expect("Ctrl+click again removes Alpha", [](Ctx& c) {
            if (X::Selection(c.E).size() != 1) return "selection size " + std::to_string(X::Selection(c.E).size());
            return ExpectSelected(c, "Gamma");
        }),
        Click(ByTag("row:Alpha")), Wait(2),
        Click(ByTag("row:Gamma"), ImGuiMod_Shift), Wait(2),
        ExpectTrue("Shift+click selects the range", [](Ctx& c) { return X::Selection(c.E).size() == 3; }),
        Expect("the clicked row is the active one", [](Ctx& c) { return ExpectSelected(c, "Gamma"); }),
        Click(ByTag("row:Beta")), Wait(2),
        ExpectTrue("a plain click replaces", [](Ctx& c) { return X::Selection(c.E).size() == 1; }),
        Key(ImGuiKey_A, ImGuiMod_Ctrl), Wait(2),
        ExpectTrue("Ctrl+A over the focused Hierarchy selects every row", [](Ctx& c) { return X::Selection(c.E).size() == 3; }),
        Shot("three selected", {kHierarchy}),
    });
    Add("hierarchy: arrow keys and type-ahead move the selection", {
        FreshScene(), HierFresh(), Wait(2),
        Click(ByTag("row:Alpha")), Wait(2),
        Key(ImGuiKey_DownArrow), Wait(2),
        Expect("Down -> Beta", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
        Key(ImGuiKey_End), Wait(2),
        Expect("End -> Gamma", [](Ctx& c) { return ExpectSelected(c, "Gamma"); }),
        Key(ImGuiKey_UpArrow), Wait(2),
        Expect("Up -> Beta", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
        Key(ImGuiKey_Home), Wait(2),
        Expect("Home -> Alpha", [](Ctx& c) { return ExpectSelected(c, "Alpha"); }),
        WaitSeconds(1.0),
        TypeText("g"), Wait(3),
        Expect("typing g jumps to Gamma", [](Ctx& c) { return ExpectSelected(c, "Gamma"); }),
    });
    Add("hierarchy: context menu Select All, Deselect All, Invert Selection, Select Children", {
        FreshScene(), HierFresh(), HierParent("Gamma", "Alpha"), Wait(3),
        RightClick(ByTag("row:Beta")), Wait(2),
        Expect("right-click selects the row", [](Ctx& c) { return ExpectSelected(c, "Beta"); }),
        Click(HierMenu(ICON_FA_OBJECT_UNGROUP "  Select All")), Wait(2),
        ExpectTrue("Select All", [](Ctx& c) { return X::Selection(c.E).size() == 3; }),
        RightClick(ByTag("row:Beta")), Wait(2),
        Click(HierMenu(ICON_FA_BAN "  Deselect All")), Wait(2),
        ExpectTrue("Deselect All", [](Ctx& c) { return X::Selection(c.E).empty(); }),
        RightClick(ByTag("row:Beta")), Wait(2),
        Click(HierMenu(ICON_FA_RIGHT_LEFT "  Invert Selection")), Wait(2),
        ExpectTrue("Invert: everything but Beta", [](Ctx& c) {
            const auto s = X::Selection(c.E);
            return s.size() == 2 && std::find(s.begin(), s.end(), ByName(c.W, "Beta")) == s.end();
        }),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Expect("right-click on a selected row keeps the selection", [](Ctx& c) {
            return X::Selection(c.E).size() == 2 ? std::string() : "selection size " + std::to_string(X::Selection(c.E).size());
        }),
        Key(ImGuiKey_Escape), Wait(2),
        Click(ByTag("row:Alpha")), Wait(2),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Click(HierMenu(ICON_FA_SITEMAP "  Select Children")), Wait(2),
        ExpectTrue("Select Children adds Gamma", [](Ctx& c) {
            const auto s = X::Selection(c.E);
            return s.size() == 2 && std::find(s.begin(), s.end(), ByName(c.W, "Gamma")) != s.end();
        }),
    });

    // ------------------------------------------------------------------ rename
    Add("hierarchy: F2 renames, Enter commits, undo restores", {
        FreshScene(), HierFresh(), Wait(2),
        Click(ByTag("row:Beta")), Wait(2),
        Key(ImGuiKey_F2), Wait(3),
        ExpectTrue("rename field open", [](Ctx& c) { return HX::Renaming(c.E) == ByName(c.W, "Beta"); }),
        TypeText("Bravo"), Wait(2),
        Key(ImGuiKey_Enter), Wait(3),
        ExpectTrue("renamed to Bravo", [](Ctx& c) { return ByName(c.W, "Bravo") != entt::null && ByName(c.W, "Beta") == entt::null; }),
        ExpectTrue("field closed", [](Ctx& c) { return HX::Renaming(c.E) == entt::null; }),
        Until("the row shows the new name", [](Ctx&) { return Find(ByTag("row:Bravo")) != nullptr; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> Beta", [](Ctx& c) { return ByName(c.W, "Beta") != entt::null && ByName(c.W, "Bravo") == entt::null; }),
    });
    Add("hierarchy: Rename from the menu; a blank name is refused; Esc cancels", {
        FreshScene(), HierFresh(), Wait(2),
        RightClick(ByTag("row:Gamma")), Wait(2),
        Click(HierMenu(ICON_FA_PEN "  Rename")), Wait(3),
        ExpectTrue("rename field open", [](Ctx& c) { return HX::Renaming(c.E) == ByName(c.W, "Gamma"); }),
        TypeText("   "), Wait(2),
        Key(ImGuiKey_Enter), Wait(3),
        ExpectTrue("blank name refused, field still open", [](Ctx& c) {
            return ByName(c.W, "Gamma") != entt::null && HX::Renaming(c.E) == ByName(c.W, "Gamma");
        }),
        Click(ByExact("##Rename", kHierarchy)), Wait(2), // Enter took the keyboard focus off the field
        Key(ImGuiKey_Escape), Wait(3),
        ExpectTrue("Esc closed the field", [](Ctx& c) { return HX::Renaming(c.E) == entt::null; }),
        ExpectTrue("name unchanged", [](Ctx& c) { return ByName(c.W, "Gamma") != entt::null; }),
        DoubleClick(ByTag("row:Alpha")), Wait(3),
        ExpectTrue("double-click opens rename", [](Ctx& c) { return HX::Renaming(c.E) == ByName(c.W, "Alpha"); }),
        TypeText("Anchor"), Key(ImGuiKey_Enter), Wait(3),
        ExpectTrue("renamed to Anchor", [](Ctx& c) { return ByName(c.W, "Anchor") != entt::null; }),
    });

    // ------------------------------------------------------------------ clipboard / duplicate / delete
    Add("hierarchy: Ctrl+D duplicates the selection, undo removes the copy", {
        FreshScene(), HierFresh(), Wait(2),
        HierRemember(),
        Click(ByTag("row:Alpha")), Wait(2),
        Key(ImGuiKey_D, ImGuiMod_Ctrl), Wait(3),
        Expect("Alpha (1) created and selected", [](Ctx& c) {
            if (ByName(c.W, "Alpha (1)") == entt::null) return "tree: " + HierSnap(c.W);
            return ExpectSelected(c, "Alpha (1)");
        }),
        ExpectTrue("Ctrl+D duplicates in place", [](Ctx& c) {
            const glm::vec3 a = c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Position;
            const glm::vec3 b = c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha (1)")).Position;
            return glm::length(a - b) < 1e-4f;
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo restores the tree"),
    });
    Add("hierarchy: menu Duplicate copies a child beside its source, offset", {
        FreshScene(), HierFresh(), HierParent("Gamma", "Alpha"), Wait(2), HierExpandAll(), Wait(3),
        HierRemember(),
        RightClick(ByTag("row:Gamma")), Wait(2),
        Click(HierMenu(ICON_FA_CLONE "  Duplicate")), Wait(3),
        Expect("Gamma (1) under Alpha", [](Ctx& c) {
            if (ByName(c.W, "Gamma (1)") == entt::null) return "tree: " + HierSnap(c.W);
            return HierParentOf(c.W, "Gamma (1)") == "Alpha" ? std::string() : "parent " + HierParentOf(c.W, "Gamma (1)");
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo restores the tree"),
    });
    Add("hierarchy: Duplicate Array dialog makes copies, undo removes them", {
        FreshScene(), HierFresh(), Wait(2),
        Do("default counts", [](Ctx& c) { HX::ResetArrayDup(c.E); }),
        HierRemember(),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Click(HierMenu(ICON_FA_TABLE_CELLS "  Duplicate Array\xE2\x80\xA6")), Wait(3),
        ExpectTrue("dialog open", [](Ctx& c) { return HX::ArrayDupShown(c.E); }),
        Shot("dialog"),
        Click(ByExact("Create", "##ArrayDup")), Wait(3),
        ExpectTrue("two copies (3 x 1 x 1)", [](Ctx& c) {
            return ByName(c.W, "Alpha (1)") != entt::null && ByName(c.W, "Alpha (2)") != entt::null && HierCount(c.W) == 5;
        }),
        ExpectTrue("dialog closed", [](Ctx& c) { return !HX::ArrayDupShown(c.E); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo restores the tree"),
    });
    Add("hierarchy: Ctrl+C / Ctrl+V pastes a copy; Ctrl+X cuts; undo both", {
        FreshScene(), HierFresh(), Wait(2),
        HierRemember(),
        Click(ByTag("row:Alpha")), Wait(2),
        Key(ImGuiKey_C, ImGuiMod_Ctrl), Wait(2),
        ExpectTrue("clipboard filled", [](Ctx& c) { return !HX::ClipboardEmpty(c.E); }),
        Key(ImGuiKey_V, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("a fourth object", [](Ctx& c) { return HierCount(c.W) == 4; }),
        ExpectTrue("the paste is selected and is a copy of Alpha", [](Ctx& c) {
            const entt::entity s = X::Selected(c.E);
            return s != entt::null && NameOf(c.W, s).rfind("Alpha", 0) == 0 && HierCountNamed(c.W, "Alpha") == 2;
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo removes the paste"),
        Click(ByTag("row:Gamma")), Wait(2),
        Key(ImGuiKey_X, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("Gamma cut", [](Ctx& c) { return ByName(c.W, "Gamma") == entt::null && HierCount(c.W) == 2; }),
        Key(ImGuiKey_V, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("pasted back", [](Ctx& c) { return ByName(c.W, "Gamma") != entt::null && HierCount(c.W) == 3; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo the paste", [](Ctx& c) { return ByName(c.W, "Gamma") == entt::null; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo the cut"),
    });
    Add("hierarchy: menu Copy, Paste, Cut; undo", {
        FreshScene(), HierFresh(), Wait(2),
        HierRemember(),
        RightClick(ByTag("row:Beta")), Wait(2),
        Click(HierMenu(ICON_FA_COPY "  Copy")), Wait(2),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Click(HierMenu(ICON_FA_PASTE "  Paste")), Wait(3),
        ExpectTrue("a pasted copy of Beta with its components", [](Ctx& c) {
            if (HierCount(c.W) != 4 || HierCountNamed(c.W, "Beta") != 2) return false;
            const entt::entity s = X::Selected(c.E);
            return s != entt::null && c.W.Registry.all_of<LightComponent>(s) && c.W.Registry.all_of<AudioSourceComponent>(s);
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo removes the paste"),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Click(HierMenu(ICON_FA_SCISSORS "  Cut")), Wait(3),
        ExpectTrue("Alpha cut", [](Ctx& c) { return ByName(c.W, "Alpha") == entt::null; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo restores Alpha"),
    });
    Add("hierarchy: Delete key and menu Delete (with children); undo", {
        FreshScene(), HierFresh(), HierParent("Gamma", "Alpha"), Wait(3),
        HierRemember(),
        Click(ByTag("row:Beta")), Wait(2),
        Key(ImGuiKey_Delete), Wait(3),
        ExpectTrue("Beta deleted", [](Ctx& c) { return ByName(c.W, "Beta") == entt::null && X::Selection(c.E).empty(); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo restores Beta"),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Click(HierMenu(ICON_FA_TRASH "  Delete")), Wait(3),
        ExpectTrue("Alpha and its child deleted", [](Ctx& c) { return HierCount(c.W) == 1 && ByName(c.W, "Beta") != entt::null; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo restores Alpha and Gamma"),
    });

    // ------------------------------------------------------------------ parenting
    Add("hierarchy: drag a row onto another parents it; undo", {
        FreshScene(), HierFresh(), Wait(2),
        HierRemember(),
        Drag(ByTag("row:Gamma"), AtItem(ByTag("row:Alpha"))), Wait(3),
        Expect("Gamma under Alpha", [](Ctx& c) {
            return HierParentOf(c.W, "Gamma") == "Alpha" ? std::string() : "parent " + HierParentOf(c.W, "Gamma");
        }),
        ExpectTrue("world position kept", [](Ctx& c) {
            const glm::vec3 p = glm::vec3(c.W.ComposeWorldTransform(ByName(c.W, "Gamma"))[3]);
            return glm::length(p - glm::vec3(3, 0, 0)) < 1e-3f;
        }),
        HierExpandAll(), Wait(3),
        Shot("parented", {kHierarchy}),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo unparents"),
    });
    Add("hierarchy: drag a row between rows reorders it; undo", {
        FreshScene(), HierFresh(), Wait(2),
        HierRemember(),
        Drag(ByTag("row:Gamma"), [](Ctx&, ImVec2& out) {
            const UiItem* a = Find(ByTag("row:Alpha"));
            if (!a) return false;
            out = ImVec2(a->Rect.GetCenter().x, a->Rect.Max.y - 2.0f); // the "insert after" strip below Alpha's name
            return true;
        }), Wait(3),
        Expect("Gamma now right after Alpha", [](Ctx& c) {
            const std::string o = HierRootOrder(c.W);
            return o == "Alpha,Gamma,Beta" ? std::string() : "order " + o;
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo restores the order"),
    });
    Add("hierarchy: drop a child on the empty space below the tree unparents it; undo", {
        FreshScene(), HierFresh(), HierParent("Gamma", "Alpha"), Wait(2), HierExpandAll(), Wait(3),
        HierRemember(),
        Drag(ByTag("row:Gamma"), [](Ctx&, ImVec2& out) {
            for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
                if (w->WasActive && !w->Hidden && Contains(w->Name, kHierarchy) && !Contains(w->Name, "/")) {
                    out = ImVec2(w->InnerRect.GetCenter().x, w->InnerRect.Max.y - 60.0f);
                    return true;
                }
            return false;
        }), Wait(3),
        ExpectTrue("Gamma is a root again", [](Ctx& c) { return HierParentOf(c.W, "Gamma") == "-"; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo re-parents"),
    });
    Add("hierarchy: Create Empty Child, Unparent and Group into Empty Parent; undo each", {
        FreshScene(), HierFresh(), Wait(2),
        HierRemember(),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Click(HierMenu(ICON_FA_DIAGRAM_PROJECT "  Create Empty Child")), Wait(3),
        Expect("Empty under Alpha, selected", [](Ctx& c) {
            if (HierParentOf(c.W, "Empty") != "Alpha") return "Empty's parent: " + HierParentOf(c.W, "Empty");
            return ExpectSelected(c, "Empty");
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo removes the child"),
        HierParent("Gamma", "Alpha"), Wait(2), HierExpandAll(), Wait(3),
        HierRemember(),
        RightClick(ByTag("row:Gamma")), Wait(2),
        Click(HierMenu(ICON_FA_LINK_SLASH "  Unparent")), Wait(3),
        ExpectTrue("Gamma is a root", [](Ctx& c) { return HierParentOf(c.W, "Gamma") == "-"; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo re-parents Gamma"),
        Click(ByTag("row:Beta")), Wait(2),
        Click(ByTag("row:Alpha"), ImGuiMod_Ctrl), Wait(2),
        RightClick(ByTag("row:Beta")), Wait(2),
        Click(HierMenu(ICON_FA_OBJECT_GROUP "  Group into Empty Parent")), Wait(3),
        Expect("Alpha and Beta grouped under a new Group", [](Ctx& c) {
            if (HierParentOf(c.W, "Alpha") != "Group" || HierParentOf(c.W, "Beta") != "Group") return "tree: " + HierSnap(c.W);
            return ExpectSelected(c, "Group");
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo removes the group"),
    });
    Add("hierarchy: Set as First / Last Sibling; undo", {
        FreshScene(), HierFresh(), Wait(2),
        HierRemember(),
        RightClick(ByTag("row:Gamma")), Wait(2),
        Click(HierMenu(ICON_FA_ANGLES_UP "  Set as First Sibling")), Wait(3),
        Expect("Gamma first", [](Ctx& c) { const std::string o = HierRootOrder(c.W); return o == "Gamma,Alpha,Beta" ? std::string() : "order " + o; }),
        RightClick(ByTag("row:Gamma")), Wait(2),
        Click(HierMenu(ICON_FA_ANGLES_DOWN "  Set as Last Sibling")), Wait(3),
        Expect("Gamma last", [](Ctx& c) { const std::string o = HierRootOrder(c.W); return o == "Alpha,Beta,Gamma" ? std::string() : "order " + o; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        Expect("undo -> Gamma first again", [](Ctx& c) { const std::string o = HierRootOrder(c.W); return o == "Gamma,Alpha,Beta" ? std::string() : "order " + o; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("second undo -> the original order"),
    });
    Add("hierarchy: Move to Camera moves the object in front of the editor camera; undo", {
        FreshScene(), HierFresh(), Wait(2),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Click(HierMenu(ICON_FA_LOCATION_CROSSHAIRS "  Move to Camera")), Wait(3),
        ExpectTrue("Alpha 5 units in front of the camera", [](Ctx& c) {
            const glm::vec3 want = c.Cam.Position + c.Cam.Front() * 5.0f;
            return glm::length(c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Position - want) < 1e-3f;
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> back at -3,0,0", [](Ctx& c) {
            return glm::length(c.W.Registry.get<TransformComponent>(ByName(c.W, "Alpha")).Position - glm::vec3(-3, 0, 0)) < 1e-4f;
        }),
    });
    Add("hierarchy: Save as Prefab... is offered (native dialog not opened)", {
        FreshScene(), HierFresh(), Wait(2),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Until("Save as Prefab... listed and enabled", [](Ctx&) {
            const UiItem* it = Find(HierMenu(ICON_FA_BOX_ARCHIVE "  Save as Prefab..."));
            return it && !(it->ItemFlags & ImGuiItemFlags_Disabled);
        }),
        Shot("row menu"),
        Key(ImGuiKey_Escape), Wait(2),
    });

    // ------------------------------------------------------------------ active / visibility / favorites / style
    Add("hierarchy: Toggle Active State (menu), the row checkbox, scene hide / lock; undo each", {
        FreshScene(), HierFresh(), Wait(2),
        RightClick(ByTag("row:Beta")), Wait(2),
        Click(HierMenu(ICON_FA_EYE "  Toggle Active State")), Wait(3),
        ExpectTrue("Beta inactive", [](Ctx& c) { return c.W.Registry.all_of<DeactivatedTag>(ByName(c.W, "Beta")); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> active", [](Ctx& c) { return !c.W.Registry.all_of<DeactivatedTag>(ByName(c.W, "Beta")); }),
        Click(HierRowPart("Gamma", "##active")), Wait(3),
        ExpectTrue("row checkbox deactivates Gamma", [](Ctx& c) { return c.W.Registry.all_of<DeactivatedTag>(ByName(c.W, "Gamma")); }),
        Expect("the checkbox click did not select the row", [](Ctx& c) { return X::Selected(c.E) == ByName(c.W, "Gamma") ? std::string("Gamma got selected") : std::string(); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> active", [](Ctx& c) { return !c.W.Registry.all_of<DeactivatedTag>(ByName(c.W, "Gamma")); }),
        Click(HierRowPart("Alpha", "##svis", 0)), Wait(3),
        ExpectTrue("Alpha hidden in the Scene view", [](Ctx& c) { return c.W.Registry.all_of<HiddenInSceneTag>(ByName(c.W, "Alpha")); }),
        Click(HierRowPart("Alpha", "##svis", 1)), Wait(3),
        ExpectTrue("Alpha locked in the Scene view", [](Ctx& c) { return c.W.Registry.all_of<SceneLockedTag>(ByName(c.W, "Alpha")); }),
        Shot("hidden and locked", {kHierarchy}),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo unlocks", [](Ctx& c) { return !c.W.Registry.all_of<SceneLockedTag>(ByName(c.W, "Alpha")); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo shows it again", [](Ctx& c) { return !c.W.Registry.all_of<HiddenInSceneTag>(ByName(c.W, "Alpha")); }),
    });
    Add("hierarchy: Favorite from the row menu toggles a favorite", {
        FreshScene(), HierFresh(), Wait(2),
        RightClick(ByTag("row:Gamma")), Wait(2),
        Click(HierMenu(ICON_FA_STAR "  Favorite")), Wait(3),
        ExpectTrue("Gamma is a favorite", [](Ctx&) {
            for (const auto& p : Enhancers::EnhancerUserState::Get().FavoritePages)
                for (const auto& it : p.Items) if (it.Label == "Gamma") return true;
            return false;
        }),
        RightClick(ByTag("row:Gamma")), Wait(2),
        Click(HierMenu(ICON_FA_STAR "  Favorite")), Wait(3),
        ExpectTrue("clicked again: removed", [](Ctx&) {
            for (const auto& p : Enhancers::EnhancerUserState::Get().FavoritePages)
                for (const auto& it : p.Items) if (it.Label == "Gamma") return false;
            return true;
        }),
    });
    Add("hierarchy: Row Style > Separator Row, then Clear Style; undo", {
        FreshScene(), HierFresh(), Wait(2),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Click(HierMenu(ICON_FA_PALETTE "  Row Style")), Wait(2),
        Click(HierMenu(ICON_FA_GRIP_LINES "  Separator Row")), Wait(3),
        ExpectTrue("Alpha is a separator row", [](Ctx& c) {
            const auto* s = c.W.Registry.try_get<HierarchyStyleComponent>(ByName(c.W, "Alpha"));
            return s && s->Separator;
        }),
        Shot("separator", {kHierarchy}),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Click(HierMenu(ICON_FA_PALETTE "  Row Style")), Wait(2),
        Click(HierMenu(ICON_FA_ERASER "  Clear Style")), Wait(3),
        ExpectTrue("style removed", [](Ctx& c) { return !c.W.Registry.all_of<HierarchyStyleComponent>(ByName(c.W, "Alpha")); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> separator again", [](Ctx& c) {
            const auto* s = c.W.Registry.try_get<HierarchyStyleComponent>(ByName(c.W, "Alpha"));
            return s && s->Separator;
        }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo -> unstyled", [](Ctx& c) { return !c.W.Registry.all_of<HierarchyStyleComponent>(ByName(c.W, "Alpha")); }),
    });
    Add("hierarchy: Default Parent (menu and D) receives new objects", {
        FreshScene(), HierFresh(), Wait(2),
        RightClick(ByTag("row:Beta")), Wait(2),
        Click(HierMenu(ICON_FA_ARROW_RIGHT_TO_BRACKET "  Default Parent")), Wait(3),
        ExpectTrue("Beta is the default parent", [](Ctx& c) { return HX::DefaultParent(c.E, c.W) == ByName(c.W, "Beta"); }),
        RightClick(ByTag("row:Alpha")), Wait(2),
        Click(HierMenu(ICON_FA_PLUS "  Create")), Wait(2),
        Click(HierMenu(ICON_FA_DIAGRAM_PROJECT "  Empty")), Wait(3),
        Expect("the new Empty went under Beta", [](Ctx& c) { return HierParentOf(c.W, "Empty") == "Beta" ? std::string() : "parent " + HierParentOf(c.W, "Empty"); }),
        MoveToItem(ByTag("row:Beta")), Wait(2),
        Key(ImGuiKey_D), Wait(3),
        ExpectTrue("D over Beta clears it", [](Ctx& c) { return HX::DefaultParent(c.E, c.W) == entt::null; }),
    });
    Add("hierarchy: hover keys A (active) and X (delete) act on the row under the mouse; undo", {
        FreshScene(), HierFresh(), Wait(2),
        SelectByName("Alpha"), Wait(2),
        MoveToItem(ByTag("row:Beta")), Wait(2),
        Key(ImGuiKey_A), Wait(3),
        ExpectTrue("A deactivated Beta", [](Ctx& c) { return c.W.Registry.all_of<DeactivatedTag>(ByName(c.W, "Beta")); }),
        ExpectTrue("Alpha untouched", [](Ctx& c) { return !c.W.Registry.all_of<DeactivatedTag>(ByName(c.W, "Alpha")); }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        ExpectTrue("undo", [](Ctx& c) { return !c.W.Registry.all_of<DeactivatedTag>(ByName(c.W, "Beta")); }),
        HierRemember(),
        MoveToItem(ByTag("row:Gamma")), Wait(2),
        Key(ImGuiKey_X), Wait(3),
        ExpectTrue("X deleted Gamma", [](Ctx& c) { return ByName(c.W, "Gamma") == entt::null; }),
        Key(ImGuiKey_Z, ImGuiMod_Ctrl), Wait(3),
        HierExpectRestored("undo restores Gamma"),
    });

    // ------------------------------------------------------------------ search / filter / sort / expand
    Add("hierarchy: the search box filters by name and t:Tag", {
        FreshScene(), HierFresh(),
        Do("tag Beta", [](Ctx& c) { TagComponent t; t.Tag = "Enemy"; c.W.Registry.emplace_or_replace<TagComponent>(ByName(c.W, "Beta"), t); }),
        HierParent("Gamma", "Alpha"), Wait(3),
        Click(ByExact("##search", kHierarchy)), Wait(2),
        TypeText("amm"), Wait(3),
        ExpectTrue("filter text reached the editor", [](Ctx& c) { return HX::Filter(c.E) == "amm"; }),
        Until("only Gamma listed (flat, without its parent)", [](Ctx& c) {
            const auto& v = HX::Visible(c.E);
            return v.size() == 1 && v[0] == ByName(c.W, "Gamma");
        }),
        Shot("filtered", {kHierarchy}),
        Key(ImGuiKey_A, ImGuiMod_Ctrl), Key(ImGuiKey_Backspace), Wait(2),
        TypeText("t:Enemy"), Wait(3),
        Until("t:Enemy lists Beta", [](Ctx& c) {
            const auto& v = HX::Visible(c.E);
            return v.size() == 1 && v[0] == ByName(c.W, "Beta");
        }),
        Key(ImGuiKey_A, ImGuiMod_Ctrl), Key(ImGuiKey_Backspace), Wait(2),
        TypeText("zzz"), Wait(3),
        Until("nothing matches", [](Ctx& c) { return HX::Visible(c.E).empty(); }),
        Key(ImGuiKey_A, ImGuiMod_Ctrl), Key(ImGuiKey_Backspace), Wait(2),
        Key(ImGuiKey_Enter), Wait(2),
        Until("cleared: the tree is back", [](Ctx& c) { return HX::Filter(c.E).empty() && HierVisible(c) == 3; }),
        HierFresh(),
    });
    Add("hierarchy: type filter popup (Lights) and sort popup (Name, Descending)", {
        FreshScene(), HierFresh(), Wait(2),
        Click(ByExact(ICON_FA_FILTER, kHierarchy)), Wait(2),
        Click(MenuItem("Lights")), Wait(3),
        Until("only the lights listed", [](Ctx& c) {
            const auto& v = HX::Visible(c.E);
            return v.size() == 2 && std::find(v.begin(), v.end(), ByName(c.W, "Alpha")) == v.end();
        }),
        Click(ByExact(ICON_FA_FILTER, kHierarchy)), Wait(2),
        Click(MenuItem("Show everything")), Wait(3),
        Until("everything listed", [](Ctx& c) { return HierVisible(c) == 3 && EditorSettings::Get().HierarchyTypeFilterMask == 0; }),
        Click(ByExact(ICON_FA_ARROW_DOWN_SHORT_WIDE, kHierarchy)), Wait(2),
        Click(MenuItem("Name")), Wait(2),
        ExpectTrue("sort mode Name", [](Ctx&) { return EditorSettings::Get().HierarchySortMode == 1; }),
        Click(ByExact(ICON_FA_ARROW_DOWN_SHORT_WIDE, kHierarchy)), Wait(2),
        Click(MenuItem("Descending")), Wait(3),
        Expect("rows sorted Z..A", [](Ctx& c) {
            const auto& v = HX::Visible(c.E);
            if (v.size() != 3) return std::string("visible ") + std::to_string(v.size());
            const std::string got = NameOf(c.W, v[0]) + "," + NameOf(c.W, v[1]) + "," + NameOf(c.W, v[2]);
            return got == "Gamma,Beta,Alpha" ? std::string() : "rows " + got;
        }),
        ExpectTrue("display sort only: sibling order untouched", [](Ctx& c) { return HierRootOrder(c.W) == "Alpha,Beta,Gamma"; }),
        HierFresh(),
    });
    Add("hierarchy: chevron, Expand all / Collapse all buttons and the E hover key", {
        FreshScene(), HierFresh(), HierParent("Gamma", "Alpha"), Wait(2), HierExpandAll(), Wait(3),
        Until("expanded: 3 rows", [](Ctx& c) { return HierVisible(c) == 3; }),
        Click(HierChevron("Alpha")), Wait(3),
        ExpectTrue("chevron collapsed Alpha", [](Ctx& c) { return HierVisible(c) == 2; }),
        Click(HierChevron("Alpha")), Wait(3),
        ExpectTrue("chevron expanded it", [](Ctx& c) { return HierVisible(c) == 3; }),
        Click(ByExact(ICON_FA_ANGLES_UP, kHierarchy)), Wait(3),
        ExpectTrue("Collapse all", [](Ctx& c) { return HierVisible(c) == 2; }),
        Click(ByExact(ICON_FA_ANGLES_DOWN, kHierarchy)), Wait(3),
        ExpectTrue("Expand all", [](Ctx& c) { return HierVisible(c) == 3; }),
        MoveToItem(ByTag("row:Alpha")), Wait(2),
        Key(ImGuiKey_E), Wait(3),
        ExpectTrue("E over Alpha collapses it", [](Ctx& c) { return HierVisible(c) == 2; }),
        MoveToItem(ByTag("row:Alpha")), Wait(2),
        Key(ImGuiKey_E), Wait(3),
        ExpectTrue("E again expands it", [](Ctx& c) { return HierVisible(c) == 3; }),
    });
}
