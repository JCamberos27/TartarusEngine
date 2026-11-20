#include "UnitTestSupport.h"
#include "../Renderer/SceneRenderer.h"
#include "../Renderer/Model.h"
#include "../Renderer/InstanceBatch.h"
#include "../Renderer/UploadStaging.h"
#include "../Renderer/MeshArena.h"
#include "../Renderer/HiZCuller.h"
#include "../Renderer/MaterialAsset.h"
#include "../Renderer/TextureCache.h"
#include "../Renderer/Texture.h"
#include "../Renderer/ImpostorLibrary.h"
#include "../Renderer/SkyAtmosphere.h"
#include "../Renderer/Camera.h"
#include "../Game/SkySettings.h"
#include "../Core/AtomicFile.h"
#include "../Assets/AssetDatabase.h"
#include "../Renderer/FurRenderer.h"
#include "../Renderer/VolumetricFog.h"
#include "../Renderer/LightBuffer.h"
#include "../Renderer/PointShadowMap.h"
#include "../Renderer/SpotShadowMap.h"
#include "../Renderer/DecalTypes.h"
#include "../Renderer/ClusterGrid.h"
#include "../Renderer/TemporalAA.h"
#include "../Renderer/ReflectionProbeCapture.h"
#include "../Renderer/GIData.h"
#include "../Renderer/GIVolumes.h"
#include "../Game/LightSwitch.h"
#include "../Game/SceneSerializer.h"
#include "../Game/TimeOfDay.h"
#include "AssetLibrary.h"
#include "../Game/World.h"
#include "../Game/Components.h"
#include "../Physics/Frustum.h"
#include "../Core/Paths.h"
#include "../Core/Log.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <json.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace {
// TooSmallOnScreen: the bounding sphere's projected height against the threshold.
void SmallObjectCull() {
    const glm::vec3 lo(-0.05f), hi(0.05f); // a 10 cm box: sphere radius ~0.087 m
    const float proj = 1.0f;               // 90-degree vertical field of view
    CHECK(!TooSmallOnScreen(lo, hi, glm::vec3(0, 0, 5), proj, 0.0f));     // off
    CHECK(!TooSmallOnScreen(lo, hi, glm::vec3(0, 0, 5), proj, 0.004f));   // 0.017 of the view: kept
    CHECK(TooSmallOnScreen(lo, hi, glm::vec3(0, 0, 30), proj, 0.004f));   // 0.0029: culled
    CHECK(!TooSmallOnScreen(lo, hi, glm::vec3(0, 0, 0.05f), proj, 0.5f)); // the eye inside it: never culled
    // A narrower lens (larger proj[1][1]) magnifies: kept again at the same distance.
    CHECK(!TooSmallOnScreen(lo, hi, glm::vec3(0, 0, 30), 4.0f, 0.004f));
}

// LightBuffer::DistanceFade: full strength near and inside the range sphere, a smooth monotonic fall by projected
// size, nothing once the sphere is a few pixels; the same at any distance for the same size.
void LightDistanceFade() {
    const float th = std::tan(glm::radians(37.5f)); // a 75-degree vertical FOV: 704 px focal at 1080p
    CHECK(LightBuffer::DistanceFade(5.0f, 2.0f, th) == 1.0f);   // inside the sphere
    CHECK(LightBuffer::DistanceFade(5.0f, 100.0f, th) == 1.0f); // 35 px: full
    CHECK(LightBuffer::DistanceFade(5.0f, 400.0f, th) == 0.0f); // 9 px: gone
    float prev = 1.0f;
    bool monotonic = true;
    for (float d = 100.0f; d <= 400.0f; d += 5.0f) {
        const float f = LightBuffer::DistanceFade(5.0f, d, th);
        monotonic = monotonic && f <= prev + 1e-6f && f >= 0.0f;
        prev = f;
    }
    CHECK(monotonic);
    const float mid = LightBuffer::DistanceFade(5.0f, 200.0f, th);
    CHECK(mid > 0.0f && mid < 1.0f);
    CHECK(std::abs(LightBuffer::DistanceFade(10.0f, 400.0f, th) - mid) < 1e-5f); // scale-invariant
}

std::vector<entt::entity> Gather(World& w, const VisibilityQuery& q) {
    std::vector<VisibleRenderable> out;
    GatherVisible(w, q, out);
    std::vector<entt::entity> ids;
    for (const auto& v : out) ids.push_back(v.Entity);
    std::sort(ids.begin(), ids.end());
    return ids;
}

// GatherVisible reads each renderable once per transform-cache rebuild and serves every view and light from that:
// the filters still apply per query, and changes since (a rebuild, a destroyed entity, a moved one after its
// rebuild) are picked up.
void GatherCache() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model && model->MeshCount() > 0);
    if (!model) return;
    World w;
    const entt::entity a = w.CreateModelEntity(model, glm::vec3(0, 0, -5), glm::vec3(0), glm::vec3(1), "A");
    const entt::entity b = w.CreateModelEntity(model, glm::vec3(0, 0, 5), glm::vec3(0), glm::vec3(1), "B");
    const entt::entity c = w.CreateModelEntity(model, glm::vec3(3, 0, -5), glm::vec3(0), glm::vec3(1), "C");
    w.Registry.get<RenderableComponent>(c).CastShadows = RenderableComponent::ShadowCasting::Off;
    w.RebuildWorldTransformCache();

    VisibilityQuery all;
    std::vector<entt::entity> abc{a, b, c};
    std::sort(abc.begin(), abc.end());
    CHECK(Gather(w, all) == abc);
    VisibilityQuery casts;
    casts.ShadowCasters = true;
    auto cast = Gather(w, casts);
    CHECK(cast.size() == 2 && std::find(cast.begin(), cast.end(), c) == cast.end());
    // Looking down -Z from the origin: A and C ahead, B behind.
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    const Frustum f = Frustum::FromViewProj(proj * glm::lookAt(glm::vec3(0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0)));
    VisibilityQuery view;
    view.Cull = &f;
    auto seen = Gather(w, view);
    CHECK(seen.size() == 2 && std::find(seen.begin(), seen.end(), b) == seen.end());

    // A tag added and the cache rebuilt (the next frame): it applies.
    w.Registry.emplace<DeactivatedTag>(a); // InactiveTag is derived from it by the rebuild
    w.RebuildWorldTransformCache();
    CHECK(Gather(w, all).size() == 2);
    w.Registry.remove<DeactivatedTag>(a);
    w.RebuildWorldTransformCache();
    // Moved behind the camera, next frame: culled by its new bounds.
    w.Registry.get<TransformComponent>(c).Position = glm::vec3(3, 0, 5);
    w.RebuildWorldTransformCache();
    seen = Gather(w, view);
    CHECK(seen.size() == 1 && seen[0] == a);
    // Destroyed with no rebuild in between: not handed out (the renderables changed).
    w.Registry.destroy(b);
    CHECK(Gather(w, all).size() == 2);
    // A second world never reads the first one's list.
    World other;
    other.RebuildWorldTransformCache();
    CHECK(Gather(other, all).empty());
}

// mergeByMaterial: the same geometry and bounds in fewer meshes. Needs the model library (Drive assets).
void MergeByMaterial() {
    const char* rel = "assets/Architecture/Doors/Models/Door_Op_1_Leaf_Interior.fbx";
    const std::string path = ProjectPaths::Resolve(rel);
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no model library (Drive assets)");
        return;
    }
    ModelImportSettings s;
    s.MaterialImportMode = ModelImportSettings::MaterialMode::None;
    s.ImportSkeleton = false;
    const auto loose = Model::ImportDeferred(path, s);
    s.MergeByMaterial = true;
    const auto merged = Model::ImportDeferred(path, s);
    CHECK(loose && merged);
    if (!loose || !merged) return;
    CHECK(merged->MeshCount() < loose->MeshCount());
    CHECK(merged->TriangleCount() == loose->TriangleCount());
    CHECK(glm::length(merged->BoundsMin() - loose->BoundsMin()) < 1e-4f);
    CHECK(glm::length(merged->BoundsMax() - loose->BoundsMax()) < 1e-4f);
    // one mesh per material name
    std::vector<std::string> names;
    for (int i = 0; i < merged->MeshCount(); ++i) names.push_back(merged->MeshMaterial(i).Name);
    std::sort(names.begin(), names.end());
    CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
}
// GroupInstances: equal keys share a group, in the order of their first item; kNoBatch items stand alone.
void InstanceGrouping() {
    const std::vector<std::uint64_t> keys{7, 3, 7, 9, 7, 3, kNoBatch, kNoBatch};
    InstanceGroups g;
    GroupInstances(keys.size(), [&](std::uint32_t i) { return keys[i]; },
                   [&](std::uint32_t a, std::uint32_t b) { return keys[a] == keys[b]; }, g);
    CHECK(g.Groups.size() == 5);
    CHECK(g.Order.size() == keys.size());
    auto members = [&](std::size_t n) {
        return std::vector<std::uint32_t>(g.Order.begin() + g.Groups[n].First, g.Order.begin() + g.Groups[n].First + g.Groups[n].Count);
    };
    CHECK((members(0) == std::vector<std::uint32_t>{0, 2, 4}));
    CHECK((members(1) == std::vector<std::uint32_t>{1, 5}));
    CHECK((members(2) == std::vector<std::uint32_t>{3}));
    CHECK((members(3) == std::vector<std::uint32_t>{6}));
    CHECK((members(4) == std::vector<std::uint32_t>{7}));
    // A hash collision with a different key: `same` keeps them apart.
    const std::vector<int> kinds{1, 2, 1};
    GroupInstances(kinds.size(), [](std::uint32_t) { return std::uint64_t(42); },
                   [&](std::uint32_t a, std::uint32_t b) { return kinds[a] == kinds[b]; }, g);
    CHECK(g.Groups.size() == 2 && g.Groups[0].Count == 2 && g.Groups[1].Count == 1);
    GroupInstances(0, [](std::uint32_t) { return std::uint64_t(1); }, [](std::uint32_t, std::uint32_t) { return true; }, g);
    CHECK(g.Groups.empty() && g.Order.empty());
}

// BuildIndirectBatches: one batch per key in first-appearance order, each batch's commands contiguous and in item
// order, its tag the first item's; nothing in, nothing out.
void IndirectBuilder() {
    auto item = [](std::uint64_t key, std::uint32_t tag, std::uint32_t first) {
        return IndirectItem{key, tag, {3, 1, first, 0, tag}};
    };
    const std::vector<IndirectItem> items{item(5, 0, 0), item(9, 1, 3), item(5, 2, 6), item(7, 3, 9), item(9, 4, 12), item(5, 5, 15)};
    std::vector<IndirectCommand> cmds;
    std::vector<IndirectBatch> batches;
    BuildIndirectBatches(items, cmds, batches);
    CHECK(batches.size() == 3);
    CHECK(cmds.size() == items.size());
    CHECK(batches[0].Key == 5 && batches[0].Tag == 0 && batches[0].First == 0 && batches[0].Count == 3);
    CHECK(batches[1].Key == 9 && batches[1].Tag == 1 && batches[1].First == 3 && batches[1].Count == 2);
    CHECK(batches[2].Key == 7 && batches[2].Tag == 3 && batches[2].First == 5 && batches[2].Count == 1);
    const std::vector<std::uint32_t> firsts{0, 6, 15, 3, 12, 9};
    for (std::size_t i = 0; i < cmds.size(); ++i) CHECK(cmds[i].FirstIndex == firsts[i]);
    CHECK(cmds[4].BaseInstance == 4 && cmds[4].Count == 3 && cmds[4].InstanceCount == 1);
    // Every batch's first group comes no later than the next batch's (the opaque pass draws them in that order).
    for (std::size_t b = 1; b < batches.size(); ++b) CHECK(batches[b - 1].Tag < batches[b].Tag);
    BuildIndirectBatches({}, cmds, batches);
    CHECK(cmds.empty() && batches.empty());
}

