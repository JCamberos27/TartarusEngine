#include "UnitTestSupport.h"
#include "../Game/Components.h"
#include "../Game/ComponentRegistry.h"
#include "../Game/ComponentReflection.h"
#include "../Game/RotationMath.h"
#include "../Game/Player.h"
#include "../Game/GameRules.h"
#include "../Game/AnimationSystem.h"
#include "../Game/HotReloadGameModule.h"
#include "../Game/GravityGun.h"
#include "../Game/DevPanel.h"
#include "../Game/AI/NpcDirector.h"
#include "../Game/Combat/PlayerVitals.h"
#include "../Game/Combat/CombatFx.h"
#include "../Renderer/WeaponFxRenderer.h"
#include "AssetLibrary.h"
#include "AssetDatabase.h"
#include "AtomicFile.h"
#include "SceneSerializer.h"
#include "SceneSerializerInternal.h"
#include "SceneFileJson.h"
#include "World.h"
#include "../Renderer/Model.h"
#include "../Core/Paths.h"
#include "../Renderer/Animation.h"
#include "../Renderer/SceneRenderer.h"
#include "../Renderer/MaterialAsset.h"
#include "../Renderer/ShaderVariant.h"
#include "../Renderer/CascadedShadowMap.h"
#include "../Renderer/Texture.h"
#include "../Core/PngWrite.h"
#include "../Core/SamplingProfiler.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef near // (minwindef.h: this file has lambdas named near and far)
#undef far
#endif
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <json.hpp>
#include <set>
#include <random>
#include <thread>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Unit tests for renderer, core and performance. Add a function per test and list it below.

// Copies component T from one entity to another through the scene's field encoding.
template <typename T>
static bool RoundTrip(World& world, AssetLibrary& assets, const char* name, const T& value, T& out) {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity src = world.CreateEmptyEntity(zero, zero, one, "Src");
    const entt::entity dst = world.CreateEmptyEntity(zero, zero, one, "Dst");
    world.Registry.emplace<T>(src, value);
    const std::string preset = SceneSerializer::ComponentToPresetJson(world, src, name);
    if (preset.empty() || !SceneSerializer::ApplyComponentPresetJson(world, assets, dst, preset)) return false;
    out = world.Registry.get<T>(dst);
    return true;
}

static void Test_FirstPersonController_NewFieldsRoundTrip() {
    World world;
    AssetLibrary assets;
    FirstPersonControllerComponent in;
    in.StickLookDegPerSec = 222.0f;
    in.EyeRadius = 0.2f;
    in.GrabRange = 55.0f;
    in.AssistRange = 12.0f;
    in.AssistConeDeg = 11.0f;
    in.ScrollTurnDeg = 33.0f;
    in.MaxHealth = 150.0f;
    in.SpawnProtection = 4.0f;
    FirstPersonControllerComponent out;
    CHECK(RoundTrip(world, assets, "First Person Controller", in, out));
    CHECK(out.StickLookDegPerSec == 222.0f && out.EyeRadius == 0.2f);
    CHECK(out.GrabRange == 55.0f && out.AssistRange == 12.0f);
    CHECK(out.AssistConeDeg == 11.0f && out.ScrollTurnDeg == 33.0f);
    CHECK(out.MaxHealth == 150.0f && out.SpawnProtection == 4.0f);
}

static void Test_FxHudSettings_RoundTrip() {
    World world;
    AssetLibrary assets;
    FxHudSettingsComponent in;
    in.FlashTime = 0.1f;
    in.PlayerFlashScale = 0.5f;
    in.FlameGlow = 90.0f;
    in.FlameScale = 2.5f;
    in.BeamRange = 60.0f;
    in.BeamHalfWidth = 0.003f;
    in.BeamFalloff = 5.0f;
    in.BeamBend = 8.0f;
    FxHudSettingsComponent out;
    CHECK(RoundTrip(world, assets, "FX & HUD Settings", in, out));
    CHECK(out.FlashTime == 0.1f && out.PlayerFlashScale == 0.5f && out.FlameGlow == 90.0f && out.FlameScale == 2.5f);
    CHECK(out.BeamRange == 60.0f && out.BeamHalfWidth == 0.003f && out.BeamFalloff == 5.0f && out.BeamBend == 8.0f);
    // A scene saved while the kill feed's fields existed still loads (the old keys are skipped).
    World old;
    AssetLibrary oldAssets;
    CHECK(SceneSerializer::LoadFromString(old, oldAssets, R"({"formatVersion":4,"empties":[{"id":0,"name":"Fx",)"
                                                          R"("FX & HUD Settings":{"Beam Range":42,"Feed Life":9,"Streak Window":2}}]})"));
    int seen = 0;
    for (entt::entity e : old.Registry.view<FxHudSettingsComponent>()) {
        ++seen;
        CHECK(old.Registry.get<FxHudSettingsComponent>(e).BeamRange == 42.0f);
    }
    CHECK(seen == 1);
}

static void Test_BloodSettings_RoundTrip() {
    World world;
    AssetLibrary assets;
    BloodSettingsComponent in;
    in.Enabled = false;
    in.Size = 0.7f;
    in.MaxSprays = 9;
    in.MaxStains = 100;
    in.StainLifetime = 42.0f;
    in.DrySeconds = 30.0f;
    in.Pools = false;
    in.BodySplats = false;
    in.GearSpatter = false;
    in.EnergyScale = 1.5f;
    in.ImpactPuffs = false;
    in.Gore = 0;
    in.Speed = 2.25f;
    BloodSettingsComponent out;
    CHECK(RoundTrip(world, assets, "Blood Settings", in, out));
    CHECK(!out.Enabled && out.Size == 0.7f && out.MaxSprays == 9 && out.MaxStains == 100 && out.StainLifetime == 42.0f);
    CHECK(out.DrySeconds == 30.0f && !out.Pools && !out.BodySplats && !out.GearSpatter);
    CHECK(out.EnergyScale == 1.5f && !out.ImpactPuffs && out.Gore == 0 && out.Speed == 2.25f);
}

static void Test_GravityGun_AssistReach() {
    GravityGunSettings s;
    s.AssistRange = FirstPersonControllerComponent{}.AssistRange;
    CHECK(GravityGun::AssistReach(s, -1.0f) == 30.0f);   // nothing under the crosshair: the full assist range
    CHECK(GravityGun::AssistReach(s, 10.0f) == 10.5f);   // a wall at 10 m cuts the search short
    s.AssistRange = 5.0f;
    CHECK(GravityGun::AssistReach(s, -1.0f) == 5.0f);
    CHECK(GravityGun::AssistReach(s, 10.0f) == 5.0f);
}

static void Test_FxHud_SettingsDrivePureHelpers() {
    FxHudSettingsComponent s;
    // Defaults are the old fixed values.
    CHECK(WeaponFxRenderer::BeamLength(s, 200.0f) == 150.0f && WeaponFxRenderer::BeamLength(s, 40.0f) == 40.0f);
    // Changed values change the result.
    s.BeamRange = 60.0f;
    CHECK(WeaponFxRenderer::BeamLength(s, 200.0f) == 60.0f);
}

// The dev panel's reset.
static void Test_DevPanelReset() {
    DevPanel panel;
    panel.Open = panel.InfiniteAmmo = panel.AiOverlay = true;
    PlayerVitals vitals;
    vitals.GodMode = true;
    panel.Reset(vitals);
    CHECK(!panel.Open && !panel.InfiniteAmmo && !panel.AiOverlay);
    CHECK(!vitals.GodMode);
}

static glm::mat4 RandomAffine(std::mt19937& rng) {
    std::uniform_real_distribution<float> d(-2.0f, 2.0f), a(-3.0f, 3.0f), sc(0.2f, 2.5f);
    const glm::quat q = glm::normalize(glm::quat(a(rng), a(rng), a(rng), a(rng) + 4.0f));
    return glm::translate(glm::mat4(1.0f), glm::vec3(d(rng), d(rng), d(rng))) * glm::mat4_cast(q) *
           glm::scale(glm::mat4(1.0f), glm::vec3(sc(rng), sc(rng), sc(rng)));
}

// The skinning palette uses affine products; they must equal the generic 4x4 ones.
static void Test_Model_AffinePaletteMatchesGeneric() {
    std::mt19937 rng(1234);
    float worst = 0.0f;
    for (int n = 0; n < 500; ++n) {
        const glm::mat4 gi = RandomAffine(rng), g = RandomAffine(rng), off = RandomAffine(rng);
        const glm::mat4 ref = gi * g * off;
        const glm::mat4 got = AffineMul(AffineMul(gi, g), off);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                worst = std::max(worst, std::abs(ref[c][r] - got[c][r]) / std::max(1.0f, std::abs(ref[c][r])));
    }
    CHECK(worst < 1e-5f);
}

// FBX axis systems: Y-up is untouched, a 3ds Max / Maya Z-up file (up +Z, front -Y) stands upright.
static void Test_Model_AxisCorrection() {
    const auto apply = [](const glm::mat4& m, glm::vec3 v) { return glm::vec3(m * glm::vec4(v, 0.0f)); };
    const auto near = [](glm::vec3 a, glm::vec3 b) { return glm::length(a - b) < 1e-5f; };
    CHECK(Model::AxisCorrection(1, 1, 2, 1) == glm::mat4(1.0f));
    const glm::mat4 z = Model::AxisCorrection(2, 1, 1, -1);
    CHECK(near(apply(z, {0, 0, 1}), {0, 1, 0}));   // up -> +Y
    CHECK(near(apply(z, {0, -1, 0}), {0, 0, 1}));  // front -> +Z
    CHECK(near(apply(z, {1, 0, 0}), {1, 0, 0}));   // right stays +X
    CHECK(std::abs(glm::determinant(z) - 1.0f) < 1e-5f);
    // A mirrored system (right derived as up x front) is still a pure rotation; bad input is identity.
    CHECK(std::abs(glm::determinant(Model::AxisCorrection(1, -1, 0, 1)) - 1.0f) < 1e-5f);
    CHECK(Model::AxisCorrection(1, 1, 1, 1) == glm::mat4(1.0f));
    const glm::mat4 nz = Model::AxisCorrection(2, -1, 1, 1);  // the opposite turn: -Z up, +Y front
    CHECK(near(apply(nz, {0, 0, -1}), {0, 1, 0}));
    CHECK(near(apply(nz, {0, 1, 0}), {0, 0, 1}));
    CHECK(Model::ParseUpAxis("-Z") == ModelImportSettings::UpAxis::NegZ && Model::ParseUpAxis("x") == ModelImportSettings::UpAxis::Y);
    CHECK(std::string(Model::UpAxisName(ModelImportSettings::UpAxis::NegZ)) == "-Z");
    CHECK(Model::AxisCorrection(5, 1, 2, 1) == glm::mat4(1.0f));
}

