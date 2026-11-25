// Unit tests: Outfits: the wardrobe (styles, pairs, coverage), and the Quantum pack checks (posed bounds, edge bands, rigid pieces under a hood, material textures) that skip without the pack.
// (Moved out of UnitTests.cpp; registered by RunUnitTests through RegisterOutfitsTests.)
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
#include <set>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

using json = nlohmann::json;
using UnitTestSupport::g_Checks;
using UnitTestSupport::g_Failures;
using UnitTestSupport::TempDir;
using UnitTestSupport::ReadAll;

namespace {

// Character outfits: a small wardrobe's items are classified from their folders, and the rules
// (pairs, a hat's fitted haircut, clears, covers, skin) resolve an outfit to the right pieces.
void TestWardrobe() {
    const char* text = R"({
      "root": "assets/Chars",
      "skinMaterials": ["M_Body", "M_Head"],
      "excludeFolders": ["StaticMesh"],
      "hatSlot": "Hat", "hairSlot": "Hair",
      "namePrefixes": ["SKM_F_", "SM_F_", "SKM_", "SM_"], "femaleFolders": ["Female"], "femalePrefixes": ["SKM_F_"],
      "slots": [
        {"id": "Hair", "folders": ["Hair"], "headAttached": true, "layer": 9, "hides": false},
        {"id": "Hat", "folders": ["Hats", "WithHair"], "headAttached": true, "layer": 10},
        {"id": "Top", "folders": ["Tops"], "layer": 4},
        {"id": "Outerwear", "folders": ["Outerwear"], "layer": 6},
        {"id": "Pants", "folders": ["Pants"], "layer": 3},
        {"id": "Shoes", "folders": ["Shoes"], "layer": 2},
        {"id": "Wrist L", "folders": ["Accessories"], "nameSuffix": "_L", "layer": 8, "hides": false}
      ],
      "bodies": {
        "Male": {"parts": [{"part": "Torso", "model": "Body/Torso.fbx"}, {"part": "Legs", "model": "Body/Legs.fbx"},
                           {"part": "Feet", "model": "Body/Feet.fbx"}, {"part": "Arms", "model": "Body/Arms.fbx"},
                           {"part": "Hands", "model": "Body/Hands.fbx"}],
                 "alternates": {"ShoeFeet": "Body/Feet_Shoes.fbx"},
                 "races": [{"name": "European", "head": "Body/Head.fbx", "skin": ""},
                           {"name": "Afro", "head": "Body/Head_Afro.fbx", "skin": "_Afro", "parts": {"Torso": "Body/Torso_Afro.fbx"}}]},
        "Female": {"parts": [{"part": "Torso", "model": "F/Body.fbx"}, {"part": "Arms", "model": "F/Arms_Full.fbx"}],
                   "races": [{"name": "European", "head": "F/Head.fbx"}]}
      },
      "covers": [{"slot": "Pants", "nameLacksAll": ["Shorts"], "hide": ["Legs"]},
                 {"slot": "Top", "gender": "Male", "nameHasAny": ["Hoodie"], "hide": ["Arms"]},
                 {"slot": "Shoes", "skin": false, "replace": "Feet", "with": "ShoeFeet"}],
      "clears": [{"slot": "Hat", "pathHasAny": ["/WithHair/"], "clear": "Hair"},
                 {"slot": "Outerwear", "nameHasAny": ["Shirt"], "clear": "Top"}],
      "pairs": [{"whenSlot": "Shoes", "whenNameHas": "Boots", "slot": "Pants", "suffix": "_Inboots"}],
      "items": [{"path": "Clothing/Male/Tops/SKM_Secret.fbx", "hidden": true}]
    })";
    Wardrobe::Wardrobe w;
    std::string err;
    CHECK(Wardrobe::Parse(text, w, &err));
    CHECK(w.Bodies[0].Parts.size() == 5 && w.Bodies[0].Parts[0].second == "assets/Chars/Body/Torso.fbx");
    CHECK(w.Covers.size() == 3 && w.Covers[1].Gender == (int)Wardrobe::Gender::Male && w.Covers[0].Gender == -1);
    CHECK(!Wardrobe::Parse("{}", w, &err) && !err.empty());
    {   // A field of the wrong type fails the parse with the error, it doesn't throw out of it.
        std::string bad = text;
        bad.replace(bad.find("\"hidden\": true"), 14, "\"hidden\": \"yes\"");
        err.clear();
        bool parsed = true;
        try { parsed = Wardrobe::Parse(bad, w, &err); } catch (...) { parsed = true; err.clear(); }
        CHECK(!parsed && err.find("wrong type") != std::string::npos);
    }
    CHECK(Wardrobe::Parse(text, w, &err));
    CHECK(Wardrobe::PrettyName("SM_F_Balaclava_Crime", w.NamePrefixes) == "Balaclava Crime");

    CHECK(Wardrobe::PrettyName("SKM_F_Hoodie_Zipper_Hood", w.NamePrefixes) == "Hoodie Zipper Hood");
    CHECK(Wardrobe::PrettyName("SKM_Tshirt_Tucked", w.NamePrefixes) == "T-Shirt Tucked");
    CHECK(Wardrobe::PrettyName("SKM_Tshirt_Tucked") == "SKM T-Shirt Tucked"); // no pack prefixes: nothing dropped
    {   // The pack's names live in the .wardrobe, not in code: without them a "Hair" slot is an ordinary middle layer
        // that hides, nothing is a fitted haircut's hat, and no folder or prefix makes an item female.
        Wardrobe::Wardrobe bare;
        CHECK(Wardrobe::Parse(R"({"slots": [{"id": "Hair", "folders": ["Hair"]}, {"id": "Hat", "folders": ["Hats"]}],
                                  "bodies": {"Male": {"parts": [{"part": "Torso", "model": "T.fbx"}]}}})", bare, &err));
        CHECK(bare.HatSlot.empty() && bare.HairSlot.empty() && bare.NamePrefixes.empty());
        CHECK(bare.Slot("Hair")->Layer == Wardrobe::kDefaultLayer && bare.Slot("Hair")->Hides);
        const auto l = Wardrobe::LayerOf(bare, "Glasses", "a/SKM_Glasses.fbx", false); // not a slot of this wardrobe
        CHECK(l.Layer == Wardrobe::kDefaultLayer && l.Hides);
        Wardrobe::Item f;
        CHECK(Wardrobe::Classify(bare, "Female/Hair/SKM_F_Bob.fbx", {}, f) && f.Sex == Wardrobe::Gender::Male && f.Name == "SKM F Bob");
    }
    CHECK(Wardrobe::SkinBase(w, "assets/M/M_Body_Afro.mat") == "M_Body" && Wardrobe::SkinBase(w, "assets/M/M_Cap.mat").empty());

    std::vector<Wardrobe::Item> cat;
    auto add = [&](const std::string& rel, std::vector<std::string> mats = {}) {
        Wardrobe::Item it;
        const bool ok = Wardrobe::Classify(w, "assets/Chars/" + rel, mats, it);
        if (ok) cat.push_back(it);
        return ok;
    };
    CHECK(add("Clothing/Male/Pants/SKM_Jeans.fbx"));
    CHECK(add("Clothing/Male/Pants/SKM_Jeans_Inboots.fbx"));
    CHECK(add("Clothing/Male/Pants/SKM_Shorts.fbx"));
    CHECK(add("Clothing/Male/Shoes/SKM_Boots.fbx"));
    CHECK(add("Clothing/Male/Shoes/SKM_Sneakers.fbx"));
    CHECK(add("Clothing/Male/Shoes/SKM_Flip_Flops.fbx", {"assets/M/M_Body.mat"}));
    CHECK(add("Clothing/Male/Hats/SKM_Cap.fbx"));
    CHECK(add("Clothing/Male/Hats/WithHair/SKM_Cap_Hair.fbx"));
    CHECK(add("Hair/SKM_Bobcut.fbx"));
    CHECK(add("Hair/SKM_Bobcut_Cap.fbx"));
    CHECK(add("Clothing/Male/Tops/SKM_Tshirt.fbx"));
    CHECK(add("Clothing/Male/Outerwear/SKM_Jacket_Shirt.fbx"));
    CHECK(add("Clothing/Female/Tops/SKM_F_Tshirt.fbx"));
    CHECK(add("Clothing/Male/Accessories/SKM_Watch_L.fbx"));
    CHECK(!add("Clothing/Male/Accessories/SKM_Watch_R.fbx"));      // no Wrist R slot
    CHECK(!add("Clothing/Male/Glasses/StaticMesh/SM_Glasses.fbx")); // excluded folder
    CHECK(!add("Clothing/Male/Tops/SKM_Secret.fbx"));              // hidden override
    CHECK(cat.back().Slot == "Wrist L");
    CHECK(cat[7].Slot == "Hat" && cat[12].Sex == Wardrobe::Gender::Female && cat[5].Skin && !cat[4].Skin);
    Wardrobe::MarkVariants(w, cat);
    CHECK(cat[1].Variant && !cat[0].Variant && cat[9].Variant && !cat[8].Variant); // Jeans_Inboots, Bobcut_Cap

    auto pieceFor = [](const Wardrobe::Resolved& r, const std::string& slot) {
        for (const auto& p : r.Pieces) if (p.Slot == slot) return p.Path;
        return std::string();
    };
    Wardrobe::Request req;
    req.Race = "European";
    req.Items = {{"Pants", "assets/Chars/Clothing/Male/Pants/SKM_Jeans.fbx"}, {"Shoes", "assets/Chars/Clothing/Male/Shoes/SKM_Boots.fbx"},
                 {"Hair", "assets/Chars/Hair/SKM_Bobcut.fbx"}, {"Hat", "assets/Chars/Clothing/Male/Hats/SKM_Cap.fbx"},
                 {"Top", "assets/Chars/Clothing/Male/Tops/SKM_Tshirt.fbx"}};
    Wardrobe::Resolved r = Wardrobe::Resolve(w, cat, req);
    CHECK(Wardrobe::Stem(pieceFor(r, "Pants")) == "SKM_Jeans_Inboots");  // boots -> the pants cut for them
    CHECK(Wardrobe::Stem(pieceFor(r, "Hair")) == "SKM_Bobcut_Cap");      // a cap -> the haircut fitted to it
    CHECK(pieceFor(r, "Legs").empty());                                   // pants cover the legs
    CHECK(Wardrobe::Stem(pieceFor(r, "Feet")) == "Feet_Shoes");           // shoes -> feet cut for shoes
    CHECK(Wardrobe::Stem(pieceFor(r, "Head")) == "Head" && !pieceFor(r, "Top").empty());
    // And back: sneakers take the plain jeans, no hat the plain cut; a WithHair cap clears the hair.
    req.Items["Pants"] = "assets/Chars/Clothing/Male/Pants/SKM_Jeans_Inboots.fbx";
    req.Items["Shoes"] = "assets/Chars/Clothing/Male/Shoes/SKM_Sneakers.fbx";
    req.Items["Hair"] = "assets/Chars/Hair/SKM_Bobcut_Cap.fbx";
    req.Items["Hat"] = "assets/Chars/Clothing/Male/Hats/WithHair/SKM_Cap_Hair.fbx";
    req.Items["Outerwear"] = "assets/Chars/Clothing/Male/Outerwear/SKM_Jacket_Shirt.fbx";
    r = Wardrobe::Resolve(w, cat, req);
    CHECK(Wardrobe::Stem(pieceFor(r, "Pants")) == "SKM_Jeans");
    CHECK(pieceFor(r, "Hair").empty() && pieceFor(r, "Top").empty() && !pieceFor(r, "Outerwear").empty());
    req.Items.erase("Hat");
    r = Wardrobe::Resolve(w, cat, req);
    CHECK(Wardrobe::Stem(pieceFor(r, "Hair")) == "SKM_Bobcut");          // no hat: the plain cut again
    // Flip-flops carry skin: the feet stay; shorts leave the legs; the Afro race brings its torso and head.
    req.Items = {{"Shoes", "assets/Chars/Clothing/Male/Shoes/SKM_Flip_Flops.fbx"}, {"Pants", "assets/Chars/Clothing/Male/Pants/SKM_Shorts.fbx"}};
    req.Race = "Afro";
    r = Wardrobe::Resolve(w, cat, req);
    CHECK(Wardrobe::Stem(pieceFor(r, "Feet")) == "Feet" && !pieceFor(r, "Legs").empty());
    CHECK(Wardrobe::Stem(pieceFor(r, "Torso")) == "Torso_Afro" && Wardrobe::Stem(pieceFor(r, "Head")) == "Head_Afro");
    // A female request ignores male items.
    req.Sex = Wardrobe::Gender::Female;
    req.Items = {{"Top", "assets/Chars/Clothing/Male/Tops/SKM_Tshirt.fbx"}};
    r = Wardrobe::Resolve(w, cat, req);
    CHECK(pieceFor(r, "Top").empty() && Wardrobe::Stem(pieceFor(r, "Torso")) == "Body");
    // A long sleeve takes a modular body's arms off and leaves its hands; a T-shirt keeps both. The rule is the male
    // body's: the female one's arms carry their hands.
    CHECK(add("Clothing/Male/Tops/SKM_Hoodie.fbx") && add("Clothing/Female/Tops/SKM_F_Hoodie.fbx"));
    req = Wardrobe::Request{};
    req.Race = "European";
    req.Items = {{"Top", "assets/Chars/Clothing/Male/Tops/SKM_Hoodie.fbx"}};
    r = Wardrobe::Resolve(w, cat, req);
    CHECK(pieceFor(r, "Arms").empty() && Wardrobe::Stem(pieceFor(r, "Hands")) == "Hands");
    req.Items = {{"Top", "assets/Chars/Clothing/Male/Tops/SKM_Tshirt.fbx"}};
    r = Wardrobe::Resolve(w, cat, req);
    CHECK(!pieceFor(r, "Arms").empty() && !pieceFor(r, "Hands").empty());
    req.Sex = Wardrobe::Gender::Female;
    req.Items = {{"Top", "assets/Chars/Clothing/Female/Tops/SKM_F_Hoodie.fbx"}};
    r = Wardrobe::Resolve(w, cat, req);
    CHECK(!pieceFor(r, "Top").empty() && Wardrobe::Stem(pieceFor(r, "Arms")) == "Arms_Full");

    // Skin: the race's variant when it exists, else the base.
    const Wardrobe::RaceDef& afro = w.Bodies[0].Races[1];
    CHECK(Wardrobe::SkinMaterialFor(w, afro, "assets/M/M_Body.mat", [](const std::string&) { return true; }) == "assets/M/M_Body_Afro.mat");
    CHECK(Wardrobe::SkinMaterialFor(w, afro, "assets/M/M_Head.mat", [](const std::string&) { return false; }) == "assets/M/M_Head.mat");
    CHECK(Wardrobe::SkinMaterialFor(w, w.Bodies[0].Races[0], "assets/M/M_Body_Afro.mat", nullptr) == "assets/M/M_Body.mat");
    CHECK(Wardrobe::SkinMaterialFor(w, afro, "assets/M/M_Cap.mat", nullptr) == "assets/M/M_Cap.mat");

    const auto colours = Wardrobe::Colourways("a/M_Hoodie_Blue.mat", {"a/M_Hoodie_Red.mat", "a/readme.txt", "a/M_Hoodie_Black.mat"});
    CHECK(colours.size() == 3 && Wardrobe::Stem(colours[0]) == "M_Hoodie_Black");

    // Layers: each hides what pokes through it from the layers under it - and nothing else.
    Wardrobe::Wardrobe lw;
    CHECK(Wardrobe::Parse(R"({"slots": [{"id": "Top", "layer": 4}, {"id": "Outerwear", "layer": 6}, {"id": "Pants", "layer": 3},
                                        {"id": "Shoes", "layer": 2}, {"id": "Hair", "headAttached": true, "layer": 9, "hides": false},
                                        {"id": "Bag", "layer": 1, "hides": false}],
                              "bodies": {"Male": {"parts": [{"part": "Torso", "model": "T.fbx"}]}},
                              "layers": [{"slot": "Top", "nameHasAny": ["Tucked"], "layer": 2},
                                         {"slot": "Shoes", "nameHasAny": ["Boots"], "layer": 5},
                                         {"slot": "Top", "nameHasAny": ["Hood_Up"], "over": ["Hair"]}]})", lw, &err));
    auto layer = [&](const char* slot, const char* item, bool body = false) { return Wardrobe::LayerOf(lw, slot, item, body); };
    const auto head = layer("Head", "a/Head.fbx", true), hair = layer("Hair", "a/SKM_Hair_Long.fbx");
    const auto tee = layer("Top", "a/SKM_Tshirt.fbx"), tucked = layer("Top", "a/SKM_Tshirt_Tucked.fbx");
    const auto hoodUp = layer("Top", "a/SKM_Hoodie_Hood_Up.fbx"), jacket = layer("Outerwear", "a/SKM_Jacket.fbx");
    const auto jeans = layer("Pants", "a/SKM_Jeans.fbx"), sneakers = layer("Shoes", "a/SKM_Sneakers.fbx"), boots = layer("Shoes", "a/SKM_Boots.fbx");
    CHECK(Wardrobe::Hides(jacket, "Top", tee) && !Wardrobe::Hides(tee, "Outerwear", jacket)); // the shirt under the jacket
    CHECK(Wardrobe::Hides(tee, "Head", head) && !Wardrobe::Hides(head, "Top", tee));          // the body under everything
    CHECK(Wardrobe::Hides(hoodUp, "Hair", hair) && !Wardrobe::Hides(tee, "Hair", hair));      // a hood that's up takes the hair
    CHECK(!Wardrobe::Hides(hair, "Head", head));                                              // hair cards hide nothing
    CHECK(Wardrobe::Hides(tee, "Pants", jeans) && Wardrobe::Hides(jeans, "Top", tucked));     // tucked or not
    CHECK(Wardrobe::Hides(jeans, "Shoes", sneakers) && Wardrobe::Hides(boots, "Pants", jeans)); // hems over shoes, into boots
    CHECK(!Wardrobe::Hides(layer("Bag", "a/SKM_Bag.fbx"), "Torso", head) && !Wardrobe::Hides(tee, "Top", tee));

    // Essential slots come from the styles: filled by every style open to the gender (a gender-only style counts
    // only for that gender); none without styles.
    Wardrobe::Wardrobe ew;
    CHECK(Wardrobe::Parse(R"({"slots": [{"id": "Top"}, {"id": "Pants"}, {"id": "Hat"}, {"id": "Tie"}],
                              "bodies": {"Male": {"parts": [{"part": "Torso", "model": "T.fbx"}]}},
                              "styles": [{"name": "A", "fill": {"Top": 1.0, "Pants": 1.0, "Hat": 0.5}},
                                         {"name": "B", "defaultFill": 1.0, "fill": {"Hat": 0.2}},
                                         {"name": "Suit", "gender": "Male", "fill": {"Top": 1.0, "Pants": 1.0, "Tie": 1.0}}]})",
                          ew, &err));
    const auto M = Wardrobe::Gender::Male, F = Wardrobe::Gender::Female;
    CHECK(Wardrobe::Essential(ew, "Top", M) && Wardrobe::Essential(ew, "Pants", F) && !Wardrobe::Essential(ew, "Hat", F));
    CHECK(!Wardrobe::Essential(ew, "Tie", M) && !Wardrobe::Essential(ew, "Tie", F)); // style A never fills it
    CHECK(!Wardrobe::Essential(lw, "Top", M));                                          // no styles: nothing is

    // Compatibility rules: tags from names, materials and earlier tags; hard and soft excludes; styles.
    Wardrobe::Wardrobe cw;
    CHECK(Wardrobe::Parse(R"({"slots": [{"id": "Top", "folders": ["Tops"]}, {"id": "Outerwear", "folders": ["Outerwear"]},
                                        {"id": "Shoes", "folders": ["Shoes"]}],
                              "bodies": {"Male": {"parts": [{"part": "Torso", "model": "T.fbx"}]}},
                              "tags": [{"slot": "Outerwear", "materialHasAny": ["Shirt"], "add": ["BuiltInTop"]},
                                       {"slot": "Outerwear", "lacksTags": ["BuiltInTop"], "add": ["Open"]},
                                       {"slot": "Top", "nameHasAny": ["Hoodie"], "add": ["Thick", "Sport"]},
                                       {"slot": "Shoes", "nameHasAny": ["Flip"], "add": ["Summer"]}],
                              "clears": [{"slot": "Outerwear", "tags": ["BuiltInTop"], "clear": "Top"}],
                              "excludes": [{"a": {"tags": ["Open"]}, "b": {"tags": ["Thick"]}},
                                           {"soft": true, "a": {"slot": "Outerwear"}, "b": {"slot": "Shoes", "nameHasAny": ["Flip"]}}],
                              "styles": [{"name": "Sport", "fill": {"Top": 1.0}}, {"name": "Summer", "fill": {"Shoes": 1.0}}]})", cw, &err));
    std::vector<Wardrobe::Item> ci;
    auto addc = [&](const std::string& rel, std::vector<std::string> mats = {}) {
        Wardrobe::Item it;
        CHECK(Wardrobe::Classify(cw, rel, mats, it));
        ci.push_back(it);
    };
    addc("Tops/SKM_Hoodie.fbx");
    addc("Tops/SKM_Tee.fbx");
    addc("Outerwear/SKM_Jacket.fbx", {"m/M_Jacket.mat", "m/M_TShirt_Black.mat"});
    addc("Outerwear/SKM_Bomber.fbx", {"m/M_Bomber.mat"});
    addc("Shoes/SKM_Flip_Flops.fbx");
    CHECK(Wardrobe::HasTag(ci[2], "BuiltInTop") && !Wardrobe::HasTag(ci[2], "Open") && Wardrobe::HasTag(ci[3], "Open"));
    CHECK(Wardrobe::Conflicts(cw, ci[3], ci[0]) && !Wardrobe::Conflicts(cw, ci[3], ci[1])); // open jacket: no hoodie
    CHECK(Wardrobe::Conflicts(cw, ci[2], ci[1]));                                            // it clears the top
    CHECK(Wardrobe::Conflicts(cw, ci[3], ci[4]) && !Wardrobe::Conflicts(cw, ci[3], ci[4], false)); // soft
    CHECK(Wardrobe::InStyle(cw, ci[0], cw.Styles[0]) && !Wardrobe::InStyle(cw, ci[4], cw.Styles[0]) &&
          Wardrobe::InStyle(cw, ci[1], cw.Styles[1])); // untagged fits any style
    Wardrobe::Request cr;
    cr.Items = {{"Outerwear", ci[3].Path}, {"Top", ci[0].Path}, {"Shoes", ci[4].Path}};
    const auto cres = Wardrobe::Resolve(cw, ci, cr);
    CHECK(cres.Dropped == std::vector<std::string>{"Top"} && cres.Clashes.size() == 1);

    const Wardrobe::Diff d = Wardrobe::MakeDiff({{"Torso", "a/T.fbx"}, {"Hat", "a/Cap.fbx"}, {"Hair", "a/H.fbx"}},
                                                {{"Torso", "a/T.fbx"}, {"Hair", "a/H2.fbx"}, {"Top", "a/Top.fbx"}});
    CHECK(d.Create.size() == 1 && d.Create[0].Slot == "Top" && d.Remodel.size() == 1 && d.Remodel[0].Slot == "Hair");
    CHECK(d.Destroy.size() == 1 && d.Destroy[0] == "Hat");
}

