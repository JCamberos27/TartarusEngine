// Editor Enhancers (docs/EDITOR_ENHANCERS.md) - tests for the pure-logic layer under the
// vHierarchy / vFolders / vInspector / vTabs / vFavorites / vRuler-style features, plus the
// shortcut-table invariants those features rely on.

#include "UnitTestSupport.h"

#include "Enhancers/EnhancerCore.h"
#include "Enhancers/EnhancerUserState.h"
#include "Enhancers/Palette.h"
#include "Enhancers/FolderStyles.h"
#include "Enhancers/ComponentTransfer.h"
#include "Enhancers/TabState.h"
#include "Shortcuts.h"
#include "World.h"
#include "AssetLibrary.h"
#include "SceneSerializer.h"
#include "ComponentRegistry.h"
#include "ReflectAttributes.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <string>

using namespace Enhancers;

namespace {

void TestEnhancerRefsRoundTrip() {
    const EditorRef e = EditorRef::MakeEntity("00112233aabbccdd", 42, "Player");
    const EditorRef a = EditorRef::MakeAsset("assets/models/crate.glb", "crate");
    const EditorRef f = EditorRef::MakeFolder("Art/Materials");
    const EditorRef s = EditorRef::MakeScene("ffeeddccbbaa9988", "C:/p/scenes/Arena.json", "Arena");
    for (const EditorRef& r : {e, a, f, s}) {
        EditorRef back;
        CHECK(RefFromJson(RefToJson(r), back));
        CHECK(back == r);
        CHECK(back.Label == r.Label);
    }
    // Identity ignores the cached label; entities need scene AND order to match.
    CHECK(EditorRef::MakeEntity("00112233aabbccdd", 42, "Renamed") == e);
    CHECK(EditorRef::MakeEntity("00112233aabbccdd", 43) != e);
    CHECK(EditorRef::MakeEntity("ffffffffffffffff", 42) != e);
    CHECK(EditorRef::MakeFolder("assets/models/crate.glb") != a); // kind participates
    // A scene matches by GUID when both know it, else by path.
    CHECK(EditorRef::MakeScene("ffeeddccbbaa9988", "D:/moved/Arena.json") == s);
    CHECK(EditorRef::MakeScene("", "C:/p/scenes/Arena.json") == EditorRef::MakeScene("", "C:/p/scenes/Arena.json"));

    // Malformed rows are rejected, and one bad row doesn't sink the list.
    EditorRef junk;
    CHECK(!RefFromJson(nlohmann::json{{"kind", "planet"}}, junk));
    CHECK(!RefFromJson(nlohmann::json{{"kind", "entity"}, {"scene", "x"}}, junk)); // no order
    CHECK(!RefFromJson(nlohmann::json{{"kind", "asset"}}, junk));                  // nothing to resolve
    CHECK(!RefFromJson(nlohmann::json("str"), junk));
    nlohmann::json list = RefsToJson({e, a});
    list.push_back({{"kind", "nope"}});
    list.push_back(RefToJson(e)); // duplicate collapses
    const auto back = RefsFromJson(list);
    CHECK(back.size() == 2);
    CHECK(RefsFromJson(nlohmann::json::object()).empty());
}

void TestEnhancerBookmarkList() {
    std::vector<EditorRef> l;
    CHECK(AddUnique(l, EditorRef::MakeFolder("A")));
    CHECK(AddUnique(l, EditorRef::MakeFolder("B")));
    CHECK(!AddUnique(l, EditorRef::MakeFolder("A", "relabelled")));
    CHECK(l.size() == 2 && l[0].Label == "relabelled");
    CHECK(FindRef(l, EditorRef::MakeFolder("B")) == 1);
    MoveRef(l, 1, 0);
    CHECK(l[0].Path == "B" && l[1].Path == "A");
    MoveRef(l, 0, 99); // clamps
    CHECK(l[1].Path == "B");
    MoveRef(l, 7, 0);  // bad source: no-op
    CHECK(l.size() == 2);
    CHECK(RemoveRef(l, EditorRef::MakeFolder("A")));
    CHECK(!RemoveRef(l, EditorRef::MakeFolder("A")));
    // Cap drops the oldest.
    std::vector<EditorRef> capped;
    for (int i = 0; i < 5; ++i) AddUnique(capped, EditorRef::MakeEntity("s", i), 3);
    CHECK(capped.size() == 3 && capped.front().Order == 2 && capped.back().Order == 4);
}

void TestEnhancerNavHistory() {
    NavHistory<int> h(4);
    CHECK(!h.CanBack() && !h.CanForward() && h.Current() == nullptr);
    h.Push(1); h.Push(2); h.Push(3);
    h.Push(3); // same as current: no-op
    CHECK(h.Size() == 3 && *h.Current() == 3);
    CHECK(*h.Back() == 2 && *h.Back() == 1 && h.Back() == nullptr);
    CHECK(*h.Forward() == 2);
    h.Push(9); // pushing after Back truncates the forward branch (3)
    CHECK(h.Size() == 3 && !h.CanForward() && *h.Current() == 9);
    CHECK(*h.Back() == 2);
    h.Push(10); h.Push(11); h.Push(12); // cap 4: oldest falls off
    CHECK(h.Size() == 4 && h.Entries().front() == 2 && *h.Current() == 12);
    h.Clear();
    CHECK(h.Size() == 0 && h.Cursor() == -1);
}

void TestEnhancerSelectionHistory() {
    using E = SelectionHistoryEntry;
    const E a1{"sceneA", {1}, {}};
    const E a12{"sceneA", {1, 2}, {}};
    const E b5{"sceneB", {5}, {}};
    const E tex{{}, {}, "assets/tex/brick.png"};
    const E emptyA{"sceneA", {}, {}};
    std::vector<E> h{emptyA, a1, b5, tex, a12};

    // Entity/empty entries only resolve in their own scene; asset entries everywhere.
    CHECK(SelectionEntryReachable(a1, "sceneA") && !SelectionEntryReachable(a1, "sceneB"));
    CHECK(SelectionEntryReachable(tex, "sceneB") && SelectionEntryReachable(tex, ""));
    CHECK(!SelectionEntryReachable(emptyA, "sceneB"));

    // Back from the end in sceneA skips nothing reachable: tex, then (skipping b5) a1, then emptyA.
    CHECK(StepSelectionHistory(h, 4, -1, "sceneA") == 3);
    CHECK(StepSelectionHistory(h, 3, -1, "sceneA") == 1);
    CHECK(StepSelectionHistory(h, 1, -1, "sceneA") == 0);
    CHECK(StepSelectionHistory(h, 0, -1, "sceneA") == -1);
    CHECK(StepSelectionHistory(h, 1, +1, "sceneA") == 3);
    CHECK(StepSelectionHistory(h, 3, +1, "sceneA") == 4);
    CHECK(StepSelectionHistory(h, 4, +1, "sceneA") == -1);
    // In sceneB only b5 and the asset are reachable.
    CHECK(StepSelectionHistory(h, 4, -1, "sceneB") == 3);
    CHECK(StepSelectionHistory(h, 3, -1, "sceneB") == 2);
    CHECK(StepSelectionHistory(h, 2, -1, "sceneB") == -1);
    // A step never lands on an entry equal to the one it starts from.
    const std::vector<E> dup{a1, a1, tex};
    CHECK(StepSelectionHistory(dup, 1, -1, "sceneA") == -1);

    // Entries survive a scene reload: the orders recorded before resolve to the same objects in
    // the reloaded registry, even though entt hands out different handles.
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    World w;
    AssetLibrary assets;
    const glm::vec3 zero(0.0f), one(1.0f);
    w.CreateEmptyEntity(zero, zero, one, "Filler");
    const entt::entity crate = w.CreateEmptyEntity(zero, zero, one, "Crate");
    const int crateOrder = w.Registry.get<OrderComponent>(crate).Value;
    const E rec{"sceneA", {crateOrder}, {}};
    World reloaded;
    CHECK(SceneSerializer::LoadFromString(reloaded, assets, SceneSerializer::SaveToString(w, assets)));
    auto resolves = [](const std::vector<int>& orders, void* ctx) {
        const World& world = *static_cast<const World*>(ctx);
        for (auto [e, o] : world.Registry.view<const OrderComponent>().each())
            if (std::find(orders.begin(), orders.end(), o.Value) != orders.end()) return true;
        return false;
    };
    CHECK(SelectionEntryReachable(rec, "sceneA", resolves, &reloaded));
    bool foundCrate = false;
    for (auto [e, o, n] : reloaded.Registry.view<const OrderComponent, const NameComponent>().each())
        if (o.Value == crateOrder) foundCrate = n.Name == "Crate";
    CHECK(foundCrate);
    // An order that no longer exists makes the entry unreachable (deleted since).
    const E gone{"sceneA", {crateOrder + 1000}, {}};
    CHECK(!SelectionEntryReachable(gone, "sceneA", resolves, &reloaded));
    const std::vector<E> h2{rec, gone};
    CHECK(StepSelectionHistory(h2, 1, -1, "sceneA", resolves, &reloaded) == 0);
}

// The ComponentRegistry name of component T on `e`, found by address so the test doesn't
// hard-code display names.
template <class T>
const char* RegistryNameOf(World& w, entt::entity e) {
    void* want = &w.Registry.get<T>(e);
    for (const auto& rc : ComponentRegistry::All())
        if (rc.Has(w.Registry, e) && rc.Get(w.Registry, e) == want) return rc.Meta.Name;
    return nullptr;
}

void TestEnhancerComponentClipboard() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    World w;
    AssetLibrary assets;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity src = w.CreateEmptyEntity(zero, zero, one, "Source");
    const entt::entity dst = w.CreateEmptyEntity(zero, zero, one, "Target");
    auto& sa = w.Registry.emplace<AudioSourceComponent>(src);
    sa.Volume = 0.3f;
    w.Registry.emplace<LightComponent>(src).Intensity = 9.0f;
    w.Registry.emplace<LightComponent>(dst).Intensity = 1.0f;
    const char* audioName = RegistryNameOf<AudioSourceComponent>(w, src);
    const char* lightName = RegistryNameOf<LightComponent>(w, src);
    CHECK(audioName && lightName);