// MeshArena::FreeList: first fit, freed runs merge with both neighbours, a full list refuses.
void MeshArenaFreeList() {
    MeshArena::FreeList f(100);
    const std::uint32_t a = f.Take(10), b = f.Take(20), c = f.Take(30);
    CHECK(a == 0 && b == 10 && c == 30);
    CHECK(f.FreeUnits() == 40);
    CHECK(f.Take(41) == ~0u);
    f.Give(b, 20); // a hole between a and c
    CHECK(f.Runs() == 2);
    CHECK(f.Take(15) == 10); // first fit: the hole
    f.Give(10, 15);
    f.Give(a, 10); // merges with the hole after it
    CHECK(f.Runs() == 2);
    f.Give(c, 30); // and everything joins up again
    CHECK(f.Runs() == 1 && f.FreeUnits() == 100);
    CHECK(f.Take(100) == 0 && f.Take(1) == ~0u);
}

// HiZCuller: the occlusion boxes are padded past the object, and swaying foliage pads them further.
void HiZPieces() {
    glm::vec3 lo(0.0f), hi(1.0f);
    HiZCuller::PadBounds(lo, hi);
    CHECK(lo.x < 0.0f && hi.x > 1.0f);
    CHECK(HiZCuller::FoliageSway(0.15f) > 0.3f);
}

// UploadStaging's ring: blocks go after the head, wrap to the start up to the oldest live block, and never overlap it.
void UploadStagingRing() {
    constexpr std::size_t kNone = ~std::size_t(0);
    CHECK(UploadStaging::RingFit(1000, 0, 0, true, 400) == 0);
    CHECK(UploadStaging::RingFit(1000, 0, 0, true, 1001) == kNone);
    CHECK(UploadStaging::RingFit(1024, 400, 0, false, 300) == 512);   // after the head, aligned to 256
    CHECK(UploadStaging::RingFit(1024, 700, 300, false, 300) == 0);   // no room at the end: wraps
    CHECK(UploadStaging::RingFit(1024, 700, 300, false, 301) == kNone); // would run into the oldest block
    CHECK(UploadStaging::RingFit(1024, 100, 600, false, 100) == 256); // wrapped: between head and tail
    CHECK(UploadStaging::RingFit(1024, 100, 600, false, 400) == kNone);
    CHECK(UploadStaging::RingFit(1024, 300, 300, false, 1) == kNone); // head == tail and not empty: full
}

// The asset pump's frame share: full while editing or when a swap waits on its loads, else the 120 Hz headroom.
void AssetPumpBudget() {
    CHECK(AssetLibrary::PumpBudgetMs(false, false, 20.0) == 3.0); // editing
    CHECK(AssetLibrary::PumpBudgetMs(true, true, 20.0) == 3.0);   // an outfit swap waiting
    CHECK(AssetLibrary::PumpBudgetMs(true, false, 20.0) == 0.25); // a slow frame: the floor
    CHECK(AssetLibrary::PumpBudgetMs(true, false, 7.0) > 1.3 && AssetLibrary::PumpBudgetMs(true, false, 7.0) < 1.4);
    CHECK(AssetLibrary::PumpBudgetMs(true, false, 1.0) == 3.0);   // lots of headroom: capped
}

// Material slots batch when they draw alike: the same asset, or separate copies with the same values; a different
// colour, queue or slot count keeps them apart, and the hash agrees with the comparison.
void InstanceSlots() {
    Material::NextHashFrame();
    auto mat = [](glm::vec3 colour) {
        auto m = std::make_shared<MaterialAsset>();
        m->Mat.BaseColor = colour;
        return m;
    };
    using Slots = std::vector<std::shared_ptr<MaterialAsset>>;
    const auto red = mat(glm::vec3(1, 0, 0));
    const Slots a{red}, sameAsset{red}, copy{mat(glm::vec3(1, 0, 0))}, blue{mat(glm::vec3(0, 0, 1))};
    CHECK(SameSlots(a, sameAsset));
    CHECK(SameSlots(a, copy));
    CHECK(HashSlots(a, 5) == HashSlots(copy, 5));
    CHECK(!SameSlots(a, blue));
    CHECK(HashSlots(a, 5) != HashSlots(blue, 5));
    CHECK(!SameSlots(a, Slots{red, red}));
    CHECK(SameSlots(Slots{}, Slots{}));
    CHECK(!SameSlots(Slots{nullptr}, a));
    Slots transparent{mat(glm::vec3(1, 0, 0))};
    transparent[0]->RenderQueue = MaterialAsset::Queue::Transparent;
    CHECK(!SameSlots(a, transparent));
}

// Placed copies of one import share its data (what the batches group by); separate imports don't.
void InstanceImportId() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    const auto copy = model->CreateInstance();
    CHECK(copy->ImportId() == model->ImportId());
    const auto other = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(other && other->ImportId() != model->ImportId());
}
// The froxel jitter's blue noise: every one of the 1024 ranks exactly once, and an even scatter -
// the first eighth of the ranks never has two points as neighbours (a white-noise pick of 128 out
// of 1024 cells almost surely would).
void VolumetricBlueNoise() {
    unsigned short ranks[32 * 32];
    VolumetricFog::BuildBlueNoise(ranks);
    std::vector<int> seen(32 * 32, 0);
    for (unsigned short r : ranks) if (r < 32 * 32) ++seen[r];
    CHECK(std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; }));
    int adjacent = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            if (ranks[y * 32 + x] >= 128) continue;
            if (ranks[y * 32 + (x + 1) % 32] < 128) ++adjacent;
            if (ranks[((y + 1) % 32) * 32 + x] < 128) ++adjacent;
        }
    CHECK(adjacent == 0);
}

// TAA mode and rain wetness save and load; an older scene (no keys) keeps its MSAA / FXAA mode and stays dry, and
// a scene that never touches them doesn't gain the keys. Also: the TAA jitter is Halton (2,3) and un-jitters exactly.
void TaaAndWetnessRoundTrip() {
    AssetLibrary assets;
    World world;
    world.AntiAliasing = World::AntiAliasingMode::Taa;
    world.TaaSharpness = 0.6f;
    world.Wetness = 0.8f;
    world.PuddleAmount = 0.4f;
    world.PuddleScale = 7.5f;
    const std::string saved = SceneSerializer::SaveToString(world);
    {
        World reloaded;
        CHECK(SceneSerializer::LoadFromString(reloaded, assets, saved));
        CHECK(reloaded.AntiAliasing == World::AntiAliasingMode::Taa && reloaded.TaaSharpness == 0.6f);
        CHECK(reloaded.TaaEnabled() && reloaded.EffectiveMsaaSamples() == 1 && !reloaded.EffectiveFxaa());
        CHECK(reloaded.Wetness == 0.8f && reloaded.PuddleAmount == 0.4f && reloaded.PuddleScale == 7.5f);
        CHECK(reloaded.MsaaSamples == world.MsaaSamples); // kept for switching back
    }
    nlohmann::json j = nlohmann::json::parse(saved);
    for (const char* k : {"antiAliasing", "taaSharpness", "wetness", "puddleAmount", "puddleScale"}) CHECK(j.erase(k) == 1);
    j["fxaa"] = true;
    {
        World old;
        CHECK(SceneSerializer::LoadFromString(old, assets, j.dump()));
        CHECK(old.AntiAliasing == World::AntiAliasingMode::Fxaa && old.EffectiveFxaa() && !old.TaaEnabled());
        CHECK(old.EffectiveMsaaSamples() == old.MsaaSamples);
        CHECK(old.Wetness == 0.0f && old.PuddleAmount == 0.0f && old.PuddleScale == 4.0f && old.TaaSharpness == 0.35f);
    }
    {
        const World plain;
        CHECK(plain.AntiAliasing == World::AntiAliasingMode::Msaa && plain.EffectiveMsaaSamples() == plain.MsaaSamples);
        const nlohmann::json pj = nlohmann::json::parse(SceneSerializer::SaveToString(plain));
        CHECK(!pj.contains("antiAliasing") && !pj.contains("wetness") && !pj.contains("puddleAmount"));
    }
    CHECK(TemporalAA::HaltonJitter(1) == glm::vec2(0.0f, 1.0f / 3.0f - 0.5f));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
    const glm::vec2 j2(0.001f, -0.002f);
    const glm::mat4 jp = TemporalAA::JitterProjection(proj, j2);
    const glm::vec4 p(1.0f, 2.0f, -7.0f, 1.0f);
    const glm::vec4 a = proj * p, b = jp * p;
    CHECK(std::abs(b.x / b.w - (a.x / a.w + j2.x)) < 1e-5f && std::abs(b.y / b.w - (a.y / a.w + j2.y)) < 1e-5f);
    const glm::mat4 back = TemporalAA::JitterProjection(jp, -j2);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(std::abs(back[c][r] - proj[c][r]) < 1e-6f);
}

// The scene's one wind (SkySettings Wind*): the settings round-trip, an older scene gets the defaults, plants and clouds
// share the bearing convention (toward the bearing; north = -Z), and the culling margin grows with the scene strength.
void SceneWindSettings() {
    AssetLibrary assets;
    World world;
    world.SkySourceMode = World::SkySource::Atmosphere;
    world.Sky.CloudWindDirectionDegrees = 120.0f;
    world.Sky.WindStrength = 0.6f;
    world.Sky.WindGustiness = 0.3f;
    world.Sky.WindGustSpeed = 6.5f;
    const std::string saved = SceneSerializer::SaveToString(world);
    {
        World reloaded;
        CHECK(SceneSerializer::LoadFromString(reloaded, assets, saved));
        CHECK(reloaded.Sky.CloudWindDirectionDegrees == 120.0f && reloaded.Sky.WindStrength == 0.6f);
        CHECK(reloaded.Sky.WindGustiness == 0.3f && reloaded.Sky.WindGustSpeed == 6.5f);
    }
    nlohmann::json j = nlohmann::json::parse(saved);
    CHECK(j.contains("sky"));
    for (const char* k : {"windStrength", "windGustiness", "windGustSpeed"}) j["sky"].erase(k);
    {
        World old;
        CHECK(SceneSerializer::LoadFromString(old, assets, j.dump()));
        const SkySettings d;
        CHECK(old.Sky.WindStrength == d.WindStrength && old.Sky.WindGustiness == d.WindGustiness);
        CHECK(old.Sky.WindGustSpeed == d.WindGustSpeed && old.Sky.CloudWindDirectionDegrees == 120.0f);
    }
    const glm::vec2 north = TimeOfDay::BearingToXZ(0.0f, 0.0f), east = TimeOfDay::BearingToXZ(90.0f, 0.0f);
    CHECK(glm::length(north - glm::vec2(0.0f, -1.0f)) < 1e-5f && glm::length(east - glm::vec2(1.0f, 0.0f)) < 1e-5f);
    const glm::vec2 turned = TimeOfDay::BearingToXZ(0.0f, 90.0f); // the compass offset turns it a quarter, staying unit
    CHECK(std::abs(std::abs(turned.x) - 1.0f) < 1e-5f && std::abs(turned.y) < 1e-5f);
    const glm::vec2 savedDir = Material::WindDirection();
    const Material::WindField savedWind = Material::Wind();
    Material::WindField w;
    w.Strength = 0.5f;
    Material::SetWind(glm::vec2(3.0f, 4.0f), w);
    CHECK(glm::length(Material::WindDirection() - glm::vec2(0.6f, 0.8f)) < 1e-5f && Material::Wind().Strength == 0.5f);
    Material::SetWind(savedDir, savedWind);
    CHECK(HiZCuller::FoliageSway(0.35f, 2.0f) > HiZCuller::FoliageSway(0.35f, 1.0f));
    CHECK(HiZCuller::FoliageSway(0.0f, 0.0f) == 0.0f);
}

