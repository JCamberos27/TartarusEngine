#include "UnitTestSupport.h"
#include "../Game/Components.h"
#include "../Game/ComponentRegistry.h"
#include "../Game/GravityGun.h"
#include "../Game/CombatHud.h"
#include "../Game/DevPanel.h"
#include "../Game/AI/NpcDirector.h"
#include "../Game/Combat/PlayerVitals.h"
#include "../Game/Combat/CombatFx.h"
#include "../Renderer/WeaponFxRenderer.h"
#include "AssetLibrary.h"
#include "SceneSerializer.h"
#include "World.h"
#include "../Renderer/Animation.h"
#include "../Renderer/SceneRenderer.h"
#include "../Renderer/MaterialAsset.h"
#include "../Renderer/ShaderVariant.h"
#include "../Renderer/CascadedShadowMap.h"
#include "../Renderer/Texture.h"
#include <filesystem>
#include <json.hpp>
#include <random>
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
    in.FeedLife = 9.0f;
    in.StreakWindow = 2.0f;
    FxHudSettingsComponent out;
    CHECK(RoundTrip(world, assets, "FX & HUD Settings", in, out));
    CHECK(out.FlashTime == 0.1f && out.PlayerFlashScale == 0.5f && out.FlameGlow == 90.0f && out.FlameScale == 2.5f);
    CHECK(out.BeamRange == 60.0f && out.BeamHalfWidth == 0.003f && out.BeamFalloff == 5.0f && out.BeamBend == 8.0f);
    CHECK(out.FeedLife == 9.0f && out.StreakWindow == 2.0f);
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
    CHECK(CombatFx::FlashPeak(s, false, false) == 18.0f);
    CHECK(CombatFx::FlashPeak(s, true, false) == 26.0f);
    CHECK(std::abs(CombatFx::FlashPeak(s, false, true) - 18.0f * 0.35f) < 1e-5f);
    CHECK(WeaponFxRenderer::BeamLength(s, 200.0f) == 150.0f && WeaponFxRenderer::BeamLength(s, 40.0f) == 40.0f);
    CHECK(CombatHud::NextStreak(s, 1, 3.9f) == 2 && CombatHud::NextStreak(s, 1, 4.1f) == 1);
    CHECK(!CombatHud::FeedExpired(s, 4.4f) && CombatHud::FeedExpired(s, 4.6f));
    // Changed values change the result.
    s.PlayerFlashScale = 0.5f;
    CHECK(CombatFx::FlashPeak(s, false, true) == 9.0f);
    s.BeamRange = 60.0f;
    CHECK(WeaponFxRenderer::BeamLength(s, 200.0f) == 60.0f);
    s.StreakWindow = 2.0f;
    CHECK(CombatHud::NextStreak(s, 1, 3.9f) == 1);
    s.FeedLife = 9.0f;
    CHECK(!CombatHud::FeedExpired(s, 4.6f) && CombatHud::FeedExpired(s, 9.5f));
}

// The kill feed and the dev panel's reset are native again (they were project C# for a while).
static void Test_CombatHud_KillFeedAndDevReset() {
    NpcDirector npcs;
    CombatHud hud;
    hud.Reset();
    for (int i = 0; i < 7; ++i) hud.OnKill(npcs, 1000u + (unsigned)i, i == 0);
    CHECK(hud.Kills() == 7 && hud.FeedRows() == 5); // the feed keeps the last five rows
    hud.Reset();
    CHECK(hud.Kills() == 0 && hud.FeedRows() == 0);

    DevPanel panel;
    panel.Open = panel.InfiniteAmmo = panel.Invisible = panel.AiOverlay = true;
    PlayerVitals vitals;
    vitals.GodMode = true;
    npcs.Frozen = npcs.HoldFire = true;
    panel.Reset(vitals, npcs);
    CHECK(!panel.Open && !panel.InfiniteAmmo && !panel.Invisible && !panel.AiOverlay);
    CHECK(!vitals.GodMode && !npcs.Frozen && !npcs.HoldFire);
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
void Test_ShaderVariant_EngineBits() {
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
void Test_SceneRenderer_WarmShaderVariantsSkipsShaderless() {
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
static void Test_World_RaycastToleratesRenderableWithoutModel() {
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity e = world.CreateEmptyEntity(zero, zero, one, "NoModel");
    world.Registry.emplace<ColliderComponent>(e);
    world.Registry.emplace<RenderableComponent>(e); // ModelRef stays null
    float dist = 0.0f;
    const entt::entity hit = world.Raycast(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 100.0f, dist);
    CHECK(hit == e);
    CHECK(std::abs(dist - 4.5f) < 1e-3f); // the top of a unit box centred on the origin
}

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

void RegisterEngineTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"BoneAnimChannel::FinalizeMatchesSearch", Test_BoneAnimChannel_FinalizeMatchesSearch});
    tests.push_back({"Material::HashCachePerFrame", Test_Material_HashCachePerFrame});
    tests.push_back({"Material::ExtraPropsHashPerFrame", Test_Material_ExtraPropsHashPerFrame});
    tests.push_back({"Texture::NormalMapBC5Renormalized", Test_Texture_NormalMapBC5Renormalized});
    tests.push_back({"CascadedShadowMap::SliceTestMatchesAllPairs", Test_CascadedShadowMap_SliceTestMatchesAllPairs});
    tests.push_back({"CascadedShadowMap::HardKernelAndSplit", Test_CascadedShadowMap_HardKernelAndSplit});
    tests.push_back({"World::RaycastToleratesRenderableWithoutModel", Test_World_RaycastToleratesRenderableWithoutModel});
    tests.push_back({"PrefabInstance::HandWrittenComponentsDiffByJsonKey", Test_PrefabInstance_HandWrittenComponentsDiffByJsonKey});
    tests.push_back({"FirstPersonController::NewFieldsRoundTrip", Test_FirstPersonController_NewFieldsRoundTrip});
    tests.push_back({"FxHudSettings::RoundTrip", Test_FxHudSettings_RoundTrip});
    tests.push_back({"BloodSettings::RoundTrip", Test_BloodSettings_RoundTrip});
    tests.push_back({"GravityGun::AssistReach", Test_GravityGun_AssistReach});
    tests.push_back({"CombatHud kill feed and DevPanel reset", Test_CombatHud_KillFeedAndDevReset});
    tests.push_back({"Model::AffinePaletteMatchesGeneric", Test_Model_AffinePaletteMatchesGeneric});
    tests.push_back({"Model::AxisCorrection", Test_Model_AxisCorrection});
    tests.push_back({"SceneRenderer::WarmShaderVariantsSkipsShaderless", Test_SceneRenderer_WarmShaderVariantsSkipsShaderless});
    tests.push_back({"ShaderVariant: engine bits", Test_ShaderVariant_EngineBits});
    tests.push_back({"FxHud::SettingsDrivePureHelpers", Test_FxHud_SettingsDrivePureHelpers});
}
