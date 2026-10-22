// Decal assets and components (docs/DECALS.md, "Editor & assets"): the .decal JSON round trip
// and its forgiving load, the Decal / Decal Grading components' defaults, registration and scene
// round trip.
#include "UnitTestSupport.h"
#include "../Renderer/DecalAsset.h"
#include "../Renderer/SubstrateJson.h"
#include "../Renderer/MaterialAsset.h"
#include "../Renderer/Texture.h"
#include "../Game/World.h"
#include "../Game/Components.h"
#include "../Game/ComponentRegistry.h"
#include "../Game/SceneSerializer.h"
#include "../Editor/AssetLibrary.h"
#include <json.hpp>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

bool Near(float a, float b, float eps = 1e-5f) { return std::abs(a - b) <= eps; }
bool Near3(const glm::vec3& a, const glm::vec3& b, float eps = 1e-5f) { return glm::length(a - b) <= eps; }

fs::path DecalTempDir() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "TartarusUnitTests" / "Decals";
    fs::create_directories(dir, ec);
    return dir;
}
std::string ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
void WriteFile(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

const RegisteredComponent* FindRegistered(const char* name) {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    for (const RegisteredComponent& rc : ComponentRegistry::All())
        if (std::strcmp(rc.Meta.Name, name) == 0) return &rc;
    return nullptr;
}

// Defaults match the schema comment in DecalAsset.h and the component contracts.
void Defaults() {
    const DecalAsset a;
    CHECK(a.Tier == 2048 && a.AtlasCols == 1 && a.AtlasRows == 1);
    CHECK(Near3(a.DefaultSize, glm::vec3(1.0f, 1.0f, 0.25f)));
    CHECK(Near(a.AlbedoOp, 1.0f) && a.NormalOp == 0.0f && a.RoughnessOp == 0.0f && a.MetalOp == 0.0f &&
          a.AOOp == 0.0f && a.EmissiveOp == 0.0f);
    CHECK(Near(a.AngleFadeStart, 0.25f) && Near(a.AngleFadeEnd, 0.05f) && Near(a.EdgeFade, 0.08f));
    CHECK(Near(a.HeightContrast, 4.0f) && !a.Paint && !a.UVPaint && !a.LumaMask && !a.InvertMask);

    const DecalComponent d;
    CHECK(d.AssetPath.empty() && Near3(d.Size, glm::vec3(1.0f, 1.0f, 0.25f)) && Near3(d.Tint, glm::vec3(1.0f)));
    CHECK(d.Opacity == 1.0f && d.AlbedoOp == 1.0f && d.NormalOp == 1.0f && d.EmissiveOp == 1.0f);
    CHECK(d.LayerMask == -1 && d.AffectStatic && d.AffectDynamic && d.Frame == -1 && d.SortOrder == 0);
    CHECK(d.RotationJitter == 0.0f && d.ScaleJitter == 0.0f && !d.RandomFlip);
    const DecalGradingComponent g;
    CHECK(g.Age == 0.0f && g.Wear == 0.0f);

    // Registered for the generic Inspector / serializer, the asset field drawing from .decal files.
    const RegisteredComponent* rc = FindRegistered("Decal");
    CHECK(rc != nullptr);
    if (rc) {
        bool assetKind = false;
        for (const ReflectField& f : rc->Meta.Fields)
            if (std::strcmp(f.Name, "Asset") == 0) assetKind = f.AssetKind == ReflectAssetKind::Decal;
        CHECK(assetKind);
        CHECK(rc->Meta.GenericSerialize);
        CHECK(!rc->Meta.Buttons.empty()); // Randomize Seed
    }
    CHECK(FindRegistered("Decal Grading") != nullptr);
}

// Every field survives Save -> Load, and saving the same values again is byte-identical.
void AssetRoundTrip() {
    const fs::path path = DecalTempDir() / "DA_RoundTrip.decal";
    DecalAsset a;
    a.Path = path.u8string();
    a.AlbedoPath = "assets/Decals/Graffiti/T_Tag_Crown.png";
    a.NormalPath = "assets/Decals/Graffiti/T_Tag_Crown_N.png";
    a.SurfacePath = "assets/Decals/Graffiti/T_Tag_Crown_S.png";
    a.AuxPath = "assets/Decals/Graffiti/T_Tag_Crown_A.png";
    a.LumaMask = true;
    a.InvertMask = true;
    a.Tier = 1024;
    a.AtlasCols = 4;
    a.AtlasRows = 2;
    a.DefaultSize = glm::vec3(1.5f, 0.75f, 0.1f);
    a.AlbedoOp = 0.9f; a.NormalOp = 0.5f; a.RoughnessOp = 0.25f; a.MetalOp = 0.125f; a.AOOp = 0.75f; a.EmissiveOp = 0.5f;
    a.BaseColor = glm::vec3(0.8f, 0.1f, 0.2f);
    a.Roughness = 0.4f; a.Metal = 0.3f;
    a.Emissive = glm::vec3(2.0f, 0.5f, 0.0f);
    a.AngleFadeStart = 0.5f; a.AngleFadeEnd = 0.1f; a.EdgeFade = 0.2f;
    a.HeightBlend = 0.6f; a.HeightContrast = 8.0f;
    a.Paint = true; a.UVPaint = true; a.PaintRoughness = 0.15f; a.PaintMetal = 0.7f; a.Relief = 0.45f;
    a.Fade = 0.1f; a.Bleach = 0.2f; a.Peel = 0.3f; a.Buff = 0.4f;
    a.SceneGrading = false;
    a.Category = "Graffiti/Tags";
    a.Tags = "graffiti, tag ,crown";
    CHECK(a.Save());
    const std::string first = ReadFile(path);

    const json j = json::parse(first, nullptr, false);
    CHECK(j.is_object() && j.value("decalVersion", 0) == 1);
    CHECK(!first.empty() && first.find("\"decalVersion\"") < first.find("\"textures\"")); // schema key order
    CHECK(j.is_object() && j["tags"].is_array() && j["tags"].size() == 3 && j["tags"][1] == "tag");

    const auto b = DecalAsset::Load(path.u8string());
    CHECK(b && !b->Missing);
    if (!b) return;
    CHECK(b->Name == "DA_RoundTrip");
    CHECK(b->AlbedoPath == a.AlbedoPath && b->NormalPath == a.NormalPath && b->SurfacePath == a.SurfacePath &&
          b->AuxPath == a.AuxPath);
    CHECK(b->LumaMask && b->InvertMask && b->Tier == 1024 && b->AtlasCols == 4 && b->AtlasRows == 2);
    CHECK(Near3(b->DefaultSize, a.DefaultSize));
    CHECK(Near(b->AlbedoOp, 0.9f) && Near(b->NormalOp, 0.5f) && Near(b->RoughnessOp, 0.25f) &&
          Near(b->MetalOp, 0.125f) && Near(b->AOOp, 0.75f) && Near(b->EmissiveOp, 0.5f));
    CHECK(Near3(b->BaseColor, a.BaseColor) && Near(b->Roughness, 0.4f) && Near(b->Metal, 0.3f));
    CHECK(Near3(b->Emissive, a.Emissive));
    CHECK(Near(b->AngleFadeStart, 0.5f) && Near(b->AngleFadeEnd, 0.1f) && Near(b->EdgeFade, 0.2f));
    CHECK(Near(b->HeightBlend, 0.6f) && Near(b->HeightContrast, 8.0f));
    CHECK(b->Paint && b->UVPaint && Near(b->PaintRoughness, 0.15f) && Near(b->PaintMetal, 0.7f) && Near(b->Relief, 0.45f));
    CHECK(Near(b->Fade, 0.1f) && Near(b->Bleach, 0.2f) && Near(b->Peel, 0.3f) && Near(b->Buff, 0.4f));
    CHECK(!b->SceneGrading);
    CHECK(b->Category == "Graffiti/Tags" && b->Tags == "graffiti, tag, crown");

    CHECK(b->Save());
    CHECK(ReadFile(path) == first); // a load + save changes nothing
    std::error_code ec;
    fs::remove(path, ec);
}

// A hand-edited file never throws out of Load: wrong types keep the defaults, values clamp,
// the tier snaps, tags may be one string, and missing / broken files report Missing.
void AssetForgivingLoad() {
    const fs::path path = DecalTempDir() / "DA_HandEdited.decal";
    WriteFile(path, R"({
        "decalVersion": 2,
        "textures": { "albedo": "a.png", "normal": 5 },
        "lumaMask": 1,
        "tier": 3000,
        "atlas": { "cols": 0, "rows": 500 },
        "defaultSize": [2, -1],
        "channels": { "albedo": 3, "normal": -1, "roughness": "high" },
        "roughness": 2,
        "emissive": [-1, 0.5, 2],
        "angleFade": [1.5, -0.5],
        "edgeFade": 0.9,
        "heightContrast": -3,
        "paint": [1, 2],
        "weathering": { "peel": 0.5 },
        "tags": "grime, water",
        "futureKey": { "anything": true }
    })");
    const auto d = DecalAsset::Load(path.u8string());
    CHECK(d && !d->Missing);
    if (d) {
        CHECK(d->AlbedoPath == "a.png" && d->NormalPath.empty());
        CHECK(d->LumaMask && d->Tier == 2048);
        CHECK(d->AtlasCols == 1 && d->AtlasRows == 64);
        CHECK(Near3(d->DefaultSize, glm::vec3(1.0f, 1.0f, 0.25f))); // too short: default kept
        CHECK(d->AlbedoOp == 1.0f && d->NormalOp == 0.0f && d->RoughnessOp == 0.0f);
        CHECK(d->Roughness == 1.0f && Near3(d->Emissive, glm::vec3(0.0f, 0.5f, 2.0f)));
        CHECK(d->AngleFadeStart == 1.0f && d->AngleFadeEnd == 0.0f && d->EdgeFade == 0.5f);
        CHECK(d->HeightContrast == 0.0f && !d->Paint && Near(d->Peel, 0.5f));
        CHECK(d->Tags == "grime, water");
    }
    const auto snapped = [&](int tier) {
        WriteFile(path, "{\"tier\": " + std::to_string(tier) + "}");
        const auto t = DecalAsset::Load(path.u8string());
        return t ? t->Tier : -1;
    };
    CHECK(snapped(100) == 512 && snapped(700) == 512 && snapped(1100) == 1024 && snapped(9000) == 4096);

    WriteFile(path, "{ not json");
    const auto broken = DecalAsset::Load(path.u8string());
    CHECK(broken && broken->Missing);
    WriteFile(path, "[1, 2, 3]");
    const auto notObject = DecalAsset::Load(path.u8string());
    CHECK(notObject && notObject->Missing);
    std::error_code ec;
    fs::remove(path, ec);
    const auto missing = DecalAsset::Load(path.u8string());
    CHECK(missing && missing->Missing && missing->Name == "DA_HandEdited");
}