    const std::vector<std::string> clip = CopyAllComponents(w, src);
    auto inClip = [&](const char* n) {
        for (const auto& j : clip) if (SceneSerializer::PresetComponentName(j) == n) return true;
        return false;
    };
    CHECK(inClip(audioName) && inClip(lightName));

    // As New: only what the target lacks (the Audio Source); its Light keeps its own values.
    CHECK(CountPastable(w, dst, clip, PasteMode::AsNew) >= 1);
    const PasteReport asNew = PasteComponents(w, assets, dst, clip, PasteMode::AsNew);
    CHECK(asNew.Applied >= 1);
    CHECK(w.Registry.all_of<AudioSourceComponent>(dst) && w.Registry.get<AudioSourceComponent>(dst).Volume == 0.3f);
    CHECK(w.Registry.get<LightComponent>(dst).Intensity == 1.0f);
    CHECK(std::find(asNew.Skipped.begin(), asNew.Skipped.end(), std::string(lightName)) != asNew.Skipped.end());

    // Values: only what the target has - now overwrites the Light, adds nothing new.
    const entt::entity bare = w.CreateEmptyEntity(zero, zero, one, "Bare");
    w.Registry.emplace<LightComponent>(bare).Intensity = 2.0f;
    const PasteReport vals = PasteComponents(w, assets, bare, clip, PasteMode::Values);
    CHECK(vals.Applied >= 1);
    CHECK(w.Registry.get<LightComponent>(bare).Intensity == 9.0f);
    CHECK(!w.Registry.all_of<AudioSourceComponent>(bare));
    CHECK(!CanPastePreset(w, bare, "not json", PasteMode::AsNew));
}