// Skin hiding: a sphere of skin under a cap of cloth - the covered half is found, the uncovered half
// isn't, erosion opens the edge ring, and the bits pack 32 to a word.
void TestOutfitCoverage() {
    auto sphere = [](float r, bool upperOnly) {
        OutfitCoverage::Mesh m;
        const int rings = 16, segs = 24;
        for (int i = 0; i <= rings; ++i)
            for (int j = 0; j < segs; ++j) {
                const float th = glm::pi<float>() * (float)i / rings, ph = glm::two_pi<float>() * (float)j / segs;
                m.Positions.push_back(r * glm::vec3(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)));
            }
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < segs; ++j) {
                const unsigned a = (unsigned)(i * segs + j), b = (unsigned)(i * segs + (j + 1) % segs);
                const unsigned c = a + segs, d = b + segs;
                if (upperOnly && i >= rings / 2) continue;
                // Outward-facing winding (the normals point away from the centre).
                m.Indices.insert(m.Indices.end(), {a, b, c, b, d, c});
            }
        return m;
    };
    const OutfitCoverage::Mesh skin = sphere(0.10f, false), cap = sphere(0.12f, true);
    const auto normals = OutfitCoverage::VertexNormals(skin);
    CHECK(glm::dot(normals[(size_t)(4 * 24)], glm::normalize(skin.Positions[(size_t)(4 * 24)])) > 0.9f);
    std::vector<std::uint8_t> covered = OutfitCoverage::Covered(skin, cap);
    int upper = 0, upperCovered = 0, lower = 0, lowerCovered = 0;
    for (size_t v = 0; v < skin.Positions.size(); ++v) {
        const float y = skin.Positions[v].y;
        if (y > 0.03f) { ++upper; upperCovered += covered[v]; }
        if (y < -0.03f) { ++lower; lowerCovered += covered[v]; }
    }
    CHECK(upper > 0 && upperCovered == upper);
    CHECK(lower > 0 && lowerCovered == 0);
    // Cloth 1.5 cm inside the skin (skin poking through) still counts; 10 cm away doesn't.
    CHECK(OutfitCoverage::Covered(skin, sphere(0.085f, true))[(size_t)(2 * 24)] == 1);
    CHECK(OutfitCoverage::Covered(skin, sphere(0.075f, true))[(size_t)(2 * 24)] == 1); // a shirt 2.5 cm out through a jacket
    CHECK(OutfitCoverage::Covered(skin, sphere(0.06f, true))[(size_t)(2 * 24)] == 0);
    CHECK(OutfitCoverage::Covered(skin, sphere(0.20f, true))[(size_t)(2 * 24)] == 0);
    // The head's rule (Backed): a neck in a collar standing 3 cm off it. Just under the rim the skin can be
    // seen over it with nothing behind (the torso's hidden too): hiding it would open a hole, so it stays.
    // Deep in a tube reaching well past it, nothing sees it. Poking through a shirt, the shirt's behind.
    auto tube = [](float r, float y0, float y1) {
        OutfitCoverage::Mesh m;
        const int rings = 20, segs = 24;
        for (int i = 0; i <= rings; ++i)
            for (int j = 0; j < segs; ++j) {
                const float ph = glm::two_pi<float>() * (float)j / segs;
                m.Positions.emplace_back(r * std::cos(ph), y0 + (y1 - y0) * (float)i / rings, r * std::sin(ph));
            }
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < segs; ++j) {
                const unsigned a = (unsigned)(i * segs + j), b = (unsigned)(i * segs + (j + 1) % segs);
                const unsigned c = a + segs, d = b + segs;
                m.Indices.insert(m.Indices.end(), {a, c, b, b, c, d}); // outward-facing
            }
        return m;
    };
    const OutfitCoverage::Mesh neck = tube(0.06f, 0.0f, 0.20f);
    CHECK(glm::dot(OutfitCoverage::VertexNormals(neck)[(size_t)(8 * 24)], glm::vec3(1.0f, 0.0f, 0.0f)) > 0.9f);
    const size_t underRim = (size_t)(8 * 24); // y = 8 cm, facing +X
    const OutfitCoverage::Mesh collar = tube(0.09f, -0.30f, 0.10f), wrap = tube(0.09f, -0.30f, 0.50f);
    // 2 cm under the rim is inside the edge band (kEdgeBand): drawn, so a collar swinging off it opens no hole.
    // 6 cm under it, the band is past and it's hidden.
    CHECK(OutfitCoverage::Hidden(neck, collar)[underRim] == 0);
    CHECK(OutfitCoverage::Hidden(neck, collar)[(size_t)(4 * 24)] == 1);
    CHECK(OutfitCoverage::Hidden(neck, collar, true)[underRim] == 0); // the head's keeps it
    CHECK(OutfitCoverage::Hidden(neck, wrap, true)[underRim] == 1);
    // Skin poking 5 mm through a shirt front: from every way it's seen, the shirt's right behind it.
    auto sheet = [](float half, float z) {
        OutfitCoverage::Mesh m;
        const int n = 20;
        for (int i = 0; i <= n; ++i)
            for (int j = 0; j <= n; ++j) m.Positions.emplace_back(-half + 2 * half * j / n, -half + 2 * half * i / n, z);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                const unsigned a = (unsigned)(i * (n + 1) + j), b = a + 1, c = a + n + 1, d = c + 1;
                m.Indices.insert(m.Indices.end(), {a, b, c, b, d, c}); // facing +Z
            }
        return m;
    };
    const OutfitCoverage::Mesh chest = sheet(0.05f, 0.0f), shirt = sheet(0.30f, -0.005f);
    const size_t middle = (size_t)(10 * 21 + 10);
    CHECK(OutfitCoverage::PokeDepth(chest, shirt, OutfitCoverage::kPokeReach)[middle] > 0.0f);
    CHECK(OutfitCoverage::Hidden(chest, shirt, true)[middle] == 1);
    const int before = (int)std::count(covered.begin(), covered.end(), (std::uint8_t)1);
    OutfitCoverage::Erode(skin, covered, 1);
    const int after = (int)std::count(covered.begin(), covered.end(), (std::uint8_t)1);
    CHECK(after < before && after > 0 && covered[0] == 1); // the pole stays, the hem ring opens
    const auto bits = OutfitCoverage::Pack({1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
    CHECK(bits.size() == 2 && bits[0] == 0x9u && bits[1] == 0x1u);
}

// The Quantum pack's wardrobe (project/assets/Characters/Quantum/Quantum.wardrobe) against the real
// files: every body part it names exists, each slot finds items, and the pack's pairs resolve. (Coverage
// on the real meshes needs them loaded - GL - so it's checked in the editor, not here.)
void TestWardrobeQuantum() {
    // The Quantum pack is project content, not engine data: a packaged engine (CI's standalone check)
    // runs without it, and then there's nothing to test. Its models are on the shared Drive, not in
    // git, so a clone has the wardrobe file but no meshes until they're fetched.
    if (!std::filesystem::exists(ProjectPaths::Resolve("assets/Characters/Quantum/Quantum.wardrobe")) ||
        !std::filesystem::exists(ProjectPaths::Resolve("assets/Characters/Quantum/Models/European/Quantum_Torso.fbx"))) {
        UnitTestSupport::Skip("no Quantum pack in this project");
        return;
    }
    AssetLibrary assets;
    std::string err;
    const auto cat = OutfitSystem::LoadCatalog(assets, "assets/Characters/Quantum/Quantum.wardrobe", true, &err);
    CHECK(cat != nullptr);
    if (!cat) { Log::Warn("Quantum wardrobe: " + err); return; }
    // Its styles make tops, pants and shoes the essentials (and Socks, which has no items), as SetGender used to hard-code.
    for (int g = 0; g < 2; ++g)
        for (const auto& slot : cat->W.Slots) {
            const bool want = slot.Id == "Top" || slot.Id == "Pants" || slot.Id == "Shoes" || slot.Id == "Socks";
            CHECK(Wardrobe::Essential(cat->W, slot.Id, (Wardrobe::Gender)g) == want);
        }
    CHECK(cat->Items.size() > 80);
    auto exists = [](const std::string& rel) { return std::filesystem::exists(ProjectPaths::Resolve(rel)); };
    for (const auto& body : cat->W.Bodies) {
        for (const auto& [part, model] : body.Parts) CHECK(exists(model));
        for (const auto& race : body.Races) {
            CHECK(exists(race.Head));
            for (const auto& [part, model] : race.Parts) CHECK(exists(model));
        }
        for (const auto& [name, model] : body.Alternates) CHECK(exists(model));
    }
    for (const auto& slot : cat->W.Slots)
        for (int g = 0; g < 2; ++g) {
            const auto n = cat->ForSlot(slot.Id, (Wardrobe::Gender)g).size();
            Log::Info("Quantum wardrobe: " + slot.Id + " " + Wardrobe::GenderName((Wardrobe::Gender)g) + " " + std::to_string(n));
            CHECK(n > 0 || (slot.Id == "FacialHair" && g == (int)Wardrobe::Gender::Female)); // beards: the male body's only
        }
    CHECK(cat->ForSlot("FacialHair", Wardrobe::Gender::Female).empty());
    // Every model under the clothing folders is offered in a slot - or hidden on purpose (an item override).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        size_t models = 0;
        const fs::path clothing = ProjectPaths::Resolve("assets/Characters/Quantum/Models/Clothing");
        for (auto it = fs::recursive_directory_iterator(clothing, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::string ext = it->path().extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            if (!it->is_regular_file(ec) || ext != ".fbx") continue;
            const std::string full = it->path().generic_string();
            const size_t at = full.rfind("assets/Characters/Quantum/");
            if (at == std::string::npos) continue;
            const std::string rel = full.substr(at);
            ++models;
            auto lower = [](std::string s) {
                for (char& c : s) c = (char)std::tolower((unsigned char)c);
                return s;
            };
            const bool hidden = std::any_of(cat->W.Items.begin(), cat->W.Items.end(), [&](const Wardrobe::ItemOverride& o) {
                return o.Hidden && lower(o.Path) == lower(rel);
            });
            if (!cat->Find(rel) && !hidden) Log::Warn("Quantum wardrobe: no slot for " + rel);
            CHECK(cat->Find(rel) != nullptr || hidden);
        }
        Log::Info("Quantum wardrobe: " + std::to_string(models) + " clothing models, " + std::to_string(cat->Items.size()) + " offered");
        CHECK(models > 150);
    }
    auto item = [&](const std::string& rel) {
        const auto* it = cat->Find("assets/Characters/Quantum/Models/" + rel);
        CHECK(it != nullptr);
        return it ? it->Path : std::string();
    };
    auto pieceFor = [](const Wardrobe::Resolved& r, const std::string& slot) {
        for (const auto& p : r.Pieces) if (p.Slot == slot) return Wardrobe::Stem(p.Path);
        return std::string();
    };
    Wardrobe::Request req;
    req.Race = "Afro";
    req.Items = {{"Pants", item("Clothing/Male/Pants/SKM_Jeans.fbx")}, {"Shoes", item("Clothing/Male/Shoes/SKM_Boots.fbx")},
                 {"Top", item("Clothing/Male/Tops/SKM_Tshirt.fbx")}, {"Balaclava", item("Clothing/Male/Balaclava/SM_Balaclava_Crime.fbx")}};
    Wardrobe::Resolved r = Wardrobe::Resolve(cat->W, cat->Items, req);
    CHECK(pieceFor(r, "Pants") == "SKM_Jeans_Inboots" && pieceFor(r, "Feet") == "Quantum_Feet_Shoes_Afro");
    CHECK(pieceFor(r, "Legs").empty() && pieceFor(r, "Head") == "Quantum_Head_Afro" && pieceFor(r, "Torso") == "Quantum_Torso_Afro");
    CHECK(pieceFor(r, "Balaclava") == "SM_Balaclava_Crime");
    // On the head: the balaclava, hoods, the black (classic) glasses and the pack's hair, beards, hats and headphones -
    // each riding the head, so first person draws it only in shadow (FirstPersonBody). The aviators are in the
    // catalog but don't go with the balaclava (they clip it); the classic glasses do.
    req = {};
    req.Sex = Wardrobe::Gender::Female;
    req.Items = {{"Balaclava", item("Clothing/Female/Balaclava/SM_F_Balaclava_Crime.fbx")},
                 {"Glasses", item("Clothing/Female/Glasses/SKM_F_Glasses_Classic.fbx")}};
    r = Wardrobe::Resolve(cat->W, cat->Items, req);
    CHECK(pieceFor(r, "Balaclava") == "SM_F_Balaclava_Crime" && pieceFor(r, "Glasses") == "SKM_F_Glasses_Classic");
    CHECK(pieceFor(r, "Torso") == "SKM_F_Vivian_Body");
    CHECK(cat->Find("assets/Characters/Quantum/Models/Clothing/Male/Hair/SKM_Hair_Short.fbx") != nullptr);
    CHECK(cat->W.HatSlot == "Headwear" && cat->W.HairSlot == "Hair");
    for (const char* s : {"Hair", "FacialHair", "Headwear", "Headphones", "Balaclava", "Glasses"})
        CHECK(cat->W.Slot(s) && cat->W.Slot(s)->HeadAttached);
    CHECK(cat->Find("assets/Characters/Quantum/Models/Clothing/Female/Glasses/SKM_F_Glasses_Aviator.fbx") != nullptr);
    req.Items["Glasses"] = item("Clothing/Female/Glasses/SKM_F_Glasses_Aviator.fbx");
    r = Wardrobe::Resolve(cat->W, cat->Items, req);
    CHECK(pieceFor(r, "Balaclava") == "SM_F_Balaclava_Crime" && pieceFor(r, "Glasses").empty() && r.Dropped.size() == 1);
    // A hood that's up goes over the balaclava, and only that way round (else each cuts holes in the other).
    for (const char* hood : {"Tops/SKM_Hoodie_Hood_Up", "Tops/SKM_Hoodie_Zipper_Hood", "Outerwear/SKM_Coat_Hoodie_Hood"}) {
        const std::string slot = std::string(hood).rfind("Tops", 0) == 0 ? "Top" : "Outerwear";
        const auto h = Wardrobe::LayerOf(cat->W, slot, item(std::string("Clothing/Male/") + hood + ".fbx"), false);
        const auto b = Wardrobe::LayerOf(cat->W, "Balaclava", item("Clothing/Male/Balaclava/SM_Balaclava_Crime.fbx"), false);
        CHECK(Wardrobe::Hides(h, "Balaclava", b, slot) && !Wardrobe::Hides(b, slot, h, "Balaclava"));
    }

    // What goes together: jackets with a top of their own take the top off, an open one keeps a thin top
    // but not a hoodie, boots cut to go under the pants keep the plain pants, socks are their own slot.
    auto male = [&](std::map<std::string, std::string> items) {
        Wardrobe::Request m;
        m.Items = std::move(items);
        return Wardrobe::Resolve(cat->W, cat->Items, m);
    };
    r = male({{"Outerwear", item("Clothing/Male/Outerwear/SKM_Leather_Jacket.fbx")}, {"Top", item("Clothing/Male/Tops/SKM_Hoodie.fbx")}});
    CHECK(pieceFor(r, "Top").empty() && r.Dropped.size() == 1);
    auto female = [&](std::map<std::string, std::string> items) {
        Wardrobe::Request m;
        m.Sex = Wardrobe::Gender::Female;
        m.Items = std::move(items);
        return Wardrobe::Resolve(cat->W, cat->Items, m);
    };
    r = female({{"Outerwear", item("Clothing/Female/Outerwear/SKM_F_Bomber.fbx")}, {"Top", item("Clothing/Female/Tops/SKM_F_Hoodie.fbx")}});
    CHECK(pieceFor(r, "Top").empty() && !pieceFor(r, "Outerwear").empty());
    r = female({{"Outerwear", item("Clothing/Female/Outerwear/SKM_F_Bomber.fbx")}, {"Top", item("Clothing/Female/Tops/SKM_F_Tshirt_Tucked.fbx")}});
    CHECK(pieceFor(r, "Top") == "SKM_F_Tshirt_Tucked" && r.Dropped.empty());
    r = male({{"Shoes", item("Clothing/Male/Shoes/SKM_Boots_Inboots.fbx")}, {"Pants", item("Clothing/Male/Pants/SKM_Jeans.fbx")},
              {"Socks", item("Clothing/Male/Shoes/SKM_Socks.fbx")}});
    CHECK(pieceFor(r, "Pants") == "SKM_Jeans" && pieceFor(r, "Socks") == "SKM_Socks" && pieceFor(r, "Shoes") == "SKM_Boots_Inboots");
    // A hood that's up: the balaclava stays under it (the hood's layered over it).
    r = male({{"Top", item("Clothing/Male/Tops/SKM_Hoodie_Hood_Up.fbx")}, {"Balaclava", item("Clothing/Male/Balaclava/SM_Balaclava_Crime.fbx")}});
    CHECK(pieceFor(r, "Balaclava") == "SM_Balaclava_Crime" && pieceFor(r, "Top") == "SKM_Hoodie_Hood_Up");
    r = male({{"Outerwear", item("Clothing/Male/Outerwear/SKM_Jacket_Classic_Tie.fbx")},
              {"Shoes", item("Clothing/Male/Shoes/SKM_Flip_Flops.fbx")}});
    CHECK(!pieceFor(r, "Shoes").empty() && r.Clashes.size() == 1); // a style clash warns, it doesn't undress

    // Headwear and hair. A hat takes the cut the pack fitted to it (a static hat too); that cut carries the hat itself,
    // so the hat piece goes - and the outfit read back from its pieces (no hat) keeps the cut.
    const std::string FH = "Clothing/Female/Hair/", FW = "Clothing/Female/Headwear/", MH = "Clothing/Male/Hair/",
                      MW = "Clothing/Male/Headwear/";
    r = female({{"Hair", item(FH + "SKM_F_Haircut_Bobcut.fbx")}, {"Headwear", item(FW + "SKM_F_Cap.fbx")}});
    CHECK(pieceFor(r, "Hair") == "SKM_F_Haircut_Bobcut_Cap" && pieceFor(r, "Headwear").empty() && r.Dropped.empty());
    r = female({{"Hair", item(FH + "SKM_F_Haircut_Bobcut_Cap.fbx")}});
    CHECK(pieceFor(r, "Hair") == "SKM_F_Haircut_Bobcut_Cap");
    r = female({{"Hair", item(FH + "SKM_F_Haircut_Bobcut_Cap.fbx")}, {"Headwear", item(FW + "SKM_F_Panama.fbx")}});
    CHECK(pieceFor(r, "Hair") == "SKM_F_Haircut_Bobcut_Panama" && pieceFor(r, "Headwear").empty());
    r = female({{"Hair", item(FH + "SKM_F_Haircut_Ponytail.fbx")}, {"Headwear", item(FW + "SM_F_Cap.fbx")}});
    CHECK(pieceFor(r, "Hair") == "SKM_F_Haircut_Ponytail_Cap" && pieceFor(r, "Headwear").empty());
    for (const char* cut : {"SKM_F_Haircut_Bobcut_Cap", "SKM_F_Haircut_Bobcut_Hat_Warm", "SKM_F_Haircut_Bobcut_Panama", "SKM_F_Haircut_Ponytail_Cap"}) {
        const auto* it = cat->Find("assets/Characters/Quantum/Models/" + FH + cut + ".fbx");
        CHECK(it && it->Variant); // the rules pick it: not offered on its own
    }
    // No cut fitted to the hat: the hair stays under it, and the hat hides what of the hair pokes through.
    r = female({{"Hair", item(FH + "SKM_F_Haircut_Half_Up.fbx")}, {"Headwear", item(FW + "SKM_F_Hat_Warm.fbx")}});
    CHECK(pieceFor(r, "Hair") == "SKM_F_Haircut_Half_Up" && pieceFor(r, "Headwear") == "SKM_F_Hat_Warm");
    {
        const auto hat = Wardrobe::LayerOf(cat->W, "Headwear", item(FW + "SKM_F_Hat_Warm.fbx"), false);
        const auto hair = Wardrobe::LayerOf(cat->W, "Hair", item(FH + "SKM_F_Haircut_Half_Up.fbx"), false);
        CHECK(Wardrobe::Hides(hat, "Hair", hair, "Headwear") && !Wardrobe::Hides(hair, "Headwear", hat, "Hair"));
    }
    // A cut with the hat or the glasses in it takes those slots' pieces off.
    r = female({{"Hair", item(FH + "SKM_F_Haircut_Ponytail_Cap_Glasses.fbx")}, {"Headwear", item(FW + "SKM_F_Hat_Up.fbx")},
                {"Glasses", item("Clothing/Female/Glasses/SKM_F_Glasses_Classic.fbx")}});
    CHECK(pieceFor(r, "Hair") == "SKM_F_Haircut_Ponytail_Cap_Glasses" && pieceFor(r, "Headwear").empty() && pieceFor(r, "Glasses").empty());
    // Headwear with hair of its own takes the hair off; a crest doesn't go under a cap; a beard isn't touched.
    r = male({{"Hair", item(MH + "SKM_Hair_Long.fbx")}, {"Headwear", item(MW + "SKM_Cap_Hair.fbx")},
              {"FacialHair", item("Clothing/Male/FacialHair/SKM_Beard.fbx")}});
    CHECK(pieceFor(r, "Hair").empty() && pieceFor(r, "Headwear") == "SKM_Cap_Hair" && pieceFor(r, "FacialHair") == "SKM_Beard");
    r = female({{"Hair", item(FH + "SKM_F_Haircut_Braids.fbx")}, {"Headwear", item(FW + "SKM_F_Hat_Warm_Braids.fbx")}});
    CHECK(pieceFor(r, "Hair").empty() && pieceFor(r, "Headwear") == "SKM_F_Hat_Warm_Braids");
    r = male({{"Hair", item(MH + "SKM_Hair_Mohawk.fbx")}, {"Headwear", item(MW + "SKM_Cap.fbx")}});
    CHECK(pieceFor(r, "Hair").empty() && pieceFor(r, "Headwear") == "SKM_Cap");
    // A hood that's up (the winter jackets' too) takes the hair, a big hat and headphones off; a cap stays, under it.
    r = male({{"Outerwear", item("Clothing/Male/Outerwear/SKM_Jacket_Winter_Closed_Hood_Closed.fbx")}, {"Hair", item(MH + "SKM_Hair_Short.fbx")},
              {"Headwear", item(MW + "SKM_Panama.fbx")}, {"Headphones", item(MW + "SKM_Headphones.fbx")}});
    CHECK(pieceFor(r, "Hair").empty() && pieceFor(r, "Headwear").empty() && pieceFor(r, "Headphones").empty() &&
          pieceFor(r, "Outerwear") == "SKM_Jacket_Winter_Closed_Hood_Closed");
    r = female({{"Outerwear", item("Clothing/Female/Outerwear/SKM_F_Jacket_Winter_Open_Hood_Opened.fbx")}, {"Headwear", item(FW + "SKM_F_Cap.fbx")}});
    CHECK(pieceFor(r, "Headwear") == "SKM_F_Cap");
    {
        const auto hood = Wardrobe::LayerOf(cat->W, "Outerwear", item("Clothing/Female/Outerwear/SKM_F_Jacket_Winter_Open_Hood_Opened.fbx"), false);
        const auto cap = Wardrobe::LayerOf(cat->W, "Headwear", item(FW + "SKM_F_Cap.fbx"), false);
        CHECK(Wardrobe::Hides(hood, "Headwear", cap, "Outerwear") && !Wardrobe::Hides(cap, "Outerwear", hood, "Headwear"));
    }
    // The balaclava takes hair, beards, hats and headphones off.
    r = male({{"Balaclava", item("Clothing/Male/Balaclava/SM_Balaclava_Crime.fbx")}, {"Hair", item(MH + "SKM_Hair_Short.fbx")},
              {"FacialHair", item("Clothing/Male/FacialHair/SKM_Beard.fbx")}, {"Headwear", item(MW + "SKM_Warm_Hat.fbx")}});
    CHECK(pieceFor(r, "Balaclava") == "SM_Balaclava_Crime" && pieceFor(r, "Hair").empty() && pieceFor(r, "FacialHair").empty() &&
          pieceFor(r, "Headwear").empty());
    // A backpack goes over the outerwear; the suit jacket brings its own shirt and tie; a shirt and tie's long sleeves
    // take the arms (the hands stay), a rolled-up shirt's don't.
    {
        const auto bag = Wardrobe::LayerOf(cat->W, "Bag", item("Clothing/Male/Bags/SKM_Backpack_Casual.fbx"), false);
        const auto coat = Wardrobe::LayerOf(cat->W, "Outerwear", item("Clothing/Male/Outerwear/SKM_Puffer_Jacket.fbx"), false);
        CHECK(Wardrobe::Hides(bag, "Outerwear", coat, "Bag") && !Wardrobe::Hides(coat, "Bag", bag, "Outerwear"));
    }
    r = male({{"Outerwear", item("Clothing/Male/Outerwear/SKM_Jacket_Classic_Close.fbx")}, {"Top", item("Clothing/Male/Tops/SKM_Shirt_Tie.fbx")}});
    CHECK(pieceFor(r, "Top").empty() && !pieceFor(r, "Outerwear").empty());
    r = male({{"Top", item("Clothing/Male/Tops/SKM_Shirt_Tie.fbx")}, {"Pants", item("Clothing/Male/Pants/SKM_Pants_Classic.fbx")}});
    CHECK(pieceFor(r, "Top") == "SKM_Shirt_Tie" && pieceFor(r, "Arms") == "Quantum_Hands" && r.Clashes.empty());
    r = male({{"Top", item("Clothing/Male/Tops/SKM_Shirt_Classic_RolledUp.fbx")}});
    CHECK(pieceFor(r, "Arms") == "Quantum_Arms_FP");
    r = male({{"Outerwear", item("Clothing/Male/Outerwear/SKM_Jacket_Tshirt.fbx")}});
    CHECK(pieceFor(r, "Arms") == "Quantum_Hands");

    // Races: the old man's own body parts (his legs are the European ones in his skin); tattoos and ornaments are
    // skins over the European body, kept out of Randomize.
    {
        Wardrobe::Request old;
        old.Race = "Old";
        r = Wardrobe::Resolve(cat->W, cat->Items, old);
        CHECK(pieceFor(r, "Torso") == "Quantum_Old_Torso" && pieceFor(r, "Feet") == "Quantum_Old_Feet" &&
              pieceFor(r, "UnderPants") == "Quantum_Old_UnderPants" && pieceFor(r, "Legs") == "Quantum_Legs" &&
              pieceFor(r, "Head") == "Quantum_Old_Head");
        old.Items = {{"Shoes", item("Clothing/Male/Shoes/SKM_Sneakers.fbx")}};
        CHECK(pieceFor(Wardrobe::Resolve(cat->W, cat->Items, old), "Feet") == "Quantum_Old_Feet_Shoes");
        const std::string skins = "assets/Characters/Quantum/Materials/Characters/";
        const auto* tattoo = Wardrobe::FindRace(cat->W, Wardrobe::Gender::Male, "Tattooed");
        CHECK(tattoo && tattoo->Name == "Tattooed" && !tattoo->Random);
        if (tattoo) {
            CHECK(Wardrobe::SkinMaterialFor(cat->W, *tattoo, skins + "M_Quantum_Arms.mat", exists) == skins + "M_Quantum_Arms_Tatto.mat");
            CHECK(Wardrobe::SkinMaterialFor(cat->W, *tattoo, skins + "M_Quantum_Body.mat", exists) == skins + "M_Quantum_Body.mat");
        }
        for (const auto& [g, name, arms] : {std::tuple{Wardrobe::Gender::Male, "Ornaments", "M_Quantum_Arms"},
                                            std::tuple{Wardrobe::Gender::Female, "Tattoo L", "M_F_Vivian_Arms"},
                                            std::tuple{Wardrobe::Gender::Female, "Tattoo R", "M_F_Vivian_Arms"},
                                            std::tuple{Wardrobe::Gender::Female, "Tattoo L+R", "M_F_Vivian_Arms"}}) {
            const auto* race = Wardrobe::FindRace(cat->W, g, name);
            CHECK(race && race->Name == name && !race->Random);
            if (race) CHECK(exists(skins + arms + race->SkinSuffix + ".mat"));
        }
    }

    // Randomize: whole outfits (top or a jacket that covers the chest, pants, shoes), nothing the rules
    // must take off, no clash, no variant picked directly - and the same seed gives the same outfit.
    for (int g = 0; g < 2; ++g)
        for (unsigned seed = 1; seed <= 300; ++seed) {
            Wardrobe::Request base;
            base.Sex = (Wardrobe::Gender)g;
            std::string style;
            const auto want = Wardrobe::Randomize(cat->W, cat->Items, base, seed, {}, "", &style);
            CHECK(!style.empty() && Wardrobe::Randomize(cat->W, cat->Items, base, seed).Items == want.Items);
            const auto got = Wardrobe::Resolve(cat->W, cat->Items, want);
            CHECK(got.Dropped.empty() && got.Clashes.empty());
            CHECK(!pieceFor(got, "Pants").empty() && !pieceFor(got, "Shoes").empty());
            for (const auto& [slot, path] : want.Items) CHECK(cat->Find(path) && !cat->Find(path)->Variant);
        }
    // The balaclava is rare in the everyday styles (the Masked style always wears it), so Randomize dresses the
    // head - hair, beards, hats - still with nothing taken off; the loud cuts and long beards stay out of Formal.
    int everyday = 0, masked = 0;
    for (unsigned seed = 1; seed <= 200; ++seed) {
        Wardrobe::Request base;
        for (const char* s : {"Casual", "Formal"}) {
            const auto want = Wardrobe::Randomize(cat->W, cat->Items, base, seed, {}, s);
            everyday += want.Items.count("Balaclava") > 0;
            if (std::string(s) == "Formal")
                for (const char* slot : {"Hair", "FacialHair"})
                    if (want.Items.count(slot)) {
                        const std::string p = want.Items.at(slot);
                        CHECK(p.find("Mohawk") == std::string::npos && p.find("Irokez") == std::string::npos &&
                              p.find("Braids") == std::string::npos && p.find("Beard_Long") == std::string::npos);
                    }
        }
        for (int g = 0; g < 2; ++g) {
            base.Sex = (Wardrobe::Gender)g;
            const auto want = Wardrobe::Randomize(cat->W, cat->Items, base, seed, {}, "Masked");
            const auto got = Wardrobe::Resolve(cat->W, cat->Items, want);
            masked += !pieceFor(got, "Balaclava").empty();
            CHECK(got.Dropped.empty() && !pieceFor(got, "Pants").empty() && !pieceFor(got, "Shoes").empty());
        }
    }
    CHECK(everyday < 40 && masked == 400);
    int withHair = 0, withHat = 0;
    for (int g = 0; g < 2; ++g)
        for (unsigned seed = 1; seed <= 200; ++seed) {
            Wardrobe::Request base;
            base.Sex = (Wardrobe::Gender)g;
            const auto want = Wardrobe::Randomize(cat->W, cat->Items, base, seed);
            const auto got = Wardrobe::Resolve(cat->W, cat->Items, want);
            CHECK(got.Dropped.empty() && got.Clashes.empty());
            withHair += !pieceFor(got, "Hair").empty();
            withHat += want.Items.count("Headwear") > 0;
        }
    Log::Info("Quantum wardrobe: Randomize - " + std::to_string(withHair) + " / 400 with hair, " +
              std::to_string(withHat) + " with headwear");
    CHECK(withHair > 100 && withHat > 20);
    // Locked slots stay as they are.
    Wardrobe::Request locked;
    locked.Items = {{"Top", item("Clothing/Male/Tops/SKM_Shirt_Hawaii.fbx")}};
    CHECK(Wardrobe::Randomize(cat->W, cat->Items, locked, 7, {"Top"}).Items.at("Top") == locked.Items["Top"]);
    CHECK(Wardrobe::FindStyle(cat->W, "Formal") && Wardrobe::FindStyle(cat->W, "Formal")->Gender == 0);
}

// A ragdoll keeps its root where the NPC stood while the body lies a body length away. The culling bounds
// (Model::PosedBounds) must hold every skinned vertex of the posed piece; the bind box padded 1.75x didn't (a
// fallen head was culled whenever the spot it stood on left the view - parts blinking out up close). Needs the pack.
void TestPosedBounds() {
    const char* path = "assets/Characters/Quantum/Models/European/Quantum_Head.fbx";
    if (!std::filesystem::exists(ProjectPaths::Resolve(path))) {
        UnitTestSupport::Skip("no Quantum pack");
        return;
    }
    auto model = Model::ImportDeferred(ProjectPaths::Resolve(path), ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    glm::vec3 lo, hi;
    model->PosedBounds(lo, hi);
    CHECK(lo == model->BoundsMin() && hi == model->BoundsMax()); // nothing poses it: the bind bounds
    const int pelvis = model->NodeIndex("pelvis");
    CHECK(pelvis >= 0);
    if (pelvis < 0) return;
    // Laid on its back, four of its own sizes forward of the root.
    const glm::vec3 bmin = model->BoundsMin(), bmax = model->BoundsMax(), size = bmax - bmin;
    const float reach = 4.0f * std::max({size.x, size.y, size.z});
    std::vector<LocalTRS> pose;
    model->BindLocalPose(pose);
    const glm::mat4 was = model->PoseNodeModelSpace(pose, pelvis);
    const glm::vec3 at(was[3]);
    model->SetPoseNodeModelSpace(pose, pelvis, glm::translate(glm::mat4(1.0f), at + glm::vec3(0.0f, 0.0f, reach)) *
                                                    glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)) *
                                                    glm::translate(glm::mat4(1.0f), -at) * was);
    model->ApplyLocalPose(pose);
    model->PosedBounds(lo, hi);
    const glm::vec3 c = (bmin + bmax) * 0.5f, h = size * 0.5f * 1.75f; // the old culling box
    int total = 0, inside = 0, outsideOld = 0;
    for (int mi = 0; mi < model->MeshCount(); ++mi)
        for (const auto& v : model->MeshSkinVertices(mi)) {
            glm::mat4 m(0.0f);
            float tw = 0.0f;
            for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
                if (v.BoneIDs[i] >= 0) { m += model->FinalBoneMatrix(v.BoneIDs[i]) * v.Weights[i]; tw += v.Weights[i]; }
            if (tw <= 1e-4f) m = glm::mat4(1.0f);
            const glm::vec3 p(m * glm::vec4(v.Position, 1.0f));
            ++total;
            inside += glm::all(glm::greaterThanEqual(p, lo)) && glm::all(glm::lessThanEqual(p, hi));
            outsideOld += glm::any(glm::lessThan(p, c - h)) || glm::any(glm::greaterThan(p, c + h));
        }
    Log::Info("PosedBounds: " + std::to_string(inside) + "/" + std::to_string(total) + " inside, " + std::to_string(outsideOld) +
              " outside the padded bind box");
    CHECK(total > 0 && inside == total);
    CHECK(outsideOld == total); // what the old test culled
    CHECK(hi.z - lo.z < reach); // still a tight box, not grown to take in the root
}