// Decal and Decal Grading save with the scene and come back the same.
void ComponentRoundTrip() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    World w;
    AssetLibrary assets;
    const entt::entity e = w.CreateEmptyEntity(glm::vec3(1, 2, 3), glm::vec3(0, 90, 0), glm::vec3(1), "Tag");
    auto& d = w.Registry.emplace<DecalComponent>(e);
    d.AssetPath = (DecalTempDir() / "DA_Scene.decal").u8string();
    d.Size = glm::vec3(2.0f, 1.0f, 0.3f);
    d.Tint = glm::vec3(0.5f, 0.25f, 1.0f);
    d.Opacity = 0.8f;
    d.AlbedoOp = 0.9f; d.NormalOp = 0.0f; d.RoughnessOp = 0.5f; d.MetalOp = 0.25f; d.AOOp = 0.75f; d.EmissiveOp = 0.1f;
    d.SortOrder = 7;
    d.LayerMask = 0x5;
    d.AffectStatic = true;
    d.AffectDynamic = false;
    d.Seed = 123456;
    d.Frame = 3;
    d.RotationJitter = 15.0f;
    d.ScaleJitter = 0.2f;
    d.RandomFlip = true;
    d.Fade = 0.1f; d.Bleach = 0.2f; d.Peel = 0.3f; d.Buff = 0.4f;
    const entt::entity g = w.CreateEmptyEntity(glm::vec3(0), glm::vec3(0), glm::vec3(1), "Decal Grading");
    auto& grade = w.Registry.emplace<DecalGradingComponent>(g);
    grade.Age = 0.35f;
    grade.Wear = 0.65f;

    const std::string saved = SceneSerializer::SaveToString(w, assets);
    CHECK(saved.find("\"Decal\"") != std::string::npos && saved.find("\"Decal Grading\"") != std::string::npos);

    World back;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(back, assets2, saved));
    const auto view = back.Registry.view<DecalComponent>();
    CHECK(view.size() == 1);
    if (view.empty()) return;
    const DecalComponent& r = back.Registry.get<DecalComponent>(*view.begin());
    std::string file = fs::u8path(r.AssetPath).filename().u8string();
    for (char& c : file) c = (char)std::tolower((unsigned char)c);
    CHECK(file == "da_scene.decal");
    CHECK(Near3(r.Size, d.Size, 1e-4f) && Near3(r.Tint, d.Tint, 1e-4f) && Near(r.Opacity, 0.8f, 1e-4f));
    CHECK(Near(r.AlbedoOp, 0.9f, 1e-4f) && r.NormalOp == 0.0f && Near(r.RoughnessOp, 0.5f, 1e-4f) &&
          Near(r.MetalOp, 0.25f, 1e-4f) && Near(r.AOOp, 0.75f, 1e-4f) && Near(r.EmissiveOp, 0.1f, 1e-4f));
    CHECK(r.SortOrder == 7 && r.LayerMask == 0x5 && r.AffectStatic && !r.AffectDynamic);
    CHECK(r.Seed == 123456 && r.Frame == 3 && Near(r.RotationJitter, 15.0f, 1e-4f) && Near(r.ScaleJitter, 0.2f, 1e-4f) &&
          r.RandomFlip);
    CHECK(Near(r.Fade, 0.1f, 1e-4f) && Near(r.Bleach, 0.2f, 1e-4f) && Near(r.Peel, 0.3f, 1e-4f) && Near(r.Buff, 0.4f, 1e-4f));

    const auto gv = back.Registry.view<DecalGradingComponent>();
    CHECK(gv.size() == 1);
    if (gv.empty()) return;
    const DecalGradingComponent& rg = back.Registry.get<DecalGradingComponent>(*gv.begin());
    CHECK(Near(rg.Age, 0.35f, 1e-4f) && Near(rg.Wear, 0.65f, 1e-4f));
}

