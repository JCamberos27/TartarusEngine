#include "UnitTestSupport.h"
#include "../Game/Components.h"
#include "../Game/ComponentRegistry.h"
#include "../Game/GravityGun.h"
#include "../Game/CombatHud.h"
#include "../Game/Combat/CombatFx.h"
#include "../Renderer/WeaponFxRenderer.h"
#include "AssetLibrary.h"
#include "SceneSerializer.h"
#include "World.h"
#include "../Renderer/Animation.h"
#include "../Renderer/SceneRenderer.h"
#include "../Renderer/MaterialAsset.h"
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
    FirstPersonControllerComponent out;
    CHECK(RoundTrip(world, assets, "First Person Controller", in, out));
    CHECK(out.StickLookDegPerSec == 222.0f && out.EyeRadius == 0.2f);
    CHECK(out.GrabRange == 55.0f && out.AssistRange == 12.0f);
    CHECK(out.AssistConeDeg == 11.0f && out.ScrollTurnDeg == 33.0f);
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
    BloodSettingsComponent out;
    CHECK(RoundTrip(world, assets, "Blood Settings", in, out));
    CHECK(!out.Enabled && out.Size == 0.7f && out.MaxSprays == 9 && out.MaxStains == 100 && out.StainLifetime == 42.0f);
    CHECK(out.DrySeconds == 30.0f && !out.Pools && !out.BodySplats && !out.GearSpatter);
}

static void Test_GravityGun_AssistReach() {
    GravityGunSettings s;
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

void RegisterEngineTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"FirstPersonController::NewFieldsRoundTrip", Test_FirstPersonController_NewFieldsRoundTrip});
    tests.push_back({"FxHudSettings::RoundTrip", Test_FxHudSettings_RoundTrip});
    tests.push_back({"BloodSettings::RoundTrip", Test_BloodSettings_RoundTrip});
    tests.push_back({"GravityGun::AssistReach", Test_GravityGun_AssistReach});
    tests.push_back({"Model::AffinePaletteMatchesGeneric", Test_Model_AffinePaletteMatchesGeneric});
    tests.push_back({"SceneRenderer::WarmShaderVariantsSkipsShaderless", Test_SceneRenderer_WarmShaderVariantsSkipsShaderless});
    tests.push_back({"FxHud::SettingsDrivePureHelpers", Test_FxHud_SettingsDrivePureHelpers});
}
