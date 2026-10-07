// Editor Enhancers (docs/EDITOR_ENHANCERS.md) - tests for the pure-logic layer under the
// vHierarchy / vFolders / vInspector / vTabs / vFavorites / vRuler-style features, plus the
// shortcut-table invariants those features rely on.

#include "UnitTestSupport.h"

#include "Enhancers/EnhancerCore.h"
#include "Enhancers/EnhancerUserState.h"
#include "Enhancers/Palette.h"
#include "Shortcuts.h"
#include "World.h"
#include "AssetLibrary.h"
#include "SceneSerializer.h"
#include "ComponentRegistry.h"

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

} // namespace

void RegisterEnhancerTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("EnhancerRefsRoundTrip", TestEnhancerRefsRoundTrip);
    tests.emplace_back("EnhancerBookmarkList", TestEnhancerBookmarkList);
    tests.emplace_back("EnhancerNavHistory", TestEnhancerNavHistory);
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
}