// Volumetric fog settings and a light's Volumetric Intensity save and load; a scene saved before
// they existed loads with the fog off and every light at 1.
void VolumetricSettingsRoundTrip() {
    AssetLibrary assets;
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity lamp = world.CreateEmptyEntity(zero, zero, one, "Lamp");
    world.Registry.emplace<LightComponent>(lamp).VolumetricIntensity = 3.5f;
    world.VolumetricEnabled = true;
    world.VolumetricDensity = 0.07f;
    world.VolumetricAnisotropy = -0.3f;
    world.VolumetricHeightFalloff = 0.25f;
    world.VolumetricBaseHeight = 2.0f;
    world.VolumetricAlbedo = glm::vec3(0.5f, 0.6f, 0.7f);
    world.VolumetricDistance = 40.0f;
    world.VolumetricQuality = 2;
    const std::string saved = SceneSerializer::SaveToString(world);
    {
        World reloaded;
        CHECK(SceneSerializer::LoadFromString(reloaded, assets, saved));
        CHECK(reloaded.VolumetricEnabled && reloaded.VolumetricQuality == 2);
        CHECK(reloaded.VolumetricDensity == 0.07f && reloaded.VolumetricAnisotropy == -0.3f);
        CHECK(reloaded.VolumetricHeightFalloff == 0.25f && reloaded.VolumetricBaseHeight == 2.0f);
        CHECK(reloaded.VolumetricAlbedo == glm::vec3(0.5f, 0.6f, 0.7f) && reloaded.VolumetricDistance == 40.0f);
        float intensity = -1.0f;
        for (auto [e, l] : reloaded.Registry.view<const LightComponent>().each()) intensity = l.VolumetricIntensity;
        CHECK(intensity == 3.5f);
    }
    // The same scene without any of the new keys: an older file.
    nlohmann::json j = nlohmann::json::parse(saved);
    for (const char* k : {"volumetricEnabled", "volDensity", "volAnisotropy", "volHeightFalloff", "volBaseHeight",
                          "volAlbedo", "volDistance", "volQuality"})
        j.erase(k);
    bool stripped = false;
    for (const char* list : {"boxes", "models", "empties"})
        if (j.contains(list) && j[list].is_array())
            for (auto& e : j[list])
                if (e.is_object() && e.contains("Light") && e["Light"].is_object())
                    stripped |= e["Light"].erase("Volumetric Intensity") > 0;
    CHECK(stripped);
    {
        World old;
        CHECK(SceneSerializer::LoadFromString(old, assets, j.dump()));
        const World defaults;
        CHECK(!old.VolumetricEnabled && old.VolumetricDensity == defaults.VolumetricDensity &&
              old.VolumetricAnisotropy == defaults.VolumetricAnisotropy && old.VolumetricQuality == 1);
        bool sawLight = false;
        for (auto [e, l] : old.Registry.view<const LightComponent>().each()) {
            sawLight = true;
            CHECK(l.VolumetricIntensity == 1.0f);
        }
        CHECK(sawLight);
    }
}

// FurRenderer::ShellCount: the shells follow the hair's height on screen - more up close, fewer far away, none
// once a hair is under a pixel - within the material's maximum.
void FurShellLod() {
    const float proj = 1.732f, h = 1080.0f, len = 0.015f; // 60-degree vertical field of view
    CHECK(FurRenderer::ShellCount(16, len, 0.5f, proj, h) == 16); // up close: the material's maximum
    const int at1 = FurRenderer::ShellCount(16, len, 1.0f, proj, h);
    const int at3 = FurRenderer::ShellCount(16, len, 3.0f, proj, h);
    const int at8 = FurRenderer::ShellCount(16, len, 8.0f, proj, h);
    CHECK(at1 >= at3 && at3 >= at8 && at1 > at8);
    CHECK(at8 >= FurRenderer::kMinShells); // still drawn: thinned to the floor, not to a single floating layer
    CHECK(FurRenderer::ShellCount(16, len, 40.0f, proj, h) == 0); // under a pixel: none
    // Monotonic all the way out.
    int last = 1 << 30;
    for (float d = 0.1f; d < 60.0f; d *= 1.1f) {
        const int n = FurRenderer::ShellCount(16, len, d, proj, h);
        CHECK(n <= last && n >= 0 && n <= 16);
        last = n;
    }
    // A smaller view needs fewer; a lower maximum caps it, below the floor too.
    CHECK(FurRenderer::ShellCount(16, len, 3.0f, proj, 540.0f) <= at3);
    CHECK(FurRenderer::ShellCount(6, len, 0.5f, proj, h) == 6);
    CHECK(FurRenderer::ShellCount(2, len, 8.0f, proj, h) == 2);
    // Off: no shells, no length, no view.
    CHECK(FurRenderer::ShellCount(0, len, 0.5f, proj, h) == 0);
    CHECK(FurRenderer::ShellCount(16, 0.0f, 0.5f, proj, h) == 0);
    CHECK(FurRenderer::ShellCount(16, len, 0.5f, proj, 0.0f) == 0);
    CHECK(FurRenderer::ShellCount(16, len, 0.0f, proj, h) == 16); // the eye at the surface: clamped, not inf
}

// The shell-fur properties save to a .mat and load back; a material without fur writes none of them, and the
// property accessors (the Inspector's path) reach the same fields.
void FurMaterialRoundTrip() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / UnitTestSupport::TempName("tartarus_fur_test");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    MaterialAsset ma;
    ma.Path = (dir / "M_Fur.mat").string();
    ma.Name = "M_Fur";
    ma.ShaderPath = "engine://Standard.shader";
    ma.Mat.Fur.Enabled = true;
    ma.Mat.Fur.Length = 0.018f;
    ma.Mat.Fur.Density = 300.0f;
    ma.Mat.Fur.Shells = 12;
    ma.Mat.Fur.Gravity = 0.5f;
    ma.Mat.Fur.Tint = glm::vec3(0.9f, 0.8f, 0.7f);
    ma.Mat.Fur.Rim = 0.6f;
    ma.FurMaskPath = "assets/Textures/T_Fur_Mask.png"; // any path: only the string round-trips
    CHECK(ma.Save());
    const auto back = MaterialAsset::Load(ma.Path);
    CHECK(back != nullptr);
    if (back) {
        const Material::FurDesc& f = back->Mat.Fur;
        CHECK(f.Enabled && f.Length == 0.018f && f.Density == 300.0f && f.Shells == 12 && f.Gravity == 0.5f &&
              f.Tint == glm::vec3(0.9f, 0.8f, 0.7f) && f.Rim == 0.6f);
        CHECK(back->FurMaskPath == ma.FurMaskPath);
    }
    // No fur: none of its keys in the file, and the defaults back.
    MaterialAsset plain;
    plain.Path = (dir / "M_Plain.mat").string();
    plain.Name = "M_Plain";
    plain.ShaderPath = "engine://Standard.shader";
    CHECK(plain.Save());
    {
        std::ifstream in(plain.Path);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("_Fur") == std::string::npos);
    }
    const auto plainBack = MaterialAsset::Load(plain.Path);
    CHECK(plainBack && plainBack->Mat.Fur == Material::FurDesc{});
    // The Inspector's accessors: built-in, kept off the surface program's bindings, clamped like the loader.
    Material m;
    CHECK(MaterialAsset::IsBuiltinProp("_FurMask") && MaterialAsset::IsFurProp("_FurShells") && !MaterialAsset::IsFurProp("_Sheen"));
    MaterialAsset::SetBool(m, "_FurEnabled", true);
    MaterialAsset::SetFloat(m, "_FurLength", 0.02f);
    MaterialAsset::SetInt(m, "_FurShells", 99);
    MaterialAsset::SetColor(m, "_FurTint", glm::vec3(0.5f));
    CHECK(MaterialAsset::GetBool(m, "_FurEnabled") && MaterialAsset::GetFloat(m, "_FurLength") == 0.02f);
    CHECK(MaterialAsset::GetInt(m, "_FurShells") == Material::kMaxFurShells);
    CHECK(MaterialAsset::GetColor(m, "_FurTint") == glm::vec3(0.5f));
    std::filesystem::remove_all(dir, ec);
}