void TestEnhancerKeepPlayChanges() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    World w;
    AssetLibrary assets;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity e = w.CreateEmptyEntity(zero, zero, one, "Speaker");
    w.Registry.emplace<AudioSourceComponent>(e).Volume = 0.3f;
    const int order = w.Registry.get<OrderComponent>(e).Value;
    const char* audioName = RegistryNameOf<AudioSourceComponent>(w, e);
    CHECK(audioName != nullptr);
    if (!audioName) return;
    const std::string snapshot = SceneSerializer::SaveToString(w, assets); // "enter Play"

    // Changes made during Play.
    w.Registry.get<AudioSourceComponent>(e).Volume = 0.8f;
    w.Registry.get<TransformComponent>(e).Position = glm::vec3(1.0f, 2.0f, 3.0f);
    const std::vector<PlayKeep> keeps{{order, audioName}, {order, kKeepTransform},
                                      {order + 999, kKeepTransform}, {order, "No Such Component"}};
    const std::vector<KeptValue> kept = CaptureKept(w, keeps);
    CHECK(kept.size() == 2); // the missing object and the unknown component are dropped

    // "Stop": the snapshot comes back, then the kept values go on top by order.
    CHECK(SceneSerializer::LoadFromString(w, assets, snapshot));
    entt::entity back = entt::null;
    for (auto [x, o] : w.Registry.view<const OrderComponent>().each()) if (o.Value == order) back = x;
    CHECK(back != entt::null);
    if (back == entt::null) return;
    CHECK(w.Registry.get<AudioSourceComponent>(back).Volume == 0.3f);
    CHECK(ApplyKept(w, assets, kept) == 2);
    CHECK(w.Registry.get<AudioSourceComponent>(back).Volume == 0.8f);
    CHECK(w.Registry.get<TransformComponent>(back).Position == glm::vec3(1.0f, 2.0f, 3.0f));
}

// --- vInspector attributes (Phase 3b) ------------------------------------------------------
struct AttrTestComp {
    bool On = true;
    int Mode = 0;
    float Speed = 2.5f;
    glm::vec3 Offset{1.0f, 2.0f, 3.0f};
    std::string Label = "default";
};

bool AttrSpeedPositive(const void* c) { return static_cast<const AttrTestComp*>(c)->Speed > 0.0f; }

void TestEnhancerFieldState() {
    using T = ReflectFieldType;
    ReflectComponent meta;
    meta.Name = "Attr Test";
    meta.Fields = {
        Field("On", T::Bool, TARTARUS_REFLECT_FIELD(AttrTestComp, On)),
        Field("Mode", T::Enum, TARTARUS_REFLECT_FIELD(AttrTestComp, Mode)).Enum("A\0B\0C\0", 3),
        Field("Speed", T::Float, TARTARUS_REFLECT_FIELD(AttrTestComp, Speed)).EnableIf("On", 1).Tab("Motion"),
        Field("Offset", T::Vec3, TARTARUS_REFLECT_FIELD(AttrTestComp, Offset)).VisibleIf("Mode", 2, true),
        Field("Label", T::String, TARTARUS_REFLECT_FIELD(AttrTestComp, Label)).ReadOnly().NonSerialized(),
        Field("Gated", T::Float, TARTARUS_REFLECT_FIELD(AttrTestComp, Speed)).ShowIf(&AttrSpeedPositive),
        Field("Greyed", T::Float, TARTARUS_REFLECT_FIELD(AttrTestComp, Speed)).EnableWhen(&AttrSpeedPositive),
        Field("Ghost", T::Int, TARTARUS_REFLECT_FIELD(AttrTestComp, Mode)).EnableIf("No Such Field", 1),
    };
    // The builder set what it said it would.
    CHECK(meta.Fields[2].Tab && std::strcmp(meta.Fields[2].Tab, "Motion") == 0);
    CHECK(meta.Fields[4].ReadOnly && meta.Fields[4].NonSerialized);
    CHECK(meta.Fields[1].EnumCount == 3);
    Attr(meta, "Mode").Tab("Motion");
    CHECK(meta.Fields[1].Tab != nullptr);
    Attr(meta, "Typo").ReadOnly(); // unknown name: harmless
    static const float kSpeeds[] = {1.0f, 5.0f, 10.0f};
    Attr(meta, "Speed").Variants(kSpeeds, 3);
    CHECK(meta.Fields[2].VariantCount == 3 && meta.Fields[2].Variants == kSpeeds);

    AttrTestComp c;
    auto st = [&](int i) { return EvaluateFieldState(meta, meta.Fields[(size_t)i], &c); };
    // EnableIf over a Bool sibling.
    CHECK(st(2).Visible && st(2).Enabled);
    c.On = false;
    CHECK(st(2).Visible && !st(2).Enabled);
    // VisibleIf with negation over an Enum sibling.
    CHECK(st(3).Visible);
    c.Mode = 2;
    CHECK(!st(3).Visible);
    // ReadOnly greys out, never hides.
    CHECK(st(4).Visible && !st(4).Enabled);
    // Conditions: hide vs grey out.
    CHECK(st(5).Visible && st(6).Enabled);
    c.Speed = -1.0f;
    CHECK(!st(5).Visible && st(5).Enabled);
    CHECK(st(6).Visible && !st(6).Enabled);
    // A sibling that doesn't exist changes nothing.
    CHECK(st(7).Visible && st(7).Enabled);
}

void TestEnhancerFieldReset() {
    using T = ReflectFieldType;
    RegisteredComponent rc;
    rc.Meta.Name = "Attr Test";
    rc.Meta.Fields = {
        Field("On", T::Bool, TARTARUS_REFLECT_FIELD(AttrTestComp, On)),
        Field("Mode", T::Int, TARTARUS_REFLECT_FIELD(AttrTestComp, Mode)),
        Field("Speed", T::Float, TARTARUS_REFLECT_FIELD(AttrTestComp, Speed)),
        Field("Offset", T::Vec3, TARTARUS_REFLECT_FIELD(AttrTestComp, Offset)),
        Field("Label", T::String, TARTARUS_REFLECT_FIELD(AttrTestComp, Label)),
    };
    AttrTestComp c;
    c.On = false; c.Mode = 7; c.Speed = 9.0f; c.Offset = glm::vec3(0.0f); c.Label = "changed";
    // No default instance: nothing to reset to.
    CHECK(!ResetReflectFieldToDefault(rc, rc.Meta.Fields[0], &c) && !c.On);
    rc.DefaultInstance = []() -> const void* { static const AttrTestComp k{}; return &k; };
    const AttrTestComp def{};
    for (const ReflectField& f : rc.Meta.Fields) {
        CHECK(!ReflectFieldIsDefault(rc, f, &c));
        CHECK(ResetReflectFieldToDefault(rc, f, &c));
        CHECK(ReflectFieldIsDefault(rc, f, &c));
    }
    CHECK(c.On == def.On && c.Mode == def.Mode && c.Speed == def.Speed && c.Offset == def.Offset && c.Label == def.Label);

    // Every registered engine component gets a default instance from Register<T>.
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    bool allHaveDefaults = true;
    for (const auto& r : ComponentRegistry::All()) allHaveDefaults = allHaveDefaults && r.DefaultInstance != nullptr;
    CHECK(allHaveDefaults);
}

