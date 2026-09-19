// #173 - headless unit tests: `TartarusEngine.exe --unit-tests`.
//
// Runs before any window, GL context, audio device or PhysX world exists, so it works on a CI
// runner with no GPU (unlike --smoke-test). Only pure C++ code is exercised here: undo deltas,
// GUIDs, atomic file writes, texture-cache keys, material JSON robustness, the component
// registry, the Animator Controller, asset identity / GUID-following references and the project
// file watcher (#132). Each CHECK prints on failure; the run returns the number of failed checks (0 = pass).
//
// Deliberately no test framework dependency: a CHECK macro and a list of functions is all this
// needs, and it keeps the engine's third-party surface unchanged.
#include "UnitTests.h"

#include "AnimatorController.h"
#include "AssetDatabase.h"
#include "AssetGuid.h"
#include "Components.h"
#include "AtomicFile.h"
#include "ComponentReflection.h"
#include "ComponentRegistry.h"
#include "MaterialAsset.h"
#include "ProjectPaths.h"
#include "ProjectWatcher.h"
#include "TextureCache.h"
#include "UndoDeltaChain.h"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

int g_Failures = 0;
int g_Checks = 0;
const char* g_CurrentTest = "";

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        ++g_Checks;                                                                              \
        if (!(cond)) {                                                                           \
            ++g_Failures;                                                                        \
            std::cout << "[UnitTest] FAIL " << g_CurrentTest << ": " #cond " (" << __FILE__     \
                      << ":" << __LINE__ << ")\n";                                               \
        }                                                                                        \
    } while (0)

std::filesystem::path TempDir() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec) / "TartarusUnitTests";
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// --- AssetGuid ------------------------------------------------------------------------------
void TestAssetGuid() {
    const AssetGuid g = AssetGuid::Generate();
    CHECK(g.IsValid());
    CHECK(AssetGuid::FromString(g.ToString()) == g);
    CHECK(AssetGuid::Generate() != g);
    CHECK(!AssetGuid::FromString("").IsValid());
    CHECK(!AssetGuid::FromString("not-a-guid").IsValid());
}

// --- UndoDelta chain: push three states, pop them back in reverse ---------------------------
struct TestEntry { std::string Delta; int Tag = 0; };

void TestUndoDeltaChain() {
    const std::string s1 = R"({"entities":[{"name":"A","x":1}],"sky":1})";
    const std::string s2 = R"({"entities":[{"name":"A","x":2}],"sky":1})";
    const std::string s3 = R"({"entities":[{"name":"A","x":2},{"name":"B","x":5}],"sky":3})";

    std::vector<TestEntry> stack;
    std::string base;
    UndoDelta::Push(stack, base, TestEntry{{}, 1}, s1);
    UndoDelta::Push(stack, base, TestEntry{{}, 2}, s2);
    UndoDelta::Push(stack, base, TestEntry{{}, 3}, s3);
    CHECK(stack.size() == 3);
    CHECK(stack.back().Delta.empty()); // the top is held in full

    auto same = [](const std::string& a, const std::string& b) {
        return nlohmann::json::parse(a) == nlohmann::json::parse(b);
    };
    TestEntry e;
    std::string full;
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Ok);
    CHECK(e.Tag == 3 && same(full, s3));
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Ok);
    CHECK(e.Tag == 2 && same(full, s2));
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Ok);
    CHECK(e.Tag == 1 && same(full, s1));
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Empty);

    // Malformed input never throws out of the helpers.
    std::string out;
    CHECK(!UndoDelta::ApplyPatch(s1, "{not json", out));
    CHECK(!UndoDelta::ApplyPatch(s1, "", out));
    CHECK(UndoDelta::MakePatch("{broken", s1).empty());
}

