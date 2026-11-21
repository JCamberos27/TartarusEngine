// Asset database unit tests (moved from UnitTests.cpp): GUIDs, .meta migration, asset identity and GUID-following references, the project watcher, folder / pack import, async CPU import.
// Pure C++ like the rest of --unit-tests (no window, GL or audio device).
#include "TimeService.h"
#include "UnitTestSupport.h"
#include "../Game/FirstPersonMovementInput.h"

#include "AnimatorController.h"
#include "AnimatorLint.h"
#include "RootMotion.h"
#include "FirstPersonBody.h"
#include "WeaponJankMeter.h"
#include "FirstPersonBodyContract.h"
#include "Curve.h"
#include "IK.h"
#include "FirstPersonWeaponWizard.h"
#include "ClipAnalysis.h"
#include "FirstPersonAnimation.h"
#include "WeaponPrefab.h"
#include "FirstPersonAdsCarry.h"
#include "AudioEngine.h"
#include <set>
#include "Log.h" // #178 stack traces
#include "InputMap.h"
#include "AssetChangeHooks.h"
#include "AssetDatabase.h"
#include "AssetGuid.h"
#include "Components.h"
#include "AtomicFile.h"
#include "Utf8.h"
#include "Camera.h"
#include "PhysicsWorld.h"  // #201 - the physics sync regression test
#include "Combat/Damage.h" // hit regions, damage zones
#include "Npc/NpcRagdoll.h" // the soldier's part table
#include "GameModuleAPI.h"  // QueryFilter / RaycastHit
#include <cmath>   // #202 isfinite
#include <limits>
#include "ComponentReflection.h"
#include "ComponentRegistry.h"
#include "AssetLibrary.h" // #178 preset apply
#include "MaterialAsset.h"
#include "PhysicMaterialAsset.h"
#include "PlayerConfig.h" // #174 - player.json round-trip
#include "ProjectSettings.h"
#include "Player.h"
#include "PostProcessVolume.h"
#include "Tonemapper.h"
#include "Core/Paths.h"
#include "ProjectWatcher.h"
#include "TextureCache.h"
#include "Model.h"       // texture-path resolver
#include "AssetImport.h" // folder import
#include "ImportQueueManager.h"
#include "SceneSerializer.h" // #121
#include "AnimationSystem.h"           // #123 - the spin systems, driven frame by frame below
#include "GameRules.h"
#include "HotReloadGameModule.h"      // Spin / Transform Controller run inside TartarusGame.dll, not the exe
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetModuleFileNameW: --unit-tests runs before EnginePaths::Init
#undef near            // windows.h #defines these two, and the tests use them as variable names
#undef far
#include "UndoDeltaChain.h"
#include "World.h"
#include "ImpostorLibrary.h"
#include "BulletHoles.h"
#include "ShellCasings.h"
#include "RotationMath.h"
#include "SkyAtmosphere.h"
#include "SkySettings.h"
#include "TimeOfDay.h"

#include <glm/gtc/matrix_transform.hpp>

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include "Wardrobe.h"
#include "OutfitCoverage.h"
#include "OutfitSystem.h"
#include "Texture.h"
#include "PngWrite.h" // async decode test fixture
#include "EditorLayerInternal.h" // ClipFilterMatch
#include <set>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

using json = nlohmann::json;