void TestEnhancerNonSerializedField() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    World w;
    AssetLibrary assets;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity e = w.CreateEmptyEntity(zero, zero, one, "Speaker");
    w.Registry.emplace<AudioSourceComponent>(e).Volume = 0.25f;
    const char* audioName = RegistryNameOf<AudioSourceComponent>(w, e);
    CHECK(audioName != nullptr);
    if (!audioName) return;
    // Flip one real field to NonSerialized for the duration of the test (restored below).
    ReflectField* vol = nullptr;
    for (const auto& rc : ComponentRegistry::All())
        if (std::strcmp(rc.Meta.Name, audioName) == 0)
            for (const ReflectField& f : rc.Meta.Fields)
                if (std::strcmp(f.Name, "Volume") == 0) vol = const_cast<ReflectField*>(&f);
    CHECK(vol != nullptr);
    if (!vol) return;
    vol->NonSerialized = true;
    const std::string preset = SceneSerializer::ComponentToPresetJson(w, e, audioName);
    World b;
    const bool loaded = SceneSerializer::LoadFromString(b, assets, SceneSerializer::SaveToString(w, assets));
    vol->NonSerialized = false;
    CHECK(loaded);
    CHECK(preset.find("\"Volume\"") == std::string::npos);
    float loadedVolume = -1.0f;
    for (auto [x, a] : b.Registry.view<const AudioSourceComponent>().each()) loadedVolume = a.Volume;
    CHECK(loadedVolume == AudioSourceComponent{}.Volume); // not written, so the default came back
    // The real registration: Audio Source's distance settings follow 3D Sound.
    const RegisteredComponent* audio = nullptr;
    for (const auto& rc : ComponentRegistry::All()) if (std::strcmp(rc.Meta.Name, audioName) == 0) audio = &rc;
    const ReflectField* minDist = nullptr;
    for (const ReflectField& f : audio->Meta.Fields) if (std::strcmp(f.Name, "Min Distance") == 0) minDist = &f;
    CHECK(minDist != nullptr);
    if (!minDist) return;
    AudioSourceComponent src;
    src.Spatial = false;
    CHECK(!EvaluateFieldState(audio->Meta, *minDist, &src).Enabled);
    src.Spatial = true;
    CHECK(EvaluateFieldState(audio->Meta, *minDist, &src).Enabled);
}

// --- vTabs (Phase 4) ------------------------------------------------------------------------
void TestEnhancerTabStrip() {
    const EditorRef a = EditorRef::MakeFolder("Art"), b = EditorRef::MakeFolder("Audio"), c = EditorRef::MakeFolder("Code");
    TabStrip s;
    CHECK(s.Open(a) == 0 && s.Active == 0);
    CHECK(s.Open(b) == 1 && s.Active == 1);
    CHECK(s.Open(a) == 0 && s.Tabs.size() == 2 && s.Active == 0); // already open: reused, activated
    CHECK(s.Open(c, false) == 1 && s.Active == 0);                  // inserted after the active tab
    CHECK(s.Tabs[1] == c && s.Tabs[2] == b);
    CHECK(s.Open(a, true, /*allowDuplicate=*/true) == 1 && s.Tabs.size() == 4 && s.Active == 1);
    // Close: the active tab moves to the right neighbour, else the left one.
    CHECK(s.Close(1) && s.Active == 1 && s.Tabs[1] == c);
    CHECK(s.Close(2) && s.Active == 1);                              // closing after the active keeps it
    CHECK(s.Close(1) && s.Active == 0 && s.Tabs.size() == 1);
    CHECK(!s.Close(5));
    // Reopen: most recent first, skipping targets that are open again.
    CHECK(s.Reopen() == 1 && s.Tabs[1] == c && s.Active == 1);
    CHECK(s.Reopen() == 2 && s.Tabs[2] == b);
    CHECK(s.Reopen() == -1); // the remaining closed entry (the duplicate "Art") is open already
    // Move keeps the same tab active.
    s.Active = 0; // Art
    s.Move(0, 2);
    CHECK(s.Tabs[2] == a && s.Active == 2);
    s.Move(1, 0); // Code <-> Audio: Art stays active at 2
    CHECK(s.Tabs[0] == b && s.Active == 2);
    s.Move(2, 99);
    CHECK(s.Active == 2);
    // Step wraps; with allowNone, -1 (the Inspector's Selection tab) is part of the cycle.
    CHECK(s.Step(+1, false) == 0 && s.Step(-1, false) == 1);
    CHECK(s.Step(+1, true) == -1);
    s.Active = -1;
    CHECK(s.Step(+1, true) == 0 && s.Step(-1, true) == 2);
    // CloseOthers.
    s.CloseOthers(1);
    CHECK(s.Tabs.size() == 1 && s.Active == 0);
    s.CloseOthers(-1);
    CHECK(s.Tabs.empty() && s.Active == -1);
    // The cap: never more than kMaxTabs, and the shown tab survives.
    TabStrip big;
    for (int i = 0; i < (int)TabStrip::kMaxTabs + 5; ++i) big.Open(EditorRef::MakeFolder("F" + std::to_string(i)));
    CHECK(big.Tabs.size() == TabStrip::kMaxTabs);
    CHECK(big.Tabs[(size_t)big.Active].Path == "F" + std::to_string(TabStrip::kMaxTabs + 4));
    CHECK(big.Closed.size() <= TabStrip::kMaxClosed);
}