// Engine variant bits (ShaderVariant.h) sit in the key's top byte, apart from the material keywords, and a material
// only drops grunge or parallax code when its own strength / scale says the shader would skip it.
static void Test_ShaderVariant_EngineBits() {
    CHECK((kVariantEngineMask & ((1u << kVariantMaterialKeywords) - 1u)) == 0u);
    for (ShaderVariantKey b : {kVariantNoLocalLights, kVariantNoDecals, kVariantNoDebugView, kVariantNoGrunge, kVariantNoParallax})
        CHECK((b & kVariantEngineMask) == b);
    Material m;
    m.GrungeStrength = 0.0f;
    m.ParallaxScale = 0.0f;
    CHECK(ShaderVariantEngineBits(m) == (kVariantNoGrunge | kVariantNoParallax));
    m.GrungeStrength = 0.4f;
    m.ParallaxScale = 0.02f;
    CHECK(ShaderVariantEngineBits(m) == 0u);
}

// Load-time shader-variant warm-up: materials without a ShaderAsset draw through the model shader
// (already built), so there is nothing to compile - and no GL context is touched.
static void Test_SceneRenderer_WarmShaderVariantsSkipsShaderless() {
    World world;
    CHECK(SceneRenderer::WarmShaderVariants(world) == 0);
    const entt::entity e = world.Registry.create();
    RenderableComponent& rc = world.Registry.emplace<RenderableComponent>(e);
    rc.Materials.push_back(nullptr);
    rc.Materials.push_back(std::make_shared<MaterialAsset>());
    CHECK(SceneRenderer::WarmShaderVariants(world) == 0);
}

// The entity-ray test falls back to a unit box when a collider's renderer has no model, instead of
// dereferencing it.
// The prefab override diff: components with a hand-written JSON block (Collider, Joint) and the
// Animation component are looked up by their JSON key. An untouched instance has no overrides,
// an edited field is one entry, and a component the instance removed is recorded and survives a
// reload.
static nlohmann::json PrefabOverrides(const std::string& scene) {
    const nlohmann::json j = nlohmann::json::parse(scene);
    if (!j.contains("prefabInstances") || j["prefabInstances"].empty()) return nullptr;
    const nlohmann::json& stub = j["prefabInstances"][0];
    return stub.contains("overrides") ? stub["overrides"] : nlohmann::json::array();
}

static entt::entity PrefabRootOf(World& world) {
    const auto view = world.Registry.view<PrefabInstanceComponent>();
    return view.begin() == view.end() ? entt::entity(entt::null) : *view.begin();
}

static void Test_PrefabInstance_HandWrittenComponentsDiffByJsonKey() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "TartarusUnitTest_PrefabDiff";
    fs::create_directories(dir, ec);
    const std::string prefabPath = (dir / "thing.prefab").string();
    AssetLibrary assets;
    {
        World source; // a prefab whose one entity has a Collider, a Joint and an Animation
        const glm::vec3 zero(0.0f), one(1.0f);
        const entt::entity e = source.CreateEmptyEntity(zero, zero, one, "Thing");
        source.Registry.emplace<ColliderComponent>(e);
        source.Registry.emplace<JointComponent>(e);
        source.Registry.emplace<SkeletalAnimationComponent>(e);
        CHECK(SceneSerializer::SavePrefab(source, e, prefabPath));
    }

    World world;
    const entt::entity root = SceneSerializer::InstantiatePrefab(world, assets, prefabPath);
    CHECK(root != entt::null);
    if (root == entt::null) { fs::remove_all(dir, ec); return; }

    // Untouched: nothing is an override, and no helper sees an added component.
    nlohmann::json ov = PrefabOverrides(SceneSerializer::SaveToString(world));
    CHECK(ov.is_array() && ov.empty());
    for (const char* component : {"Collider", "Joint", "Animation"})
        CHECK(!SceneSerializer::IsPrefabComponentAdded(world, root, component));

    // One edited Collider field is one override entry and survives a reload.
    world.Registry.get<ColliderComponent>(root).IsTrigger = true;
    std::string saved = SceneSerializer::SaveToString(world);
    ov = PrefabOverrides(saved);
    int colliderEntries = 0;
    bool triggerSet = false, anyOp = false;
    for (const nlohmann::json& o : ov) {
        if (o.contains("op")) anyOp = true;
        if (o.value("c", "") == "Collider") {
            ++colliderEntries;
            if (o.value("f", "") == "Is Trigger" && o.contains("v") && o["v"] == true) triggerSet = true;
        }
    }
    CHECK(ov.is_array() && colliderEntries == 1 && triggerSet && !anyOp);
    {
        World reloaded;
        CHECK(SceneSerializer::LoadFromString(reloaded, assets, saved));
        const entt::entity r = PrefabRootOf(reloaded);
        CHECK(r != entt::null && reloaded.Registry.all_of<ColliderComponent>(r));
        if (r != entt::null && reloaded.Registry.all_of<ColliderComponent>(r))
            CHECK(reloaded.Registry.get<ColliderComponent>(r).IsTrigger);
    }

    // A removed Collider is recorded as removeComponent and is still gone after a reload.
    world.Registry.remove<ColliderComponent>(root);
    saved = SceneSerializer::SaveToString(world);
    ov = PrefabOverrides(saved);
    bool removedRecorded = false;
    for (const nlohmann::json& o : ov)
        if (o.value("op", "") == "removeComponent" && o.value("c", "") == "Collider") removedRecorded = true;
    CHECK(removedRecorded);
    {
        World reloaded;
        CHECK(SceneSerializer::LoadFromString(reloaded, assets, saved));
        const entt::entity r = PrefabRootOf(reloaded);
        CHECK(r != entt::null && !reloaded.Registry.all_of<ColliderComponent>(r));
    }
    fs::remove_all(dir, ec);
}

