#include "UnitTestSupport.h"
#include "../Renderer/SceneRenderer.h"
#include "../Renderer/Model.h"
#include "../Renderer/InstanceBatch.h"
#include "../Renderer/MeshArena.h"
#include "../Renderer/HiZCuller.h"
#include "../Renderer/MaterialAsset.h"
#include "../Renderer/FurRenderer.h"
#include "../Renderer/VolumetricFog.h"
#include "../Renderer/LightBuffer.h"
#include "../Renderer/TemporalAA.h"
#include "../Renderer/ReflectionProbeCapture.h"
#include "../Game/SceneSerializer.h"
#include "AssetLibrary.h"
#include "../Game/World.h"
#include "../Game/Components.h"
#include "../Physics/Frustum.h"
#include "../Core/ProjectPaths.h"
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
        Log::Info("GatherCache: no test model - skipped.");
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
        Log::Info("MergeByMaterial: no model library - skipped.");
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

// HiZCuller: the pyramid level a rectangle tests at spans at most two texels each way; boxes near the eye always draw.
void HiZPieces() {
    CHECK(HiZCuller::LevelFor(10, 10, 11, 11) == 0);
    CHECK(HiZCuller::LevelFor(10, 10, 12, 10) == 1);       // 10>>1=5, 12>>1=6
    CHECK(HiZCuller::LevelFor(0, 0, 1023, 0) == 9);        // 1023>>9 = 1
    CHECK(HiZCuller::LevelFor(511, 0, 512, 0) == 0);        // two pixels straddling a boundary
    for (int x0 = 0; x0 < 40; x0 += 3)
        for (int x1 = x0; x1 < 300; x1 += 7) {
            const int l = HiZCuller::LevelFor(x0, 0, x1, 0);
            CHECK((x1 >> l) - (x0 >> l) <= 1);
            CHECK(l == 0 || (x1 >> (l - 1)) - (x0 >> (l - 1)) > 1);
        }
    glm::vec3 lo(0.0f), hi(1.0f);
    HiZCuller::PadBounds(lo, hi);
    CHECK(lo.x < 0.0f && hi.x > 1.0f);
    CHECK(HiZCuller::NearEye(lo, hi, glm::vec3(0.5f), 0.5f));
    CHECK(HiZCuller::NearEye(lo, hi, glm::vec3(1.3f, 0.5f, 0.5f), 0.5f));
    CHECK(!HiZCuller::NearEye(lo, hi, glm::vec3(3.0f, 0.5f, 0.5f), 0.5f));
    CHECK(HiZCuller::FoliageSway(0.15f) > 0.3f);
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
        Log::Info("InstanceImportId: no test model - skipped.");
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
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tartarus_fur_test";
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
    ma.FurMaskPath = "assets/Characters/Spider/Textures/T_Spider_FurMask.png";
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

} // namespace

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

void RegisterRenderTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Reflection probe captures + puddle SSR setting", ProbeCaptureAndPuddleSsr});
    tests.push_back({"Small-object cull: projected bounding-sphere height", SmallObjectCull});
    tests.push_back({"Light distance fade: by projected size, smooth, then dropped", LightDistanceFade});
    tests.push_back({"GatherVisible: per-frame candidates, per-query filters, invalidation", GatherCache});
    tests.push_back({"Model import: mergeByMaterial keeps geometry, one mesh per material", MergeByMaterial});
    tests.push_back({"GPU instancing: grouping keeps first-appearance order, kNoBatch stands alone", InstanceGrouping});
    tests.push_back({"GPU instancing: material slots batch by asset or identical values", InstanceSlots});
    tests.push_back({"GPU-driven draws: indirect batches by key, in first-appearance order", IndirectBuilder});
    tests.push_back({"GPU-driven draws: mesh arena free list is first-fit and coalesces", MeshArenaFreeList});
    tests.push_back({"GPU occlusion culling: pyramid level choice, padding and near-eye boxes", HiZPieces});
    tests.push_back({"GPU instancing: placed copies share one import id", InstanceImportId});
    tests.push_back({"Volumetric fog: blue-noise jitter ranks are a permutation, evenly spread", VolumetricBlueNoise});
    tests.push_back({"Volumetric fog: settings and Volumetric Intensity round-trip; old scenes default off / 1", VolumetricSettingsRoundTrip});
    tests.push_back({"TAA mode + rain wetness round-trip; old scenes keep MSAA/FXAA and stay dry; jitter un-jitters", TaaAndWetnessRoundTrip});
    tests.push_back({"Fur: shell count follows the hair's size on screen", FurShellLod});
    tests.push_back({"Fur: material properties round-trip through a .mat and the accessors", FurMaterialRoundTrip});
}