// --- Importing an asset pack laid out <Asset>/Models/x.fbx + <Asset>/Textures/.../x_*.png -----
// The Quantum Hawaiian shirt's loose short sleeve over the arms: the skin Hidden works out in the bind pose
// must stay under the cloth once the arms swing down (both meshes CPU-skinned, every bone under upperarm_l/_r
// turned about its shoulder). With a one-ring edge, 13-30 hidden arm vertices came out as holes. Needs the pack.
void TestOutfitEdgeBandPosed() {
    const char* armsPath = "assets/Characters/Quantum/FirstPerson/Quantum_Arms_FP.fbx";
    const char* shirtPath = "assets/Characters/Quantum/Models/Clothing/Male/Tops/SKM_Shirt_Hawaii.fbx";
    if (!std::filesystem::exists(ProjectPaths::Resolve(armsPath)) || !std::filesystem::exists(ProjectPaths::Resolve(shirtPath))) {
        UnitTestSupport::Skip("no Quantum pack");
        return;
    }
    // The model's geometry with its upper arms turned down by `deg` (0 = the bind pose).
    auto posed = [](const char* path, float deg, OutfitCoverage::Mesh& out) {
        ModelImportSettings settings; // the .meta's frame setting, as the game imports it (the shirt is an Unreal export)
        if (std::ifstream f(ProjectPaths::Resolve(std::string(path) + ".meta")); f) {
            const json meta = json::parse(f, nullptr, false);
            if (meta.is_object() && meta.contains("importer") && meta["importer"].is_object())
                settings.RootNodeInModelSpace = meta["importer"].value("rootNodeInModelSpace", false);
        }
        auto model = Model::ImportDeferred(ProjectPaths::Resolve(path), settings);
        if (!model) return false;
        out = {};
        model->CollisionGeometry(out.Positions, out.Indices);
        // Posed the way the renderer poses it (local pose -> final bone matrices), so a file whose skeleton sits
        // under an extra node (the UE exports: RootNode -> <Mesh> -> root, the axis turn on RootNode) poses right.
        std::vector<LocalTRS> pose;
        model->BindLocalPose(pose);
        for (const char* side : {"_l", "_r"}) {
            const int ua = model->NodeIndex(std::string("upperarm") + side), la = model->NodeIndex(std::string("lowerarm") + side);
            if (ua < 0 || la < 0) return false;
            const glm::mat4 was = model->PoseNodeModelSpace(pose, ua);
            const glm::vec3 s(was[3]), e(model->PoseNodeModelSpace(pose, la)[3]);
            const glm::vec3 axis = glm::normalize(glm::cross(e - s, glm::vec3(0.0f, -1.0f, 0.0f)));
            model->SetPoseNodeModelSpace(pose, ua, glm::translate(glm::mat4(1.0f), s) *
                                                       glm::rotate(glm::mat4(1.0f), glm::radians(deg), axis) *
                                                       glm::translate(glm::mat4(1.0f), -s) * was);
        }
        model->ApplyLocalPose(pose);
        size_t k = 0;
        for (int mi = 0; mi < model->MeshCount(); ++mi)
            for (const auto& v : model->MeshSkinVertices(mi)) {
                glm::mat4 m(0.0f);
                float tw = 0.0f;
                for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
                    if (v.BoneIDs[i] >= 0) { m += model->FinalBoneMatrix(v.BoneIDs[i]) * v.Weights[i]; tw += v.Weights[i]; }
                if (k < out.Positions.size() && tw > 1e-4f) out.Positions[k] = glm::vec3(m * glm::vec4(out.Positions[k], 1.0f));
                ++k;
            }
        return k == out.Positions.size();
    };
    OutfitCoverage::Mesh arms, shirt;
    CHECK(posed(armsPath, 0.0f, arms) && posed(shirtPath, 0.0f, shirt));
    const auto hidden = OutfitCoverage::Hidden(arms, shirt);
    const size_t hiddenN = std::count(hidden.begin(), hidden.end(), (std::uint8_t)1);
    CHECK(hiddenN > 300); // the sleeves' skin is still hidden - the band isn't the whole arm
    // Nothing hidden past the sleeve near its hem: the rolled cuff sits over ~20 arm vertices only in the bind
    // pose (they "poke through" it there), and hiding them opened the black patch in the Scene view.
    {
        const auto shirtCov = OutfitCoverage::Covered(arms, shirt);
        std::map<std::pair<unsigned, unsigned>, int> edges;
        for (size_t t = 0; t + 2 < shirt.Indices.size(); t += 3)
            for (int k = 0; k < 3; ++k) {
                const unsigned x = shirt.Indices[t + k], y = shirt.Indices[t + (k + 1) % 3];
                ++edges[{std::min(x, y), std::max(x, y)}];
            }
        std::vector<glm::vec3> hem; // the open edges out along the arms: the sleeves' hems (UV seams are harmless here)
        for (const auto& [e, n] : edges)
            if (n == 1)
                for (unsigned v : {e.first, e.second})
                    if (std::abs(shirt.Positions[v].x) > 0.25f) hem.push_back(shirt.Positions[v]);
        CHECK(!hem.empty());
        size_t pastHem = 0;
        for (size_t v = 0; v < hidden.size(); ++v) {
            if (!hidden[v] || shirtCov[v]) continue;
            for (const auto& q : hem)
                if (glm::distance(q, arms.Positions[v]) < OutfitCoverage::kEdgeBand) { ++pastHem; break; }
        }
        CHECK(pastHem == 0);
    }
    for (float deg : {20.0f, 35.0f, 50.0f}) {
        OutfitCoverage::Mesh pa, ps;
        CHECK(posed(armsPath, deg, pa) && posed(shirtPath, deg, ps));
        const auto covered = OutfitCoverage::Covered(pa, ps);
        const auto poke = OutfitCoverage::PokeDepth(pa, ps, OutfitCoverage::kPokeReach);
        size_t holes = 0;
        for (size_t v = 0; v < hidden.size() && v < covered.size(); ++v) holes += hidden[v] && !covered[v] && poke[v] <= 0.0f;
        Log::Info("OutfitEdgeBandPosed: " + std::to_string(hiddenN) + " arm vertices hidden; " + std::to_string((int)deg) +
                  " deg down, " + std::to_string(holes) + " out from under the sleeve.");
        CHECK(holes == 0);
    }
}