// Reflection probe room captures: each cube face's view looks down its GL face axis and is a pure rotation (culling
// holds); only probes whose volume changed are queued again. Puddle SSR: off by default, saved only when on, and a
// probe's Capture Scene defaults on and survives a save.
void ProbeCaptureAndPuddleSsr() {
    static const glm::vec3 kAxis[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const glm::vec3 pos(1.0f, 2.0f, 3.0f);
    for (int f = 0; f < 6; ++f) {
        const glm::mat4 v = ReflectionProbeCapture::FaceView(pos, f);
        const glm::vec4 ahead = v * glm::vec4(pos + kAxis[f], 1.0f); // straight ahead = view -Z
        CHECK(std::abs(ahead.x) < 1e-4f && std::abs(ahead.y) < 1e-4f && std::abs(ahead.z + 1.0f) < 1e-4f);
        CHECK(std::abs(glm::determinant(glm::mat3(v)) - 1.0f) < 1e-4f);
    }
    using V = ReflectionProbeCapture::Volume;
    const std::vector<V> have = {{{0, 0, 0}, {1, 1, 1}}, {{5, 0, 0}, {2, 2, 2}}};
    std::vector<V> want = have;
    want[1].Center.x += 0.5f;
    want.push_back({{9, 0, 0}, {1, 1, 1}});
    const std::vector<bool> ch = ReflectionProbeCapture::Changed(have, want);
    CHECK(ch.size() == 3 && !ch[0] && ch[1] && ch[2]);
    CHECK(ReflectionProbeCapture::Changed(have, have) == std::vector<bool>(2, false));

    AssetLibrary assets;
    World world;
    CHECK(!world.PuddleSsr);
    const entt::entity probe = world.Registry.create();
    world.Registry.emplace<TransformComponent>(probe);
    CHECK(world.Registry.emplace<ReflectionProbeComponent>(probe).CaptureScene);
    CHECK(!nlohmann::json::parse(SceneSerializer::SaveToString(world)).contains("puddleSsr"));
    world.PuddleSsr = true;
    world.Registry.get<ReflectionProbeComponent>(probe).CaptureScene = false;
    const std::string saved = SceneSerializer::SaveToString(world);
    World reloaded;
    CHECK(SceneSerializer::LoadFromString(reloaded, assets, saved));
    CHECK(reloaded.PuddleSsr);
    int probes = 0;
    for (auto [e, rp] : reloaded.Registry.view<const ReflectionProbeComponent>().each()) { ++probes; CHECK(!rp.CaptureScene); }
    CHECK(probes == 1);
}

// GI: L1 SH projection of known environments through the cube texels the baker reads back.
void GISphericalHarmonics() {
    constexpr int kSize = 16;
    constexpr float kPi = 3.14159265f;
    // The faces cover the sphere once, each face's centre texel looks down its axis.
    float total = 0.0f;
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) total += GI::CubeTexelSolidAngle(x, y, kSize);
    CHECK(std::abs(total * 6.0f - 4.0f * kPi) < 1e-3f);
    const glm::vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        const glm::vec3 d = GI::CubeTexelDir(f, kSize / 2, kSize / 2, kSize);
        CHECK(glm::dot(d, axes[f]) > 0.99f);
    }
    // GL's face convention: +X's texel row 0 looks up (+Y is -t), +Y's row 0 looks to -Z, +Z's column 0 to -X.
    CHECK(GI::CubeTexelDir(0, kSize / 2, 0, kSize).y > 0.5f);
    CHECK(GI::CubeTexelDir(2, kSize / 2, 0, kSize).z < -0.5f);
    CHECK(GI::CubeTexelDir(4, 0, kSize / 2, kSize).x < -0.5f);

    auto project = [&](auto radiance) {
        GI::Projector p;
        for (int f = 0; f < 6; ++f)
            for (int y = 0; y < kSize; ++y)
                for (int x = 0; x < kSize; ++x) {
                    const glm::vec3 d = GI::CubeTexelDir(f, x, y, kSize);
                    p.Add(d, radiance(d), GI::CubeTexelSolidAngle(x, y, kSize));
                }
        return p.Finish();
    };
    // A uniform environment of radiance L reads L for every normal (the sky irradiance cube's convention).
    const GI::ProbeSH flat = project([](const glm::vec3&) { return glm::vec3(1.0f, 2.0f, 3.0f); });
    for (const glm::vec3& n : axes) CHECK(glm::length(GI::Evaluate(flat, n) - glm::vec3(1, 2, 3)) < 2e-3f);
    // A cosine lobe about +Z (radiance max(0, d.z)): L1 gives E/pi = 1/4 + 1/3 n.z, so 7/12 facing it, 1/4 side-on,
    // nothing facing away.
    const GI::ProbeSH lobe = project([](const glm::vec3& d) { return glm::vec3(std::max(d.z, 0.0f)); });
    CHECK(std::abs(GI::Evaluate(lobe, {0, 0, 1}).x - 7.0f / 12.0f) < 0.01f);
    CHECK(std::abs(GI::Evaluate(lobe, {1, 0, 0}).x - 0.25f) < 0.01f);
    CHECK(GI::Evaluate(lobe, {0, 0, -1}).x == 0.0f);
    CHECK(GI::Evaluate(lobe, {0, 0, 1}).y == GI::Evaluate(lobe, {0, 0, 1}).x);
}

// A synthetic bake: probes lit by the analytic falloff of a point at the box's +X end. Grid, lookup, fade, file
// round trip, the invalid-probe fill and the GIVolumes gather with circuit weights.
// Shader constants that mirror C++ ones: each GLSL copy matches its C++ source, so a change to one side that misses
// the other fails here instead of reading out of range on the GPU.
void ShaderConstantsMatch() {
    auto src = [](const char* name) {
        std::ifstream in(EnginePaths::Resolve(std::string("assets/shaders/") + name), std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    auto has = [](const std::string& text, const std::string& line) { return text.find(line) != std::string::npos; };
    const std::string grid = src("ClusterGrid.glsl"), cull = src("ClusterCull.comp.glsl");
    const std::string decal = src("ClusterDecalCull.comp.glsl"), shading = src("ModelShading.glsl");
    const std::string fog = src("VolumetricInject.comp.glsl");
    if (grid.empty() || cull.empty() || decal.empty() || shading.empty() || fog.empty()) {
        UnitTestSupport::Skip("no shaders next to the exe");
        return;
    }
    const auto n = [](int v) { return std::to_string(v); };
    CHECK(has(grid, "const uint C_GX = " + n(ClusterGrid::GRID_X) + "u, C_GY = " + n(ClusterGrid::GRID_Y) + "u, C_GZ = " +
                        n(ClusterGrid::GRID_Z) + "u;"));
    CHECK(has(cull, "const uint GLOBAL_CAPACITY = " + n(ClusterGrid::GLOBAL_INDEX_CAPACITY) + "u;"));
    CHECK(has(cull, "const uint MAX_LIGHTS = " + n(LightBuffer::kMaxLights) + "u;"));
    CHECK(has(decal, "const uint MAX_PER_CLUSTER = " + n(Decals::MAX_PER_CLUSTER) + "u;"));
    for (const std::string* text : {&shading, &fog}) {
        CHECK(has(*text, "uniform mat4 uSpotShadowVP[" + n(SpotShadowMap::kMaxSpots) + "];"));
        CHECK(has(*text, "uniform float uPointShadowFar[" + n(PointShadowMap::kMaxPoints) + "];"));
    }
}

void GIVolumeLookup() {
    // Box, grid and fade.
    const glm::mat4 xf = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(2, 1, -3)), glm::radians(90.0f), glm::vec3(0, 1, 0));
    const GI::Box box = GI::Box::FromTransform(xf, glm::vec3(4, 3, 4));
    CHECK(glm::length(box.Half - glm::vec3(2, 1.5f, 2)) < 1e-5f);
    CHECK(GI::GridDims(box.Half, 0.5f) == glm::ivec3(9, 7, 9));
    CHECK(GI::GridDims(glm::vec3(0.1f), 0.5f) == glm::ivec3(2));
    CHECK(glm::length(box.ToWorld(box.ToLocal({3, 0.5f, -2})) - glm::vec3(3, 0.5f, -2)) < 1e-5f);
    CHECK(box.Fade({2, 1, -3}, 0.3f) == 1.0f);                 // centre
    CHECK(box.Fade({2, 1, -3 + 2.0f - 0.15f}, 0.3f) > 0.45f);   // half way through the fade (local x is world -z)
    CHECK(box.Fade({2, 1, -3 + 2.0f - 0.15f}, 0.3f) < 0.55f);
    CHECK(box.Fade({2, 1, -3 + 2.5f}, 0.3f) == 0.0f);          // outside
    const glm::ivec3 dims = GI::GridDims(box.Half, 0.5f);
    CHECK(glm::length(GI::ProbeLocal({0, 0, 0}, dims, box.Half) + box.Half) < 1e-5f);
    CHECK(glm::length(GI::ProbeLocal(dims - 1, dims, box.Half) - box.Half) < 1e-5f);

    GI::VolumeBake vb;
    vb.Name = "Living";
    vb.Bounds = box;
    vb.Dims = dims;
    GI::Layer lamp{"Living Lamp", {}}, outside{"", {}};
    for (int z = 0; z < dims.z; ++z)
        for (int y = 0; y < dims.y; ++y)
            for (int x = 0; x < dims.x; ++x) {
                const glm::vec3 q = GI::ProbeLocal({x, y, z}, dims, box.Half);
                const float e = 1.0f / (1.0f + glm::dot(q - glm::vec3(2, 0, 0), q - glm::vec3(2, 0, 0)));
                GI::ProbeSH sh;
                for (int c = 0; c < 3; ++c) sh.c[c] = glm::vec4(e, 0, 0, 0);
                lamp.Probes.push_back(sh);
                GI::ProbeSH o;
                for (int c = 0; c < 3; ++c) o.c[c] = glm::vec4(0.1f, 0, 0.05f, 0);
                outside.Probes.push_back(o);
            }
    vb.Layers = {lamp, outside};
    // Trilinear between probes: on a probe exactly its value, between two the mean (DC only, so any normal).
    const glm::vec3 p0 = GI::ProbeLocal({8, 3, 4}, dims, box.Half), p1 = GI::ProbeLocal({7, 3, 4}, dims, box.Half);
    const float e0 = vb.Layers[0].Probes[GI::ProbeIndex({8, 3, 4}, dims)].c[0].x;
    const float e1 = vb.Layers[0].Probes[GI::ProbeIndex({7, 3, 4}, dims)].c[0].x;
    CHECK(std::abs(vb.Sample(0, p0, {0, 1, 0}).x - e0) < 1e-5f);
    CHECK(std::abs(vb.Sample(0, (p0 + p1) * 0.5f, {0, 1, 0}).x - 0.5f * (e0 + e1)) < 1e-5f);
    CHECK(vb.Sample(0, p0, {0, 1, 0}).x > vb.Sample(0, -p0, {0, 1, 0}).x); // brighter by the light

    // File round trip (16-bit floats: within half precision).
    GI::BakeFile file;
    file.Volumes.push_back(vb);
    GI::BakeFile back;
    const auto bytes = GI::Serialize(file);
    CHECK(bytes.size() > (std::size_t)dims.x * dims.y * dims.z * 24 * 2);
    CHECK(GI::Deserialize(back, bytes.data(), bytes.size()));
    CHECK(back.Volumes.size() == 1 && back.Find("Living") && back.Volumes[0].Dims == dims);
    CHECK(back.Volumes[0].Layers.size() == 2 && back.Volumes[0].Layers[1].Circuit.empty());
    CHECK(std::abs(back.Volumes[0].Layers[0].Probes[5].c[1].x - lamp.Probes[5].c[1].x) < 1e-3f);
    CHECK(!GI::Deserialize(back, bytes.data(), bytes.size() / 2)); // truncated
    const std::string path = (std::filesystem::temp_directory_path() / UnitTestSupport::TempName("tartarus_gi_test", ".gi")).string();
    CHECK(GI::Save(file, path));
    const auto cached = GI::Cached(path);
    CHECK(cached && cached->Find("Living"));
    CHECK(GI::Cached(path) == cached); // unchanged on disk: the same load
    std::filesystem::remove(path);
    GI::ClearCache();

    // Invalid probes (inside a wall) take their valid neighbours' light.
    {
        const glm::ivec3 d(3, 1, 1);
        std::vector<GI::ProbeSH> pr(3);
        pr[0].c[0].x = 1.0f; pr[1].c[0].x = -5.0f; pr[2].c[0].x = 3.0f;
        GI::FillInvalid(pr, {true, false, true}, d);
        CHECK(std::abs(pr[1].c[0].x - 2.0f) < 1e-6f);
        std::vector<GI::ProbeSH> chain(4);
        chain[0].c[2].x = 4.0f;
        GI::FillInvalid(chain, {true, false, false, false}, glm::ivec3(4, 1, 1));
        CHECK(std::abs(chain[3].c[2].x - 4.0f) < 1e-6f); // spreads until every reachable probe is filled
    }
    CHECK((GI::SplitCircuits(" Living ; Hall,,Living;") == std::vector<std::string>{"Living", "Hall"}));

    // Atlas packing: rows along x up to the width, then along z.
    glm::ivec3 atlas;
    const auto org = GIVolumes::Pack({{9, 7, 27}, {9, 7, 27}, {5, 4, 12}}, 16, atlas);
    CHECK(org[0] == glm::ivec3(0, 0, 0) && org[1] == glm::ivec3(0, 0, 27) && org[2] == glm::ivec3(9, 0, 27));
    CHECK(atlas == glm::ivec3(14, 7, 54));

    // The runtime: a GI Volume baked with a circuit layer and an Outside layer; the circuit's weight follows its lights.
    World world;
    const entt::entity vol = world.Registry.create();
    world.Registry.emplace<TransformComponent>(vol);
    world.Registry.emplace<NameComponent>(vol, NameComponent{"Living"});
    auto& gc = world.Registry.emplace<GIVolumeComponent>(vol);
    gc.Size = glm::vec3(4, 3, 4);
    gc.Circuits = "Living Lamp";
    const entt::entity lampE = world.Registry.create();
    world.Registry.emplace<TransformComponent>(lampE);
    world.Registry.emplace<LightComponent>(lampE);
    world.Registry.emplace<LightCircuitComponent>(lampE).Circuit = "Living Lamp";
    world.Registry.emplace<LightSwitchComponent>(world.Registry.create()).Circuit = "Living Lamp";
    world.RebuildWorldTransformCache();
    CHECK(GIVolumes::CircuitWeights(world).at("Living Lamp") == 1.0f);

    GI::BakeFile synth;
    GI::VolumeBake sv = vb;
    sv.Bounds = GI::Box::FromTransform(glm::mat4(1.0f), gc.Size);
    synth.Volumes.push_back(sv);
    GIVolumes gi;
    gi.SetOverride(std::make_shared<GI::BakeFile>(synth));
    gi.Gather(world);
    CHECK(gi.Count() == 1 && gi.Volumes()[0].ReplacesSky);
    CHECK(gi.Volumes()[0].Weights == (std::vector<float>{1.0f, 1.0f}));
    float fade = 0.0f;
    const glm::vec3 lit = gi.SampleCPU({1.5f, 0, 0}, {0, 1, 0}, &fade);
    CHECK(fade == 1.0f && lit.x > 0.1f);
    // Switched off (Play's switch, LightSwitch::Set): only the Outside layer is left.
    LightSwitch::SetAll(world, false);
    CHECK(GIVolumes::CircuitWeights(world).at("Living Lamp") == 0.0f);
    gi.Gather(world);
    CHECK(gi.Volumes()[0].Weights == (std::vector<float>{0.0f, 1.0f}));
    const glm::vec3 dark = gi.SampleCPU({1.5f, 0, 0}, {0, 1, 0});
    CHECK(std::abs(dark.x - 0.15f) < 1e-4f && dark.x < lit.x); // the Outside: 0.1 + 0.05 n.y
    LightSwitch::SetAll(world, true);
    gi.Gather(world);
    CHECK(gi.Volumes()[0].Weights[0] == 1.0f);
    // Solo (the baker's bounce two): only the named circuit shines.
    const std::string outsideName;
    gi.SetSolo(&outsideName);
    gi.Gather(world);
    CHECK(gi.Volumes()[0].Weights == (std::vector<float>{0.0f, 1.0f}));
    gi.SetSolo(nullptr);
    // Outside the box, or a grid that no longer matches the bake: nothing.
    gi.Gather(world);
    float outFade = 1.0f;
    CHECK(gi.SampleCPU({0, 0, 3.0f}, {0, 1, 0}, &outFade) == glm::vec3(0.0f) && outFade == 0.0f);
    gc.Spacing = 0.25f;
    gi.Gather(world);
    CHECK(gi.Count() == 0);

    // More volumes than 32 (the Forest house has 46): every one is kept, and a draw's mask reaches the high ones.
    {
        World many;
        GI::BakeFile mf;
        for (int i = 0; i < 46; ++i) {
            const entt::entity e = many.Registry.create();
            auto& t = many.Registry.emplace<TransformComponent>(e);
            t.Position = glm::vec3(10.0f * i, 0.0f, 0.0f);
            const std::string nm = "V" + std::to_string(i);
            many.Registry.emplace<NameComponent>(e, NameComponent{nm});
            auto& c = many.Registry.emplace<GIVolumeComponent>(e);
            c.Size = glm::vec3(4, 3, 4);
            GI::VolumeBake b = sv;
            b.Name = nm;
            b.Bounds.Center = t.Position;
            mf.Volumes.push_back(b);
        }
        many.RebuildWorldTransformCache();
        GIVolumes g;
        g.SetOverride(std::make_shared<GI::BakeFile>(mf));
        g.Gather(many);
        CHECK(g.Count() == 46);
        const std::uint64_t m = g.MaskFor(glm::vec3(449, 0, 0), glm::vec3(451, 1, 1)); // only V45 is there
        CHECK(m != 0 && (m & (m - 1)) == 0);
        int bit = 0;
        while (!((m >> bit) & 1ull)) ++bit;
        CHECK(g.Volumes()[bit].Name == "V45");
        CHECK(g.MaskFor(glm::vec3(-1000), glm::vec3(1000)) == g.AllMask());
        CHECK(g.AllMask() == (1ull << 46) - 1ull);
    }

    // The component saves and loads.
    gc.Spacing = 0.5f;
    gc.BakeFile = "scenes/Test.gi";
    gc.Outside = true;
    AssetLibrary assets;
    World reloaded;
    CHECK(SceneSerializer::LoadFromString(reloaded, assets, SceneSerializer::SaveToString(world)));
    int n = 0;
    for (auto [e, g] : reloaded.Registry.view<const GIVolumeComponent>().each()) {
        ++n;
        CHECK(g.Circuits == "Living Lamp" && g.BakeFile == "scenes/Test.gi" && g.Outside && g.Size == glm::vec3(4, 3, 4));
    }
    CHECK(n == 1);
}

} // namespace