// --- AtomicFile ----------------------------------------------------------------------------
void TestAtomicFile() {
    const auto path = TempDir() / "atomic.txt";
    CHECK(AtomicFile::WriteBytes(path, "first", true));
    CHECK(ReadAll(path) == "first");
    CHECK(AtomicFile::WriteBytes(path, "second, longer", true)); // replace in place
    CHECK(ReadAll(path) == "second, longer");
    // A directory that can't exist: must fail cleanly and leave nothing behind.
    const auto bad = TempDir() / "atomic.txt" / "child" / "x.txt"; // parent is a FILE
    CHECK(!AtomicFile::WriteBytes(bad, "nope", true));
    CHECK(ReadAll(path) == "second, longer");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// --- TextureCache keys: pixel-affecting settings change the hash, sampler state doesn't ------
void TestTextureCacheHash() {
    TextureImportSettings a;
    const uint64_t h = TextureCache::HashSettings(a);

    TextureImportSettings b = a; b.AnisoLevel = 16;
    CHECK(TextureCache::HashSettings(b) == h);
    b = a; b.FilterMode = TextureImportSettings::Filter::Point;
    CHECK(TextureCache::HashSettings(b) == h);
    b = a; b.WrapMode = TextureImportSettings::Wrap::ClampToEdge;
    CHECK(TextureCache::HashSettings(b) == h);

    b = a; b.MaxTextureSize = 512;
    CHECK(TextureCache::HashSettings(b) != h);
    b = a; b.IsSRGB = false;
    CHECK(TextureCache::HashSettings(b) != h);
    b = a; b.CompressionMode = TextureImportSettings::Compression::Normal;
    const uint64_t hc = TextureCache::HashSettings(b);
    CHECK(hc != h);
    b.GenerateMipmaps = false; // compressed entries bake their mips, so this matters now
    CHECK(TextureCache::HashSettings(b) != hc);
    b = a; b.GenerateMipmaps = false; // uncompressed: mips are generated on the GPU, not cached
    CHECK(TextureCache::HashSettings(b) == h);
}

// --- MaterialAsset: wrong-typed JSON must load with defaults, never throw ---------------------
void TestMaterialRobustness() {
    const auto path = TempDir() / "bad.mat";
    const char* cases[] = {
        R"({"baseColor":"red","metallic":"very","roughness":[1,2],"properties":5})",
        R"({"baseColor":[1,2],"emissiveColor":null,"renderQueue":"x","opacity":{}})",
        R"([1,2,3])",
        R"({"albedoMap":42,"normalMap":{"a":1},"shader":7})",
    };
    for (const char* json : cases) {
        CHECK(AtomicFile::WriteBytes(path, json, true));
        bool threw = false;
        std::shared_ptr<MaterialAsset> m;
        try { m = MaterialAsset::Load(path.string(), nullptr); } catch (...) { threw = true; }
        CHECK(!threw);
    }
    CHECK(AtomicFile::WriteBytes(path, "{ this is not json", true));
    bool threw = false;
    try { (void)MaterialAsset::Load(path.string(), nullptr); } catch (...) { threw = true; }
    CHECK(!threw);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // #208 - a material follows its shader by GUID when the .shader is renamed.
    const auto dir = TempDir() / "shaderguid";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto oldShader = dir / "Old.shader", newShader = dir / "New.shader", mat = dir / "m.mat";
    CHECK(AtomicFile::WriteBytes(oldShader, "Vertex { engine://ModelVertex.glsl }", true));
    // A temp folder is outside the project, where EnsureGuid won't write .meta files: provide one.
    const AssetGuid fresh = AssetGuid::Generate();
    CHECK(AtomicFile::WriteBytes(dir / "Old.shader.meta", json({{"guid", fresh.ToString()}, {"type", "shader"}}).dump(), true));
    const AssetGuid g = AssetDatabase::EnsureGuid(oldShader.string());
    CHECK(g == fresh);
    CHECK(g.IsValid());
    const json mj = {{"matVersion", 2}, {"shader", oldShader.string()}, {"shaderGuid", g.ToString()}};
    CHECK(AtomicFile::WriteBytes(mat, mj.dump(), true));
    std::filesystem::rename(oldShader, newShader, ec);
    AssetDatabase::NotifyMoved(oldShader.string(), newShader.string());
    auto moved = MaterialAsset::Load(mat.string(), nullptr);
    CHECK(moved && std::filesystem::path(moved->ShaderPath) == newShader);
    // No GUID (or an unknown one) keeps the stored path.
    CHECK(AtomicFile::WriteBytes(mat, json({{"matVersion", 2}, {"shader", "missing.shader"}}).dump(), true));
    auto kept = MaterialAsset::Load(mat.string(), nullptr);
    CHECK(kept && kept->ShaderPath == "missing.shader");
    std::filesystem::remove_all(dir, ec);
}

// --- Component registry: unique names / keys, enum labels well-formed --------------------------
void TestComponentRegistry() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    CHECK(!ComponentRegistry::All().empty());
    std::set<std::string> names;
    for (const RegisteredComponent& rc : ComponentRegistry::All()) {
        CHECK(rc.Meta.Name && *rc.Meta.Name);
        CHECK(names.insert(rc.Meta.Name).second); // no duplicate display names (they're JSON keys)
        CHECK(rc.Has && rc.Get && rc.Add && rc.Remove);
        std::set<std::string> fields;
        for (const ReflectField& f : rc.Meta.Fields) {
            CHECK(fields.insert(f.Name).second);
            if (f.Type == ReflectFieldType::Enum && !f.EditorHidden) {
                CHECK(f.EnumLabels != nullptr && f.EnumCount > 0);
                for (int i = 0; f.EnumLabels && i < f.EnumCount; ++i) CHECK(*ReflectEnumLabel(f, i) != '\0');
                CHECK(*ReflectEnumLabel(f, f.EnumCount) == '\0'); // out of range -> ""
            }
        }
    }
}