// A scene's embedded material keeps its grunge map, strength and scale and its Receive Decals flag;
// one saved before them loads with the defaults. Through the serializer's own slot write/read (a
// box or model would need GL); the grunge map is a pending texture, so no GL either.
void EmbeddedMaterialGrunge() {
    MaterialAsset ma;
    TextureCpuData cpu;
    cpu.Ok = true;
    cpu.Path = (DecalTempDir() / "T_Grunge_Test.png").u8string();
    ma.Mat.GrungeMap = Texture::CreatePending(std::move(cpu));
    CHECK(ma.Mat.GrungeMap != nullptr);
    ma.Mat.GrungeStrength = 0.6f;
    ma.Mat.GrungeScale = 1.25f;
    ma.Mat.ReceiveDecals = false;
    const json em = json::parse(SceneSerializer::EmbeddedMaterialToJson(ma));
    CHECK(em.contains("grungeMap"));
    if (em.contains("grungeMap")) {
        const json& ref = em["grungeMap"];
        std::string file = fs::u8path(ref.is_object() ? ref.value("path", std::string()) : ref.get<std::string>())
                               .filename().u8string();
        for (char& c : file) c = (char)std::tolower((unsigned char)c);
        CHECK(file == "t_grunge_test.png");
    }
    CHECK(Near(em.value("grungeStrength", 0.0f), 0.6f) && Near(em.value("grungeScale", 0.0f), 1.25f));
    CHECK(em.contains("receiveDecals") && em["receiveDecals"] == false);

    // Read back (the map key left out: loading a texture file needs GL).
    json noMap = em;
    noMap.erase("grungeMap");
    AssetLibrary assets;
    const auto back = SceneSerializer::EmbeddedMaterialFromJson(assets, noMap.dump());
    CHECK(back != nullptr);
    if (!back) return;
    CHECK(!back->Mat.GrungeMap && Near(back->Mat.GrungeStrength, 0.6f) && Near(back->Mat.GrungeScale, 1.25f) &&
          !back->Mat.ReceiveDecals);
    // Written again it is the same JSON (map aside).
    CHECK(json::parse(SceneSerializer::EmbeddedMaterialToJson(*back)) == noMap);

    // The defaults are written as nothing, and nothing reads as the defaults.
    MaterialAsset plain;
    const json pj = json::parse(SceneSerializer::EmbeddedMaterialToJson(plain));
    CHECK(!pj.contains("grungeMap") && !pj.contains("grungeStrength") && !pj.contains("grungeScale") &&
          !pj.contains("receiveDecals"));
    const auto old = SceneSerializer::EmbeddedMaterialFromJson(assets, R"({"baseColor": [1, 1, 1], "roughness": 0.7})");
    CHECK(old != nullptr);
    if (!old) return;
    CHECK(!old->Mat.GrungeMap && old->Mat.GrungeStrength == 0.0f && old->Mat.GrungeScale == 0.5f && old->Mat.ReceiveDecals);
}