void TestEnhancerTabStateRoundTrip() {
    // Component sub-targets are part of an entity ref's identity and survive JSON.
    const EditorRef ent = EditorRef::MakeEntity("00112233aabbccdd", 4, "Player");
    const EditorRef comp = EditorRef::MakeComponent("00112233aabbccdd", 4, "Light", "Player");
    CHECK(ent != comp);
    EditorRef back;
    CHECK(RefFromJson(RefToJson(comp), back) && back == comp && back.Sub == "Light");
    CHECK(RefFromJson(RefToJson(ent), back) && back == ent && back.Sub.empty());

    TabState& ts = TabState::Get();
    ts.Reset();
    ts.Inspector.Open(ent);
    ts.Inspector.Open(comp);
    ts.Inspector.Open(EditorRef::MakeAsset("assets/tex/brick.png", "brick.png"));
    ts.Inspector.Active = 1;
    ts.Inspector.Close(2);
    ts.Assets.Open(EditorRef::MakeFolder(""));
    ts.Assets.Open(EditorRef::MakeFolder("Art/Props"));
    ts.Starred.push_back(EditorRef::MakeFolder("Art"));
    const nlohmann::json j = ts.ToJson();
    ts.Reset();
    CHECK(ts.Inspector.Tabs.empty());
    ts.FromJson(j);
    CHECK(ts.Inspector.Tabs.size() == 2 && ts.Inspector.Tabs[1] == comp && ts.Inspector.Active == 1);
    CHECK(ts.Inspector.Closed.size() == 1 && ts.Inspector.Closed[0].Path == "assets/tex/brick.png");
    CHECK(ts.Assets.Tabs.size() == 2 && ts.Assets.Tabs[0].Path.empty() && ts.Assets.Active == 1);
    CHECK(ts.Starred.size() == 1 && ts.Starred[0].Path == "Art");
    // A bad active index from a hand-edited file is repaired.
    nlohmann::json bad = j;
    bad["inspector"]["active"] = 42;
    ts.FromJson(bad);
    CHECK(ts.Inspector.Active == 0);
    ts.Reset();
}

// --- vFavorites (Phase 5) -------------------------------------------------------------------
void TestEnhancerFavoritePages() {
    std::vector<FavoritePage> pages;
    EnsureFavoritePage(pages);
    CHECK(pages.size() == 1 && pages[0].Name == "Favorites" && pages[0].Items.empty());
    EnsureFavoritePage(pages);
    CHECK(pages.size() == 1);

    const EditorRef tex = EditorRef::MakeAsset("assets/tex/brick.png", "brick.png");
    const EditorRef art = EditorRef::MakeFolder("Art", "Art");
    const EditorRef hero = EditorRef::MakeEntity("00112233aabbccdd", 9, "Hero");
    CHECK(AddFavorite(pages, 0, tex));
    CHECK(!AddFavorite(pages, 0, tex)); // already on the page
    pages.push_back(FavoritePage{"Level", {}});
    CHECK(AddFavorite(pages, 1, art) && AddFavorite(pages, 7, hero)); // a page index past the end clamps
    CHECK(pages[1].Items.size() == 2 && pages[1].Items[1] == hero);
    CHECK(FindFavorite(pages, tex) == 0 && FindFavorite(pages, hero) == 1);
    CHECK(FindFavorite(pages, EditorRef::MakeFolder("Nope")) == -1);
    // The same target may sit on two pages; removing takes it off all of them.
    CHECK(AddFavorite(pages, 0, hero));
    CHECK(RemoveFavorite(pages, hero) && FindFavorite(pages, hero) == -1);
    CHECK(!RemoveFavorite(pages, hero));
    MoveFavoritePage(pages, 1, 0);
    CHECK(pages[0].Name == "Level" && pages[1].Name == "Favorites");
    MoveFavoritePage(pages, 0, 99);
    CHECK(pages[1].Name == "Level");

    // Round trip through the per-user file, with the one-time migration flag.
    auto& us = EnhancerUserState::Get();
    const nlohmann::json saved = us.ToJson();
    us.Reset();
    const unsigned rev = us.Revision();
    us.FavoritePages = pages;
    us.FavoritesMigrated = true;
    us.MarkDirty();
    CHECK(us.Revision() != rev); // star caches key off this
    const nlohmann::json j = us.ToJson();
    us.Reset();
    CHECK(us.FavoritePages.empty() && !us.FavoritesMigrated);
    us.FromJson(j);
    CHECK(us.FavoritesMigrated && us.FavoritePages.size() == 2);
    CHECK(us.FavoritePages[1].Name == "Level" && us.FavoritePages[1].Items.size() == 1 && us.FavoritePages[1].Items[0] == art);
    CHECK(us.FavoritePages[0].Items.size() == 1 && us.FavoritePages[0].Items[0].Path == "assets/tex/brick.png");
    // A file from before vFavorites has no pages and isn't migrated yet.
    nlohmann::json old = j;
    old.erase("favoritePages");
    old.erase("favoritesMigrated");
    us.FromJson(old);
    CHECK(us.FavoritePages.empty() && !us.FavoritesMigrated);
    us.FromJson(saved); // restore the in-memory state (FromJson leaves it clean: nothing is written)
}