// Moved from UnitTests.cpp (pure move).
namespace {
using UnitTestSupport::TempDir;
using UnitTestSupport::ReadAll;

// --- #121: asset data migrates out of scene files into .meta, and never clobbers it ---------
void TestAssetMetaMigration() {
    auto migrate = [](const std::string& entry, const std::string& meta) {
        return json::parse(SceneSerializer::MigrateAssetMetaFields(entry, meta));
    };

    // A bare sidecar takes everything the old scene entry had. textureImport lands on "importer".
    const std::string entry = R"({"path":"t.png","folder":"Props","displayName":"Crate",)"
                              R"("labels":["a","b"],"textureImport":{"isSRGB":false,"anisoLevel":4}})";
    json add = migrate(entry, R"({"guid":"g","type":"texture"})");
    CHECK(add["folder"] == "Props");
    CHECK(add["displayName"] == "Crate");
    CHECK(add["labels"].size() == 2);
    CHECK(add["importer"]["isSRGB"] == false && add["importer"]["anisoLevel"] == 4);
    CHECK(!add.contains("textureImport")); // renamed, not carried through under the old key

    // The sidecar wins wherever it already has a value: the scene copy is the stale one.
    add = migrate(entry, R"({"folder":"Newer","importer":{"isSRGB":true}})");
    CHECK(!add.contains("folder") && !add.contains("importer"));
    CHECK(add["displayName"] == "Crate" && add["labels"].size() == 2); // the rest still migrates

    // Nothing to do -> "{}", so the caller skips the .meta write entirely.
    CHECK(SceneSerializer::MigrateAssetMetaFields(entry,
              R"({"folder":"F","displayName":"D","labels":[],"importer":{}})") == "{}");
    CHECK(SceneSerializer::MigrateAssetMetaFields(R"({"path":"t.png"})", "{}") == "{}");

    // modelImport maps onto the same "importer" key.
    add = migrate(R"({"modelImport":{"globalScale":0.01}})", "{}");
    CHECK(add["importer"]["globalScale"] == 0.01);

    // Malformed input on either side is never a reason to guess at a sidecar write.
    CHECK(SceneSerializer::MigrateAssetMetaFields("not json", "{}") == "{}");
    CHECK(SceneSerializer::MigrateAssetMetaFields(entry, "not json") == "{}");
    CHECK(SceneSerializer::MigrateAssetMetaFields("[1,2]", "{}") == "{}");
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

// Async loading's CPU halves (AssetLibrary::RequestModelAsync's workers run these): no GL anywhere, so
// they work here, headless. A deferred import is complete except for its GPU objects, and a texture decode
// honours Max Size. The Vivian head/body also pin the female-import fix: upright, at head height.
void TestAsyncImportCpu() {
    auto deferred = [](const std::string& rel) {
        return Model::ImportDeferred(ProjectPaths::Resolve(rel), ModelImportSettings{});
    };
    // The model half needs the Quantum pack's meshes (project content from the shared Drive, absent
    // from a packaged engine and from a clone that hasn't fetched them).
    const bool quantum = std::filesystem::exists(
        ProjectPaths::Resolve("assets/Characters/Quantum/Models/Female/Body/Heads/SKM_F_Vivian_Head.fbx"));
    if (quantum) {
        const auto head = deferred("assets/Characters/Quantum/Models/Female/Body/Heads/SKM_F_Vivian_Head.fbx");
        CHECK(head && head->NeedsGpuUpload());
        CHECK(head && head->MeshCount() > 1);
        CHECK(head && head->BoundsMax().y > 1.6f && head->BoundsMin().y > 1.3f); // on the neck, not lying at the feet
        const auto body = deferred("assets/Characters/Quantum/Models/Female/Body/Parts/SKM_F_Vivian_Body.fbx");
        CHECK(body && body->BoundsMax().y > 1.4f && body->BoundsMax().z < 0.5f); // upright, not stretched along Z
        // Offsets carrying a group node's frame, not the mesh node's (the jackets' "Cloth" group): chest height.
        const auto jacket = deferred("assets/Characters/Quantum/Models/Clothing/Male/Outerwear/SKM_Leather_Jacket.fbx");
        CHECK(jacket && jacket->BoundsMax().y > 1.3f && jacket->BoundsMax().z < 0.6f);
        const auto oldHead = deferred("assets/Characters/Quantum/Models/Female/Body/Heads/SKM_F_Vivian_Head_Old.fbx");
        CHECK(oldHead && oldHead->BoundsMax().y > 1.6f && oldHead->BoundsMin().y > 1.3f);
    } else {
        UnitTestSupport::Skip("no Quantum pack: model checks");
    } // the models are destroyed here with nothing uploaded, which must not touch GL

    const auto dir = TempDir();
    const std::string png = (dir / "async_decode.png").string();
    std::vector<unsigned char> px(64 * 32 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)(i * 7);
    CHECK(PngWrite::ToFile(std::filesystem::path(png).u8string(), 64, 32, 4, px.data(), 64 * 4));
    TextureImportSettings s;
    s.MaxTextureSize = 16;
    TextureCpuData d = Texture::DecodeFile(png, s);
    CHECK(d.Ok && d.SourceWidth == 64 && d.SourceHeight == 32 && d.Width == 16 && d.Height == 8 && d.Channels == 4);
    CHECK(d.Pixels.size() == (size_t)16 * 8 * 4 && d.GLFormat == 0);
    const auto pending = Texture::CreatePending(std::move(d));
    CHECK(pending && pending->IsPendingUpload() && !pending->IsValid() && pending->Width() == 64);
    CHECK(Texture::CreatePending(Texture::DecodeFile((dir / "missing.png").string(), s)) == nullptr);
    std::error_code ec;
    std::filesystem::remove(png, ec);
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
    // A folder with or without its trailing '/' is one folder; "inside" means a real child.
    const std::string sub = (dir / "Sub").generic_string();
    CHECK(AssetDatabase::SamePath(sub, sub + "/"));
    CHECK(!AssetDatabase::SamePath(a, ""));
    CHECK(AssetDatabase::IsUnderPath(a, sub + "/"));
    CHECK(AssetDatabase::IsUnderPath((dir / "sub" / "deep" / "x.png").string(), sub));
    CHECK(!AssetDatabase::IsUnderPath(sub, sub));
    CHECK(!AssetDatabase::IsUnderPath((dir / "Sub2" / "x.png").string(), sub)); // a sibling sharing a prefix
#ifdef _WIN32
    CHECK(AssetDatabase::PathKey("C:/") == "c:/");
    CHECK(AssetDatabase::IsUnderPath("C:/x/y.png", "C:/"));
#endif
    // The raw-text GUID scan (what a corrupt .meta and the thumbnail prune read).
    CHECK(AssetDatabase::FindMetaGuid("{\"guid\": \"0123456789ABCDEF\"}") == "0123456789abcdef");
    CHECK(AssetDatabase::FindMetaGuid("<<<<<<< HEAD\n  \"guid\"\t:\n\"aaaaaaaaaaaaaaaa\",") == "aaaaaaaaaaaaaaaa");
    CHECK(AssetDatabase::FindMetaGuid("\"guid\": 7, \"x\": {\"guid\":\"1111222233334444\"}") == "1111222233334444");
    CHECK(AssetDatabase::FindMetaGuid("\"guid\": \"0123456789abcdeg\"").empty()); // not hex
    CHECK(AssetDatabase::FindMetaGuid("\"guid\": \"0123456789abcdef0\"").empty()); // 17 digits
    CHECK(AssetDatabase::FindMetaGuid("\"guid\": \"0123456789abcdef").empty());    // cut off

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

// --- Asset change hooks: what SyncProjectChanges hands each cache after a file changes -----------
void TestAssetChangeHooks() {
    using E = AssetChangeHooks::Event;
    const std::string dir = (TempDir() / "hooks").string();
    const std::string ctrl = dir + "/Run.controller", styles = dir + "/editor_ui_styles.json";
    int ctrlHits = 0, styleHits = 0, decalHits = 0;
    E last;
    const int a = AssetChangeHooks::Register("animatorcontroller", [&](const E& e) { ++ctrlHits; last = e; return true; });
    const int b = AssetChangeHooks::RegisterFile(styles, [&](const E&) { ++styleHits; return false; });
    const int c = AssetChangeHooks::Register("decal", [&](const E&) { ++decalHits; return true; });
    CHECK(a > 0 && b > 0 && c > 0 && a != b && b != c);
    CHECK(AssetChangeHooks::Register("", [](const E&) { return true; }) == 0); // no type: refused
    CHECK(AssetChangeHooks::Register("decal", {}) == 0);                       // no function: refused

    auto make = [](E::Kind k, const std::string& path, const std::string& type, const std::string& old = {}) {
        E e; e.Type = k; e.Path = path; e.OldPath = old; e.AssetType = type; return e;
    };
    // By type: only the controller hook runs, and the count is what reported applying something.
    CHECK(AssetChangeHooks::Dispatch(make(E::Kind::Modified, ctrl, "animatorcontroller")) == 1);
    CHECK(ctrlHits == 1 && styleHits == 0 && decalHits == 0 && last.Path == ctrl);
    // By file (a non-asset): runs, reports nothing applied; a differently spelled path still matches.
    std::string stylesAlt = styles;
    std::replace(stylesAlt.begin(), stylesAlt.end(), '/', '\\');
    CHECK(AssetChangeHooks::Dispatch(make(E::Kind::Modified, stylesAlt, "")) == 0);
    CHECK(styleHits == 1 && ctrlHits == 1);
    // A move reaches the hooks of either end: a .controller renamed to .txt still tells the controller cache.
    CHECK(AssetChangeHooks::Dispatch(make(E::Kind::Moved, dir + "/Run.txt", "", ctrl)) == 1);
    CHECK(ctrlHits == 2 && last.Type == E::Kind::Moved && last.OldPath == ctrl);
    CHECK(AssetChangeHooks::Dispatch(make(E::Kind::Moved, dir + "/x.json", "", styles)) == 0 && styleHits == 2);
    // Something nobody listens for.
    CHECK(AssetChangeHooks::Dispatch(make(E::Kind::Added, dir + "/a.png", "texture")) == 0);
    // Rescan reaches every hook.
    CHECK(AssetChangeHooks::Dispatch(make(E::Kind::Rescan, "", "")) == 2);
    CHECK(ctrlHits == 3 && styleHits == 3 && decalHits == 1);
    // A hook added from inside a hook runs from the next Dispatch on.
    int lateHits = 0, late = 0;
    const int d = AssetChangeHooks::Register("prefab", [&](const E&) {
        if (!late) late = AssetChangeHooks::Register("prefab", [&](const E&) { ++lateHits; return true; });
        return true;
    });
    CHECK(AssetChangeHooks::Dispatch(make(E::Kind::Modified, dir + "/p.prefab", "prefab")) == 1 && lateHits == 0);
    CHECK(AssetChangeHooks::Dispatch(make(E::Kind::Modified, dir + "/p.prefab", "prefab")) == 2 && lateHits == 1);
    // A hook unregistered by an earlier hook in the same Dispatch isn't called (its owner is gone).
    int victimHits = 0, victim = 0;
    const int killer = AssetChangeHooks::Register("mask", [&](const E&) { AssetChangeHooks::Unregister(victim); return true; });
    victim = AssetChangeHooks::Register("mask", [&](const E&) { ++victimHits; return true; });
    CHECK(AssetChangeHooks::Dispatch(make(E::Kind::Modified, dir + "/m.mask", "mask")) == 1 && victimHits == 0);
    // Unregistered hooks stop running.
    for (int id : {a, b, c, d, late, killer}) AssetChangeHooks::Unregister(id);
    CHECK(AssetChangeHooks::Dispatch(make(E::Kind::Rescan, "", "")) == 0);
    CHECK(ctrlHits == 3 && styleHits == 3 && decalHits == 1 && lateHits == 1);
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
    using Changes = std::vector<ProjectWatcher::Change>;
    auto has = [](const Changes& v, K kind, const fs::path& p, const fs::path& old = {}) {
        for (const auto& c : v)
            if (c.Type == kind && AssetDatabase::PathKey(c.Path) == AssetDatabase::PathKey(p.string()) &&
                (old.empty() || AssetDatabase::PathKey(c.OldPath) == AssetDatabase::PathKey(old.string())))
                return true;
        return false;
    };
    // Drains until `done` holds for everything gathered (or 30 s pass: a loaded machine delivers late), then four quiet
    // polls (200 ms) more so a stray extra event still shows. Start() returns with the watch armed. Without `done`, the
    // first quiet spell after anything arrived ends it; that stopped early when a step's events came in two bursts.
    auto settle = [&](const std::function<bool(const Changes&)>& done = {}) {
        Changes all;
        bool overflow = false;
        int quiet = 0;
        for (int i = 0; i < 600; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto got = ProjectWatcher::Drain(150, overflow);
            all.insert(all.end(), got.begin(), got.end());
            quiet = got.empty() ? quiet + 1 : 0;
            if (!all.empty() && quiet >= 4 && (!done || done(all))) break;
        }
        return all;
    };

    const fs::path a = dir / "a.png";
    CHECK(AtomicFile::WriteBytes(a, "1", true));
    std::ofstream(dir / "Library" / "cache.bin") << "x"; // ignored folder
    auto c1 = settle([&](const Changes& v) { return has(v, K::Added, a) || has(v, K::Modified, a); });
    CHECK(has(c1, K::Added, a) || has(c1, K::Modified, a));
    CHECK(std::none_of(c1.begin(), c1.end(), [](const auto& c) { return c.Path.find("Library") != std::string::npos; }));
    CHECK(std::none_of(c1.begin(), c1.end(), [](const auto& c) { return c.Path.find(".tmp-") != std::string::npos; }));

    const fs::path b = dir / "b.png";
    fs::rename(a, b, ec);
    auto c2 = settle([&](const Changes& v) { return has(v, K::Renamed, b, a); });
    CHECK(has(c2, K::Renamed, b, a));

    const fs::path moved = dir / "sub" / "b.png";
    fs::rename(b, moved, ec);                       // across folders: Removed + Added, paired
    auto c3 = settle([&](const Changes& v) { return has(v, K::Renamed, moved, b); });
    CHECK(has(c3, K::Renamed, moved, b));

    std::ofstream(moved, std::ios::app) << "more";
    auto c4 = settle([&](const Changes& v) { return has(v, K::Modified, moved); });
    CHECK(has(c4, K::Modified, moved));

    const fs::path t = dir / "temp.png";              // created and deleted before it settles
    std::ofstream(t) << "x";
    fs::remove(t, ec);
    fs::remove(moved, ec);
    auto c5 = settle([&](const Changes& v) { return has(v, K::Removed, moved); });
    CHECK(has(c5, K::Removed, moved));
    CHECK(!has(c5, K::Added, t) && !has(c5, K::Removed, t));

    // Deleted, then put back by an atomic save before the delete settled: a write, not a delete
    // (a test's cleanup deleting a .prefab, the next one saving it again).
    const fs::path back = dir / "back.prefab";
    CHECK(AtomicFile::WriteBytes(back, "1", true));
    settle([&](const Changes& v) { return has(v, K::Added, back) || has(v, K::Modified, back); });
    fs::remove(back, ec);
    CHECK(AtomicFile::WriteBytes(back, "2", true));
    auto cBack = settle([&](const Changes& v) { return has(v, K::Modified, back) || has(v, K::Removed, back); });
    CHECK(has(cBack, K::Modified, back) && !has(cBack, K::Removed, back));

    // A file written continuously must not starve other settled changes.
    {
        const fs::path busy = dir / "busy.png", quiet = dir / "quiet.png";
        std::atomic<bool> writing{true};
        std::thread writer([&] {
            while (writing) { std::ofstream(busy, std::ios::app) << "x"; std::this_thread::sleep_for(std::chrono::milliseconds(30)); }
        });
        std::ofstream(quiet) << "q";
        Changes got;
        bool overflow = false;
        for (int i = 0; i < 600 && !has(got, K::Added, quiet) && !has(got, K::Modified, quiet); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto d = ProjectWatcher::Drain(200, overflow);
            got.insert(got.end(), d.begin(), d.end());
        }
        writing = false;
        writer.join();
        CHECK(has(got, K::Added, quiet) || has(got, K::Modified, quiet));
        settle();
    }

    // Two Removed + one Added sharing a file name is ambiguous: no move is guessed.
    {
        fs::create_directories(dir / "d1", ec);
        fs::create_directories(dir / "d2", ec);
        fs::create_directories(dir / "d3", ec);
        std::ofstream(dir / "d1" / "m.png") << "1";
        std::ofstream(dir / "d2" / "m.png") << "2";
        settle([&](const Changes& v) {
            return (has(v, K::Added, dir / "d1" / "m.png") || has(v, K::Modified, dir / "d1" / "m.png")) &&
                   (has(v, K::Added, dir / "d2" / "m.png") || has(v, K::Modified, dir / "d2" / "m.png"));
        });
        fs::remove(dir / "d1" / "m.png", ec);
        fs::remove(dir / "d2" / "m.png", ec);
        std::ofstream(dir / "d3" / "m.png") << "3";
        auto c6 = settle([&](const Changes& v) {
            return has(v, K::Removed, dir / "d1" / "m.png") && has(v, K::Removed, dir / "d2" / "m.png") &&
                   has(v, K::Added, dir / "d3" / "m.png");
        });
        CHECK(has(c6, K::Removed, dir / "d1" / "m.png"));
        CHECK(has(c6, K::Removed, dir / "d2" / "m.png"));
        CHECK(has(c6, K::Added, dir / "d3" / "m.png"));
        CHECK(std::none_of(c6.begin(), c6.end(), [](const auto& c) { return c.Type == K::Renamed; }));
    }

    // bin/obj folders are ordinary now.
    {
        fs::create_directories(dir / "bin", ec);
        std::ofstream(dir / "bin" / "x.png") << "x";
        auto c7 = settle([&](const Changes& v) { return has(v, K::Added, dir / "bin" / "x.png") || has(v, K::Modified, dir / "bin" / "x.png"); });
        CHECK(has(c7, K::Added, dir / "bin" / "x.png") || has(c7, K::Modified, dir / "bin" / "x.png"));
    }

    ProjectWatcher::Stop();
    CHECK(!ProjectWatcher::IsRunning());
    fs::remove_all(dir, ec);
}

void TestAssetPackImport() {
    namespace fs = std::filesystem;
    const fs::path base = TempDir() / "asset_pack_import";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path pack = base / "Library" / "Gun";
    auto touch = [](const fs::path& p) {
        std::error_code e;
        fs::create_directories(p.parent_path(), e);
        std::ofstream(p, std::ios::binary) << "x";
    };
    touch(pack / "Models" / "Gun.fbx");
    touch(pack / "Textures" / "Gun_Textures" / "Gun_Albedo_Transparency.png");
    touch(pack / "Textures" / "Gun_Normal.png");
    touch(pack / "Textures" / "Detail.png");
    touch(pack / "Models" / "Detail.png"); // closer to the model: must win over Textures/Detail.png
    touch(pack / "Source" / "Gun.blend");
    const std::string models = (pack / "Models").string();
    auto same = [](const std::string& a, const fs::path& b) {
        std::error_code e;
        return fs::equivalent(a, b, e);
    };

    // The FBX points at the author's machine; the file sits in the sibling Textures/ tree.
    CHECK(same(Model::ResolveTexturePathIn(models, "C:\\Users\\author\\Gun_Normal.png"), pack / "Textures" / "Gun_Normal.png"));
    // Case differs and it's two folders down.
    CHECK(same(Model::ResolveTexturePathIn(models, "gun_albedo_transparency.png"), pack / "Textures" / "Gun_Textures" / "Gun_Albedo_Transparency.png"));
    // Spelling and format differ: separators dropped, .tga shipped as .png.
    CHECK(same(Model::ResolveTexturePathIn(models, "D:/tex/gun_AlbedoTransparency.tga"), pack / "Textures" / "Gun_Textures" / "Gun_Albedo_Transparency.png"));
    CHECK(same(Model::ResolveTexturePathIn(models, "Detail.png"), pack / "Models" / "Detail.png"));
    // Not anywhere: a clean path that doesn't exist, never a wrong file.
    CHECK(!fs::exists(Model::ResolveTexturePathIn(models, "Missing_Roughness.png"), ec));

    // Textures renamed to <Set>_<MapType> after export: found through the material's name.
    using Map = Model::TextureSetMap;
    const fs::path garage = base / "Library" / "Garage";
    touch(garage / "Models" / "Crate.fbx");
    touch(garage / "Textures" / "Garage_Props" / "Garage_Props_Base_Color.png");
    touch(garage / "Textures" / "Garage_Props" / "Garage_Props_Normal.png");
    touch(garage / "Textures" / "Garage_Props_Dark" / "Garage_Props_Dark_Base_Color.png");
    touch(garage / "Textures" / "Bed_Blanket_Clean" / "Bed_Blanket_Clean_Base_Color.png");
    touch(garage / "Textures" / "Bed_Frame_A_Base_Color.png");
    touch(garage / "Textures" / "Bed_Frame_B_Base_Color.png");
    touch(garage / "Textures" / "Mat_Sofa_Base_Color.png");
    touch(garage / "Textures" / "T_Crate_AO.png");
    const std::string gm = (garage / "Models").string();
    CHECK(same(Model::FindTextureSetMap(gm, {"Garage_props", "Crate"}, Map::Albedo),
               garage / "Textures" / "Garage_Props" / "Garage_Props_Base_Color.png")); // exact set beats "_Dark"
    CHECK(same(Model::FindTextureSetMap(gm, {"M_Garage_Props_Mat"}, Map::Normal),
               garage / "Textures" / "Garage_Props" / "Garage_Props_Normal.png"));
    CHECK(Model::FindTextureSetMap(gm, {"Garage_props"}, Map::Roughness).empty());
    CHECK(same(Model::FindTextureSetMap(gm, {"Bed_blanket"}, Map::Albedo),
               garage / "Textures" / "Bed_Blanket_Clean" / "Bed_Blanket_Clean_Base_Color.png")); // one variant
    CHECK(Model::FindTextureSetMap(gm, {"Bed_Frame"}, Map::Albedo).empty());                   // two: no guess
    CHECK(same(Model::FindTextureSetMap(gm, {"mat_sofa"}, Map::Albedo), garage / "Textures" / "Mat_Sofa_Base_Color.png"));
    CHECK(same(Model::FindTextureSetMap(gm, {"1364", "Crate"}, Map::Occlusion), garage / "Textures" / "T_Crate_AO.png"));

    // Classification.
    CHECK(AssetImport::ImportKind("a/b.FBX") == "model");
    CHECK(AssetImport::ImportKind("a/b.Png") == "texture");
    CHECK(AssetImport::ImportKind("a/manifest.json").empty());
    CHECK(AssetImport::IsSourceOnlyFile("a/Gun.blend"));
    CHECK(AssetImport::IsSourceOnlyFile("a/Thumbs.db"));
    CHECK(!AssetImport::IsSourceOnlyFile("a/Gun.fbx"));

    // Texture kind from the file name.
    using TK = AssetImport::TextureKind;
    CHECK(AssetImport::GuessTextureKind("a/Crate_Normal.png") == TK::Normal);
    CHECK(AssetImport::GuessTextureKind("a/Door_1_W_1_K_Nm.tga") == TK::Normal);
    CHECK(AssetImport::GuessTextureKind("a/T_Quantum_Arms_Unity_Normal.1003.png") == TK::Normal);
    CHECK(AssetImport::GuessTextureKind("a/Sofa_Normal_OpenGL_4K.png") == TK::Normal);
    CHECK(AssetImport::GuessTextureKind("a/Wall_Normal_Map.png") == TK::Normal);
    CHECK(AssetImport::GuessTextureKind("a/Crate_Roughness.png") == TK::Data);
    CHECK(AssetImport::GuessTextureKind("a/Mat_Sofa_Ambient_Occlusion.png") == TK::Data);
    CHECK(AssetImport::GuessTextureKind("a/T_Body.1002_Occlusion.png") == TK::Data);
    CHECK(AssetImport::GuessTextureKind("a/Floor_Mask_Map.png") == TK::Data);
    CHECK(AssetImport::GuessTextureKind("a/AKS74U_Metallic.png") == TK::Data);
    CHECK(AssetImport::GuessTextureKind("a/AKS74U_Albedo_Transparency.png") == TK::Color);
    CHECK(AssetImport::GuessTextureKind("a/Garage_Props_Base_Color.png") == TK::Color);
    CHECK(AssetImport::GuessTextureKind("a/Clocks_Emissive.png") == TK::Color);
    CHECK(AssetImport::GuessTextureKind("a/proto_grid_dark.png") == TK::Color);
    CHECK(!AssetLibrary::DefaultTextureSettings("a/Crate_Roughness.png").IsSRGB);
    CHECK(AssetLibrary::DefaultTextureSettings("a/Crate_Normal.png").TextureType == TextureImportSettings::Type::NormalMap);
    CHECK(AssetLibrary::DefaultTextureSettings("a/Crate_Albedo.png").IsSRGB);

    // A dropped folder is copied whole, layout kept, source files left behind.
    const fs::path assets = base / "project" / "assets";
    const fs::path project = base / "project", library = base / "library";
    const fs::path privateAssets = assets / "External";
    auto importDir = [&](const fs::path& source, const std::string& selectedLibrary, const std::string& kind = "") {
        return fs::path(AssetImport::ProjectImportDirectory(project.string(), source.string(), selectedLibrary, kind)).lexically_normal();
    };
    // Destination selection works before the target directories have been created.
    CHECK(importDir(library / "Gun", library.string()) == privateAssets.lexically_normal());
    CHECK(importDir(library / "Gun" / "a.fbx", library.string(), "Models") == (privateAssets / "Models").lexically_normal());
    CHECK(importDir(library, library.string()) == privateAssets.lexically_normal());
    CHECK(importDir(base / "library-other" / "a.fbx", library.string(), "Models") == (assets / "Models").lexically_normal());
    CHECK(importDir(library / ".." / "outside" / "a.png", library.string(), "Textures") == (assets / "Textures").lexically_normal());
    CHECK(importDir(pack, "", "Models") == (assets / "Models").lexically_normal());
    {   // a destination inside the source folder is rejected up front
        const AssetImport::FolderCopy inside = AssetImport::CopyFolderInto(pack.string(), (pack / "Models").string());
        CHECK(inside.Error == "destination is inside the source folder");
        CHECK(inside.Files.empty());
        const AssetImport::FolderCopy self = AssetImport::CopyFolderInto(pack.string(), pack.string());
        CHECK(self.Error == "destination is inside the source folder");
    }
    const AssetImport::FolderCopy privateCopy = AssetImport::CopyFolderInto(pack.string(), privateAssets.string());
    CHECK(privateCopy.Error.empty() && same(privateCopy.Folder, privateAssets / "Gun"));
    CHECK(fs::exists(privateAssets / "Gun" / "Textures" / "Gun_Textures" / "Gun_Albedo_Transparency.png", ec));
    const AssetImport::FolderCopy first = AssetImport::CopyFolderInto(pack.string(), assets.string());
    CHECK(first.Error.empty());
    CHECK(same(first.Folder, assets / "Gun"));
    CHECK(first.Files.size() == 5);
    CHECK(first.Skipped == 1);
    CHECK(fs::exists(assets / "Gun" / "Textures" / "Gun_Textures" / "Gun_Albedo_Transparency.png", ec));
    CHECK(!fs::exists(assets / "Gun" / "Source" / "Gun.blend", ec));
    // ...and the copied model still finds its textures.
    CHECK(same(Model::ResolveTexturePathIn((assets / "Gun" / "Models").string(), "C:/elsewhere/Gun_Normal.png"),
               assets / "Gun" / "Textures" / "Gun_Normal.png"));
    // Identical-file check behind "already in the project - using that copy".
    touch(base / "same_a.bin");
    touch(base / "same_b.bin");
    { std::ofstream(base / "diff.bin", std::ios::binary) << "y"; }
    CHECK(AssetImport::SameContents((base / "same_a.bin").string(), (base / "same_b.bin").string()));
    CHECK(!AssetImport::SameContents((base / "same_a.bin").string(), (base / "diff.bin").string()));
    CHECK(!AssetImport::SameContents((base / "same_a.bin").string(), (base / "missing.bin").string()));

    // A batch's summary counts what each import returned.
    {
        ImportQueueManager queue;
        queue.Enqueue({"a.fbx", "b.fbx", "c.png", "d.txt"});
        int calls = 0;
        queue.Update([&](const std::string& p) -> std::string {
            ++calls;
            return p == "d.txt" ? "" : p == "c.png" ? "texture" : "model";
        }, 10);
        CHECK(calls == 4);
        CHECK(!queue.IsActive());
        CHECK(queue.LastSummary().find("2 models, 1 texture") != std::string::npos);
        CHECK(queue.LastSummary().find("1 failed") != std::string::npos);
        queue.Enqueue({"e.fbx", "f.fbx"});
        queue.Update([](const std::string&) -> std::string { return "model"; }, 1);
        queue.CancelRemaining();
        CHECK(queue.LastSummary().find("1 model") != std::string::npos);
        CHECK(queue.LastSummary().find("1 cancelled") != std::string::npos);
    }

    // A second drop of the same folder never overwrites the first.
    const AssetImport::FolderCopy second = AssetImport::CopyFolderInto(pack.string(), assets.string());
    CHECK(same(second.Folder, assets / "Gun (2)"));

    fs::remove_all(base, ec);
}

} // namespace

void RegisterAssetsTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"AssetGuid", TestAssetGuid});
    tests.push_back({"AssetMetaMigration", TestAssetMetaMigration});
    tests.push_back({"AsyncImportCpu", TestAsyncImportCpu});
    tests.push_back({"AssetIdentity", TestAssetIdentity});
    tests.push_back({"ProjectWatcher", TestProjectWatcher});
    tests.push_back({"AssetChangeHooks", TestAssetChangeHooks});
    tests.push_back({"AssetPackImport", TestAssetPackImport});
}