// Substrates (Substrate.h): a .decal's block, a placed decal's Custom override and an embedded
// material's block all round-trip; a wrong type falls back to the defaults.
void SubstrateRoundTrip() {
    const std::string kAlb = "assets/Architecture/Building/Textures/Wall_Concrete/Wall_Concrete_Base_Color.png";
    const std::string kAO = "assets/Architecture/Building/Textures/Wall_Concrete/Wall_Concrete_AO.png";
    // .decal
    DecalAsset a;
    a.Path = (DecalTempDir() / "DA_Substrate.decal").u8string();
    a.AlbedoPath = "assets/Decals/T_Test.png";
    a.Substrate.Albedo = kAlb;
    a.Substrate.AO = kAO;
    a.Substrate.Tiling = 1.5f;
    a.Substrate.Tint = glm::vec3(0.9f, 0.8f, 0.7f);
    a.Substrate.Strength = 0.75f;
    a.Substrate.RoughRange = glm::vec2(0.85f, 0.95f);
    a.Substrate.Mirror = true;
    a.Substrate.Crop = glm::vec4(0.0f, 0.21f, 1.0f, 0.93f);
    CHECK(a.Save());
    const auto b = DecalAsset::Load(a.Path);
    CHECK(b && !b->Missing && b->Substrate == a.Substrate && b->Substrate.Enabled());
    if (b) {
        CHECK(b->Save());
        CHECK(ReadFile(a.Path).find("\"substrate\"") != std::string::npos);
    }
    // No albedo: nothing written, nothing enabled.
    DecalAsset none;
    none.Path = (DecalTempDir() / "DA_NoSubstrate.decal").u8string();
    none.AlbedoPath = "assets/Decals/T_Test.png";
    CHECK(none.Save());
    CHECK(ReadFile(none.Path).find("substrate") == std::string::npos);
    const auto nb = DecalAsset::Load(none.Path);
    CHECK(nb && !nb->Substrate.Enabled());
    // Wrong types keep the defaults; out-of-range values clamp.
    WriteFile(none.Path, R"({"textures": {"albedo": "x.png"}, "substrate": {"albedo": "c.png", "tiling": "big",
        "strength": 4, "mirror": 1, "crop": [0, 0, 1], "roughnessRange": [-1, 2]}})");
    const auto bad = DecalAsset::Load(none.Path);
    CHECK(bad && bad->Substrate.Albedo == "c.png" && Near(bad->Substrate.Tiling, 1.5f) &&
          Near(bad->Substrate.Strength, 1.0f) && bad->Substrate.Mirror && bad->Substrate.Crop == glm::vec4(0, 0, 1, 1) &&
          bad->Substrate.RoughRange == glm::vec2(0.0f, 1.0f));

    // Component: Custom and its maps through the scene file, by the keys the dress scripts write.
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    World w;
    AssetLibrary assets;
    const entt::entity e = w.CreateEmptyEntity(glm::vec3(0), glm::vec3(0), glm::vec3(1), "Peel");
    auto& d = w.Registry.emplace<DecalComponent>(e);
    CHECK(d.SubstrateMode == 0); // Auto
    d.AssetPath = (DecalTempDir() / "DA_Substrate.decal").u8string();
    d.SubstrateMode = 2;
    d.SubAlbedo = kAlb;
    d.SubTiling = 2.0f;
    d.SubRoughMin = 0.3f;
    d.SubRoughMax = 0.92f;
    d.SubMirror = true;
    d.SubCropY1 = 0.5f;
    const std::string saved = SceneSerializer::SaveToString(w, assets);
    CHECK(saved.find("\"Custom\"") != std::string::npos); // the Enum saves its label
    CHECK(saved.find("\"Substrate Rough Max\"") != std::string::npos);
    World back;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(back, assets2, saved));
    const auto view = back.Registry.view<DecalComponent>();
    CHECK(view.size() == 1);
    if (!view.empty()) {
        const DecalComponent& r = back.Registry.get<DecalComponent>(*view.begin());
        CHECK(r.SubstrateMode == 2 && r.SubAlbedo.find("Wall_Concrete_Base_Color.png") != std::string::npos);
        CHECK(Near(r.SubTiling, 2.0f) && Near(r.SubRoughMin, 0.3f) && Near(r.SubRoughMax, 0.92f) && r.SubMirror &&
              Near(r.SubCropY1, 0.5f) && Near(r.SubCropX1, 1.0f));
    }

    // Embedded material.
    MaterialAsset ma;
    ma.Mat.Substrate = a.Substrate;
    const json em = json::parse(SceneSerializer::EmbeddedMaterialToJson(ma));
    CHECK(em.contains("substrate") && em["substrate"].is_object());
    const auto mb = SceneSerializer::EmbeddedMaterialFromJson(assets, em.dump());
    CHECK(mb != nullptr);
    if (mb) {
        CHECK(mb->Mat.Substrate.Albedo.find("Wall_Concrete_Base_Color.png") != std::string::npos);
        CHECK(Near(mb->Mat.Substrate.Strength, 0.75f) && mb->Mat.Substrate.Mirror && mb->Mat.Substrate.Normal.empty() &&
              Near(mb->Mat.Substrate.Crop.w, 0.93f));
        CHECK(mb->Mat.ComputeHash() != MaterialAsset().Mat.ComputeHash());
    }
    MaterialAsset plain;
    CHECK(!json::parse(SceneSerializer::EmbeddedMaterialToJson(plain)).contains("substrate"));

    // Presets: a block naming a .substrate loads its values, a key beside it overrides one, and it is
    // written back as the reference plus only that override.
    const fs::path preset = DecalTempDir() / "SB_Test.substrate";
    WriteFile(preset, R"({"albedo": ")" + kAlb + R"(", "tiling": 2.5, "tint": [1.3, 1.3, 1.3], "detail": 1})");
    SubstratePresets::ClearCache();
    const std::string presetPath = preset.generic_u8string();
    SubstrateDesc viaPreset;
    CHECK(SubstrateJson::Read(json{{"preset", presetPath}, {"strength", 0.5}}, viaPreset, SubstrateJson::PlainPath));
    CHECK(viaPreset.Preset == presetPath && viaPreset.Albedo == kAlb && Near(viaPreset.Tiling, 2.5f) &&
          Near(viaPreset.Tint.r, 1.3f) && Near(viaPreset.Detail, 1.0f) && Near(viaPreset.Strength, 0.5f));
    const json back2 = SubstrateJson::Write<json>(viaPreset, [](const std::string& p) { return p; });
    CHECK(back2.size() == 2 && back2.value("preset", std::string()) == presetPath && Near(back2.value("strength", 0.0f), 0.5f));
    SubstrateDesc viaString;
    CHECK(SubstrateJson::Read(json(presetPath), viaString, SubstrateJson::PlainPath) && viaString.Albedo == kAlb &&
          Near(viaString.Strength, 1.0f));
    // An embedded material keeps just the reference.
    MaterialAsset pm;
    pm.Mat.Substrate = viaString;
    const json pem = json::parse(SceneSerializer::EmbeddedMaterialToJson(pm));
    CHECK(pem.contains("substrate") && pem["substrate"].size() == 1 && pem["substrate"].contains("preset"));
    const auto pmb = SceneSerializer::EmbeddedMaterialFromJson(assets, pem.dump());
    CHECK(pmb && pmb->Mat.Substrate.Albedo == kAlb && Near(pmb->Mat.Substrate.Tiling, 2.5f));
    // The component's Preset mode round-trips by label.
    {
        World pw;
        const entt::entity pe = pw.CreateEmptyEntity(glm::vec3(0), glm::vec3(0), glm::vec3(1), "PresetPeel");
        auto& pd = pw.Registry.emplace<DecalComponent>(pe);
        pd.AssetPath = (DecalTempDir() / "DA_Substrate.decal").u8string();
        pd.SubstrateMode = 4;
        pd.SubPreset = "assets/Decals/Substrates/SB_Floor_Concrete.substrate";
        const std::string ps = SceneSerializer::SaveToString(pw, assets);
        CHECK(ps.find("\"Preset\"") != std::string::npos);
        World pback;
        AssetLibrary assets3;
        CHECK(SceneSerializer::LoadFromString(pback, assets3, ps));
        const auto pv = pback.Registry.view<DecalComponent>();
        CHECK(pv.size() == 1);
        if (!pv.empty()) {
            const DecalComponent& r = pback.Registry.get<DecalComponent>(*pv.begin());
            CHECK(r.SubstrateMode == 4 && r.SubPreset == pd.SubPreset);
        }
    }
    SubstratePresets::ClearCache();
}
} // namespace

void RegisterDecalAssetTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Decals: asset and component defaults, registration", Defaults});
    tests.push_back({"Decals: .decal saves and loads every field, byte-stable", AssetRoundTrip});
    tests.push_back({"Decals: a hand-edited .decal loads with defaults and clamps", AssetForgivingLoad});
    tests.push_back({"Decals: Decal and Decal Grading round-trip through the scene file", ComponentRoundTrip});
    tests.push_back({"Decals: an embedded material saves and loads grunge and Receive Decals", EmbeddedMaterialGrunge});
    tests.push_back({"Decals: substrate blocks round-trip (.decal, Custom override, material)", SubstrateRoundTrip});
}