// Moved from UnitTests.cpp (pure move).
namespace {
using json = nlohmann::json;
using UnitTestSupport::TempDir;

void TestCameraRoll() {
    auto near = [](glm::vec3 a, glm::vec3 b) { return glm::length(a - b) < 1e-4f; };
    Camera cam; // yaw -90: looking down -Z, right = +X, up = +Y
    CHECK(near(cam.Right(), {1, 0, 0}));
    CHECK(near(cam.Up(), {0, 1, 0}));
    const glm::mat4 level = cam.ViewMatrix();
    cam.Roll = 90.0f;
    CHECK(near(cam.Front(), {0, 0, -1}));   // roll never changes where the camera looks
    CHECK(near(cam.Right(), {0, 1, 0}));
    CHECK(near(cam.Up(), {-1, 0, 0}));
    // A point to the camera's rolled right lands on view-space +X.
    const glm::vec4 v = cam.ViewMatrix() * glm::vec4(cam.Position + glm::vec3(0, 1, -5), 1.0f);
    CHECK(v.x > 0.99f && std::abs(v.y) < 1e-4f);
    cam.Roll = 0.0f;
    const glm::mat4 back = cam.ViewMatrix();
    bool same = true;
    for (int c = 0; c < 4; ++c) same &= near(glm::vec3(level[c]), glm::vec3(back[c]));
    CHECK(same);
}

// #369 - a texture a .mat names as a data map defaults to linear (and a normal map to the NormalMap
// type) when its .meta has no importer block, instead of the sRGB colour default that bends normals.
// An explicit importer block still wins. Only the settings step is exercised - it needs no GL.
void TestMaterialTextureDefaults() {
    namespace fs = std::filesystem;
    const fs::path dir = TempDir() / "mat_tex_defaults";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    auto make = [&](const char* name, const char* metaJson) {
        const fs::path p = dir / name;
        std::ofstream(p, std::ios::binary) << "not decoded by this test";
        if (metaJson) std::ofstream(fs::path(p.string() + ".meta"), std::ios::binary) << metaJson;
        return p.generic_string();
    };
    const char* bare = R"({"guid":"a1b2c3d4e5f60718","metaVersion":1,"type":"texture"})";
    AssetLibrary assets;
    using Use = AssetLibrary::TextureUse;

    // A bare sidecar: normal maps become linear NormalMap textures, other data maps linear.
    const std::string normal = make("n.png", bare), rough = make("r.png", bare), albedo = make("a.png", bare);
    assets.ResolveTextureSettings(normal, Use::Normal);
    assets.ResolveTextureSettings(rough, Use::Data);
    assets.ResolveTextureSettings(albedo, Use::Color);
    CHECK(!assets.GetTextureSettings(normal).IsSRGB);
    CHECK(assets.GetTextureSettings(normal).TextureType == TextureImportSettings::Type::NormalMap);
    CHECK(!assets.GetTextureSettings(rough).IsSRGB);
    CHECK(assets.GetTextureSettings(rough).TextureType == TextureImportSettings::Type::Default);
    // Colour maps keep the sRGB default, and nothing is remembered for them.
    CHECK(assets.GetTextureSettings(albedo).IsSRGB);
    CHECK(assets.TextureSettingsMap().count(albedo) == 0);

    // No .meta at all behaves the same as a bare one.
    const std::string noMeta = make("m.png", nullptr);
    assets.ResolveTextureSettings(noMeta, Use::Normal);
    CHECK(!assets.GetTextureSettings(noMeta).IsSRGB);

    // An explicit importer block is an override: even a normal-map slot keeps what the file says.
    const std::string forced = make("f.png", R"({"guid":"0011223344556677","type":"texture","importer":{"isSRGB":true,"textureType":0,"anisoLevel":2}})");
    assets.ResolveTextureSettings(forced, Use::Normal);
    CHECK(assets.GetTextureSettings(forced).IsSRGB);
    CHECK(assets.GetTextureSettings(forced).AnisoLevel == 2);

    // Settings already known in memory are never replaced by a later, different use.
    assets.ResolveTextureSettings(rough, Use::Normal);
    CHECK(assets.GetTextureSettings(rough).TextureType == TextureImportSettings::Type::Default);

    // Nothing was written to any sidecar.
    CHECK(ReadAll(dir / "n.png.meta") == bare);
    fs::remove_all(dir, ec);
}

void TestLodGroup() {
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity group = world.CreateEmptyEntity(zero, zero, one, "Group");
    world.Registry.emplace<LODGroupComponent>(group).Size = 2.0f;
    entt::entity lv[3];
    for (int i = 0; i < 3; ++i) {
        lv[i] = world.CreateEmptyEntity(zero, zero, one, "LOD" + std::to_string(i));
        world.Registry.emplace<RenderableComponent>(lv[i]);
        CHECK(world.SetParent(lv[i], group));
    }
    // 90 degree FOV: yScale = 1, so a 2 m object at distance d is 1/d of the view tall.
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
    auto shown = [&](float dist) {
        CHECK(world.ApplyLod(glm::vec3(0.0f, 0.0f, dist), proj) == 1);
        int visible = -1, count = 0;
        for (int i = 0; i < 3; ++i)
            if (!world.Registry.all_of<LodCulledTag>(lv[i])) { visible = i; ++count; }
        return count <= 1 ? visible : -2;
    };
    CHECK(shown(1.0f) == 0);   // 100% of the view  -> LOD 0 (>= 0.6)
    CHECK(shown(2.0f) == 1);   // 50%               -> LOD 1 (>= 0.3)
    CHECK(shown(5.0f) == 2);   // 20%               -> LOD 2 (>= 0.1)
    CHECK(shown(20.0f) == -1); // 5%, below the last used level -> culled
    world.Registry.destroy(group);
    CHECK(world.ApplyLod(zero, proj) == 0); // no groups left: nothing stays tagged
    CHECK(world.Registry.view<LodCulledTag>().empty());
}

// Octahedral impostors (ImpostorLibrary): past the start distance a matched group shows its impostor - every level
// tagged, the group listed by ForEachImpostor - held a little either way, never where the levels cull it, and the
// hand-over cross-fades both ways like a level change.
void TestLodImpostor() {
    ImpostorLibrary::TestOverride(0, 10.0f);
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity group = world.CreateEmptyEntity(zero, zero, one, "Group");
    world.Registry.emplace<LODGroupComponent>(group).Size = 4.0f; // 4 m: LOD 2 from 6.7 m, culled past 20 m
    entt::entity lv[3];
    for (int i = 0; i < 3; ++i) {
        lv[i] = world.CreateEmptyEntity(zero, zero, one, "LOD" + std::to_string(i));
        world.Registry.emplace<RenderableComponent>(lv[i]);
        world.SetParent(lv[i], group);
    }
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
    world.RebuildWorldTransformCache();
    auto impostor = [&](float* fade = nullptr) {
        int n = 0;
        world.ForEachImpostor([&](entt::entity g, int set, float f) {
            n += g == group && set == 0;
            if (fade) *fade = f;
        });
        return n == 1;
    };
    auto tagged = [&](int i) { return world.Registry.all_of<LodCulledTag>(lv[i]); };
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 1.0f), proj);
    CHECK(!impostor() && !tagged(0) && tagged(1) && tagged(2));
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 9.0f), proj); // LOD 2, short of 10 m
    CHECK(!impostor() && !tagged(2));
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 12.0f), proj); // past it: the impostor, every level tagged
    CHECK(impostor() && tagged(0) && tagged(1) && tagged(2));
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 9.0f), proj); // back on LOD 2
    CHECK(!impostor() && !tagged(2));
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 25.0f), proj); // the levels cull it there: so does the impostor
    CHECK(!impostor() && tagged(0) && tagged(1) && tagged(2));

    // The hand-over fades: LOD 2 out as the impostor comes in, then the other way.
    world.LodSmooth = true;
    world.LodClockOverride = 10.0;
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 9.0f), proj);
    world.LodClockOverride += 1.0;
    world.RebuildWorldTransformCache();
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 12.0f), proj);
    float fade = 0.0f;
    CHECK(impostor(&fade) && fade > 0.0f && world.LodFadeOf(lv[2]) < 0.0f && !tagged(2));
    world.LodClockOverride += World::kLodFadeSeconds + 0.1;
    world.RebuildWorldTransformCache();
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 12.0f), proj);
    CHECK(impostor(&fade) && fade == 0.0f && world.LodFadeOf(lv[2]) == 0.0f && tagged(2));
    world.LodClockOverride += 1.0;
    world.RebuildWorldTransformCache();
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 9.8f), proj); // held just inside the start (smooth views hold a level)
    CHECK(impostor(&fade) && fade == 0.0f && tagged(2));
    world.LodClockOverride += 1.0;
    world.RebuildWorldTransformCache();
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 9.0f), proj);
    CHECK(impostor(&fade) && fade < 0.0f && world.LodFadeOf(lv[2]) > 0.0f && !tagged(2));
    world.LodClockOverride += World::kLodFadeSeconds + 0.1;
    world.RebuildWorldTransformCache();
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 9.0f), proj);
    CHECK(!impostor() && world.LodFadeOf(lv[2]) == 0.0f && !tagged(2));
    ImpostorLibrary::TestOverride(-1, 0.0f);
}