// CasterReachesSlice tests the slice hull's edges (precomputed in Update) instead of the direction between every
// pair of slice corners; both are complete separating-axis tests, so they must agree on every caster.
static void Test_CascadedShadowMap_SliceTestMatchesAllPairs() {
    CascadedShadowMap csm;
    const glm::mat4 view = glm::lookAt(glm::vec3(3.0f, 1.7f, -2.0f), glm::vec3(10.0f, 1.0f, 20.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(70.0f), 16.0f / 9.0f, 0.05f, 500.0f);
    csm.Update(view, proj, glm::normalize(glm::vec3(-0.4f, -0.8f, 0.3f)), 150.0f);
    const auto reference = [&](int c, const glm::vec3& bmin, const glm::vec3& bmax, float kernel, float bias) {
        if (c <= 0 || c >= csm.Count()) return true;
        const glm::mat4& vp = csm.LightViewProj(c);
        glm::vec2 lo(1e30f), hi(-1e30f);
        for (int k = 0; k < 8; ++k) {
            const glm::vec4 q = vp * glm::vec4((k & 1) ? bmax.x : bmin.x, (k & 2) ? bmax.y : bmin.y, (k & 4) ? bmax.z : bmin.z, 1.0f);
            lo = glm::min(lo, glm::vec2(q) / q.w);
            hi = glm::max(hi, glm::vec2(q) / q.w);
        }
        const float texels = std::max(kernel, 0.5f) * (1.0f + 0.35f * (float)c) * 1.25f + std::max(bias, 0.0f) + 3.0f;
        lo -= glm::vec2(texels * 2.0f / (float)csm.Resolution());
        hi += glm::vec2(texels * 2.0f / (float)csm.Resolution());
        const auto& s = csm.SliceCorners(c);
        const glm::vec2 rect[4] = {{lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
        const glm::vec2 axes0[2] = {{1.0f, 0.0f}, {0.0f, 1.0f}};
        const auto separated = [&](const glm::vec2& n) {
            float s0 = 1e30f, s1 = -1e30f, r0 = 1e30f, r1 = -1e30f;
            for (const glm::vec2& p : s) { s0 = std::min(s0, glm::dot(n, p)); s1 = std::max(s1, glm::dot(n, p)); }
            for (const glm::vec2& p : rect) { r0 = std::min(r0, glm::dot(n, p)); r1 = std::max(r1, glm::dot(n, p)); }
            return s1 < r0 || r1 < s0;
        };
        for (const glm::vec2& n : axes0) if (separated(n)) return false;
        for (int a = 0; a < 8; ++a)
            for (int b = a + 1; b < 8; ++b) {
                const glm::vec2 d = s[b] - s[a];
                if (glm::dot(d, d) > 1e-12f && separated(glm::vec2(-d.y, d.x))) return false;
            }
        return true;
    };
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> pos(-120.0f, 160.0f), size(0.05f, 6.0f);
    int disagree = 0, culled = 0;
    for (int i = 0; i < 4000; ++i) {
        const glm::vec3 a(pos(rng), pos(rng) * 0.1f, pos(rng)), e(size(rng), size(rng), size(rng));
        for (int c = 1; c < csm.Count(); ++c) { // cascade 0 has its own test below
            const bool got = csm.CasterReachesSlice(c, a, a + e, 3.0f, 1.0f), want = reference(c, a, a + e, 3.0f, 1.0f);
            disagree += got != want;
            culled += !want;
        }
    }
    CHECK(disagree == 0);
    CHECK(culled > 1000); // the slices do cull: the test exercises both answers
    // Cascade 0: everything at the eye (the view model, at its own FOV) and along the view inside the near slice casts;
    // something well behind the camera, outside the near slice's light-space box, doesn't.
    const glm::vec3 eye(3.0f, 1.7f, -2.0f), fwd = glm::normalize(glm::vec3(10.0f, 1.0f, 20.0f) - eye);
    const glm::vec3 side = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 h(0.1f);
    CHECK(csm.CasterReachesSlice(0, eye - h, eye + h, 3.0f, 1.0f));
    CHECK(csm.CasterReachesSlice(0, eye + side * 1.5f - h, eye + side * 1.5f + h, 3.0f, 1.0f)); // beside the eye
    bool alongAll = true;
    for (float d = 0.5f; d < csm.SplitDepthsVec4().x; d += 0.5f)
        alongAll = alongAll && csm.CasterReachesSlice(0, eye + fwd * d - h, eye + fwd * d + h, 3.0f, 1.0f);
    CHECK(alongAll);
    const glm::vec3 behind = eye - fwd * 40.0f;
    CHECK(!csm.CasterReachesSlice(0, behind - h, behind + h, 3.0f, 1.0f));
    // The scissor rectangle is the same test: the eye's surroundings stay inside it.
    glm::vec2 lo, hi;
    csm.ReadRect(0, 3.0f, 1.0f, lo, hi);
    const glm::vec4 e0 = csm.LightViewProj(0) * glm::vec4(eye, 1.0f);
    CHECK(e0.x / e0.w > lo.x && e0.x / e0.w < hi.x && e0.y / e0.w > lo.y && e0.y / e0.w < hi.y);
}

// Material::Hash is cached per frame: an edit shows from the next frame on, and a copy changed within the frame
// hashes its own values (the draw loop's material dedupe and sort keys must never mix two materials up).
static void Test_Material_HashCachePerFrame() {
    Material::NextHashFrame();
    Material a;
    const std::uint64_t h0 = a.Hash();
    CHECK(h0 == a.ComputeHash());
    Material b = a;
    b.BaseColor = glm::vec3(0.2f, 0.4f, 0.6f);
    CHECK(b.Hash() == b.ComputeHash() && b.Hash() != h0);
    a.Roughness = 0.9f;
    CHECK(a.Hash() == h0); // same frame: the cached value
    Material::NextHashFrame();
    CHECK(a.Hash() == a.ComputeHash() && a.Hash() != h0);
    Material c;
    c = b; // assignment drops the cache too
    c.Metallic = 1.0f;
    CHECK(c.Hash() == c.ComputeHash() && c.Hash() != b.Hash());
}

// The data-driven bind's redundant-bind key (custom properties included) is cached per frame like Hash():
// an ExtraProp edit shows from the next frame, and a copy hashes its own values.
static void Test_Material_ExtraPropsHashPerFrame() {
    Material::NextHashFrame();
    Material a;
    a.ExtraProps["_Glow"] = MaterialProp{};
    const std::uint64_t h0 = a.HashWithExtraProps();
    CHECK(h0 != a.Hash()); // the custom property is in it
    a.ExtraProps["_Glow"].F = 2.0f;
    CHECK(a.HashWithExtraProps() == h0); // same frame: the cached value
    Material::NextHashFrame();
    CHECK(a.HashWithExtraProps() != h0);
    Material b = a;
    b.ExtraProps["_Glow"].F = 3.0f;
    CHECK(b.HashWithExtraProps() != a.HashWithExtraProps());
}

// A compressed normal map becomes BC5 (x, y) with one level per mip, and every level decodes to unit-length
// normals pointing the way the source's do: the 1x1 level is the renormalized mean, not a shortened one.
static void Test_Texture_NormalMapBC5Renormalized() {
    const int w = 8, h = 8;
    std::vector<unsigned char> raw((size_t)w * h * 4);
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-0.7f, 0.7f);
    glm::vec3 sum(0.0f);
    std::vector<glm::vec3> src;
    for (int i = 0; i < w * h; ++i) {
        const glm::vec3 n = glm::normalize(glm::vec3(u(rng), u(rng), 1.0f));
        src.push_back(n);
        sum += n;
        for (int c = 0; c < 3; ++c) raw[(size_t)i * 4 + c] = (unsigned char)std::lround((n[c] * 0.5f + 0.5f) * 255.0f);
        raw[(size_t)i * 4 + 3] = 255;
    }
    TextureImportSettings s;
    s.TextureType = TextureImportSettings::Type::NormalMap;
    s.CompressionMode = TextureImportSettings::Compression::Normal;
    const TextureCpuData d = Texture::DecodeMemory("unit-test-normal", raw, w, h, s);
    CHECK(d.Ok && d.GLFormat == 0x8DBD /*GL_COMPRESSED_RG_RGTC2*/);
    CHECK(d.LevelSizes.size() == 4); // 8, 4, 2, 1
    if (!d.Ok || d.LevelSizes.size() != 4) return;
    // One BC5 channel block (8 bytes) at texel (x, y) of its 4x4.
    auto channel = [](const unsigned char* b, int t) {
        const int e0 = b[0], e1 = b[1];
        std::uint64_t bits = 0;
        for (int k = 0; k < 6; ++k) bits |= (std::uint64_t)b[2 + k] << (8 * k);
        const int idx = (int)((bits >> (3 * t)) & 7);
        float pal[8] = {(float)e0, (float)e1};
        if (e0 > e1) for (int k = 1; k <= 6; ++k) pal[k + 1] = ((7 - k) * e0 + k * e1) / 7.0f;
        else { for (int k = 1; k <= 4; ++k) pal[k + 1] = ((5 - k) * e0 + k * e1) / 5.0f; pal[6] = 0.0f; pal[7] = 255.0f; }
        return pal[idx] / 255.0f * 2.0f - 1.0f;
    };
    auto texel = [&](const unsigned char* block, int t) {
        const float x = channel(block, t), y = channel(block + 8, t);
        return glm::vec3(x, y, std::sqrt(std::max(1.0f - x * x - y * y, 0.0f)));
    };
    // Level 0, the first block's 16 texels: close to the source, and x, y inside the unit circle.
    const unsigned char* level0 = d.Pixels.data();
    for (int t = 0; t < 16; ++t) {
        const glm::vec3 n = texel(level0, t);
        CHECK(glm::dot(glm::vec2(n), glm::vec2(n)) <= 1.0f + 1e-3f);
        CHECK(glm::dot(n, src[(size_t)(t / 4) * w + (t % 4)]) > 0.99f);
    }
    // The 1x1 level: the mean direction, at full length (x, y as far out as the normalized mean's).
    size_t offset = 0;
    for (size_t l = 0; l + 1 < d.LevelSizes.size(); ++l) offset += d.LevelSizes[l];
    const glm::vec3 last = texel(d.Pixels.data() + offset, 0);
    const glm::vec3 mean = glm::normalize(sum);
    CHECK(glm::dot(last, mean) > 0.998f);
    CHECK(std::abs(glm::length(glm::vec2(last)) - glm::length(glm::vec2(mean))) < 0.03f);
}

// BoneAnimChannel::Finalize's fast paths (constant tracks, evenly spaced keys) sample exactly what the general
// search does, including at and between keys, outside the clip, and on unevenly spaced tracks.
static void Test_BoneAnimChannel_FinalizeMatchesSearch() {
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    BoneAnimChannel ch;
    for (int k = 0; k < 40; ++k) {
        const float t = 2.0f + 1.5f * (float)k; // evenly spaced
        ch.Positions.push_back({glm::vec3(u(rng), u(rng), u(rng)), t});
        ch.Rotations.push_back({glm::normalize(glm::quat(1.0f + u(rng) * 0.2f, u(rng) * 0.2f, u(rng) * 0.2f, u(rng) * 0.2f)), t});
        ch.Scales.push_back({glm::vec3(1.0f), t}); // constant
    }
    BoneAnimChannel uneven = ch;
    for (size_t k = 0; k < uneven.Rotations.size(); ++k) uneven.Rotations[k].TimeTicks += (k % 3 == 1) ? 0.4f : 0.0f;
    BoneAnimChannel fast = ch, fastUneven = uneven;
    fast.Finalize();
    fastUneven.Finalize();
    CHECK(fast.PositionInfo.Uniform && !fast.PositionInfo.Constant && fast.ScaleInfo.Constant);
    CHECK(!fastUneven.RotationInfo.Uniform);
    LocalTRS bind;
    float worst = 0.0f;
    for (int s = -20; s < 2000; ++s) {
        const float t = (float)s * 0.0371f;
        for (int pass = 0; pass < 2; ++pass) {
            const LocalTRS a = (pass ? uneven : ch).Sample(t, bind), b = (pass ? fastUneven : fast).Sample(t, bind);
            worst = std::max({worst, glm::length(a.T - b.T), glm::length(a.S - b.S), 1.0f - std::abs(glm::dot(a.R, b.R))});
        }
        // Exactly on a key.
        const float k = 2.0f + 1.5f * (float)(s % 40);
        const LocalTRS a = ch.Sample(k, bind), b = fast.Sample(k, bind);
        worst = std::max({worst, glm::length(a.T - b.T), 1.0f - std::abs(glm::dot(a.R, b.R))});
    }
    CHECK(worst < 1e-5f);
}

// Softness 0 is the hard sun edge (one 2x2 compare); every positive softness keeps the kernel it always had, so
// saved scenes look the same. The split default reproduces the old fixed lambda, and a higher one tightens cascade 0.
static void Test_CascadedShadowMap_HardKernelAndSplit() {
    CHECK(CascadedShadowMap::SunKernelTexels(0.53f, 0.0f) == 0.0f);
    CHECK(CascadedShadowMap::SunKernelTexels(0.53f, -1.0f) == 0.0f);
    for (float ang : {0.1f, 0.53f, 2.0f, 9.0f})
        for (float soft : {0.02f, 0.15f, 0.6f, 1.0f, 4.0f})
            CHECK(CascadedShadowMap::SunKernelTexels(ang, soft) == std::clamp(ang * 3.0f, 1.0f, 14.0f) * soft);

    const glm::mat4 view = glm::lookAt(glm::vec3(-10.3f, 1.6f, 4.9f), glm::vec3(-6.0f, 1.2f, 8.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
    const glm::vec3 sun = glm::normalize(glm::vec3(0.3f, -0.5f, 0.8f));
    CascadedShadowMap def, tight;
    CHECK(def.SplitLambda() == 0.75f);
    def.Update(view, proj, sun, 60.0f);
    tight.SetSplitLambda(0.92f);
    tight.Update(view, proj, sun, 60.0f);
    // The old inline split: lambda 0.75 blend of uniform and logarithmic over [near, near + distance].
    const float n = 0.1f, f = n + 60.0f;
    for (int i = 0; i < def.Count(); ++i) {
        const float p = float(i + 1) / float(def.Count());
        const float want = glm::mix(n + (f - n) * p, n * std::pow(f / n, p), 0.75f);
        CHECK(std::abs(def.SplitDepthsVec4()[i] - want) < 1e-3f * want);
    }
    CHECK(tight.SplitDepthsVec4().x < def.SplitDepthsVec4().x * 0.6f);
    CHECK(tight.TexelWorldSizesVec4().x < def.TexelWorldSizesVec4().x * 0.7f);
    tight.SetSplitLambda(5.0f);
    CHECK(tight.SplitLambda() <= 0.98f);

    World w;
    AssetLibrary assets;
    w.ShadowCascadeSplit = 0.9f;
    World back;
    CHECK(SceneSerializer::LoadFromString(back, assets, SceneSerializer::SaveToString(w, assets)));
    CHECK(std::abs(back.ShadowCascadeSplit - 0.9f) < 1e-6f);
}

// PNG writes from several threads at different compression levels each get their own level: the bytes match a
// single-threaded encode at that level (stb keeps the level in one global; PngWrite holds a lock around it).
static void Test_PngWrite_LevelPerCallUnderThreads() {
    const int w = 96, h = 64;
    std::vector<unsigned char> px((size_t)w * h * 4);
    std::mt19937 rng(3);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)((i / 4 % 7 == 0) ? rng() : i * 13);
    std::string fast, best;
    CHECK(PngWrite::Encode(fast, w, h, 4, px.data(), w * 4, 0) && PngWrite::Encode(best, w, h, 4, px.data(), w * 4, 9));
    CHECK(fast.size() > best.size());
    std::vector<std::string> got(8);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            for (int k = 0; k < 20; ++k) {
                std::string out;
                PngWrite::Encode(out, w, h, 4, px.data(), w * 4, (t & 1) ? 9 : 0);
                if (k == 19) got[(size_t)t] = out;
            }
        });
    for (std::thread& th : threads) th.join();
    for (int t = 0; t < 8; ++t) CHECK(got[(size_t)t] == ((t & 1) ? best : fast));
    std::string none;
    CHECK(!PngWrite::Encode(none, 0, h, 4, px.data(), 0) && !PngWrite::Encode(none, w, h, 5, px.data(), w * 5) && none.empty());
}

// A .meta that is valid JSON but not an object (an array, a number) reads as empty and is rewritten as one, instead of
// throwing out of the merge.
static void Test_AssetDatabase_NonObjectMetaIsRewritten() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "TartarusUnitTest_MetaShape";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "thing.txt";
    CHECK(AtomicFile::WriteBytes(file, "x", true));
    const AssetGuid fresh = AssetGuid::Generate();
    CHECK(AtomicFile::WriteBytes(dir / "thing.txt.meta", nlohmann::json({{"guid", fresh.ToString()}, {"type", "asset"}}).dump(), true));
    CHECK(AssetDatabase::EnsureGuid(file.string()) == fresh);
    for (const char* bad : {"[1, 2]", "42", "\"text\""}) {
        CHECK(AtomicFile::WriteBytes(dir / "thing.txt.meta", bad, true));
        CHECK(AssetDatabase::ReadMetaFields(file.string()) == "{}");
        bool threw = false, merged = false;
        try { merged = AssetDatabase::MergeMetaFields(file.string(), R"({"note": 1})"); } catch (...) { threw = true; }
        CHECK(!threw && merged);
        const nlohmann::json back = nlohmann::json::parse(AssetDatabase::ReadMetaFields(file.string()), nullptr, false);
        CHECK(back.is_object() && back.value("note", 0) == 1 && back.value("guid", std::string()) == fresh.ToString());
    }
    AssetDatabase::ForgetPath(file.string());
    fs::remove_all(dir, ec);
}