// The balaclava rides the head bone under a hood that's up: only what pokes out through the hood is hidden
// (Hidden's `rigid`). Hiding the covered rest showed through the hood's face opening as missing balaclava
// round the ears, the ears behind it. And the ears stay hidden under the balaclava. Needs the Quantum pack.
void TestOutfitRigidUnderHood() {
    const std::string C = "assets/Characters/Quantum/Models/Clothing/Male/";
    const std::string headPath = "assets/Characters/Quantum/Models/Young/Quantum_Young_Head.fbx";
    if (!std::filesystem::exists(ProjectPaths::Resolve(C + "Tops/SKM_Hoodie_Hood_Up.fbx")) || !std::filesystem::exists(ProjectPaths::Resolve(headPath))) {
        UnitTestSupport::Skip("no Quantum pack");
        return;
    }
    auto geo = [](const std::string& rel) {
        OutfitCoverage::Mesh m;
        auto mm = Model::ImportDeferred(ProjectPaths::Resolve(rel), ModelImportSettings{});
        if (mm) mm->CollisionGeometry(m.Positions, m.Indices);
        return m;
    };
    const auto bala = geo(C + "Balaclava/SM_Balaclava_Crime.fbx"), hood = geo(C + "Tops/SKM_Hoodie_Hood_Up.fbx"), head = geo(headPath);
    auto roundEars = [](const glm::vec3& p) { return std::abs(p.x) > 0.06f && p.y > 1.58f && p.y < 1.76f; };
    const auto hb = OutfitCoverage::Hidden(bala, hood, false, true);
    size_t n = 0, hidden = 0;
    for (size_t v = 0; v < hb.size(); ++v)
        if (roundEars(bala.Positions[v])) { ++n; hidden += hb[v]; }
    const auto hh = OutfitCoverage::Hidden(head, bala, true);
    size_t en = 0, ehidden = 0;
    for (size_t v = 0; v < hh.size(); ++v)
        if (roundEars(head.Positions[v])) { ++en; ehidden += hh[v]; }
    Log::Info("OutfitRigidUnderHood: balaclava round the ears " + std::to_string(hidden) + " / " + std::to_string(n) + " hidden by the hood; head " +
              std::to_string(ehidden) + " / " + std::to_string(en) + " by the balaclava.");
    CHECK(n > 0 && hidden * 20 < n);   // was 631 / 824
    CHECK(en > 0 && ehidden * 10 > en * 8); // the ears: ~89 % (5012 of 5647 once, when the band covered the head)
}