void TestEnhancerGlobMatch() {
    CHECK(GlobMatch("*", ""));
    CHECK(GlobMatch("*", "anything/at/all"));
    CHECK(GlobMatch("*Materials*", "Art/Materials/Metal"));
    CHECK(GlobMatch("*materials", "Art/MATERIALS")); // case-insensitive
    CHECK(GlobMatch("Art/*/Textures", "Art/Props/Textures"));
    CHECK(!GlobMatch("Art/*/Textures", "Art/Props/Textures/Old"));
    CHECK(GlobMatch("Tex?ures", "Textures"));
    CHECK(!GlobMatch("Tex?ures", "Texures"));
    CHECK(GlobMatch("a*b*c", "aXXbYYc"));
    CHECK(!GlobMatch("a*b*c", "aXXbYY"));
    CHECK(GlobMatch("**x", "x"));
    CHECK(!GlobMatch("", "x"));
    CHECK(GlobMatch("", ""));
    // Pathological backtracking stays correct (and fast - iterative).
    CHECK(!GlobMatch("*a*a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
}

void TestEnhancerFuzzyScore() {
    CHECK(FuzzyScore("", "whatever") == 0);
    CHECK(FuzzyScore("xyz", "Player") < 0);
    CHECK(FuzzyScore("playerx", "Player") < 0);
    CHECK(FuzzyScore("plr", "Player") > 0);
    // Word starts beat buried matches; camelCase humps count as word starts.
    CHECK(FuzzyScore("pc", "PlayerController") > FuzzyScore("pc", "SpaceCrate"));
    CHECK(FuzzyScore("mat", "Materials") > FuzzyScore("mat", "Format"));
    // Consecutive runs beat scattered ones.
    CHECK(FuzzyScore("cam", "MainCamera") > FuzzyScore("cam", "CharacterAnimator"));
    // Separators are word starts.
    CHECK(FuzzyScore("dt", "door_trigger") > FuzzyScore("dt", "edited"));
    CHECK(FuzzyScore("CUBE", "cube") > 0); // case-insensitive
}

void TestEnhancerRemapFolderKeys() {
    std::map<std::string, int> m{{"Art", 1}, {"Art/Materials", 2}, {"Art/Materials/Metal", 3}, {"Artwork", 4}, {"Audio", 5}};
    CHECK(RemapFolderKeys(m, "Art", "Graphics") == 3);
    CHECK(m.count("Graphics") && m.count("Graphics/Materials") && m.count("Graphics/Materials/Metal"));
    CHECK(m.count("Artwork") && m["Artwork"] == 4); // a shared prefix is not a parent folder
    CHECK(!m.count("Art") && m["Graphics/Materials/Metal"] == 3);
    CHECK(RemapFolderKeys(m, "Graphics/Materials", "") == 2); // delete drops the subtree
    CHECK(m.size() == 3 && m.count("Graphics"));
    CHECK(RemapFolderKeys(m, "Nope", "X") == 0);
    CHECK(RemapFolderKeys(m, "Audio", "Audio") == 0);

    std::string p = "Art/Materials";
    CHECK(RemapFolderPath(p, "Art", "Graphics") == 1 && p == "Graphics/Materials");
    std::string q = "Artwork/X";
    CHECK(RemapFolderPath(q, "Art", "Graphics") == 0 && q == "Artwork/X");
    CHECK(RemapFolderPath(q, "Artwork", "") == 1 && q.empty());
}

void TestEnhancerFormatLength() {
    char b[32];
    CHECK(std::string(FormatLength(0.0042f, false, b, sizeof(b))) == "4.2 mm");
    CHECK(std::string(FormatLength(0.375f, false, b, sizeof(b))) == "37.5 cm");
    CHECK(std::string(FormatLength(1.84f, false, b, sizeof(b))) == "1.840 m");
    CHECK(std::string(FormatLength(2310.0f, false, b, sizeof(b))) == "2.31 km");
    CHECK(std::string(FormatLength(-1.5f, false, b, sizeof(b))) == "-1.500 m");
    CHECK(std::string(FormatLength(1.8f, true, b, sizeof(b))) == "5' 10.9\"");
    CHECK(std::string(FormatLength(0.3048f, true, b, sizeof(b))) == "1' 0.0\"");
    CHECK(std::string(FormatLength(0.3040f, true, b, sizeof(b))) == "1' 0.0\""); // 11.97" rounds up into the next foot
    CHECK(std::string(FormatLength(0.0254f, true, b, sizeof(b))) == "0' 1.0\"");
    CHECK(std::string(FormatLength(2000.0f, true, b, sizeof(b))) == "1.24 mi");
    CHECK(std::string(FormatLength(std::numeric_limits<float>::infinity(), false, b, sizeof(b))) == "--");
    char tiny[4];
    FormatLength(1234.5f, false, tiny, sizeof(tiny)); // truncates, never overruns
    CHECK(std::strlen(tiny) == 3);
}

void TestEnhancerIconTable() {
    std::size_t n = 0;
    const FAIcon* t = FAIconTable(&n);
    CHECK(n > 1000);
    bool sorted = true;
    for (std::size_t i = 1; i < n; ++i) sorted = sorted && std::strcmp(t[i - 1].Name, t[i].Name) < 0;
    CHECK(sorted); // FAIconGlyph binary-searches
    CHECK(FAIconGlyph("cube") && std::string(FAIconGlyph("cube")) == "\xef\x86\xb2");
    CHECK(FAIconGlyph("folder-open") != nullptr);
    CHECK(FAIconGlyph("definitely-not-an-icon") == nullptr);
    CHECK(FAIconGlyph("") == nullptr && FAIconGlyph(nullptr) == nullptr);
    for (const auto& name : Palette::Defaults().Icons) CHECK(FAIconGlyph(name.c_str()) != nullptr);
}

void TestEnhancerPalette() {
    std::uint32_t c = 0;
    CHECK(ParseHexColor("#E5484D", c) && c == PackRGBA(0xE5, 0x48, 0x4D, 255));
    CHECK(ParseHexColor("e5484d80", c) && c == PackRGBA(0xE5, 0x48, 0x4D, 0x80));
    CHECK(!ParseHexColor("#E5484", c) && !ParseHexColor("#GG0000", c) && !ParseHexColor("", c));
    CHECK(ToHexColor(PackRGBA(1, 2, 3)) == "#010203");
    CHECK(ToHexColor(PackRGBA(1, 2, 3, 4)) == "#01020304");

    // Export -> import is lossless.
    Palette p = Palette::Defaults();
    p.Colors.push_back(PackRGBA(10, 20, 30, 40));
    p.Icons = {"cube", "tree"};
    CHECK(Palette::FromJson(p.ToJson()) == p);
    // Junk entries are skipped; an empty list falls back to the defaults.
    nlohmann::json j = {{"colors", {"#123456", "nonsense", 7}}, {"icons", {"cube", "no-such-icon"}}};
    const Palette q = Palette::FromJson(j);
    CHECK(q.Colors.size() == 1 && q.Icons.size() == 1 && q.Icons[0] == "cube");
    CHECK(Palette::FromJson(nlohmann::json::object()) == Palette::Defaults());
}

void TestEnhancerUserStateRoundTrip() {
    auto& st = EnhancerUserState::Get();
    const nlohmann::json saved = st.ToJson(); // tests share the singleton - restore it after
    st.Reset();
    st.SceneBookmarks.push_back(EditorRef::MakeScene("aa", "C:/s.json", "S"));
    st.EntityBookmarks.push_back(EditorRef::MakeEntity("aa", 3, "Lamp"));
    st.FolderBookmarks.push_back(EditorRef::MakeFolder("Art"));
    st.InspectorBookmarks.push_back(EditorRef::MakeAsset("m.mat"));
    st.DefaultParents["aa"] = 7;
    nlohmann::json j = st.ToJson();
    j["fromTheFuture"] = {{"keep", true}}; // a newer build's section
    st.FromJson(j);
    CHECK(st.SceneBookmarks.size() == 1 && st.EntityBookmarks.size() == 1 && st.EntityBookmarks[0].Order == 3);
    CHECK(st.FolderBookmarks.size() == 1 && st.InspectorBookmarks.size() == 1);
    CHECK(st.DefaultParents.size() == 1 && st.DefaultParents["aa"] == 7);
    CHECK(st.ToJson().contains("fromTheFuture")); // unknown keys survive a round trip
    st.FromJson(nlohmann::json("garbage"));
    CHECK(st.EntityBookmarks.empty() && st.DefaultParents.empty());
    st.FromJson(saved);

    // The project key normalizes separators, case and trailing slashes.
    CHECK(EnhancerUserState::ProjectKey("C:\\Proj\\Game\\") == EnhancerUserState::ProjectKey("c:/proj/game"));
    CHECK(EnhancerUserState::ProjectKey("C:/Proj/Game") != EnhancerUserState::ProjectKey("C:/Proj/Game2"));
    CHECK(EnhancerUserState::ProjectKey("x").size() == 16);
}

void TestEnhancerShortcutTable() {
    Shortcuts::Init();
    // Every builtin binding is conflict-free under the scope rules (including the hover
    // contexts, which overlap their own panel's focus context).
    for (const auto& s : Shortcuts::All())
        if (!s.Overridden) CHECK(Shortcuts::Conflicts(s.Id.c_str(), s.Default).empty());
    // Ctrl+Shift+T is free for vTabs' "reopen closed tab"; Statistics moved to Alt+Shift+T.
    const Shortcuts::Shortcut* stats = Shortcuts::Find("stats.toggle");
    CHECK(stats && stats->Default.Alt && stats->Default.Shift && !stats->Default.Ctrl && stats->Default.Key == ImGuiKey_T);
}

void TestEnhancerTreeLineMasks() {
    // root
    //  |- A        (has a later sibling)
    //  |   |- A1   (has a later sibling)
    //  |   '- A2   (last)
    //  '- B        (last)
    //      '- B1   (last)
    const std::uint32_t a  = TreeLineChildMask(0, 0, false);
    const std::uint32_t b  = TreeLineChildMask(0, 0, true);
    const std::uint32_t a1 = TreeLineChildMask(a, 1, false);
    const std::uint32_t a2 = TreeLineChildMask(a, 1, true);
    const std::uint32_t b1 = TreeLineChildMask(b, 1, true);
    CHECK(a == 0x1u && b == 0x0u);
    CHECK(a1 == 0x3u); // level 0 continues (B follows A), level 1 continues (A2 follows A1)
    CHECK(a2 == 0x1u); // level 0 still continues past A2, its own elbow ends
    CHECK(b1 == 0x0u); // nothing continues under the last branch
    // Deep trees saturate instead of shifting past the word.
    CHECK(TreeLineChildMask(0xFFFFFFFFu, 40, false) == 0x7FFFFFFFu);
}

void TestEnhancerHierStyleRoundTrip() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    World a;
    AssetLibrary assets;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity sep = a.CreateEmptyEntity(zero, zero, one, "Lighting");
    const entt::entity tinted = a.CreateEmptyEntity(zero, zero, one, "Tinted");
    const entt::entity plain = a.CreateEmptyEntity(zero, zero, one, "Plain");
    (void)plain;
    HierarchyStyleComponent s1;
    s1.Icon = "lightbulb"; s1.Color = PackRGBA(0xF2, 0xC9, 0x4C, 0x80);
    s1.FillMode = HierarchyStyleComponent::Flat; s1.Separator = true;
    a.Registry.emplace<HierarchyStyleComponent>(sep, s1);
    HierarchyStyleComponent s2;
    s2.Color = PackRGBA(0x4C, 0x8D, 0xF6);
    a.Registry.emplace<HierarchyStyleComponent>(tinted, s2);

    const std::string text = SceneSerializer::SaveToString(a, assets);
    std::size_t keys = 0;
    for (std::size_t at = text.find("hierStyle"); at != std::string::npos; at = text.find("hierStyle", at + 1)) ++keys;
    CHECK(keys == 2); // the unstyled row writes nothing

    World b;
    CHECK(SceneSerializer::LoadFromString(b, assets, text));
    auto byName = [&](const char* n) {
        for (auto [e, nm] : b.Registry.view<const NameComponent>().each()) if (nm.Name == n) return e;
        return (entt::entity)entt::null;
    };
    const auto* r1 = b.Registry.try_get<HierarchyStyleComponent>(byName("Lighting"));
    const auto* r2 = b.Registry.try_get<HierarchyStyleComponent>(byName("Tinted"));
    CHECK(r1 && r1->Icon == "lightbulb" && r1->Color == s1.Color && r1->FillMode == HierarchyStyleComponent::Flat && r1->Separator);
    CHECK(r2 && r2->Icon.empty() && r2->Color == s2.Color && r2->FillMode == HierarchyStyleComponent::Gradient && !r2->Separator);
    CHECK(!b.Registry.all_of<HierarchyStyleComponent>(byName("Plain")));
}

void TestEnhancerFolderKinds() {
    FolderSummary s;
    CHECK(DominantKind(s) == FolderKind::Count); // empty
    s.Add(FolderKind::Texture);
    CHECK(DominantKind(s) == FolderKind::Count); // one file is not "mostly textures"
    s.Add(FolderKind::Texture); s.Add(FolderKind::Texture);
    s.Add(FolderKind::Model);
    CHECK(DominantKind(s) == FolderKind::Texture); // 3 of 4 = 75%
    s.Add(FolderKind::Model); s.Add(FolderKind::Sound);
    CHECK(DominantKind(s) == FolderKind::Count); // 3 of 6 = 50% < 60%
    FolderSummary other;
    other.Add(FolderKind::Other); other.Add(FolderKind::Other); other.Add(FolderKind::Other);
    CHECK(DominantKind(other) == FolderKind::Count); // "Other" never names a folder

    FolderKind top[4];
    const int n = TopKinds(s, top, 4);
    CHECK(n == 3 && top[0] == FolderKind::Texture && top[1] == FolderKind::Model && top[2] == FolderKind::Sound);
    CHECK(TopKinds(s, top, 1) == 1 && top[0] == FolderKind::Texture);
    for (int k = 0; k < kFolderKindCount; ++k) {
        const char* name = FolderKindIconName((FolderKind)k);
        if (name) CHECK(FAIconGlyph(name) != nullptr);
    }
}

void TestEnhancerFolderStyleResolve() {
    auto& fs = FolderStyles::Get();
    const nlohmann::json saved = fs.ToJson(); // shared singleton - restore after
    fs.Reset();
    FolderSummary textures;
    textures.Add(FolderKind::Texture); textures.Add(FolderKind::Texture);

    // Default, then automatic icon from content.
    CHECK(fs.Resolve("Art", nullptr).Icon.empty());
    ResolvedFolderStyle r = fs.Resolve("Art/Tex", &textures);
    CHECK(r.Icon == "image" && r.IconSource == FolderStyleSource::Auto && r.Color == 0);
    fs.AutoIcons = false;
    CHECK(fs.Resolve("Art/Tex", &textures).Icon.empty());
    fs.AutoIcons = true;
    // Built-in virtual folders.
    CHECK(fs.Resolve("Scenes", nullptr).Icon == "map" && fs.Resolve("Scenes", nullptr).IconSource == FolderStyleSource::Builtin);

    // A rule beats the automatic icon; matched on the leaf name or the full path.
    fs.Rules.push_back({"Tex*", FolderStyle{"star", PackRGBA(1, 2, 3)}});
    fs.Rules.push_back({"Art/*", FolderStyle{"tree", 0}});
    r = fs.Resolve("Art/Tex", &textures);
    CHECK(r.Icon == "star" && r.IconSource == FolderStyleSource::Rule && r.Color == PackRGBA(1, 2, 3)); // first match wins
    CHECK(fs.Resolve("Art/Props", nullptr).Icon == "tree");
    CHECK(fs.Resolve("Other/Textures", nullptr).Icon == "star"); // leaf-name match anywhere
    CHECK(fs.Resolve("Artwork", nullptr).Icon.empty());

    // The folder's own style beats rules, field by field.
    fs.Folders["Art/Tex"] = FolderStyle{"", PackRGBA(9, 9, 9)};
    r = fs.Resolve("Art/Tex", &textures);
    CHECK(r.Icon == "star" && r.Color == PackRGBA(9, 9, 9)); // colour overridden, rule's icon kept
    fs.Folders["Art/Tex"].Icon = "cube";
    r = fs.Resolve("Art/Tex", &textures);
    CHECK(r.Icon == "cube" && r.IconSource == FolderStyleSource::Explicit);
    // An icon name that no longer exists falls back to the plain glyph.
    fs.Folders["Broken"] = FolderStyle{"not-an-icon", 0};
    CHECK(fs.Resolve("Broken", nullptr).Icon.empty());

    // Rename / delete re-keys the folder and everything under it - and only that.
    fs.Folders["Art/Tex/Old"] = FolderStyle{"music", 0};
    fs.Folders["Artwork"] = FolderStyle{"ghost", 0};
    CHECK(fs.OnFolderPathChanged("Art", "Graphics") == 2);
    CHECK(fs.Folders.count("Graphics/Tex") && fs.Folders.count("Graphics/Tex/Old") && fs.Folders.count("Artwork"));
    CHECK(fs.OnFolderPathChanged("Graphics/Tex", "") == 2);
    CHECK(!fs.Folders.count("Graphics/Tex") && !fs.Folders.count("Graphics/Tex/Old"));

    // JSON round trip, including rules and the auto-icon switch.
    fs.AutoIcons = false;
    const nlohmann::json j = fs.ToJson();
    fs.FromJson(j);
    CHECK(!fs.AutoIcons && fs.Rules.size() == 2 && fs.Rules[0].Pattern == "Tex*" && fs.Rules[0].Style.Color == PackRGBA(1, 2, 3));
    CHECK(fs.Folders.count("Artwork") && fs.Folders["Artwork"].Icon == "ghost");
    CHECK(fs.ToJson() == j);
    // Garbage in, empty store out.
    fs.FromJson(nlohmann::json::array());
    CHECK(fs.Folders.empty() && fs.Rules.empty() && fs.AutoIcons);

    fs.FromJson(saved);
}

} // namespace

void RegisterEnhancerTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("EnhancerRefsRoundTrip", TestEnhancerRefsRoundTrip);
    tests.emplace_back("EnhancerBookmarkList", TestEnhancerBookmarkList);
    tests.emplace_back("EnhancerNavHistory", TestEnhancerNavHistory);
    tests.emplace_back("EnhancerSelectionHistory", TestEnhancerSelectionHistory);
    tests.emplace_back("EnhancerComponentClipboard", TestEnhancerComponentClipboard);
    tests.emplace_back("EnhancerKeepPlayChanges", TestEnhancerKeepPlayChanges);
    tests.emplace_back("EnhancerFavoritePages", TestEnhancerFavoritePages);
    tests.emplace_back("EnhancerTabStrip", TestEnhancerTabStrip);
    tests.emplace_back("EnhancerTabStateRoundTrip", TestEnhancerTabStateRoundTrip);
    tests.emplace_back("EnhancerFieldState", TestEnhancerFieldState);
    tests.emplace_back("EnhancerFieldReset", TestEnhancerFieldReset);
    tests.emplace_back("EnhancerNonSerializedField", TestEnhancerNonSerializedField);
    tests.emplace_back("EnhancerGlobMatch", TestEnhancerGlobMatch);
    tests.emplace_back("EnhancerFuzzyScore", TestEnhancerFuzzyScore);
    tests.emplace_back("EnhancerRemapFolderKeys", TestEnhancerRemapFolderKeys);
    tests.emplace_back("EnhancerFormatLength", TestEnhancerFormatLength);
    tests.emplace_back("EnhancerIconTable", TestEnhancerIconTable);
    tests.emplace_back("EnhancerPalette", TestEnhancerPalette);
    tests.emplace_back("EnhancerUserStateRoundTrip", TestEnhancerUserStateRoundTrip);
    tests.emplace_back("EnhancerShortcutTable", TestEnhancerShortcutTable);
    tests.emplace_back("EnhancerTreeLineMasks", TestEnhancerTreeLineMasks);
    tests.emplace_back("EnhancerHierStyleRoundTrip", TestEnhancerHierStyleRoundTrip);
    tests.emplace_back("EnhancerFolderKinds", TestEnhancerFolderKinds);
    tests.emplace_back("EnhancerFolderStyleResolve", TestEnhancerFolderStyleResolve);
}