// The sampler walks the main thread's stack while it is suspended. Suspended inside the loader (LoadLibrary / FreeLibrary
// hold its lock), a walk through RtlLookupFunctionEntry waited on that lock forever; the walk now reads the modules'
// unwind tables itself. Sampling a thread that loads and frees a DLL in a loop must finish, with real stacks.
static void Test_SamplingProfiler_SurvivesTheLoaderLock() {
#if defined(_WIN32) && defined(_M_X64)
    SamplingProfiler::Start();
    // Bounded by work, not a fixed window: a loaded machine gave the sampler only 9 ticks in 400 ms. A deadlock still
    // shows (the loop never returns); 10 s is a cap for a sampler that stopped ticking.
    const auto start = std::chrono::steady_clock::now();
    const auto until = start + std::chrono::seconds(10);
    int loads = 0;
    while (std::chrono::steady_clock::now() < until &&
           (std::chrono::steady_clock::now() < start + std::chrono::milliseconds(400) || loads <= 10 ||
            SamplingProfiler::SampleCount() <= 20)) {
        if (HMODULE m = LoadLibraryW(L"version.dll")) { FreeLibrary(m); ++loads; }
    }
    SamplingProfiler::Stop();
    std::printf("[UnitTest] sampler: %zu samples over %d DLL loads, mean depth %.1f\n", SamplingProfiler::SampleCount(), loads,
                SamplingProfiler::MeanDepth());
    CHECK(loads > 10 && SamplingProfiler::SampleCount() > 20 && SamplingProfiler::MeanDepth() > 3.0);
#endif
}

// Moved from UnitTests.cpp (pure move).
namespace {
using json = nlohmann::json;
using UnitTestSupport::TempDir;
using UnitTestSupport::ReadAll;

// #123 - decomposes keep the Euler triple nearest the old one instead of flipping to an
// equivalent (the alternate YXZ solution and +-360 wraps are the same rotation).
void TestNearestEuler() {
    auto sameRotation = [](const glm::vec3& a, const glm::vec3& b) {
        const glm::mat4 ma = ComposeTransform(glm::vec3(0.0f), a, glm::vec3(1.0f));
        const glm::mat4 mb = ComposeTransform(glm::vec3(0.0f), b, glm::vec3(1.0f));
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (std::abs(ma[c][r] - mb[c][r]) > 1e-4f) return false;
        return true;
    };
    const glm::vec3 flipped(180.0f, 0.0f, 180.0f);
    const glm::vec3 near0 = NearestEquivalentEuler(flipped, glm::vec3(0.0f, 170.0f, 0.0f));
    CHECK(sameRotation(near0, flipped));
    CHECK(std::abs(near0.x) < 1e-3f && std::abs(near0.y - 180.0f) < 1e-3f && std::abs(near0.z) < 1e-3f);
    const glm::vec3 e(30.0f, -20.0f, 50.0f);
    CHECK(NearestEquivalentEuler(e, e) == e);
    const glm::vec3 wrapped = NearestEquivalentEuler(glm::vec3(10.0f, -170.0f, 0.0f), glm::vec3(10.0f, 185.0f, 0.0f));
    CHECK(std::abs(wrapped.y - 190.0f) < 1e-3f && sameRotation(wrapped, glm::vec3(10.0f, -170.0f, 0.0f)));
    const glm::vec3 alt(180.0f - 25.0f, 40.0f + 180.0f, -60.0f + 180.0f);
    CHECK(sameRotation(alt, glm::vec3(25.0f, 40.0f, -60.0f)));
    const glm::vec3 back = NearestEquivalentEuler(alt, glm::vec3(20.0f, 45.0f, -55.0f));
    CHECK(glm::length(back - glm::vec3(25.0f, 40.0f, -60.0f)) < 1e-3f);
}

// #123 - quaternion is the authoritative transform rotation; Euler is only a stable editor view,
// and v3 Euler scene data migrates to the v4 quaternion representation without changing pose.
void TestQuaternionTransformStorage() {
    auto nearMatrix = [](const glm::mat3& a, const glm::mat3& b, float eps = 1e-4f) {
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (std::abs(a[c][r] - b[c][r]) > eps) return false;
        return true;
    };

    TransformComponent transform;
    const glm::vec3 authored(0.0f, 180.0f, 0.0f);
    transform.SetRotationEuler(authored);
    CHECK(glm::length(transform.EulerDegrees() - authored) < 1e-3f); // preserve the authored hint
    CHECK(std::abs(glm::length(transform.Rotation) - 1.0f) < 1e-5f);

    // Cross the old Euler singularity by composing quaternions. The stored orientation remains
    // exact even though no unique Euler triple exists at the middle pose.
    const glm::quat base = QuaternionFromEulerYXZ(glm::vec3(89.9f, 15.0f, -20.0f));
    const glm::quat turned = RotateAboutLocalAxis(base, glm::vec3(0.3f, 1.0f, 0.2f), 120.0f);
    transform.SetRotationQuaternion(turned);
    CHECK(SameRotation(transform.Rotation, turned));
    CHECK(nearMatrix(glm::mat3(ComposeTransform(transform)), glm::mat3_cast(turned)));

    World migrated;
    AssetLibrary assets;
    const std::string v3 = R"({"formatVersion":3,"empties":[{"name":"Legacy","id":0,"parentId":-1,)"
        R"("position":[0,0,0],"rotation":[25,40,-60],"scale":[1,1,1]}]})";
    CHECK(SceneSerializer::LoadFromString(migrated, assets, v3));
    const auto view = migrated.Registry.view<TransformComponent>();
    CHECK(view.size() == 1);
    if (!view.empty()) {
        const auto& loaded = migrated.Registry.get<TransformComponent>(*view.begin());
        CHECK(SameRotation(loaded.Rotation, QuaternionFromEulerYXZ(glm::vec3(25.0f, 40.0f, -60.0f))));
        CHECK(glm::length(loaded.EulerDegrees() - glm::vec3(25.0f, 40.0f, -60.0f)) < 1e-3f);
    }

    const json saved = json::parse(SceneSerializer::SaveToString(migrated, assets));
    CHECK(saved.value("formatVersion", 0) == 4);
    CHECK(saved["empties"].size() == 1);
    CHECK(saved["empties"][0]["rotation"].is_array() && saved["empties"][0]["rotation"].size() == 4);

    World reloaded;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(reloaded, assets2, saved.dump()));
    const auto reloadedView = reloaded.Registry.view<TransformComponent>();
    CHECK(reloadedView.size() == 1);
    if (!reloadedView.empty()) {
        CHECK(SameRotation(reloaded.Registry.get<TransformComponent>(*reloadedView.begin()).Rotation,
                           QuaternionFromEulerYXZ(glm::vec3(25.0f, 40.0f, -60.0f))));
    }

    // File loads upgrade even an empty v3 scene: the format changed independently of whether
    // that particular file happened to contain a transform, and the original remains backed up.
    const auto migrationPath = TempDir() / "quaternion-v3-migration.json";
    const auto backupPath = std::filesystem::path(migrationPath.string() + ".bak");
    std::error_code ec;
    std::filesystem::remove(migrationPath, ec);
    std::filesystem::remove(backupPath, ec);
    const std::string emptyV3 = R"({"formatVersion":3,"empties":[]})";
    CHECK(AtomicFile::WriteBytes(migrationPath, emptyV3, false));
    World emptyMigrated;
    AssetLibrary assets3;
    CHECK(SceneSerializer::Load(emptyMigrated, assets3, migrationPath.string(), true));
    CHECK(json::parse(ReadAll(migrationPath)).value("formatVersion", 0) == 4);
    CHECK(ReadAll(backupPath) == emptyV3);
    std::filesystem::remove(migrationPath, ec);
    std::filesystem::remove(backupPath, ec);
}