// --- Animator Controller: transitions, exit time, Any state, triggers, JSON round trip (#175) --
void TestAnimatorController() {
    using AC = AnimatorController;
    AC c;
    c.Parameters = {{"Speed", AC::ParamType::Float, 0.0f}, {"Jump", AC::ParamType::Trigger, 0.0f},
                    {"Grounded", AC::ParamType::Bool, 1.0f}};
    c.States = {{"Idle", "idle", 1.0f, true}, {"Walk", "walk", 1.0f, true},
                {"Run", "run", 1.2f, true}, {"Jump", "jump", 1.0f, false}};
    c.DefaultState = "Idle";
    AC::Transition idleWalk{"Idle", "Walk", {{"Speed", AC::Op::Greater, 0.1f}}, false, 0.9f, 0.2f};
    AC::Transition walkIdle{"Walk", "Idle", {{"Speed", AC::Op::Less, 0.1f}}, false, 0.9f, 0.2f};
    AC::Transition walkRun{"Walk", "Run", {{"Speed", AC::Op::Greater, 3.0f}}, false, 0.9f, 0.2f};
    AC::Transition anyJump{"Any", "Jump", {{"Jump", AC::Op::If, 0.0f}}, false, 0.9f, 0.1f};
    AC::Transition jumpIdle{"Jump", "Idle", {}, true, 0.95f, 0.2f};
    AC::Transition broken{"Idle", "Run", {}, false, 0.9f, 0.2f}; // no condition, no exit time: never
    c.Transitions = {idleWalk, walkIdle, walkRun, anyJump, jumpIdle, broken};

    CHECK(c.DefaultStateIndex() == 0);
    CHECK(c.FindState("Run") == 2);
    CHECK(c.FindState("Nope") == -1);

    std::vector<AnimatorParam> p = {{"Speed", 0, 0.0f}, {"Jump", 3, 0.0f}, {"Grounded", 2, 1.0f}};
    CHECK(c.PickTransition(0, 0.0f, p) == -1);            // idle, not moving
    p[0].Value = 1.0f;
    CHECK(c.PickTransition(0, 0.0f, p) == 0);             // Idle -> Walk
    CHECK(c.PickTransition(1, 0.0f, p) == -1);            // walking, not fast enough to run
    p[0].Value = 4.0f;
    CHECK(c.PickTransition(1, 0.0f, p) == 2);             // Walk -> Run
    p[1].Value = 1.0f;                                   // SetTrigger("Jump")
    CHECK(c.PickTransition(2, 0.3f, p) == 3);             // Any -> Jump beats nothing else
    CHECK(p[1].Value == 0.0f);                           // ... and consumed the trigger
    CHECK(c.PickTransition(3, 0.5f, p) == -1);            // Jump waits for its exit time
    CHECK(c.PickTransition(3, 0.96f, p) == 4);            // Jump -> Idle at 95%
    p[1].Value = 1.0f;
    CHECK(c.PickTransition(3, 0.2f, p) == -1);            // Any never re-enters the state it targets
    CHECK(p[1].Value == 1.0f);                           // an unused trigger stays set
    std::vector<AnimatorParam> none;
    CHECK(c.PickTransition(0, 0.0f, none) == -1);         // missing parameters never match
    CHECK(c.PickTransition(-1, 0.0f, p) == -1);
    CHECK(c.PickTransition(99, 0.0f, p) == -1);

    // JSON round trip keeps everything.
    AC back;
    CHECK(AC::FromJsonString(c.ToJsonString(), back));
    CHECK(back.Parameters.size() == 3 && back.Parameters[1].Type == AC::ParamType::Trigger);
    CHECK(back.States.size() == 4 && back.States[2].Speed == 1.2f && !back.States[3].Loop);
    CHECK(back.Transitions.size() == 6 && back.Transitions[3].From == "Any");
    CHECK(back.Transitions[4].HasExitTime && back.Transitions[4].ExitTime == 0.95f);
    CHECK(back.Transitions[1].Conditions.size() == 1 && back.Transitions[1].Conditions[0].Mode == AC::Op::Less);
    CHECK(back.DefaultState == "Idle");

    // Robustness: garbage is rejected, unknown enum names fall back, bad entries are skipped.
    AC junk;
    CHECK(!AC::FromJsonString("{not json", junk));
    CHECK(!AC::FromJsonString("[1,2]", junk));
    CHECK(AC::FromJsonString(R"({"parameters":[{"name":"x","type":"banana"},{"type":"int"}],
        "states":[{"name":"A","speed":"fast"},5],"transitions":[{"from":"A","to":"A",
        "conditions":[{"param":"x","mode":"sideways"}]}]})", junk));
    CHECK(junk.Parameters.size() == 1 && junk.Parameters[0].Type == AC::ParamType::Float);
    CHECK(junk.States.size() == 1 && junk.States[0].Speed == 1.0f);
    CHECK(junk.Transitions.size() == 1 && junk.Transitions[0].Conditions[0].Mode == AC::Op::Greater);

    // The component's setters create parameters on first use and keep their type.
    AnimatorControllerComponent comp;
    comp.SetFloat("Speed", 2.5f);
    comp.SetTrigger("Jump");
    comp.SetBool("Grounded", true);
    CHECK(comp.Params.size() == 3);
    CHECK(comp.GetFloat("Speed") == 2.5f && comp.GetFloat("Jump") == 1.0f && comp.GetFloat("Grounded") == 1.0f);
    comp.ResetTrigger("Jump");
    CHECK(comp.GetFloat("Jump") == 0.0f && comp.GetFloat("Missing") == 0.0f);
}