// Opening another scene: the editor keeps its World and the scene loader clears the registry in place, so nothing of
// the outgoing scene's LOD state may outlive it. A scene with no LOD groups at all used to keep the outgoing scene's
// records: every group then standing on its impostor stayed listed by ForEachImpostor, and ImpostorRenderer drew it
// at the origin (its group gone, GetCachedWorldTransform answers identity) - a Forest tree standing in AnimTestbed.
void TestLodImpostorSceneSwitch() {
    ImpostorLibrary::TestOverride(0, 10.0f);
    const glm::vec3 zero(0.0f), one(1.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
    const glm::vec3 eye(0.0f, 0.0f, 12.0f); // past the 10 m start: a 4 m group on LOD 2 shows its impostor
    std::string forest, testbed, grove;
    {
        // "Forest": a LOD group, three levels under it.
        World a;
        AssetLibrary assets;
        const entt::entity group = a.CreateEmptyEntity(zero, zero, one, "Tree");
        a.Registry.emplace<LODGroupComponent>(group).Size = 4.0f;
        for (int i = 0; i < 3; ++i) a.SetParent(a.CreateEmptyEntity(zero, zero, one, "LOD" + std::to_string(i)), group);
        forest = SceneSerializer::SaveToString(a, assets);
        // "Grove": the same group further off, and a second one - a scene with groups of its own.
        a.Registry.get<TransformComponent>(group).Position = glm::vec3(5.0f, 0.0f, 0.0f);
        const entt::entity other = a.CreateEmptyEntity(glm::vec3(-5.0f, 0.0f, 0.0f), zero, one, "Tree 2");
        a.Registry.emplace<LODGroupComponent>(other).Size = 4.0f;
        for (int i = 0; i < 3; ++i) a.SetParent(a.CreateEmptyEntity(zero, zero, one, "LOD" + std::to_string(i)), other);
        grove = SceneSerializer::SaveToString(a, assets);
    }
    {
        // "AnimTestbed": no LOD group anywhere.
        World b;
        AssetLibrary assets;
        b.CreateEmptyEntity(zero, zero, one, "Lane Post");
        b.CreateEmptyEntity(glm::vec3(2.0f, 0.0f, 0.0f), zero, one, "Ledge");
        testbed = SceneSerializer::SaveToString(b, assets);
    }
    World world; // one World and one library across the switches, as the editor's
    AssetLibrary assets;
    // What ImpostorRenderer would draw this view: each listed group's count, and how many are not this scene's groups.
    int listed = 0, foreign = 0;
    auto view = [&]() {
        world.RebuildWorldTransformCache();
        world.ApplyLod(eye, proj);
        listed = foreign = 0;
        world.ForEachImpostor([&](entt::entity g, int, float) {
            ++listed;
            foreign += !world.Registry.valid(g) || !world.Registry.all_of<LODGroupComponent>(g);
        });
    };
    CHECK(SceneSerializer::LoadFromString(world, assets, forest));
    view();
    CHECK(listed == 1 && foreign == 0); // the Forest's tree, on its impostor
    CHECK(SceneSerializer::LoadFromString(world, assets, testbed));
    view();
    std::printf("[UnitTest] LodImpostorSceneSwitch: after the switch to a scene with no LOD groups, %d impostor(s) "
                "listed, %d of them not this scene's\n", listed, foreign);
    CHECK(listed == 0 && foreign == 0); // nothing of the Forest's left to draw
    CHECK(world.Registry.view<LodCulledTag>().empty());
    // Back, and on to a scene with groups of its own: only its own, each once.
    CHECK(SceneSerializer::LoadFromString(world, assets, forest));
    view();
    CHECK(listed == 1 && foreign == 0);
    CHECK(SceneSerializer::LoadFromString(world, assets, grove));
    view();
    CHECK(listed == 2 && foreign == 0);
    // The smooth views (a fade under way when the scene goes) leave nothing behind either.
    world.LodSmooth = true;
    world.LodClockOverride = 100.0;
    CHECK(SceneSerializer::LoadFromString(world, assets, forest));
    world.RebuildWorldTransformCache();
    world.ApplyLod(glm::vec3(0.0f, 0.0f, 9.0f), proj); // LOD 2
    world.LodClockOverride += 1.0;
    view();                                            // and over to the impostor: a cross-fade begins
    CHECK(listed == 1 && foreign == 0);
    CHECK(SceneSerializer::LoadFromString(world, assets, testbed));
    view();
    CHECK(listed == 0 && foreign == 0);
    // The same in one scene: the last LOD group deleted while it stands on its impostor.
    world.LodSmooth = false;
    CHECK(SceneSerializer::LoadFromString(world, assets, forest));
    view();
    CHECK(listed == 1 && foreign == 0);
    for (entt::entity g : std::vector<entt::entity>(world.Registry.view<LODGroupComponent>().begin(),
                                                    world.Registry.view<LODGroupComponent>().end()))
        world.DestroyEntityAndChildren(g);
    view();
    CHECK(listed == 0 && foreign == 0);
    ImpostorLibrary::TestOverride(-1, 0.0f);
}

// LodSmooth cross-fades end: one still view finishes its fade, and two views that disagree on a group's level (the
// Scene and Game views, every frame) don't keep restarting it - each frame used to begin a fresh fade, so the plants
// stayed dithered for as long as both views were up.
void TestLodFadeCompletes() {
    auto make = [](World& world, entt::entity* lv) {
        const glm::vec3 zero(0.0f), one(1.0f);
        const entt::entity group = world.CreateEmptyEntity(zero, zero, one, "Group");
        world.Registry.emplace<LODGroupComponent>(group).Size = 2.0f;
        for (int i = 0; i < 3; ++i) {
            lv[i] = world.CreateEmptyEntity(zero, zero, one, "LOD" + std::to_string(i));
            world.Registry.emplace<RenderableComponent>(lv[i]);
            world.SetParent(lv[i], group);
        }
        world.LodSmooth = true;
        world.LodClockOverride = 10.0;
    };
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
    const glm::vec3 nearView(0.0f, 0.0f, 1.0f), farView(0.0f, 0.0f, 2.0f); // LOD 0 / LOD 1
    // 60 Hz frames on the world's LOD clock.
    auto frames = [&](World& world, double seconds, const std::function<void()>& each) {
        for (int i = 0, n = (int)(seconds * 60.0); i < n; ++i) {
            world.LodClockOverride += 1.0 / 60.0;
            world.RebuildWorldTransformCache();
            each();
        }
    };
    {
        World world;
        entt::entity lv[3];
        make(world, lv);
        world.RebuildWorldTransformCache();
        world.ApplyLod(nearView, proj);
        frames(world, 0.1, [&] { world.ApplyLod(nearView, proj); });
        world.RebuildWorldTransformCache();
        world.ApplyLod(farView, proj);                  // 0 -> 1: a fade starts
        CHECK(world.LodFadeOf(lv[1]) > 0.0f && world.LodFadeOf(lv[0]) < 0.0f);
        frames(world, World::kLodFadeSeconds + 0.3, [&] { world.ApplyLod(farView, proj); });
        CHECK(world.LodFadeOf(lv[0]) == 0.0f && world.LodFadeOf(lv[1]) == 0.0f);
        CHECK(world.Registry.all_of<LodCulledTag>(lv[0]) && !world.Registry.all_of<LodCulledTag>(lv[1]));
    }
    {
        World world;
        entt::entity lv[3];
        make(world, lv);
        world.RebuildWorldTransformCache();
        world.ApplyLod(nearView, proj);
        frames(world, World::kLodFadeSeconds + 0.4, [&] { world.ApplyLod(nearView, proj); world.ApplyLod(farView, proj); });
        world.RebuildWorldTransformCache();
        world.ApplyLod(nearView, proj);
        CHECK(world.LodFadeOf(lv[0]) == 0.0f && world.LodFadeOf(lv[1]) == 0.0f); // whole, not dithered
        CHECK(!world.Registry.all_of<LodCulledTag>(lv[0]) && world.Registry.all_of<LodCulledTag>(lv[1]));
        world.ApplyLod(farView, proj);
        CHECK(world.LodFadeOf(lv[0]) == 0.0f && world.LodFadeOf(lv[1]) == 0.0f);
        CHECK(world.Registry.all_of<LodCulledTag>(lv[0]) && !world.Registry.all_of<LodCulledTag>(lv[1]));
    }
}

// --- #202: a degenerate camera frustum must not produce a NaN projection -------------------
void TestCameraFrustumValidation() {
    auto finite = [](const glm::mat4& m) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(m[c][r])) return false;
        return true;
    };

    // The guard is at the point of use, so it holds whatever the values came from.
    Camera cam;
    cam.NearPlane = 10.0f; cam.FarPlane = 10.0f;      // far == near: divides by zero
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    cam.NearPlane = 0.0f;  cam.FarPlane = 100.0f;     // non-positive near
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    cam.NearPlane = -5.0f; cam.FarPlane = -10.0f;     // both wrong, and inverted
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    cam.NearPlane = 0.1f;  cam.FarPlane = 1000.0f;
    CHECK(finite(cam.ProjectionMatrix(0.0f)));        // zero aspect
    CHECK(finite(cam.ProjectionMatrix(std::numeric_limits<float>::quiet_NaN())));
    cam.Orthographic = true;
    cam.NearPlane = 5.0f; cam.FarPlane = 5.0f;
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    // The ortho path divides by the half-height too: glm::ortho computes 2/(top-bottom).
    cam.NearPlane = 0.1f; cam.FarPlane = 100.0f;
    cam.OrthoHalfHeight = 0.0f;
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    cam.OrthoHalfHeight = -3.0f;
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    cam.OrthoHalfHeight = std::numeric_limits<float>::quiet_NaN();
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    // A usable ortho camera is still passed through untouched.
    cam.OrthoHalfHeight = 8.0f;
    {
        const glm::mat4 want = glm::ortho(-8.0f * 1.5f, 8.0f * 1.5f, -8.0f, 8.0f, 0.1f, 100.0f);
        const glm::mat4 have = cam.ProjectionMatrix(1.5f);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                CHECK(std::fabs(have[c][r] - want[c][r]) < 1e-6f);
    }
    cam.Orthographic = false;
    cam.NearPlane = 0.1f; cam.FarPlane = 1000.0f;

    // MakePerspective is the shared chokepoint every direct call site now goes through, so the
    // same guarantees have to hold when it is called on its own.
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(finite(MakePerspective(60.0f, inf, 0.1f, 100.0f)));   // zero-height viewport: w/0 == inf
    CHECK(finite(MakePerspective(60.0f, nan, 0.1f, 100.0f)));   // 0x0 viewport: 0/0 == NaN
    CHECK(finite(MakePerspective(60.0f, 0.0f, 0.1f, 100.0f)));
    CHECK(finite(MakePerspective(60.0f, -2.0f, 0.1f, 100.0f)));
    CHECK(finite(MakePerspective(0.0f, 1.5f, 0.1f, 100.0f)));   // degenerate fov
    CHECK(finite(MakePerspective(180.0f, 1.5f, 0.1f, 100.0f))); // tan(90 deg) is infinite
    CHECK(finite(MakePerspective(nan, 1.5f, 0.1f, 100.0f)));
    CHECK(finite(MakePerspective(60.0f, 1.5f, 0.0f, 100.0f)));
    CHECK(finite(MakePerspective(60.0f, 1.5f, 50.0f, 50.0f)));  // far == near
    CHECK(finite(MakePerspective(60.0f, 1.5f, 100.0f, 1.0f)));  // inverted
    CHECK(finite(MakePerspective(60.0f, 1.5f, nan, inf)));
    // Valid input is reproduced exactly - the chokepoint must not reshape good cameras.
    {
        const glm::mat4 want = glm::perspective(glm::radians(55.0f), 1.5f, 0.2f, 300.0f);
        const glm::mat4 have = MakePerspective(55.0f, 1.5f, 0.2f, 300.0f);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                CHECK(std::fabs(have[c][r] - want[c][r]) < 1e-6f);
    }

    // A sane frustum is left exactly alone - the guard must not quietly reshape good cameras.
    cam.NearPlane = 0.3f; cam.FarPlane = 250.0f;
    const glm::mat4 expected = glm::perspective(glm::radians(cam.Fov), 1.5f, 0.3f, 250.0f);
    const glm::mat4 got = cam.ProjectionMatrix(1.5f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(std::fabs(got[c][r] - expected[c][r]) < 1e-6f);

    // The first-person view-model sub-pass rebuilds a projection from the world's, changing only
    // the FOV. It doesn't know the camera's near/far — it reads them back out of the matrix it
    // was handed, so that readback is the thing to pin down: get it wrong and the arms and the
    // scene they are composited over disagree about where the near plane sits.
    {
        const glm::mat4& worldProj = got;
        const float nearZ = std::fabs(worldProj[3][2] / (worldProj[2][2] - 1.0f));
        const float farZ  = std::fabs(worldProj[3][2] / (worldProj[2][2] + 1.0f));
        CHECK(std::fabs(nearZ - cam.NearPlane) < 1e-4f);
        // far recovers through a near-zero denominator (proj[2][2] + 1), so float rounding is
        // amplified ~5 orders of magnitude here — exact to a few hundredths is the ceiling.
        CHECK(std::fabs(farZ - cam.FarPlane) < 0.05f);

        // What the sub-pass then builds: same clip range, different cone.
        const glm::mat4 vm = MakePerspective(60.0f, 1.5f, nearZ, farZ);
        const glm::mat4 want = glm::perspective(glm::radians(60.0f), 1.5f, nearZ, farZ);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                CHECK(std::fabs(vm[c][r] - want[c][r]) < 1e-3f);
        // 60 deg is narrower than the world's 75: a larger y scale, i.e. the held weapon is
        // framed tighter than the scene behind it, not stretched to match it.
        CHECK(vm[1][1] > worldProj[1][1]);
    }

    // And a scene carrying an unusable CameraComponent is corrected on load, not just tolerated.
    World world;
    AssetLibrary assets;
    const std::string sceneJson = R"({"formatVersion":3,"empties":[{"name":"Cam","id":1,"parentId":-1,)"
        R"("position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1],)"
        R"("Camera":{"Field of View":0.0,"Near":0.0,"Far":-5.0}}]})";
    CHECK(SceneSerializer::LoadFromString(world, assets, sceneJson));
    int cams = 0;
    for (entt::entity e : world.Registry.view<CameraComponent>()) {
        ++cams;
        const auto& cc = world.Registry.get<CameraComponent>(e);
        CHECK(cc.NearPlane > 0.0f);
        CHECK(cc.FarPlane > cc.NearPlane);
        CHECK(cc.FovDegrees > 0.0f && cc.FovDegrees < 180.0f);
    }
    CHECK(cams == 1);
}