// An undo/redo snapshot carries every asset's folder, name, labels and import settings. Restoring one whose block
// matches the live library skips the per-asset re-apply (it went through each .meta); a changed one still restores.
void TestUndoSnapshotSkipsUnchangedAssetMeta() {
    const auto file = TempDir() / "undo-meta-sound.wav";
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(file.string() + ".meta"), ec);
    CHECK(AtomicFile::WriteBytes(file, std::string("RIFF"), true));
    // A temp folder is outside the project, where EnsureGuid won't write .meta files: provide one.
    CHECK(AtomicFile::WriteBytes(std::filesystem::path(file.string() + ".meta"),
                                 json({{"guid", AssetGuid::Generate().ToString()}, {"type", "audio"}}).dump(), true));
    const std::string path = file.string();
    World w;
    AssetLibrary a;
    a.RegisterSound(path);
    a.SetDisplayName(path, "Before");
    const std::string snapshot = SceneSerializer::SaveToString(w, a);
    auto metaName = [&] {
        try { return json::parse(AssetDatabase::ReadMetaFields(path)).value("displayName", std::string()); }
        catch (...) { return std::string(); }
    };
    CHECK(metaName() == "Before");
    // The .meta changes on disk while the library still has the snapshot's name: an undo of a scene edit leaves it.
    CHECK(AssetDatabase::MergeMetaFields(path, json{{"displayName", "OnDisk"}}.dump()));
    CHECK(SceneSerializer::LoadFromString(w, a, snapshot));
    CHECK(a.DisplayName(path) == "Before");
    CHECK(metaName() == "OnDisk");
    // A rename since the snapshot: undo takes it back, in the library and in the .meta.
    a.SetDisplayName(path, "After");
    CHECK(SceneSerializer::LoadFromString(w, a, snapshot));
    CHECK(a.DisplayName(path) == "Before");
    CHECK(metaName() == "Before");
    // Labels added since the snapshot (a key the snapshot doesn't have): undo drops them, as ClearMetadataOnly did.
    a.SetLabels(path, {"w5"});
    CHECK(SceneSerializer::LoadFromString(w, a, snapshot));
    CHECK(a.Labels(path).empty());
    CHECK(a.DisplayName(path) == "Before");
    std::filesystem::remove(file, ec);
    std::filesystem::remove(std::filesystem::path(path + ".meta"), ec);
}

// The editor's scene open parses the file once (SceneSerializer::ReadSceneFile) and hands that to
// Load: Load uses it as it is while the file is unchanged, and reads the file again once it isn't.
void TestSceneLoadTakesParsedFile() {
    auto names = [](const World& w) {
        std::set<std::string> out;
        for (entt::entity e : w.Registry.view<NameComponent>()) out.insert(w.Registry.get<NameComponent>(e).Name);
        return out;
    };
    auto sceneNamed = [](const std::string& name) {
        return R"({"formatVersion":4,"empties":[{"name":")" + name +
               R"(","id":0,"parentId":-1,"position":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]})";
    };
    const auto path = TempDir() / "parsed-scene-load.json";
    std::error_code ec;
    CHECK(AtomicFile::WriteBytes(path, sceneNamed("OnDisk"), false));
    const auto parsed = SceneSerializer::ReadSceneFile(path.string());
    CHECK(parsed != nullptr);
    if (!parsed) return;
    // A name only the parse has: seeing it after the load means the file wasn't read again.
    parsed->Root["empties"][0]["name"] = "FromParse";
    {
        World w;
        AssetLibrary assets;
        CHECK(SceneSerializer::Load(w, assets, path.string(), false, parsed.get()));
        CHECK(names(w) == std::set<std::string>{"FromParse"});
    }
    // The file changed after the parse (size differs): Load reads it again.
    CHECK(AtomicFile::WriteBytes(path, sceneNamed("ChangedOnDisk"), false));
    {
        World w;
        AssetLibrary assets;
        CHECK(SceneSerializer::Load(w, assets, path.string(), false, parsed.get()));
        CHECK(names(w) == std::set<std::string>{"ChangedOnDisk"});
    }
    // Same size, newer write time: read again too.
    CHECK(AtomicFile::WriteBytes(path, sceneNamed("OnDisk"), false));
    const auto again = SceneSerializer::ReadSceneFile(path.string());
    CHECK(again != nullptr);
    if (again) {
        again->Root["empties"][0]["name"] = "FromParse";
        std::filesystem::last_write_time(path, again->WriteTime + std::chrono::seconds(5), ec);
        World w;
        AssetLibrary assets;
        CHECK(SceneSerializer::Load(w, assets, path.string(), false, again.get()));
        CHECK(names(w) == std::set<std::string>{"OnDisk"});
    }
    // Missing or not JSON: no parse, and Load reports the file as before.
    CHECK(AtomicFile::WriteBytes(path, std::string("{ not json"), false));
    CHECK(SceneSerializer::ReadSceneFile(path.string()) == nullptr);
    std::filesystem::remove(path, ec);
    CHECK(SceneSerializer::ReadSceneFile(path.string()) == nullptr);
    {
        World w;
        AssetLibrary assets;
        CHECK(!SceneSerializer::Load(w, assets, path.string(), false, parsed.get()));
    }
}

// #123 - spinning about a diagonal axis turns the object about that axis. Adding axis * angle to
// the Euler components (the old behaviour) added equal pitch and yaw, which is another rotation.
void TestRotateEulerAboutLocalAxis() {
    auto rotationOf = [](const glm::vec3& euler) { return glm::mat3(ComposeTransform(glm::vec3(0.0f), euler, glm::vec3(1.0f))); };
    auto sameRotation = [&](const glm::vec3& a, const glm::mat3& expected) {
        const glm::mat3 m = rotationOf(a);
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (std::abs(m[c][r] - expected[c][r]) > 1e-4f) return false;
        return true;
    };
    // A diagonal axis from the identity is exactly angleAxis about it, at any angle...
    const glm::vec3 diag(1.0f, 1.0f, 0.0f);
    for (float angle : {10.0f, 90.0f, 200.0f, -45.0f}) {
        const glm::mat3 expected = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(angle), glm::normalize(diag)));
        CHECK(sameRotation(RotateEulerAboutLocalAxis(glm::vec3(0.0f), diag, angle), expected));
    }
    // ...and the axis is left unmoved by the turn (the old add-to-Euler result moved it).
    const glm::mat3 turned = rotationOf(RotateEulerAboutLocalAxis(glm::vec3(0.0f), diag, 90.0f));
    CHECK(glm::length(turned * glm::normalize(diag) - glm::normalize(diag)) < 1e-4f);
    CHECK(glm::length(rotationOf(glm::vec3(90.0f, 90.0f, 0.0f)) * glm::normalize(diag) - glm::normalize(diag)) > 0.1f);
    // The axis is local: the result is the starting orientation followed by the turn.
    const glm::vec3 start(30.0f, -20.0f, 50.0f);
    const glm::mat3 local = rotationOf(start) * glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(60.0f), glm::normalize(diag)));
    CHECK(sameRotation(RotateEulerAboutLocalAxis(start, diag, 60.0f), local));
    // The default spin, +Y from an untilted start, is still plain yaw.
    CHECK(sameRotation(RotateEulerAboutLocalAxis(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 37.0f), rotationOf(glm::vec3(0.0f, 37.0f, 0.0f))));
    // Axis magnitude is irrelevant, and a zero axis or zero angle is a no-op.
    CHECK(sameRotation(RotateEulerAboutLocalAxis(start, diag * 5.0f, 60.0f), local));
    CHECK(RotateEulerAboutLocalAxis(start, glm::vec3(0.0f), 60.0f) == start);
    CHECK(RotateEulerAboutLocalAxis(start, diag, 0.0f) == start);
}

// #123 - the Euler-driven spin systems (Animator, Transform Controller), run frame by frame, turn about the axis they were
// given rather than adding it to the Euler components.
void TestSpinSystemsAboutDiagonalAxis() {
    auto matrixOf = [](const glm::vec3& euler) { return glm::mat3(ComposeTransform(glm::vec3(0.0f), euler, glm::vec3(1.0f))); };
    auto turnAbout = [](const glm::vec3& axis, float deg) { return glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(deg), axis)); };
    auto near = [](const glm::mat3& a, const glm::mat3& b, float eps) {
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (std::abs(a[c][r] - b[c][r]) > eps) return false;
        return true;
    };

    // TransformControllerSystem is compiled only into TartarusGame.dll, so run
    // it the way the editor does: load the built module next to the exe and tick it.
    HotReloadGameModule gameModule;
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    gameModule.Initialize(std::filesystem::path(exePath).parent_path() / TARTARUS_GAME_MODULE_FILENAME);

    // Animator: the base orientation turned by |w| * t about w's own direction.
    {
        World world;
        const glm::vec3 base(20.0f, 30.0f, 0.0f);
        entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), base, glm::vec3(1.0f), "a");
        AnimatorComponent anim; anim.SpinDegPerSec = glm::vec3(60.0f, 60.0f, 0.0f);
        world.Registry.emplace<AnimatorComponent>(e, anim);
        UpdateAnimators(world, 0.5f); // initialises and advances t to 0.5 s
        const float angle = glm::length(anim.SpinDegPerSec) * 0.5f;
        CHECK(near(glm::mat3_cast(world.Registry.get<TransformComponent>(e).Rotation),
                   matrixOf(base) * turnAbout(glm::normalize(anim.SpinDegPerSec), angle), 1e-3f));
        UpdateAnimators(world, 100.0f); // a long time later: same form, still reversible and bounded
        const float later = std::fmod(glm::length(anim.SpinDegPerSec) * 100.5f, 360.0f);
        CHECK(near(glm::mat3_cast(world.Registry.get<TransformComponent>(e).Rotation),
                   matrixOf(base) * turnAbout(glm::normalize(anim.SpinDegPerSec), later), 2e-2f));
    }

    // Transform controller: same rule for RotationDegPerSec.
    {
        World world;
        const glm::vec3 base(0.0f, 15.0f, 0.0f);
        entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), base, glm::vec3(1.0f), "t");
        TransformControllerComponent ctl; ctl.RotationDegPerSec = glm::vec3(0.0f, 45.0f, 45.0f);
        world.Registry.emplace<TransformControllerComponent>(e, ctl);
        gameModule.Tick(world, 2.0f, 0.0f, true);
        CHECK(near(glm::mat3_cast(world.Registry.get<TransformComponent>(e).Rotation),
                   matrixOf(base) * turnAbout(glm::normalize(ctl.RotationDegPerSec), glm::length(ctl.RotationDegPerSec) * 2.0f), 1e-3f));
    }

    // The module also carries the weapon/body rules, built against these same frames, so the
    // engine calls its copies while it's loaded and its own again once it's gone.
    CHECK(GameRules::FromModule());
    gameModule.Shutdown();
    CHECK(!GameRules::FromModule());
}