// --- #132: asset identity - path keys, asset types, GUID-following references ------------------
void TestAssetIdentity() {
    namespace fs = std::filesystem;
    // One file, three spellings, one key.
    const fs::path dir = TempDir() / "identity";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Sub", ec);
    const std::string a = (dir / "Sub" / "Tex.png").string();
    CHECK(AssetDatabase::PathKey(a) == AssetDatabase::PathKey((dir / "sub" / "tex.png").generic_string()));
    CHECK(AssetDatabase::PathKey(a) == AssetDatabase::PathKey((dir / "Sub" / "." / "Tex.png").string()));
    CHECK(AssetDatabase::PathKey(a) != AssetDatabase::PathKey((dir / "Sub" / "Tex2.png").string()));

    // Asset types by extension; .json only under project/scenes, never the editor's safety copies.
    CHECK(AssetDatabase::AssetType("x/a.PNG") == "texture");
    CHECK(AssetDatabase::AssetType("x/a.controller") == "animatorcontroller");
    CHECK(AssetDatabase::AssetType("x/a.tescript") == "script");
    CHECK(AssetDatabase::AssetType("x/a.shader") == "shader");
    CHECK(AssetDatabase::AssetType("x/a.txt").empty());
    CHECK(AssetDatabase::AssetType("x/settings.json").empty());
    CHECK(AssetDatabase::AssetType(ProjectPaths::Resolve("scenes/Level.json")) == "scene");
    CHECK(AssetDatabase::AssetType(ProjectPaths::Resolve("scenes/Level.recovery.json")).empty());
    CHECK(AssetDatabase::AssetType(ProjectPaths::Resolve("settings.json")).empty());

    // A reference ("file#clip") follows its file to a new name by GUID; the suffix survives.
    const std::string model = (dir / "Hero.fbx").string(), moved = (dir / "Sub" / "HeroRig.fbx").string();
    CHECK(AtomicFile::WriteBytes(model, "not really an fbx", true));
    const AssetGuid g = AssetGuid::Generate();
    CHECK(AtomicFile::WriteBytes(model + ".meta", json({{"guid", g.ToString()}, {"type", "model"}}).dump(), true));
    const std::string ref = model + "#Run";
    const std::string refGuid = AssetDatabase::RefGuid(ref);
    CHECK(refGuid == g.ToString());
    CHECK(AssetDatabase::FollowRef(ref, refGuid) == ref);      // still there: unchanged
    fs::rename(model, moved, ec);
    AssetDatabase::NotifyMoved(model, moved);                    // moves the .meta too
    CHECK(fs::exists(moved + ".meta", ec) && !fs::exists(model + ".meta", ec));
    const std::string followed = AssetDatabase::FollowRef(ref, refGuid);
    CHECK(AssetDatabase::PathKey(followed.substr(0, followed.find('#'))) == AssetDatabase::PathKey(moved));
    CHECK(followed.size() > 4 && followed.compare(followed.size() - 4, 4, "#Run") == 0);
    CHECK(AssetDatabase::FollowRef("Run", "") == "Run");        // an own clip name: not a file
    CHECK(AssetDatabase::FollowRef(ref, "0000000000000001") == ref); // unknown GUID: unchanged
    CHECK(AssetDatabase::RefGuid("primitive://cube#3").empty());

    // A material follows a renamed texture by the GUID it saved.
    const std::string tex = (dir / "Albedo.png").string(), tex2 = (dir / "Sub" / "Albedo2.png").string();
    CHECK(AtomicFile::WriteBytes(tex, "png", true));
    const AssetGuid tg = AssetGuid::Generate();
    CHECK(AtomicFile::WriteBytes(tex + ".meta", json({{"guid", tg.ToString()}, {"type", "texture"}}).dump(), true));
    CHECK(AssetDatabase::EnsureGuid(tex) == tg);
    MaterialAsset ma;
    ma.Path = (dir / "m.mat").string();
    ma.AlbedoMapPath = tex;
    CHECK(ma.Save());
    CHECK(ReadAll(ma.Path).find(tg.ToString()) != std::string::npos); // "textureGuids" written
    fs::rename(tex, tex2, ec);
    AssetDatabase::NotifyMoved(tex, tex2);
    auto back = MaterialAsset::Load(ma.Path, nullptr);
    CHECK(back && AssetDatabase::PathKey(back->AlbedoMapPath) == AssetDatabase::PathKey(tex2));
    fs::remove_all(dir, ec);
}