// The serializer is the one place where a silent mistake costs authored work: a field dropped on
// load, an unstable entity order, a component written but not read. None of that shows up as a
// The physical sky (World::SkySource::Atmosphere): the clock's sun/moon/star placement, the
// CPU transmittance the directional light is filtered by, the day/night light hand-off, the
// presets, and the scene file round trip. All CPU-side - the GPU passes are covered by the
// smoke_sky_* scenes.
void TestPhysicalSky() {
    using namespace TimeOfDay;
    auto Elevation = [](const glm::vec3& d) { return std::asin(glm::clamp(d.y, -1.0f, 1.0f)); }; // above the horizon (radians)
    auto near = [](float a, float b, float eps) { return std::abs(a - b) <= eps; };

    // Equinox at the equator: the noon sun is overhead, 06:00 on the eastern horizon (+X),
    // 18:00 on the western (-X).
    const glm::vec3 noon = SunDirection(12.0f, 80.0f, 0.0f, 0.0f);
    CHECK(noon.y > 0.99f);
    const glm::vec3 dawn = SunDirection(6.0f, 80.0f, 0.0f, 0.0f);
    CHECK(std::abs(dawn.y) < 0.03f && dawn.x > 0.95f);
    CHECK(SunDirection(18.0f, 80.0f, 0.0f, 0.0f).x < -0.95f);
    // 38 N at the June solstice: noon elevation 90 - 38 + 23.44 = 75.4 deg, due south (+Z).
    const glm::vec3 june = SunDirection(12.0f, 172.0f, 38.0f, 0.0f);
    CHECK(near(glm::degrees(Elevation(june)), 75.44f, 0.6f));
    CHECK(june.z > 0.0f && std::abs(june.x) < 1e-3f);
    // Turning the compass rotates the sky about +Y and nothing else.
    const glm::vec3 turned = SunDirection(12.0f, 172.0f, 38.0f, 90.0f);
    CHECK(near(glm::length(turned), 1.0f, 1e-4f) && near(turned.y, june.y, 1e-5f));
    CHECK(std::abs(turned.z) < 1e-3f);
    // Day length: 12 h at the equinox anywhere, polar day and night past the circles.
    CHECK(near(SunsetHour(80.0f, 45.0f), 18.0f, 0.1f));
    CHECK(SunsetHour(172.0f, 80.0f) == 24.0f);
    CHECK(SunsetHour(355.0f, 80.0f) == 12.0f);
    CHECK(SunsetHour(172.0f, 38.0f) > 19.0f && SunsetHour(172.0f, 38.0f) < 19.6f);

    // The moon: new = dark, full = lit, and a full moon sits opposite the sun.
    CHECK(MoonIllumination(0.0f) < 1e-4f && MoonIllumination(0.5f) > 0.9999f);
    CHECK(near(MoonIllumination(0.25f), 0.5f, 1e-4f));
    const glm::vec3 fullMoon = MoonDirection(0.0f, 80.0f, 0.0f, 0.0f, 0.5f);
    CHECK(glm::dot(fullMoon, SunDirection(0.0f, 80.0f, 0.0f, 0.0f)) < -0.9f);
    CHECK(MoonPhase(80.0f, 0.0f, 0.3f) >= 0.0f && MoonPhase(80.0f, 0.0f, 0.3f) < 1.0f);

    // The star field turns rigidly about the celestial pole, which stays put.
    const glm::mat3 stars = StarRotation(3.0f, 100.0f, 38.0f, 0.0f);
    CHECK(near(glm::determinant(stars), 1.0f, 1e-4f));
    const glm::vec3 pole(0.0f, std::sin(glm::radians(38.0f)), -std::cos(glm::radians(38.0f)));
    CHECK(glm::length(stars * pole - pole) < 1e-4f);

    // Transmittance: blue is scattered most, the horizon passes far less than the zenith, the
    // planet blocks everything below it, and thinner air higher up lets more through.
    SkySettings s;
    const glm::vec3 zenith = SkyAtmosphere::Transmittance(s, 0.0f, glm::vec3(0, 1, 0));
    CHECK(zenith.b < zenith.g && zenith.g < zenith.r);
    CHECK(zenith.r > 0.9f && zenith.b > 0.7f && zenith.b < 0.82f);
    const glm::vec3 horizon = SkyAtmosphere::Transmittance(s, 0.0f, glm::normalize(glm::vec3(0.0f, 0.02f, 1.0f)));
    CHECK(horizon.r < zenith.r * 0.7f && horizon.b < horizon.r * 0.5f);
    CHECK(glm::length(SkyAtmosphere::Transmittance(s, 0.0f, glm::normalize(glm::vec3(0.0f, -0.2f, 1.0f)))) == 0.0f);
    CHECK(SkyAtmosphere::Transmittance(s, 5.0f, glm::vec3(0, 1, 0)).b > zenith.b);
    s.RayleighScale = 0.0f; s.MieScale = 0.0f; s.OzoneScale = 0.0f;
    CHECK(glm::length(SkyAtmosphere::Transmittance(s, 0.0f, glm::vec3(0, 1, 0)) - glm::vec3(1.0f)) < 1e-5f);

    // Resolved lighting: a warm, dimmed sun by day; the moon (brightened for the night) after.
    SkyAtmosphere sky; // no GL until PrepareFrame
    SkySettings clock;
    clock.TimeOfDayHours = 12.0f;
    SkyLighting day = sky.ResolveLighting(clock, glm::vec3(0, -1, 0), glm::vec3(5.0f), 0.53f, glm::vec3(0, 2, 0));
    CHECK(!day.LightIsMoon && day.LightDir.y > 0.9f && day.NightBoost == 1.0f);
    CHECK(day.LightRadiance.r > day.LightRadiance.b && day.LightRadiance.r < 5.0f && day.LightRadiance.b > 3.0f);
    clock.TimeOfDayHours = 0.5f;
    const float phase = MoonPhase(clock.DayOfYear, clock.TimeOfDayHours, 0.0f);
    clock.MoonPhaseOffset = 0.5f - phase - std::floor(0.5f - phase); // full moon
    SkyLighting night = sky.ResolveLighting(clock, glm::vec3(0, -1, 0), glm::vec3(5.0f), 0.53f, glm::vec3(0, 2, 0));
    CHECK(night.LightIsMoon && night.LightDir.y > 0.2f && night.MoonLitFraction > 0.99f);
    CHECK(night.NightBoost > 16.0f);
    CHECK(glm::length(night.LightRadiance) < glm::length(day.LightRadiance));
    CHECK(night.SunDir.y < 0.0f);
    clock.MoonEnabled = false;
    night = sky.ResolveLighting(clock, glm::vec3(0, -1, 0), glm::vec3(5.0f), 0.53f, glm::vec3(0, 2, 0));
    CHECK(!night.LightIsMoon && glm::length(night.LightRadiance) == 0.0f);
    // Clock off: the directional light's own aim is the sun.
    SkySettings authored;
    authored.TimeOfDayEnabled = false;
    const glm::vec3 aim = glm::normalize(glm::vec3(-0.3f, -0.8f, -0.5f));
    const SkyLighting fixed = sky.ResolveLighting(authored, aim, glm::vec3(5.0f), 0.53f, glm::vec3(0.0f));
    CHECK(glm::dot(fixed.SunDir, -aim) > 0.9999f && glm::dot(fixed.LightDir, -aim) > 0.9999f);

    // Cloud wind at a high frame rate: each frame's step is tiny, and the texture offsets must
    // still advance steadily. (They once wrapped at a period that wasn't a whole number of tiles
    // and, with the step lost to rounding, flipped between two layouts every frame.)
    {
        SkyAtmosphere windSky; // Tick touches no GL
        SkySettings w;
        w.CloudWindSpeed = 8.0f;
        const float tile = 45.0f; // the weather map's period at CloudScale 1
        glm::vec2 prev = windSky.Clouds().WeatherOffset();
        float travelled = 0.0f, worstStep = 0.0f;
        for (int i = 0; i < 2400; ++i) { // 10 s at 240 fps
            windSky.Tick(w, 1.0f / 240.0f, false);
            const glm::vec2 cur = windSky.Clouds().WeatherOffset();
            glm::vec2 step = cur - prev;
            step -= glm::round(step / tile) * tile; // wrapping by a whole tile is invisible
            travelled += glm::length(step);
            worstStep = std::max(worstStep, glm::length(step));
            prev = cur;
        }
        CHECK(worstStep < 1e-3f);                  // no jumps
        CHECK(near(travelled, 0.08f, 0.004f));     // 8 m/s for 10 s
    }

    // Presets change the look but keep the location and quality; the timed ones land where named.
    SkySettings p;
    p.LatitudeDegrees = -33.0f;
    p.CloudQuality = 3;
    ApplySkyPreset(p, SkyPreset::Overcast);
    CHECK(p.CloudCoverage > 0.9f && p.LatitudeDegrees == -33.0f && p.CloudQuality == 3);
    ApplySkyPreset(p, SkyPreset::Sunset);
    const float sunsetElev = glm::degrees(Elevation(SunDirection(p.TimeOfDayHours, p.DayOfYear, p.LatitudeDegrees, 0.0f)));
    CHECK(sunsetElev > -0.5f && sunsetElev < 4.0f);
    ApplySkyPreset(p, SkyPreset::Night);
    CHECK(MoonIllumination(MoonPhase(p.DayOfYear, p.TimeOfDayHours, p.MoonPhaseOffset)) > 0.99f);
    CHECK(SunDirection(p.TimeOfDayHours, p.DayOfYear, p.LatitudeDegrees, 0.0f).y < 0.0f);
    SkySettings untouched = p;
    ApplySkyPreset(untouched, SkyPreset::Custom);
    CHECK(untouched == p);
    for (int i = 0; i < (int)SkyPreset::Count; ++i) CHECK(std::string(SkyPresetName((SkyPreset)i)) != "?");

    // Scene files: every field round-trips, old scenes load the defaults, bad values clamp.
    World w;
    w.SkySourceMode = World::SkySource::Atmosphere;
    w.Sky.CloudCoverage = 0.77f;
    w.Sky.RayleighTint = glm::vec3(0.5f, 1.0f, 0.625f);
    w.Sky.CloudQuality = 2;
    w.Sky.MoonEnabled = false;
    w.Sky.NightBrightness = 2.5f;
    w.Sky.TimeOfDayHours = 17.25f;
    AssetLibrary assets;
    const std::string saved = SceneSerializer::SaveToString(w, assets);
    World r;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(r, assets2, saved));
    CHECK(r.SkySourceMode == World::SkySource::Atmosphere);
    CHECK(r.Sky == w.Sky);
    World old;
    AssetLibrary assets3;
    CHECK(SceneSerializer::LoadFromString(old, assets3, R"({"formatVersion":4,"skySource":2})"));
    CHECK(old.SkySourceMode == World::SkySource::Atmosphere && old.Sky == SkySettings{});
    World clamped;
    AssetLibrary assets4;
    CHECK(SceneSerializer::LoadFromString(clamped, assets4,
        R"({"formatVersion":4,"skySource":9,"sky":{"cloudCoverage":7,"cloudQuality":9,"latitude":-400,"clouds":"yes"}})"));
    CHECK(clamped.SkySourceMode == World::SkySource::Atmosphere);
    CHECK(clamped.Sky.CloudCoverage == 1.0f && clamped.Sky.CloudQuality == 3 && clamped.Sky.LatitudeDegrees == -90.0f);
    CHECK(clamped.Sky.CloudsEnabled); // a non-boolean is ignored, keeping the default
}