// --- Active in hierarchy (#201) -------------------------------------------------------------
void TestActiveInHierarchy() {
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity parent = world.CreateEmptyEntity(zero, zero, one, "Parent");
    const entt::entity child = world.CreateEmptyEntity(zero, zero, one, "Child");
    const entt::entity grandchild = world.CreateEmptyEntity(zero, zero, one, "Grandchild");
    CHECK(world.SetParent(child, parent));
    CHECK(world.SetParent(grandchild, child));
    auto inactive = [&](entt::entity e) { return world.Registry.all_of<InactiveTag>(e); };

    world.Registry.emplace<DeactivatedTag>(parent);
    world.SyncActiveInHierarchy();
    CHECK(inactive(parent) && inactive(child) && inactive(grandchild)); // whole subtree hides

    world.Registry.emplace<DeactivatedTag>(child);
    world.Registry.remove<DeactivatedTag>(parent);
    world.SyncActiveInHierarchy();
    CHECK(!inactive(parent) && inactive(child) && inactive(grandchild));

    world.Registry.remove<DeactivatedTag>(child);
    world.SyncActiveInHierarchy();
    CHECK(!inactive(parent) && !inactive(child) && !inactive(grandchild)); // nothing left behind
}

// --- #178: component presets round-trip through the scene field encoding --------------------
void TestComponentPreset() {
    UnitTestSupport::EnsureEngineComponents();
    World world;
    AssetLibrary assets;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity src = world.CreateEmptyEntity(zero, zero, one, "Source");
    const entt::entity dst = world.CreateEmptyEntity(zero, zero, one, "Target");

    TransformControllerComponent ctl;
    ctl.RotationDegPerSec = glm::vec3(1.0f, 0.0f, 0.0f);
    ctl.ScalePulseFrequencyHz = 123.5f;
    world.Registry.emplace<TransformControllerComponent>(src, ctl);

    const std::string preset = SceneSerializer::ComponentToPresetJson(world, src, "Transform Controller");
    CHECK(!preset.empty());
    CHECK(SceneSerializer::PresetComponentName(preset) == "Transform Controller");

    // Applies to an entity that does not have the component yet: it is added, not skipped.
    CHECK(!world.Registry.all_of<TransformControllerComponent>(dst));
    CHECK(SceneSerializer::ApplyComponentPresetJson(world, assets, dst, preset));
    CHECK(world.Registry.all_of<TransformControllerComponent>(dst));
    const TransformControllerComponent& got = world.Registry.get<TransformControllerComponent>(dst);
    CHECK(got.ScalePulseFrequencyHz == 123.5f);
    CHECK(got.RotationDegPerSec.x == 1.0f && got.RotationDegPerSec.y == 0.0f && got.RotationDegPerSec.z == 0.0f);

    // A field the preset does not mention keeps its current value rather than snapping to the
    // default, so a preset saved before a field existed stays usable.
    world.Registry.get<TransformControllerComponent>(dst).ScalePulseFrequencyHz = 7.0f;
    CHECK(SceneSerializer::ApplyComponentPresetJson(world, assets, dst,
              R"({"preset":1,"component":"Transform Controller","fields":{"Rotation Deg/Sec":[0.0,0.0,1.0]}})"));
    CHECK(world.Registry.get<TransformControllerComponent>(dst).RotationDegPerSec.z == 1.0f);
    CHECK(world.Registry.get<TransformControllerComponent>(dst).ScalePulseFrequencyHz == 7.0f); // untouched

    // #12: Script Path now defaults to empty (it named a script that never shipped). Files saved
    // with the old default still read it back as written, and an empty path round-trips.
    CHECK(TransformControllerComponent{}.ScriptPath.empty());
    CHECK(SceneSerializer::ApplyComponentPresetJson(world, assets, dst,
              R"({"preset":1,"component":"Transform Controller","fields":{"Script Path":"scripts/TransformController.tescript"}})"));
    CHECK(world.Registry.get<TransformControllerComponent>(dst).ScriptPath == "scripts/TransformController.tescript");
    world.Registry.get<TransformControllerComponent>(src).ScriptPath.clear();
    CHECK(SceneSerializer::ApplyComponentPresetJson(world, assets, dst,
              SceneSerializer::ComponentToPresetJson(world, src, "Transform Controller")));
    CHECK(world.Registry.get<TransformControllerComponent>(dst).ScriptPath.empty());

    // Rejections: malformed, unknown component, missing fields block, and a component that has
    // no generic serialisation. None of these may half-write anything.
    CHECK(!SceneSerializer::ApplyComponentPresetJson(world, assets, dst, "not json"));
    CHECK(!SceneSerializer::ApplyComponentPresetJson(world, assets, dst,
              R"({"preset":1,"component":"NoSuchComponent","fields":{}})"));
    CHECK(!SceneSerializer::ApplyComponentPresetJson(world, assets, dst, R"({"preset":1,"component":"Transform Controller"})"));
    CHECK(SceneSerializer::PresetComponentName("not json").empty());
    CHECK(SceneSerializer::PresetComponentName(R"({"component":"NoSuchComponent"})").empty());

    // A component the source entity does not have yields no preset at all.
    CHECK(SceneSerializer::ComponentToPresetJson(world, src, "NoSuchComponent").empty());
}