// --- #132: the project watcher reports adds, renames, moves, edits and deletes ----------------
void TestProjectWatcher() {
    namespace fs = std::filesystem;
    using K = ProjectWatcher::Change::Kind;
    const fs::path dir = TempDir() / "watch";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Library", ec);
    fs::create_directories(dir / "sub", ec);
    CHECK(ProjectWatcher::Start(dir.string()));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto settle = [&] {
        std::vector<ProjectWatcher::Change> all;
        bool overflow = false;
        for (int i = 0; i < 80; ++i) { // up to 4 s (slow CI runners)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto got = ProjectWatcher::Drain(150, overflow);
            all.insert(all.end(), got.begin(), got.end());
            if (!all.empty() && got.empty()) break;
        }
        return all;
    };
    auto has = [](const std::vector<ProjectWatcher::Change>& v, K kind, const fs::path& p, const fs::path& old = {}) {
        for (const auto& c : v)
            if (c.Type == kind && AssetDatabase::PathKey(c.Path) == AssetDatabase::PathKey(p.string()) &&
                (old.empty() || AssetDatabase::PathKey(c.OldPath) == AssetDatabase::PathKey(old.string())))
                return true;
        return false;
    };

    const fs::path a = dir / "a.png";
    CHECK(AtomicFile::WriteBytes(a, "1", true));
    std::ofstream(dir / "Library" / "cache.bin") << "x"; // ignored folder
    auto c1 = settle();
    CHECK(has(c1, K::Added, a) || has(c1, K::Modified, a));
    CHECK(std::none_of(c1.begin(), c1.end(), [](const auto& c) { return c.Path.find("Library") != std::string::npos; }));
    CHECK(std::none_of(c1.begin(), c1.end(), [](const auto& c) { return c.Path.find(".tmp-") != std::string::npos; }));

    const fs::path b = dir / "b.png";
    fs::rename(a, b, ec);
    auto c2 = settle();
    CHECK(has(c2, K::Renamed, b, a));

    const fs::path moved = dir / "sub" / "b.png";
    fs::rename(b, moved, ec);                       // across folders: Removed + Added, paired
    auto c3 = settle();
    CHECK(has(c3, K::Renamed, moved, b));

    std::ofstream(moved, std::ios::app) << "more";
    auto c4 = settle();
    CHECK(has(c4, K::Modified, moved));

    const fs::path t = dir / "temp.png";              // created and deleted before it settles
    std::ofstream(t) << "x";
    fs::remove(t, ec);
    fs::remove(moved, ec);
    auto c5 = settle();
    CHECK(has(c5, K::Removed, moved));
    CHECK(!has(c5, K::Added, t) && !has(c5, K::Removed, t));

    ProjectWatcher::Stop();
    CHECK(!ProjectWatcher::IsRunning());
    fs::remove_all(dir, ec);
}

} // namespace

int RunUnitTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"AssetGuid", TestAssetGuid},
        {"UndoDeltaChain", TestUndoDeltaChain},
        {"AtomicFile", TestAtomicFile},
        {"TextureCacheHash", TestTextureCacheHash},
        {"MaterialRobustness", TestMaterialRobustness},
        {"ComponentRegistry", TestComponentRegistry},
        {"AnimatorController", TestAnimatorController},
        {"AssetIdentity", TestAssetIdentity},
        {"ProjectWatcher", TestProjectWatcher},
    };
    for (const auto& [name, fn] : tests) {
        g_CurrentTest = name;
        const int before = g_Failures;
        try {
            fn();
        } catch (const std::exception& e) {
            ++g_Failures;
            std::cout << "[UnitTest] FAIL " << name << ": threw " << e.what() << "\n";
        } catch (...) {
            ++g_Failures;
            std::cout << "[UnitTest] FAIL " << name << ": threw a non-std exception\n";
        }
        std::cout << "[UnitTest] " << (g_Failures == before ? "PASS " : "FAIL ") << name << "\n";
    }
    std::cout << "[UnitTest] " << g_Checks << " checks, " << g_Failures << " failure(s)\n";
    std::cout.flush();
    return g_Failures;
}