// --- TextureCache: one texture loaded two ways (a model's own material at the defaults, a material asset at its .meta's)
// keeps both entries - they used to share one and evict each other every session, re-decoding (and re-compressing) both.
void TestTextureCacheVariants() {
    const std::string src = (std::filesystem::temp_directory_path() / "tt_cache_variant_test.png").string();
    {
        std::ofstream f(src, std::ios::binary);
        f << "not really a png - only its size and time stamp key the entry";
    }
    TextureImportSettings a, b;
    b.MaxTextureSize = 4096;
    b.CompressionMode = TextureImportSettings::Compression::HighQuality;
    TextureCache::Image ia, ib, out;
    ia.SourceWidth = ia.Width = 2; ia.SourceHeight = ia.Height = 2; ia.Channels = 4; ia.Pixels.assign(16, 7);
    ib = ia;
    ib.Pixels.assign(16, 9);
    TextureCache::Store(src, a, ia);
    TextureCache::Store(src, b, ib);
    CHECK(TextureCache::Load(src, a, out) && out.Pixels.size() == 16 && out.Pixels[0] == 7);
    CHECK(TextureCache::Load(src, b, out) && out.Pixels.size() == 16 && out.Pixels[0] == 9);
    // The entries land in the project's real Library/Textures: remove exactly the two this test wrote.
    const std::string pa = TextureCache::EntryPath(src, a), pb = TextureCache::EntryPath(src, b);
    CHECK(pa != pb);
    std::error_code ec;
    CHECK(std::filesystem::remove(std::filesystem::path(pa), ec));
    CHECK(std::filesystem::remove(std::filesystem::path(pb), ec));
    std::filesystem::remove(src, ec);
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

} // namespace

void RegisterRenderTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"GI: L1 SH projection of known environments", GISphericalHarmonics});
    tests.push_back({"GI: volume lookup, bake file, weights follow circuits", GIVolumeLookup});
    tests.push_back({"Shader constants match their C++ sources", ShaderConstantsMatch});
    tests.push_back({"Reflection probe captures + puddle SSR setting", ProbeCaptureAndPuddleSsr});
    tests.push_back({"Small-object cull: projected bounding-sphere height", SmallObjectCull});
    tests.push_back({"Light distance fade: by projected size, smooth, then dropped", LightDistanceFade});
    tests.push_back({"GatherVisible: per-frame candidates, per-query filters, invalidation", GatherCache});
    tests.push_back({"Model import: mergeByMaterial keeps geometry, one mesh per material", MergeByMaterial});
    tests.push_back({"GPU instancing: grouping keeps first-appearance order, kNoBatch stands alone", InstanceGrouping});
    tests.push_back({"GPU instancing: material slots batch by asset or identical values", InstanceSlots});
    tests.push_back({"Async uploads: staging ring placement wraps and never overlaps", UploadStagingRing});
    tests.push_back({"Async uploads: the pump budget follows the frame headroom in Play", AssetPumpBudget});
    tests.push_back({"GPU-driven draws: indirect batches by key, in first-appearance order", IndirectBuilder});
    tests.push_back({"GPU-driven draws: mesh arena free list is first-fit and coalesces", MeshArenaFreeList});
    tests.push_back({"GPU occlusion culling: box padding", HiZPieces});
    tests.push_back({"Scene wind: settings round-trip, old-scene defaults, one bearing convention", SceneWindSettings});
    tests.push_back({"GPU instancing: placed copies share one import id", InstanceImportId});
    tests.push_back({"Volumetric fog: blue-noise jitter ranks are a permutation, evenly spread", VolumetricBlueNoise});
    tests.push_back({"Volumetric fog: settings and Volumetric Intensity round-trip; old scenes default off / 1", VolumetricSettingsRoundTrip});
    tests.push_back({"TAA mode + rain wetness round-trip; old scenes keep MSAA/FXAA and stay dry; jitter un-jitters", TaaAndWetnessRoundTrip});
    tests.push_back({"Fur: shell count follows the hair's size on screen", FurShellLod});
    tests.push_back({"Fur: material properties round-trip through a .mat and the accessors", FurMaterialRoundTrip});
    tests.push_back({"LodGroup", TestLodGroup});
    tests.push_back({"LodImpostor", TestLodImpostor});
    tests.push_back({"LodImpostorSceneSwitch", TestLodImpostorSceneSwitch});
    tests.push_back({"LodFadeCompletes", TestLodFadeCompletes});
    tests.push_back({"PhysicalSky", TestPhysicalSky});
    tests.push_back({"MaterialRobustness", TestMaterialRobustness});
    tests.push_back({"MaterialTextureDefaults", TestMaterialTextureDefaults});
    tests.push_back({"TextureCacheVariants", TestTextureCacheVariants});
    tests.push_back({"TextureCacheHash", TestTextureCacheHash});
    tests.push_back({"CameraFrustumValidation", TestCameraFrustumValidation});
    tests.push_back({"CameraRoll", TestCameraRoll});
}