// --- #202: a scene whose parentIds form a loop must not load a cyclic hierarchy -------------
void TestHierarchyCycleRepair() {
    // Built with AttachChildRaw, which is exactly what the scene loader uses: it only refuses
    // self-parenting, so this is the shape a hand-edited or badly merged scene file produces.
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity a = world.CreateEmptyEntity(zero, zero, one, "A");
    const entt::entity b = world.CreateEmptyEntity(zero, zero, one, "B");
    const entt::entity c = world.CreateEmptyEntity(zero, zero, one, "C");

    world.AttachChildRaw(b, a);   // B under A
    world.AttachChildRaw(c, b);   // C under B
    world.AttachChildRaw(a, c);   // ...and A under C: a three-entity loop

    CHECK(world.RepairHierarchyCycles() == 1);

    // Every entity now reaches a root in a bounded walk.
    auto reachesRoot = [&](entt::entity e) {
        int hops = 0;
        for (entt::entity w = e; w != entt::null; ) {
            if (++hops > 64) return false;
            const auto* h = world.Registry.try_get<HierarchyComponent>(w);
            w = h ? h->Parent : entt::null;
        }
        return true;
    };
    CHECK(reachesRoot(a) && reachesRoot(b) && reachesRoot(c));

    // The cut is minimal: only the link that closed the loop went, so the rest of the chain
    // is still parented and no Children list keeps a stale entry.
    int parented = 0;
    for (entt::entity e : {a, b, c})
        if (const auto* h = world.Registry.try_get<HierarchyComponent>(e); h && h->Parent != entt::null) ++parented;
    CHECK(parented == 2);
    for (entt::entity e : {a, b, c}) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        if (!h) continue;
        for (entt::entity kid : h->Children) {
            const auto* kh = world.Registry.try_get<HierarchyComponent>(kid);
            CHECK(kh && kh->Parent == e); // no orphaned child entry left behind
        }
    }

    // Idempotent, and a healthy hierarchy is left completely alone.
    CHECK(world.RepairHierarchyCycles() == 0);

    World clean;
    const entt::entity p1 = clean.CreateEmptyEntity(zero, zero, one, "P");
    const entt::entity k1 = clean.CreateEmptyEntity(zero, zero, one, "K");
    CHECK(clean.SetParent(k1, p1));
    CHECK(clean.RepairHierarchyCycles() == 0);
    CHECK(clean.Registry.get<HierarchyComponent>(k1).Parent == p1);

    // End to end: this is the path that actually matters - a scene FILE whose parentIds form a
    // loop must not produce a cyclic world. Two empties, each claiming the other as its parent.
    World loaded;
    AssetLibrary assets;
    const std::string sceneJson = R"({"formatVersion":3,"empties":[
        {"name":"X","id":1,"parentId":2,"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},
        {"name":"Y","id":2,"parentId":1,"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}]})";
    CHECK(SceneSerializer::LoadFromString(loaded, assets, sceneJson));
    int loadedRoots = 0, loadedCount = 0;
    for (entt::entity e : loaded.Registry.view<HierarchyComponent>()) {
        ++loadedCount;
        // Bounded walk that BREAKS, not just CHECKs: a surviving cycle would otherwise spin
        // here forever and hang the whole suite instead of failing it.
        int hops = 0;
        bool terminated = true;
        for (entt::entity w = e; w != entt::null; ) {
            if (++hops > 64) { terminated = false; break; }
            const auto* h = loaded.Registry.try_get<HierarchyComponent>(w);
            w = h ? h->Parent : entt::null;
        }
        CHECK(terminated);
        const auto* h = loaded.Registry.try_get<HierarchyComponent>(e);
        if (!h || h->Parent == entt::null) ++loadedRoots;
    }
    CHECK(loadedCount == 2);
    CHECK(loadedRoots == 1); // one link cut, the other kept
}

// The player's acceleration: 0 = the move is the input (as before); otherwise an exponential approach
// that lands in the same place whatever the frame rate, and arrives rather than creeping.
void TestPlayerAcceleration() {
    // Body clips have different root travel by direction; sprint input must have equal speed.
    for (const glm::vec3 direction : {glm::vec3(0,0,1),glm::vec3(0,0,-1),glm::vec3(1,0,0),
                                     glm::vec3(-1,0,0),glm::normalize(glm::vec3(1,0,1))}) {
        const glm::vec3 wish=direction*6.8f;
        for (float weight : {0.0f,0.5f,1.0f}) {
            const glm::vec3 target=PlayerMovementTarget(wish,direction*2.0f,weight,true);
            CHECK(glm::length(target-wish)<1e-5f);
            CHECK(std::abs(glm::length(target)-6.8f)<1e-5f);
        }
    }
    CHECK(PlayerMovementTarget(glm::vec3(0,0,4),glm::vec3(0,0,2),0.5f,false)==glm::vec3(0,0,3));
    CHECK(PlayerMovementTarget(glm::vec3(0),glm::vec3(0,0,2),1.0f,true)==glm::vec3(0,0,2));
    const glm::vec3 run(0.0f, 0.0f, 4.5f);
    CHECK(PlayerApproachVelocity(glm::vec3(0.0f), run, 1.0f / 60.0f, 0.0f, 0.0f) == run);
    CHECK(PlayerApproachVelocity(run, glm::vec3(0.0f), 1.0f / 60.0f, 0.07f, 0.0f) == glm::vec3(0.0f)); // no decel time: stops dead
    auto after = [&](float hz, float seconds, glm::vec3 from, glm::vec3 to) {
        glm::vec3 v = from;
        for (int i = 0, n = (int)std::lround(seconds * hz); i < n; ++i) v = PlayerApproachVelocity(v, to, 1.0f / hz, 0.07f, 0.05f);
        return v;
    };
    // 0.07 s: 63% of the way; the same at 30, 144 and 300 fps.
    const float at30 = after(30.0f, 0.07f * 3.0f, glm::vec3(0.0f), run).z, at144 = after(144.0f, 0.07f * 3.0f, glm::vec3(0.0f), run).z,
                at300 = after(300.0f, 0.07f * 3.0f, glm::vec3(0.0f), run).z;
    CHECK(std::abs(at30 - at300) < 0.05f && std::abs(at144 - at300) < 0.05f && std::abs(at300 - 4.5f * (1.0f - std::exp(-3.0f))) < 0.05f);
    CHECK(after(144.0f, 0.5f, glm::vec3(0.0f), run) == run);             // arrives
    CHECK(after(144.0f, 0.4f, run, glm::vec3(0.0f)) == glm::vec3(0.0f)); // and stops
    // Slowing down uses the deceleration time: quicker here than speeding up.
    const float up = after(144.0f, 0.05f, glm::vec3(0.0f), run).z, down = 4.5f - after(144.0f, 0.05f, run, glm::vec3(0.0f)).z;
    CHECK(down > up);
}

// --- #202 / #122: a scene survives save -> load -> save unchanged ---------------------------
// crash - the scene just quietly comes back different. Round-tripping and comparing the two
// documents catches the whole class at once, which is what #202 asks for under "scene
// round-trips" and what #121/#342 changed enough of to be worth pinning down.
void TestSceneRoundTrip() {
    UnitTestSupport::EnsureEngineComponents();

    World a;
    AssetLibrary assetsA;
    const glm::vec3 zero(0.0f), one(1.0f);

    // A deliberately awkward scene: hierarchy, a nameless entity, non-default transforms, and
    // components spanning every reflected field type (bool, int, float, vec3, colour, string,
    // enum) plus the tag-style components that are written as flags.
    const entt::entity parent = a.CreateEmptyEntity(glm::vec3(1.5f, -2.25f, 3.0f),
                                                    glm::vec3(0.0f, 45.0f, 0.0f), one, "Parent");
    const entt::entity child  = a.CreateEmptyEntity(glm::vec3(0.5f, 0.0f, 0.0f), zero,
                                                    glm::vec3(2.0f, 2.0f, 2.0f), "Child");
    CHECK(a.SetParent(child, parent));
    const entt::entity nameless = a.CreateEmptyEntity(zero, zero, one, ""); // #122 - must survive
    // Entities only, no CreateBox: a box carries a primitive Renderable, and building one calls
    // into GL, which does not exist here. That is why this suite stays on empties.
    const entt::entity box = a.CreateEmptyEntity(glm::vec3(0.0f, 1.0f, 0.0f),
                                                 glm::vec3(0.0f, 30.0f, 0.0f),
                                                 glm::vec3(2.0f, 0.5f, 2.0f), "Box");

    TransformControllerComponent ctl; ctl.RotationDegPerSec = glm::vec3(0.0f, 0.0f, 1.0f); ctl.ScalePulseFrequencyHz = 42.5f;
    a.Registry.emplace<TransformControllerComponent>(parent, ctl);

    CameraComponent cam; cam.FovDegrees = 72.5f; cam.NearPlane = 0.25f; cam.FarPlane = 750.0f;
    a.Registry.emplace<CameraComponent>(child, cam);

    ColliderComponent col;
    col.Kind = ColliderComponent::Shape::Sphere;   // an enum, round-tripped by label
    col.HalfExtents = glm::vec3(1.25f, 0.0f, 0.0f);
    col.Center = glm::vec3(0.0f, 0.5f, 0.0f);
    a.Registry.emplace<ColliderComponent>(box, col);

    a.Registry.emplace<TagComponent>(nameless, TagComponent{"Ball"});
    a.Registry.emplace<LayerComponent>(box, LayerComponent{3});
    a.Registry.emplace<StaticTag>(box);
    a.Registry.emplace<DeactivatedTag>(nameless);
    a.SyncActiveInHierarchy();

    const std::string first = SceneSerializer::SaveToString(a, assetsA);
    CHECK(!first.empty());

    World b;
    AssetLibrary assetsB;
    CHECK(SceneSerializer::LoadFromString(b, assetsB, first));
    const std::string second = SceneSerializer::SaveToString(b, assetsB);

    // Compared as parsed JSON, not as text: key order and whitespace are not the contract, the
    // data is. A mismatch here means the scene came back different from the one that was saved.
    const json j1 = json::parse(first, nullptr, false);
    const json j2 = json::parse(second, nullptr, false);
    CHECK(!j1.is_discarded() && !j2.is_discarded());
    CHECK(j1 == j2);

    // Spot-check the values themselves, so a round-trip that is merely self-consistent (both
    // sides dropping the same field) still fails.
    int found = 0;
    for (entt::entity e : b.Registry.view<NameComponent>()) {
        const std::string& n = b.Registry.get<NameComponent>(e).Name;
        if (n == "Parent") {
            ++found;
            CHECK(b.Registry.all_of<TransformControllerComponent>(e));
            const auto& tc = b.Registry.get<TransformControllerComponent>(e);
            CHECK(tc.ScalePulseFrequencyHz == 42.5f && tc.RotationDegPerSec.z == 1.0f);
            CHECK(b.Registry.all_of<HierarchyComponent>(e));
            CHECK(b.Registry.get<HierarchyComponent>(e).Children.size() == 1);
        } else if (n == "Child") {
            ++found;
            CHECK(b.Registry.all_of<CameraComponent>(e));
            const auto& cc = b.Registry.get<CameraComponent>(e);
            CHECK(cc.FovDegrees == 72.5f && cc.NearPlane == 0.25f && cc.FarPlane == 750.0f);
        } else if (n == "Box") {
            ++found;
            CHECK(b.Registry.all_of<ColliderComponent>(e));
            const auto& cl = b.Registry.get<ColliderComponent>(e);
            CHECK(cl.Kind == ColliderComponent::Shape::Sphere);
            CHECK(cl.HalfExtents.x == 1.25f && cl.Center.y == 0.5f);
            CHECK(b.Registry.all_of<StaticTag>(e));
            CHECK(b.Registry.all_of<LayerComponent>(e) && b.Registry.get<LayerComponent>(e).Layer == 3);
        }
    }
    CHECK(found == 3);

    // The nameless entity is still there, still deactivated, still tagged (#122).
    int tagged = 0;
    for (entt::entity e : b.Registry.view<TagComponent>())
        if (b.Registry.get<TagComponent>(e).Tag == "Ball") {
            ++tagged;
            CHECK(b.Registry.all_of<DeactivatedTag>(e));
        }
    CHECK(tagged == 1);
}

// --- Component registry: unique names / keys, enum labels well-formed --------------------------
void TestComponentRegistry() {
    UnitTestSupport::EnsureEngineComponents();
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

} // namespace

static constexpr size_t kSnapshotBytes = 28878;
static constexpr std::uint64_t kSnapshotHash = 0x8f6b8763b0d778abull;
// The scene snapshot (SaveToString: every undo step and the scene open take one) must keep its
// exact bytes when its builder changes: the undo history compares and diffs these strings, and
// the files Save writes come from the same builder. A fixed scene - models sharing a few paths,
// .mat and embedded material slots, lights, colliders, tags, layers, parenting, empties, a
// prefab instance stub, non-ASCII paths - is
// pinned by the size and FNV-1a hash of its snapshot. A deliberate format change updates both
// (the test prints the new values).
static void Test_SceneSnapshot_BytesPinned() {
    auto fnv = [](const std::string& s) {
        std::uint64_t h = 1469598103934665603ull;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        return h;
    };
    auto build = [](World& w, int count) {
        std::vector<std::shared_ptr<MaterialAsset>> mats;
        for (int i = 0; i < 3; ++i) {
            auto m = std::make_shared<MaterialAsset>();
            // One non-ASCII name (UTF-8 "\xC3\xA9"): the cache must give what AssetPathForWrite gives.
            m->Path = ProjectPaths::Resolve("assets/Materials/w5_fixture_" + std::string(i == 2 ? "caf\xC3\xA9_" : "") + std::to_string(i) + ".mat");
            m->Name = "w5_fixture_" + std::to_string(i);
            mats.push_back(m);
        }
        auto embedded = std::make_shared<MaterialAsset>(); // no Path: written inline
        embedded->Mat.BaseColor = glm::vec3(0.25f, 0.5f, 0.75f);
        embedded->Mat.Roughness = 0.3f;
        std::vector<entt::entity> groups; // a few dozen children each, like a real scene's folders
        for (int g = 0; g < count / 30 + 1; ++g)
            groups.push_back(w.CreateEmptyEntity({1.0f * g, 2, 3}, {0, 45, 0}, {1, 1, 1}, "Group " + std::to_string(g)));
        for (int i = 0; i < count; ++i) {
            const std::string path = ProjectPaths::Resolve("assets/Props/w5_fixture_" + std::string(i % 5 == 4 ? "\xC3\xBC" : "") + std::to_string(i % 5) + ".fbx");
        entt::entity e = w.CreateModelEntity(Model::CreatePrimitive("terrain", path) /* CPU-only stand-in: no GL in unit tests */,
                                                 {i * 0.5f, 0.25f * (i % 7), -i * 0.125f}, {0, (float)(i % 360), 0},
                                                 {1, 1 + (i % 3) * 0.5f, 1}, "Prop " + std::to_string(i));
            auto& rc = w.Registry.get<RenderableComponent>(e);
            rc.Materials = {mats[i % 3], (i % 4 == 0) ? embedded : nullptr};
            if (i % 10 == 0) w.Registry.emplace<LightComponent>(e);
            if (i % 6 == 0) w.Registry.emplace<ColliderComponent>(e);
            if (i % 5 == 0) w.Registry.emplace<TagComponent>(e).Tag = "Fixture";
            if (i % 8 == 0) w.Registry.emplace<LayerComponent>(e).Layer = 3;
            if (i % 3 == 0) w.SetParent(e, groups[(size_t)(i / 3) % groups.size()]);
        }
        for (int i = 0; i < count / 4; ++i)
            w.CreateEmptyEntity({0, (float)i, 0}, {0, 0, 0}, {1, 1, 1}, "Pivot " + std::to_string(i));
        // A prefab instance: written as a stub (its .prefab is absent here), its child not at all.
        entt::entity inst = w.CreateEmptyEntity({4, 0, 4}, {0, 30, 0}, {1, 1, 1}, "Crate Stack");
        w.Registry.emplace<PrefabInstanceComponent>(inst).SourcePath =
            ProjectPaths::Resolve("assets/Prefabs/w5_fixture_\xC3\xA9.prefab");
        entt::entity part = w.CreateModelEntity(Model::CreatePrimitive("terrain", ProjectPaths::Resolve("assets/Props/w5_fixture_0.fbx")),
                                                {0, 1, 0}, {0, 0, 0}, {1, 1, 1}, "Crate");
        w.SetParent(part, inst);
    };
    {
        World w; AssetLibrary assets;
        build(w, 60);
        const std::string s = SceneSerializer::SaveToString(w, assets);
        const std::uint64_t h = fnv(s);
        std::printf("  scene snapshot: %zu bytes, fnv %016llx\n", s.size(), (unsigned long long)h);
        std::fflush(stdout);
        CHECK(s.size() == kSnapshotBytes);
        CHECK(h == kSnapshotHash);
        CHECK(s.find("\"prefabInstances\"") != std::string::npos);  // the stub is in it
        CHECK(s.find("w5_fixture_\xC3\xBC" "4") != std::string::npos);  // and a non-ASCII path
        CHECK(SceneSerializer::SaveToString(w, assets) == s); // deterministic
    }
    {   // Timing only (a Forest-sized scene): printed, not checked.
        World w; AssetLibrary assets;
        build(w, 20000);
        SceneSerializer::SaveToString(w, assets); // warm the path caches
        const auto t0 = std::chrono::steady_clock::now();
        size_t bytes = 0;
        for (int i = 0; i < 3; ++i) bytes += SceneSerializer::SaveToString(w, assets).size();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 3;
        std::printf("  scene snapshot, 20k models + 5k empties: %.1f ms (%zu bytes)\n", ms, bytes / 3);
        std::fflush(stdout);
    }
}

// A scene loads to the same thing every time (undo/redo reload the whole scene from its snapshot), the component
// blocks are read by their key with the old precedence, and a material slot follows its .mat by the GUID it saved.
// (Empties only: a box or a model would need GL for its mesh.)
static void Test_SceneReload_SameResult() {
    namespace fs = std::filesystem;
    using nlohmann::json;
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    std::error_code ec;
    const fs::path dir = TempDir() / "reload_same";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Sub", ec);

    const json scene = {
        {"formatVersion", 4},
        {"empties", json::array({
            {{"id", 0}, {"name", "Crate"}, {"position", {1, 2, 3}}, {"order", 0},
             {"tag", "Loot"}, {"layer", 3}, {"static", true}, {"active", false},
             {"collider", {{"isTrigger", true}}}, {"Health", {{"Max", 250.0}}}},
            // Both keys: the current one wins, as before.
            {{"id", 1}, {"name", "Spawn A"}, {"parentId", 0}, {"order", 1},
             {"Dummy Spawn", {{"Roam", false}}}, {"NPC Spawn", {{"Roam", true}}}},
            // Only the legacy key: still read.
            {{"id", 2}, {"name", "Spawn B"}, {"parentId", 1}, {"order", 2}, {"NPC Spawn", {{"Roam", false}}}},
            // A non-object under a component's key adds the component with its defaults, as before.
            {{"id", 3}, {"name", "Odd"}, {"order", 3}, {"Health", 5}},
        })},
    };

    AssetLibrary assets;
    World w;
    CHECK(SceneSerializer::LoadFromString(w, assets, scene.dump()));
    auto byName = [](World& world, const char* name) {
        for (auto [e, n] : world.Registry.view<NameComponent>().each()) if (n.Name == name) return e;
        return entt::entity{entt::null};
    };
    const entt::entity crate = byName(w, "Crate"), a = byName(w, "Spawn A"), b = byName(w, "Spawn B"), odd = byName(w, "Odd");
    CHECK(crate != entt::null && a != entt::null && b != entt::null && odd != entt::null);
    if (crate != entt::null) {
        const auto* hp = w.Registry.try_get<HealthComponent>(crate);
        CHECK(hp && hp->Max == 250.0f);
        CHECK((w.Registry.all_of<ColliderComponent, TagComponent, LayerComponent, StaticTag, DeactivatedTag>(crate)));
    }
    if (a != entt::null) { const auto* s = w.Registry.try_get<NpcSpawnComponent>(a); CHECK(s && !s->Roam); }
    if (b != entt::null) { const auto* s = w.Registry.try_get<NpcSpawnComponent>(b); CHECK(s && !s->Roam); }
    if (odd != entt::null) { const auto* h = w.Registry.try_get<HealthComponent>(odd); CHECK(h && h->Max == HealthComponent{}.Max); }

    // Save, reload, save: the same bytes and the same entities every time.
    const std::string first = SceneSerializer::SaveToString(w, assets);
    for (int i = 0; i < 2; ++i) {
        World again;
        CHECK(SceneSerializer::LoadFromString(again, assets, first));
        CHECK(again.Registry.view<NameComponent>().size() == w.Registry.view<NameComponent>().size());
        CHECK(SceneSerializer::SaveToString(again, assets) == first);
    }
    CHECK(SceneSerializer::LoadFromString(w, assets, first)); // in place, the way Undo does it
    CHECK(SceneSerializer::SaveToString(w, assets) == first);

    // A material slot as MaterialSlotsToJson writes it ({"guid","path"}) follows its .mat after a move.
    MaterialAsset mat;
    mat.Path = (dir / "Old.mat").string();
    mat.Mat.Roughness = 0.42f;
    CHECK(mat.Save());
    const AssetGuid g = AssetGuid::Generate();
    CHECK(AtomicFile::WriteBytes(mat.Path + ".meta", json({{"guid", g.ToString()}, {"type", "material"}}).dump(), true));
    CHECK(AssetDatabase::EnsureGuid(mat.Path) == g);
    const std::string movedMat = (dir / "Sub" / "Moved.mat").string();
    fs::rename(mat.Path, movedMat, ec);
    AssetDatabase::NotifyMoved(mat.Path, movedMat);
    RenderableComponent rc;
    SceneDetail::ReadMaterialSlots(assets, json{{"materials", json::array({json{{"guid", g.ToString()}, {"path", mat.Path}}, nullptr})}}, rc);
    CHECK(rc.Materials.size() == 2 && rc.Materials[0] && !rc.Materials[1]);
    CHECK(!rc.Materials.empty() && rc.Materials[0] &&
          AssetDatabase::PathKey(rc.Materials[0]->Path) == AssetDatabase::PathKey(movedMat));
    CHECK(!rc.Materials.empty() && rc.Materials[0] && rc.Materials[0]->Mat.Roughness == 0.42f);
    fs::remove_all(dir, ec);
}

void RegisterEngineTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"SceneSerializer::ReloadSameResult", Test_SceneReload_SameResult});
    tests.push_back({"SceneSerializer::SnapshotBytesPinned", Test_SceneSnapshot_BytesPinned});
    tests.push_back({"SamplingProfiler::SurvivesTheLoaderLock", Test_SamplingProfiler_SurvivesTheLoaderLock});
    tests.push_back({"PngWrite::LevelPerCallUnderThreads", Test_PngWrite_LevelPerCallUnderThreads});
    tests.push_back({"AssetDatabase::NonObjectMetaIsRewritten", Test_AssetDatabase_NonObjectMetaIsRewritten});
    tests.push_back({"BoneAnimChannel::FinalizeMatchesSearch", Test_BoneAnimChannel_FinalizeMatchesSearch});
    tests.push_back({"Material::HashCachePerFrame", Test_Material_HashCachePerFrame});
    tests.push_back({"Material::ExtraPropsHashPerFrame", Test_Material_ExtraPropsHashPerFrame});
    tests.push_back({"Texture::NormalMapBC5Renormalized", Test_Texture_NormalMapBC5Renormalized});
    tests.push_back({"CascadedShadowMap::SliceTestMatchesAllPairs", Test_CascadedShadowMap_SliceTestMatchesAllPairs});
    tests.push_back({"CascadedShadowMap::HardKernelAndSplit", Test_CascadedShadowMap_HardKernelAndSplit});
    tests.push_back({"PrefabInstance::HandWrittenComponentsDiffByJsonKey", Test_PrefabInstance_HandWrittenComponentsDiffByJsonKey});
    tests.push_back({"FirstPersonController::NewFieldsRoundTrip", Test_FirstPersonController_NewFieldsRoundTrip});
    tests.push_back({"FxHudSettings::RoundTrip", Test_FxHudSettings_RoundTrip});
    tests.push_back({"BloodSettings::RoundTrip", Test_BloodSettings_RoundTrip});
    tests.push_back({"GravityGun::AssistReach", Test_GravityGun_AssistReach});
    tests.push_back({"DevPanel reset", Test_DevPanelReset});
    tests.push_back({"Model::AffinePaletteMatchesGeneric", Test_Model_AffinePaletteMatchesGeneric});
    tests.push_back({"Model::AxisCorrection", Test_Model_AxisCorrection});
    tests.push_back({"SceneRenderer::WarmShaderVariantsSkipsShaderless", Test_SceneRenderer_WarmShaderVariantsSkipsShaderless});
    tests.push_back({"ShaderVariant: engine bits", Test_ShaderVariant_EngineBits});
    tests.push_back({"FxHud::SettingsDrivePureHelpers", Test_FxHud_SettingsDrivePureHelpers});
    tests.push_back({"SceneRoundTrip", TestSceneRoundTrip});
    tests.push_back({"ComponentRegistry", TestComponentRegistry});
    tests.push_back({"ComponentPreset", TestComponentPreset});
    tests.push_back({"HierarchyCycleRepair", TestHierarchyCycleRepair});
    tests.push_back({"ActiveInHierarchy", TestActiveInHierarchy});
    tests.push_back({"PlayerAcceleration", TestPlayerAcceleration});
    tests.push_back({"NearestEuler", TestNearestEuler});
    tests.push_back({"QuaternionTransformStorage", TestQuaternionTransformStorage});
    tests.push_back({"UndoSnapshotSkipsUnchangedAssetMeta", TestUndoSnapshotSkipsUnchangedAssetMeta});
    tests.push_back({"SceneLoadTakesParsedFile", TestSceneLoadTakesParsedFile});
    tests.push_back({"RotateEulerAboutLocalAxis", TestRotateEulerAboutLocalAxis});
    tests.push_back({"SpinSystemsAboutDiagonalAxis", TestSpinSystemsAboutDiagonalAxis});
}