// Every texture a Quantum material names is on disk: the Casual Bundle import wrote them, and a texture it retired
// must not still be named. Needs the pack (project content, fetched from the Drive).
void TestQuantumMaterialTextures() {
    namespace fs = std::filesystem;
    const fs::path root = ProjectPaths::Resolve("assets/Characters/Quantum/Materials");
    if (!fs::exists(ProjectPaths::Resolve("assets/Characters/Quantum/Models/European/Quantum_Torso.fbx")) || !fs::exists(root)) {
        UnitTestSupport::Skip("no Quantum pack");
        return;
    }
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    size_t mats = 0, refs = 0, missing = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || lower(it->path().extension().string()) != ".mat") continue;
        std::ifstream in(it->path(), std::ios::binary);
        const json j = json::parse(in, nullptr, false);
        CHECK(!j.is_discarded() && j.is_object());
        if (j.is_discarded() || !j.is_object()) continue;
        ++mats;
        if (!j.contains("properties") || !j["properties"].is_object()) continue;
        for (const auto& [key, v] : j["properties"].items()) {
            if (!v.is_string()) continue;
            const std::string tex = v.get<std::string>();
            const std::string ext = lower(fs::path(tex).extension().string());
            if (tex.empty() || (ext != ".png" && ext != ".tga" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp")) continue;
            ++refs;
            if (!fs::exists(ProjectPaths::Resolve(tex), ec)) {
                ++missing;
                Log::Warn("QuantumMaterialTextures: " + it->path().filename().string() + " " + key + " -> missing " + tex);
            }
        }
    }
    Log::Info("QuantumMaterialTextures: " + std::to_string(mats) + " materials, " + std::to_string(refs) + " texture paths, " +
              std::to_string(missing) + " missing.");
    CHECK(mats > 300 && refs > mats && missing == 0);
}

} // namespace

void RegisterOutfitsTests(UnitTestSupport::TestList& tests) {
    tests.insert(tests.end(), {
        {"Wardrobe", TestWardrobe},
        {"OutfitCoverage", TestOutfitCoverage},
        {"WardrobeQuantum", TestWardrobeQuantum},
        {"PosedBounds", TestPosedBounds},
        {"OutfitEdgeBandPosed", TestOutfitEdgeBandPosed},
        {"OutfitRigidUnderHood", TestOutfitRigidUnderHood},
        {"QuantumMaterialTextures", TestQuantumMaterialTextures},
    });
}
