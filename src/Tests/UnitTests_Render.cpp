#include "UnitTestSupport.h"
#include <limits>
#include <random>
#include <cstring>
#include "../Renderer/SceneRenderer.h"
#include "../Renderer/RenderSettingsEpoch.h"
#include "../Renderer/Model.h"
#include "../Renderer/InstanceBatch.h"
#include "../Renderer/OpaqueBatchKey.h"
#include "../Renderer/UploadStaging.h"
#include "../Renderer/MeshArena.h"
#include "../Renderer/HiZCuller.h"
#include "../Renderer/GpuCull.h"
#include "../Physics/Frustum.h"
#include "../Renderer/GpuScene.h"
#include "../Renderer/ShaderLibrary.h"
#include "../Renderer/SceneViewPass.h"
#include "../Renderer/MaterialAsset.h"
#include "../Renderer/TextureCache.h"
#include "../Renderer/Texture.h"
#include "../Renderer/ImpostorLibrary.h"
#include "../Renderer/AssetCost.h"
#include "../Renderer/SkyAtmosphere.h"
#include "../Renderer/ProgramBinaryCache.h"
#include "../Renderer/Material.h"
#include "../Renderer/RenderFrameContext.h"
#include "../Core/TimeService.h"
#include "../Renderer/Camera.h"
#include "../Game/SkySettings.h"
#include "../Core/AtomicFile.h"
#include "../Assets/AssetDatabase.h"
#include "../Renderer/FurRenderer.h"
#include "../Renderer/VolumetricFog.h"
#include "../Renderer/LightBuffer.h"
#include "../Renderer/PointShadowMap.h"
#include "../Renderer/SpotShadowMap.h"
#include "../Renderer/ShadowFrame.h"
#include "../Renderer/DecalTypes.h"
#include "../Renderer/ClusterGrid.h"
#include "../Renderer/TemporalAA.h"
#include "../Renderer/ReflectionProbeCapture.h"
#include "../Renderer/GIData.h"
#include "../Renderer/GIVolumes.h"
#include "../Renderer/ShadowSlotScoring.h"
#include "../Game/LightSwitch.h"
#include "../Game/SceneSerializer.h"
#include "../Game/TimeOfDay.h"
#include "AssetLibrary.h"
#include "AsyncTicketCount.h"
#include "StepPending.h"
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

// LightBuffer::BoxWeight (CPU mirror of LightBuffer.glsl LightBoxFade): uniform fade = the old single-fade result;
// a per-face fade only changes its own face (Bounds Fade - / Bounds Fade +).
void LightBoundsFaceFade() {
    const glm::vec3 half(2.0f, 1.0f, 3.0f), f(0.05f);
    CHECK(LightBuffer::BoxWeight(glm::vec3(0.0f), half, f, f) == 1.0f);
    CHECK(LightBuffer::BoxWeight(glm::vec3(2.0f, 0, 0), half, f, f) == 0.0f);             // on the +x face
    CHECK(std::abs(LightBuffer::BoxWeight(glm::vec3(-1.975f, 0, 0), half, f, f) - 0.5f) < 1e-3f); // half way in
    // +x face widened to 1 m: 0.5 m inside it is half lit; the -x face keeps its 5 cm fade.
    const glm::vec3 fp(1.0f, 0.05f, 0.05f);
    CHECK(std::abs(LightBuffer::BoxWeight(glm::vec3(1.5f, 0, 0), half, f, fp) - 0.5f) < 1e-3f);
    CHECK(LightBuffer::BoxWeight(glm::vec3(-1.5f, 0, 0), half, f, fp) == 1.0f);
    CHECK(LightBuffer::BoxWeight(glm::vec3(0, 0, 2.99f), half, f, fp) < 0.21f);            // z faces untouched
    // BoundLast: < 0 = the light's Bounds Fade, else the face's own (>= 1 mm).
    LightBuffer lb;
    lb.AddPoint(glm::vec3(0.0f), glm::vec3(1.0f), 1.0f, 5.0f);
    lb.BoundLast(glm::mat3(1.0f), glm::vec3(0.0f), half, 0.05f, glm::vec3(-1.0f, 0.3f, -1.0f), glm::vec3(0.0f, -1.0f, -1.0f));
    CHECK(lb.FaceFadeOfLast(false) == glm::vec3(0.05f, 0.3f, 0.05f));
    CHECK(lb.FaceFadeOfLast(true) == glm::vec3(1e-3f, 0.05f, 0.05f));
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

// GatherVisible's candidate patch (renderables created, destroyed both ways, re-parented, given another model or none,
// tags and layers changed, moved) hands every query exactly what a from-scratch build hands it, frame after frame of
// random changes; and those frames do take the patch.
void GatherPatch() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
        return;
    }
    const auto modelA = Model::ImportDeferred(path, ModelImportSettings{});
    const auto modelB = Model::ImportDeferred(path, ModelImportSettings{}); // another model, as a swap brings
    CHECK(modelA && modelB);
    if (!modelA || !modelB) return;
    World w;
    std::mt19937 rng(1124);
    auto pick = [&](size_t n) { return (size_t)std::uniform_int_distribution<size_t>(0, n - 1)(rng); };
    auto at = [&]() { return glm::vec3((float)pick(200) - 100.0f, 0.0f, (float)pick(200) - 100.0f); };
    std::vector<entt::entity> live;
    auto spawn = [&]() {
        const entt::entity e = w.CreateModelEntity(pick(2) ? modelA : modelB, at(), glm::vec3(0), glm::vec3(1), "R");
        if (!live.empty() && pick(3) == 0) w.SetParent(e, live[pick(live.size())]);
        live.push_back(e);
    };
    for (int i = 0; i < 300; ++i) spawn();
    const glm::mat4 proj = glm::perspective(glm::radians(75.0f), 1.0f, 0.1f, 300.0f);
    struct Result { std::vector<entt::entity> Ids; std::vector<glm::vec3> Mins; };
    auto gather = [&](const VisibilityQuery& q) {
        std::vector<VisibleRenderable> out;
        GatherVisible(w, q, out);
        std::sort(out.begin(), out.end(), [](const VisibleRenderable& a, const VisibleRenderable& b) { return a.Entity < b.Entity; });
        Result r;
        for (const auto& v : out) {
            r.Ids.push_back(v.Entity);
            r.Mins.push_back(v.WorldMin);
        }
        return r;
    };
    auto queries = [&](const glm::vec3& eye, const glm::vec3& look) {
        std::vector<Result> rs;
        const Frustum f = Frustum::FromViewProj(proj * glm::lookAt(eye, look, glm::vec3(0, 1, 0)));
        VisibilityQuery all, view, casts, editor;
        view.Cull = &f;
        casts.Cull = &f;
        casts.ShadowCasters = true;
        editor.Cull = &f;
        editor.EditorView = true;
        editor.LayerVisibleMask = ~0u & ~(1u << 3);
        for (const VisibilityQuery* q : {&all, &view, &casts, &editor}) rs.push_back(gather(*q));
        glm::vec3 lo, hi;
        if (RenderableBounds(w, lo, hi)) rs.push_back({{}, {lo, hi}});
        return rs;
    };
    w.RebuildWorldTransformCache();
    ForgetGatherCandidates();
    queries(glm::vec3(0, 2, 0), glm::vec3(0, 2, -1));
    int patched = 0, frames = 0;
    for (int frame = 0; frame < 300; ++frame) {
        live.erase(std::remove_if(live.begin(), live.end(), [&](entt::entity e) { return !w.Registry.valid(e); }), live.end());
        const int ops = 1 + (int)pick(4);
        for (int op = 0; op < ops && !live.empty(); ++op) {
            live.erase(std::remove_if(live.begin(), live.end(), [&](entt::entity e) { return !w.Registry.valid(e); }), live.end());
            if (live.empty()) break;
            const entt::entity e = live[pick(live.size())];
            switch (pick(12)) {
            case 0: case 1: spawn(); break;
            case 2: w.DestroyEntityAndChildren(e); break;
            case 3: w.Registry.destroy(e); break; // its children orphaned
            case 4: w.SetParent(e, pick(3) == 0 ? entt::entity{entt::null} : live[pick(live.size())]); break;
            case 5: { auto& r = w.Registry.get<RenderableComponent>(e); r.ModelRef = r.ModelRef == modelA ? modelB : modelA; break; }
            case 6: if (w.Registry.all_of<DeactivatedTag>(e)) w.Registry.remove<DeactivatedTag>(e); else w.Registry.emplace<DeactivatedTag>(e); break;
            case 7: if (w.Registry.all_of<HiddenInSceneTag>(e)) w.Registry.remove<HiddenInSceneTag>(e); else w.Registry.emplace<HiddenInSceneTag>(e); break;
            case 8: w.Registry.emplace_or_replace<LayerComponent>(e).Layer = (int)pick(5); break;
            case 9: w.Registry.get<RenderableComponent>(e).CastShadows = pick(2) ? RenderableComponent::ShadowCasting::Off
                                                                                     : RenderableComponent::ShadowCasting::On; break;
            case 10: { auto& r = w.Registry.get<RenderableComponent>(e); r.ModelRef = r.ModelRef ? nullptr : modelA; break; } // none, or one again
            default: w.Registry.get<TransformComponent>(e).Position = at(); break;
            }
        }
        w.RebuildWorldTransformCache();
        const glm::vec3 eye((float)pick(100) - 50.0f, 2.0f, (float)pick(100) - 50.0f);
        const glm::vec3 look = eye + glm::vec3((float)pick(3) - 1.0f, 0.0f, pick(2) ? 1.0f : -1.0f);
        const std::uint64_t fullBefore = GatherCandidateFullBuilds();
        const auto patchedResults = queries(eye, look);
        ++frames;
        patched += GatherCandidateFullBuilds() == fullBefore ? 1 : 0;
        ForgetGatherCandidates();
        const auto scratch = queries(eye, look);
        bool same = patchedResults.size() == scratch.size();
        for (size_t i = 0; same && i < 4; ++i) same = patchedResults[i].Ids == scratch[i].Ids && patchedResults[i].Mins == scratch[i].Mins;
        CHECK(same);
        if (!same) {
            std::printf("[GatherPatch] frame %d differs from a from-scratch build\n", frame);
            break;
        }
        // RenderableBounds: a superset either way (a patched cell only grows).
        if (patchedResults.size() == 5 && scratch.size() == 5) {
            const glm::vec3 plo = patchedResults[4].Mins[0], phi = patchedResults[4].Mins[1];
            const glm::vec3 slo = scratch[4].Mins[0], shi = scratch[4].Mins[1];
            CHECK(glm::all(glm::lessThanEqual(plo, slo + 1e-3f)) && glm::all(glm::greaterThanEqual(phi, shi - 1e-3f)));
        }
    }
    std::printf("[GatherPatch] %d of %d frames patched\n", patched, frames);
    CHECK(patched * 2 > frames);
}

// GpuScene (G-1): after every frame of random changes (spawn, destroy, re-parent, model swap or none, tags, layers,
// moves), its mirror of the GPU buffers holds exactly what the candidate list says for every slot, though most frames
// write only the slots that changed. The GL buffers themselves are compared with the list by the smoke run
// (GpuScene::SelfTest, smoke_min).
void GpuSceneMirror() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
        return;
    }
    if (!GpuScene::Enabled()) {
        UnitTestSupport::Skip("TARTARUS_NO_GPU_SCENE");
        return;
    }
    const auto modelA = Model::ImportDeferred(path, ModelImportSettings{});
    const auto modelB = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(modelA && modelB);
    if (!modelA || !modelB) return;
    World w;
    std::mt19937 rng(1125);
    auto pick = [&](size_t n) { return (size_t)std::uniform_int_distribution<size_t>(0, n - 1)(rng); };
    auto at = [&]() { return glm::vec3((float)pick(200) - 100.0f, 0.0f, (float)pick(200) - 100.0f); };
    std::vector<entt::entity> live;
    auto spawn = [&]() {
        const entt::entity e = w.CreateModelEntity(pick(2) ? modelA : modelB, at(), glm::vec3(0), glm::vec3(1), "R");
        if (!live.empty() && pick(3) == 0) w.SetParent(e, live[pick(live.size())]);
        live.push_back(e);
    };
    for (int i = 0; i < 300; ++i) spawn();
    w.RebuildWorldTransformCache();
    ForgetGatherCandidates();
    GpuScene scene;
    // Every slot of the mirror against the list; false with the first that differs.
    auto matches = [&](int frame) {
        if (scene.XformMirror().size() != (size_t)scene.Count() * 2 || scene.BoundsMirror().size() != (size_t)scene.Count() * 2)
            return false;
        SceneSlot s;
        glm::mat4 xf[2];
        glm::vec4 b[2];
        for (std::uint32_t i = 0; i < scene.Count(); ++i) {
            if (!ReadSceneSlot(i, s)) return false;
            GpuScene::Pack(s, xf, b);
            const bool same = std::memcmp(&scene.XformMirror()[(size_t)i * 2], xf, sizeof xf) == 0 &&
                              std::memcmp(&scene.BoundsMirror()[(size_t)i * 2], b, sizeof b) == 0;
            if (!same) {
                std::printf("[GpuSceneMirror] frame %d: slot %u of %u differs\n", frame, i, scene.Count());
                return false;
            }
        }
        return true;
    };
    // G1-b: what GatherVisible hands out points at its slot - the matrices the batched draws read from the scene are
    // the ones the item itself carries.
    std::vector<VisibleRenderable> vis;
    std::uint64_t slotChecks = 0;
    auto slotsPoint = [&](int frame) {
        GatherVisible(w, VisibilityQuery{}, vis);
        for (const VisibleRenderable& v : vis) {
            const bool ok = v.Slot < scene.Count() &&
                            std::memcmp(&scene.XformMirror()[(size_t)v.Slot * 2], &v.Xform, sizeof(glm::mat4)) == 0 &&
                            scene.XformMirror()[(size_t)v.Slot * 2 + 1] == glm::mat4(v.Normal);
            if (!ok) {
                std::printf("[GpuSceneMirror] frame %d: gathered slot %u of %u doesn't hold its item\n", frame, v.Slot, scene.Count());
                return false;
            }
            ++slotChecks;
        }
        return true;
    };
    scene.Sync(w, false);
    CHECK(scene.LastSync().Full && scene.Count() == 300);
    CHECK(matches(-1));
    CHECK(slotsPoint(-1));
    // A full sync uploads every slot in one span (it once planned just the first and the last).
    CHECK(scene.LastRuns().size() == 1 && scene.LastRuns()[0] == GpuScene::Run(0u, 299u));
    // Nothing changed: nothing written.
    w.RebuildWorldTransformCache();
    scene.Sync(w, false);
    CHECK(!scene.LastSync().Full && scene.LastSync().Slots == 0);
    // Fields written in place with nothing told (G2-DIRTY): Cast Shadows and the first material's queue. A frame that
    // changes only one of them, with nothing moved, must still bring that slot up to date.
    auto glass = std::make_shared<MaterialAsset>();
    glass->RenderQueue = MaterialAsset::Queue::Transparent;
    auto castNext = [](RenderableComponent& r) {
        r.CastShadows = (RenderableComponent::ShadowCasting)(((int)r.CastShadows + 1) % 4);
    };
    {
        auto& r = w.Registry.get<RenderableComponent>(live[0]);
        r.CastShadows = RenderableComponent::ShadowCasting::Off;
        scene.Sync(w, false); // transforms untouched: no rebuild, no patch
        CHECK(!scene.LastSync().Full && scene.LastSync().Slots == 1);
        CHECK(matches(-2));
        if (r.Materials.empty()) r.Materials.resize(1);
        r.Materials[0] = glass;
        scene.Sync(w, false);
        CHECK(!scene.LastSync().Full && scene.LastSync().Slots == 1);
        CHECK(matches(-3));
        glass->RenderQueue = MaterialAsset::Queue::Opaque; // the asset edited (Inspector): its users change too
        scene.Sync(w, false);
        CHECK(!scene.LastSync().Full && scene.LastSync().Slots == 1);
        CHECK(matches(-4));
        glass->RenderQueue = MaterialAsset::Queue::Transparent;
    }
    int incremental = 0, frames = 0;
    std::uint64_t written = 0, total = 0;
    for (int frame = 0; frame < 300; ++frame) {
        live.erase(std::remove_if(live.begin(), live.end(), [&](entt::entity e) { return !w.Registry.valid(e); }), live.end());
        const int ops = 1 + (int)pick(4);
        for (int op = 0; op < ops && !live.empty(); ++op) {
            live.erase(std::remove_if(live.begin(), live.end(), [&](entt::entity e) { return !w.Registry.valid(e); }), live.end());
            if (live.empty()) break;
            const entt::entity e = live[pick(live.size())];
            switch (pick(13)) {
            case 0: case 1: spawn(); break;
            case 2: w.DestroyEntityAndChildren(e); break;
            case 3: w.Registry.destroy(e); break;
            case 4: w.SetParent(e, pick(3) == 0 ? entt::entity{entt::null} : live[pick(live.size())]); break;
            case 5: { auto& r = w.Registry.get<RenderableComponent>(e); r.ModelRef = r.ModelRef == modelA ? modelB : modelA; break; }
            case 6: if (w.Registry.all_of<DeactivatedTag>(e)) w.Registry.remove<DeactivatedTag>(e); else w.Registry.emplace<DeactivatedTag>(e); break;
            case 7: if (w.Registry.all_of<HiddenInSceneTag>(e)) w.Registry.remove<HiddenInSceneTag>(e); else w.Registry.emplace<HiddenInSceneTag>(e); break;
            case 8: w.Registry.emplace_or_replace<LayerComponent>(e).Layer = (int)pick(7) - 2; break; // negatives too
            case 9: { auto& r = w.Registry.get<RenderableComponent>(e); r.ModelRef = r.ModelRef ? nullptr : modelA; break; }
            case 10: castNext(w.Registry.get<RenderableComponent>(e)); break;
            case 11: {
                auto& r = w.Registry.get<RenderableComponent>(e);
                if (r.Materials.empty()) r.Materials.resize(1);
                r.Materials[0] = r.Materials[0] ? nullptr : glass;
                break;
            }
            default: w.Registry.get<TransformComponent>(e).Position = at(); break;
            }
        }
        w.RebuildWorldTransformCache();
        scene.Sync(w, false);
        ++frames;
        // Without an upload the GL buffers are stale, so every sync plans the whole list.
        CHECK(scene.Count() == 0 || (scene.LastRuns().size() == 1 && scene.LastRuns()[0] == GpuScene::Run(0u, scene.Count() - 1)));
        if (!scene.LastSync().Full) {
            ++incremental;
            written += scene.LastSync().Slots;
            total += scene.Count();
        }
        const bool same = matches(frame) && slotsPoint(frame);
        CHECK(same);
        if (!same) break;
    }
    CHECK(slotChecks > 0);
    std::printf("[GpuSceneMirror] %d of %d frames incremental, %llu of %llu slots written on them\n", incremental, frames,
                (unsigned long long)written, (unsigned long long)total);
    CHECK(incremental * 2 > frames);
    CHECK(written * 10 < total); // a few changes a frame: a few slots, not the list
}

// G2-DIRTY at scale: past one sweep slice, an in-place write that reports itself (NoteRenderSettingsChanged) reaches
// the next sync; one that doesn't is still caught by the sweep within a lap (slots / 1024 syncs), and a quiet frame
// sends nothing.
void GpuSceneLiveEpoch() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
        return;
    }
    if (!GpuScene::Enabled()) {
        UnitTestSupport::Skip("TARTARUS_NO_GPU_SCENE");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    World w;
    constexpr int kCount = 5000; // 5 sweep slices of 1024
    std::vector<entt::entity> all;
    for (int i = 0; i < kCount; ++i)
        all.push_back(w.CreateModelEntity(model, glm::vec3((float)(i % 100), 0.0f, (float)(i / 100)), glm::vec3(0), glm::vec3(1), "R"));
    w.RebuildWorldTransformCache();
    ForgetGatherCandidates();
    GpuScene scene;
    scene.Sync(w, false);
    CHECK(scene.LastSync().Full);
    for (int i = 0; i < 6; ++i) { // a full lap of the sweep with nothing changed
        scene.Sync(w, false);
        CHECK(!scene.LastSync().Full && scene.LastSync().Slots == 0);
    }
    auto syncsUntilSeen = [&](int limit) {
        for (int n = 1; n <= limit; ++n) {
            scene.Sync(w, false);
            if (scene.LastSync().Slots > 0) {
                CHECK(!scene.LastSync().Full && scene.LastSync().Slots == 1);
                return n;
            }
        }
        return limit + 1;
    };
    // Reported: the next sync.
    w.Registry.get<RenderableComponent>(all[2500]).CastShadows = RenderableComponent::ShadowCasting::Off;
    NoteRenderSettingsChanged();
    CHECK(syncsUntilSeen(1) == 1);
    // The first material swapped to a Transparent one on a still entity (the Inspector's slot writes report it).
    auto glass = std::make_shared<MaterialAsset>();
    glass->RenderQueue = MaterialAsset::Queue::Transparent;
    {
        auto& r = w.Registry.get<RenderableComponent>(all[1200]);
        if (r.Materials.empty()) r.Materials.resize(1);
        r.Materials[0] = glass;
        NoteRenderSettingsChanged();
        CHECK(syncsUntilSeen(1) == 1);
    }
    // The asset's own queue edited (Inspector Render Mode): no renderable write, its users change.
    glass->RenderQueue = MaterialAsset::Queue::Opaque;
    NoteRenderSettingsChanged();
    CHECK(syncsUntilSeen(1) == 1);
    // Not reported: within one lap, wherever the sweep stands.
    for (int k : {10, 2600, 4999}) {
        w.Registry.get<RenderableComponent>(all[k]).CastShadows = RenderableComponent::ShadowCasting::ShadowsOnly;
        CHECK(syncsUntilSeen(5) <= 5);
    }
}

// Every Sync keys SyncOnce: SyncOnce(A), a Sync for another world (a bake, a preview), then SyncOnce(A) must sync A
// again, or A's depth casters read B's transforms until something in A moves.
void GpuSceneSyncOnceKey() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
        return;
    }
    if (!GpuScene::Enabled()) {
        UnitTestSupport::Skip("TARTARUS_NO_GPU_SCENE");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    World a, b;
    entt::entity first = entt::null;
    for (int i = 0; i < 3; ++i) {
        const entt::entity e = a.CreateModelEntity(model, glm::vec3((float)i, 0.0f, 0.0f), glm::vec3(0), glm::vec3(1), "A");
        if (i == 0) first = e;
    }
    for (int i = 0; i < 5; ++i) b.CreateModelEntity(model, glm::vec3(0.0f, 0.0f, (float)i), glm::vec3(0), glm::vec3(1), "B");
    a.RebuildWorldTransformCache();
    b.RebuildWorldTransformCache();
    ForgetGatherCandidates();
    GpuScene scene;
    scene.Sync(a, false);
    const std::uint32_t countA = scene.Count();
    CHECK(scene.UpToDateFor(a) && !scene.UpToDateFor(b));
    scene.Sync(b, false); // another world in between
    CHECK(scene.Count() != countA);
    CHECK(!scene.UpToDateFor(a) && scene.UpToDateFor(b));
    scene.Sync(a, false); // what SyncOnce(A) now does
    CHECK(scene.LastSync().Full && scene.Count() == countA && scene.UpToDateFor(a));
    a.Registry.get<TransformComponent>(first).Position.y = 2.0f; // A itself moves
    a.RebuildWorldTransformCache();
    CHECK(!scene.UpToDateFor(a));
    ForgetGatherCandidates(); // no cache left pointing at these worlds
}

// G-MB: a multi-draw copy's slot state holds what the per-draw hook would bind for it (ReflectionProbeArray::Bind's
// probes and blends, GIVolumes::SetMask's mask), probes picked like SelectionKey; the mirror marks only real changes.
void GpuSceneObjectState() {
    World world;
    const glm::vec3 probeAt[3] = {{0, 0, 0}, {10, 0, 0}, {0, 0, 10}};
    for (const glm::vec3& at : probeAt) {
        const entt::entity e = world.Registry.create();
        world.Registry.emplace<TransformComponent>(e).Position = at;
        world.Registry.emplace<ReflectionProbeComponent>(e).Size = glm::vec3(4.0f, 2.0f, 6.0f);
    }
    world.RebuildWorldTransformCache();
    ReflectionProbeArray probes;
    probes.Update(world);
    CHECK(probes.Count() == 3);
    const glm::vec3 centre(3.0f, 0.0f, 1.0f); // nearest probe 0, then 1
    const std::uint32_t key = probes.SelectionKey(centre);
    const ReflectionProbeArray::Selection sel = probes.Resolve(centre);
    CHECK(sel.Count == 2 && (int)(key >> 30) == 2);
    CHECK(sel.Center[0] == probes.Volume((key >> 20) & 0x3FF).Center && sel.Center[1] == probes.Volume((key >> 10) & 0x3FF).Center);
    CHECK(sel.Center[0] == probeAt[0] && sel.Center[1] == probeAt[1]); // the nearest two, in order
    CHECK(sel.HalfSize[0] == glm::vec3(2.0f, 1.0f, 3.0f) && sel.Layer[0] == -1 && sel.Layer[1] == -1); // no cube array
    CHECK(std::abs(sel.Blend[0] + sel.Blend[1] - 1.0f) < 1e-6f && sel.Blend[0] > sel.Blend[1]);
    CHECK((std::uint32_t)std::lround(sel.Blend[0] * 64.0f) == (key & 0x7F));

    const std::uint64_t mask = 0x8000000100000003ull;
    const GpuScene::ObjectState s = GpuScene::MakeObjectState(mask, sel);
    CHECK(s.Head[0] == 3u && s.Head[1] == 0x80000001u && s.Head[2] == 2u && s.Head[3] == 0u);
    CHECK(s.Probe[0] == glm::vec4(sel.Center[0], -1.0f) && s.Probe[1] == glm::vec4(sel.HalfSize[0], sel.Blend[0]));
    CHECK(s.Probe[2] == glm::vec4(sel.Center[1], -1.0f) && s.Probe[3] == glm::vec4(sel.HalfSize[1], sel.Blend[1]));
    const GpuScene::ObjectState none = GpuScene::MakeObjectState(0, ReflectionProbeArray().Resolve(centre));
    CHECK(none.Head[2] == 0u && none.Probe[0] == glm::vec4(0.0f)); // no probes: count 0 (the sky, as uProbeCount 0)

    // The mirror: slots past the synced count are ignored; a write that changes nothing is not uploaded again.
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path) || !GpuScene::Enabled()) return;
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    for (int i = 0; i < 3; ++i) world.CreateModelEntity(model, glm::vec3((float)i, 0.0f, 0.0f), glm::vec3(0), glm::vec3(1), "M");
    world.RebuildWorldTransformCache();
    ForgetGatherCandidates();
    GpuScene scene;
    scene.Sync(world, false);
    CHECK(scene.Count() >= 3 && scene.ObjectStateMirror().size() == scene.Count());
    scene.SetObjectState(1, s);
    scene.SetObjectState(scene.Count(), s); // out of range: ignored
    CHECK(scene.PendingObjectStates() == 1);
    CHECK(std::memcmp(&scene.ObjectStateMirror()[1], &s, sizeof s) == 0);
    scene.SetObjectState(1, s); // unchanged
    CHECK(scene.PendingObjectStates() == 1);
    scene.SetObjectState(0, none); // the mirror starts zeroed: nothing changed
    CHECK(scene.PendingObjectStates() == 1);
    ForgetGatherCandidates();
}

// G-MB: the slot state needs a 9th fragment SSBO block, past GL 4.3's minimum of 8; below it the run compiles without
// it (a global define every top-level source carries right after #version, ahead of the first #line).
void GpuSceneSlotStateBlockBudget() {
    CHECK(GpuScene::kSlotStateFragmentBlocks == 9);
    CHECK(!GpuScene::SlotStateFits(8) && GpuScene::SlotStateFits(9) && GpuScene::SlotStateFits(16));
    CHECK(GpuScene::SlotStateSupported()); // InitSlotStateSupport never ran: on

    const std::filesystem::path path = std::filesystem::temp_directory_path() / UnitTestSupport::TempName("tartarus_defines", ".glsl");
    { std::ofstream(path) << "// lead comment\n#version 430 core\nvoid main() {}\n"; }
    const std::string saved = ShaderLibrary::GlobalDefines();
    ShaderLibrary::SetGlobalDefines("#define _NO_SLOT_STATE 1\n");
    const std::string withDefine = ShaderLibrary::ReadFileAt(path.string());
    ShaderLibrary::SetGlobalDefines(saved);
    const std::string without = ShaderLibrary::ReadFileAt(path.string());
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const std::size_t version = withDefine.find("#version 430 core\n"), define = withDefine.find("#define _NO_SLOT_STATE 1\n");
    CHECK(version != std::string::npos && define == version + 18);
    CHECK(withDefine.find("#line", define) != std::string::npos); // the file's numbering still starts after it
    CHECK(without.find("_NO_SLOT_STATE") == std::string::npos);
}

// G4-c: the GPU scene's change log for the cached local shadows. Over random frames (moves, Cast Shadows writes,
// removals, a quiet frame now and then), every slot whose packed data changed is logged with a box holding where it was
// and where it is, and a frame that changed nothing logs nothing.
void GpuSceneChangeLog() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
        return;
    }
    if (!GpuScene::Enabled()) {
        UnitTestSupport::Skip("TARTARUS_NO_GPU_SCENE");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    World w;
    std::vector<entt::entity> all;
    for (int i = 0; i < 64; ++i)
        all.push_back(w.CreateModelEntity(model, glm::vec3((float)(i % 8) * 3.0f, 0.0f, (float)(i / 8) * 3.0f), glm::vec3(0), glm::vec3(1), "L"));
    w.RebuildWorldTransformCache();
    ForgetGatherCandidates();
    GpuScene scene;
    GpuScene::ChangeLog log;
    scene.TakeChangeLog(w, log);
    CHECK(log.All); // the first take: nothing before it is known
    scene.Sync(w, false);
    scene.TakeChangeLog(w, log);
    CHECK(log.All); // a full sync
    scene.Sync(w, false);
    scene.TakeChangeLog(w, log);
    CHECK(!log.All && log.Changes.empty()); // nothing changed
    std::mt19937 rng(4242);
    std::uniform_int_distribution<int> pick(0, (int)all.size() - 1), what(0, 9);
    std::uniform_real_distribution<float> step(-2.0f, 2.0f);
    int checked = 0, quietFrames = 0;
    for (int frame = 0; frame < 300; ++frame) {
        const std::vector<glm::mat4> xfBefore = scene.XformMirror();
        const std::vector<glm::vec4> boundsBefore = scene.BoundsMirror();
        const int edits = what(rng) < 3 ? 0 : 1 + what(rng) % 3; // some frames change nothing
        for (int k = 0; k < edits; ++k) {
            const entt::entity e = all[(size_t)pick(rng)];
            if (!w.Registry.valid(e)) continue;
            const int kind = what(rng);
            if (kind < 6) w.Registry.get<TransformComponent>(e).Position += glm::vec3(step(rng), 0.0f, step(rng));
            else if (kind < 9) {
                auto& r = w.Registry.get<RenderableComponent>(e);
                r.CastShadows = (RenderableComponent::ShadowCasting)(((int)r.CastShadows + 1) % 4);
            } else if (frame % 50 == 7) w.Registry.destroy(e); // now and then one leaves
        }
        w.RebuildWorldTransformCache();
        scene.Sync(w, false);
        scene.TakeChangeLog(w, log);
        if (log.All) continue; // a rebuild: anything may have changed, nothing to check slot by slot
        if (edits == 0) {
            CHECK(log.Changes.empty());
            ++quietFrames;
        }
        const std::vector<glm::mat4>& xf = scene.XformMirror();
        const std::vector<glm::vec4>& bounds = scene.BoundsMirror();
        const std::uint32_t had = (std::uint32_t)(boundsBefore.size() / 2);
        for (std::uint32_t i = 0; i < scene.Count(); ++i) {
            const bool changed = i >= had || std::memcmp(&xf[(size_t)i * 2], &xfBefore[(size_t)i * 2], 2 * sizeof(glm::mat4)) != 0 ||
                                 std::memcmp(&bounds[(size_t)i * 2], &boundsBefore[(size_t)i * 2], 2 * sizeof(glm::vec4)) != 0;
            const auto it = std::find_if(log.Changes.begin(), log.Changes.end(), [&](const GpuScene::SlotChange& c) { return c.Slot == i; });
            if (!changed) continue;
            ++checked;
            CHECK(it != log.Changes.end());
            if (it == log.Changes.end()) return;
            const glm::vec3 lo(bounds[(size_t)i * 2]), hi(bounds[(size_t)i * 2 + 1]);
            if (it->Bounded) { // holds where it is now and where it was
                CHECK(glm::all(glm::lessThanEqual(it->Min, lo)) && glm::all(glm::greaterThanEqual(it->Max, hi)));
                if (i < had) {
                    const glm::vec3 oldLo(boundsBefore[(size_t)i * 2]), oldHi(boundsBefore[(size_t)i * 2 + 1]);
                    CHECK(glm::all(glm::lessThanEqual(it->Min, oldLo)) && glm::all(glm::greaterThanEqual(it->Max, oldHi)));
                }
            }
        }
    }
    std::printf("[GpuSceneChangeLog] %d changed slots checked, %d quiet frames\n", checked, quietFrames);
    CHECK(checked > 50 && quietFrames > 10);
    // Another world synced in between: the next take says All.
    World other;
    other.CreateModelEntity(model, glm::vec3(0.0f), glm::vec3(0), glm::vec3(1), "O");
    other.RebuildWorldTransformCache();
    scene.Sync(other, false);
    scene.Sync(w, false);
    scene.TakeChangeLog(w, log);
    CHECK(log.All);
    ForgetGatherCandidates();
}

// G4-c: which quiet local lights a frame's changes wake. One slot moving inside one spot's reach wakes that spot only;
// moving out of a point's range (it was there) wakes the point; a light whose inputs moved, a dark light, a reported
// render-settings write and an All log wake the rest.
void LocalShadowQuietWake() {
    LocalShadowQuiet q;
    bool spotLive[2] = {true, true};
    bool pointLive[1] = {true};
    std::uint64_t spotKey[2] = {11, 12}, pointKey[1] = {21};
    // Spot 0 looks down -Z from (0, 2, 0), spot 1 down -Z from (100, 2, 0); the point sits at (0, 2, 50), range 5.
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 20.0f);
    Frustum spotReach[2] = {Frustum::FromViewProj(proj * glm::lookAt(glm::vec3(0, 2, 0), glm::vec3(0, 2, -1), glm::vec3(0, 1, 0))),
                            Frustum::FromViewProj(proj * glm::lookAt(glm::vec3(100, 2, 0), glm::vec3(100, 2, -1), glm::vec3(0, 1, 0)))};
    AABB pointRange[1] = {AABB{glm::vec3(-5, -3, 45), glm::vec3(5, 7, 55)}};
    LocalShadowQuiet::Lights lights;
    lights.SpotCount = 2;
    lights.SpotLive = spotLive;
    lights.SpotKey = spotKey;
    lights.SpotReach = spotReach;
    lights.PointCount = 1;
    lights.PointLive = pointLive;
    lights.PointKey = pointKey;
    lights.PointRange = pointRange;
    GpuScene::ChangeLog none;
    none.All = false;
    auto quietAll = [&] {
        q.Spots[0].Quiet = q.Spots[1].Quiet = q.Points[0].Quiet = true;
    };
    q.Wake(none, 7, lights); // the first wake takes the keys and the epoch: everything awake
    CHECK(!q.Spots[0].Quiet && !q.Spots[1].Quiet && !q.Points[0].Quiet);
    quietAll();
    q.Wake(none, 7, lights);
    CHECK(q.Spots[0].Quiet && q.Spots[1].Quiet && q.Points[0].Quiet); // nothing changed
    GpuScene::ChangeLog moved;
    moved.All = false;
    moved.Changes.push_back({3, glm::vec3(-0.5f, 1.5f, -6.0f), glm::vec3(0.5f, 2.5f, -5.0f), true}); // in front of spot 0
    q.Wake(moved, 7, lights);
    CHECK(!q.Spots[0].Quiet && q.Spots[1].Quiet && q.Points[0].Quiet);
    quietAll();
    moved.Changes[0] = {4, glm::vec3(-1.0f, 0.0f, 30.0f), glm::vec3(1.0f, 1.0f, 46.0f), true}; // left the point's range
    q.Wake(moved, 7, lights);
    CHECK(q.Spots[0].Quiet && q.Spots[1].Quiet && !q.Points[0].Quiet);
    quietAll();
    moved.Changes[0] = {5, glm::vec3(0.0f), glm::vec3(0.0f), false}; // unbounded: reaches every light
    q.Wake(moved, 7, lights);
    CHECK(!q.Spots[0].Quiet && !q.Spots[1].Quiet && !q.Points[0].Quiet);
    quietAll();
    spotKey[1] = 13; // spot 1's light moved
    q.Wake(none, 7, lights);
    CHECK(q.Spots[0].Quiet && !q.Spots[1].Quiet && q.Points[0].Quiet);
    quietAll();
    spotLive[0] = false; // spot 0 went dark: awake when it comes back
    q.Wake(none, 7, lights);
    spotLive[0] = true;
    q.Wake(none, 7, lights);
    CHECK(!q.Spots[0].Quiet && q.Spots[1].Quiet && q.Points[0].Quiet);
    quietAll();
    q.Wake(none, 8, lights); // a reported render-settings write (a material edit)
    CHECK(!q.Spots[0].Quiet && !q.Spots[1].Quiet && !q.Points[0].Quiet);
    quietAll();
    GpuScene::ChangeLog everything; // All: a rebuild, another world
    q.Wake(everything, 8, lights);
    CHECK(!q.Spots[0].Quiet && !q.Spots[1].Quiet && !q.Points[0].Quiet);
}

// G4-c: a LOD level change wakes the lights around the renderers it swapped (no slot moves), and a flip that is undone
// before the next look (another view's levels in between) wakes nothing.
void LocalShadowLodToggles() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    World w;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity group = w.CreateEmptyEntity(zero, zero, one, "Group");
    w.Registry.emplace<LODGroupComponent>(group).Size = 2.0f;
    entt::entity lv[2];
    for (int i = 0; i < 2; ++i) {
        lv[i] = w.CreateModelEntity(model, zero, zero, one, "LOD" + std::to_string(i));
        CHECK(w.SetParent(lv[i], group));
    }
    w.RebuildWorldTransformCache();
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
    auto lod = [&](float dist) { w.ApplyLod(glm::vec3(0.0f, 0.0f, dist), proj); }; // 1: LOD 0 shows, 2: LOD 1
    LodToggleWake wake;
    GpuScene::ChangeLog log;
    auto look = [&] {
        log.All = false;
        log.Changes.clear();
        wake.Append(w, log);
    };
    lod(1.0f);
    CHECK(w.IsLodCulled(lv[1]) && !w.IsLodCulled(lv[0]));
    look(); // the first look: everything
    CHECK(log.All);
    look(); // nothing flipped
    CHECK(!log.All && log.Changes.empty());
    lod(2.0f); // LOD 0 out, LOD 1 in: both renderers' boxes
    look();
    CHECK(!log.All && log.Changes.size() == 2);
    for (const GpuScene::SlotChange& c : log.Changes)
        CHECK(c.Bounded && glm::all(glm::lessThanEqual(c.Min, glm::vec3(0.01f))) && glm::all(glm::greaterThanEqual(c.Max, glm::vec3(-0.01f))));
    lod(1.0f); // another view's levels, then this one's again before the next look
    lod(2.0f);
    look();
    CHECK(!log.All && log.Changes.empty());
    w.Registry.get<RenderableComponent>(lv[0]).CastShadows = RenderableComponent::ShadowCasting::Off;
    lod(1.0f); // LOD 0 casts nothing: only LOD 1 going out counts
    look();
    CHECK(log.Changes.size() == 1);
    w.ClearLod(); // its untags count too
    look();
    CHECK(!log.All && log.Changes.size() == 1);
    lod(1.0f); // LOD 1 out again, then destroyed before the look: nothing to wake (its slot going is GpuScene's log)
    w.Registry.destroy(lv[1]);
    look();
    CHECK(!log.All && log.Changes.empty());
    World other; // another world: everything
    wake.Append(other, log);
    CHECK(log.All);
}

// GpuScene's upload plan and packing, without GL.
void GpuSceneRuns() {
    using Run = GpuScene::Run;
    std::vector<Run> runs;
    GpuScene::BuildRuns({}, runs);
    CHECK(runs.empty());
    GpuScene::BuildRuns({5}, runs);
    CHECK(runs.size() == 1 && runs[0] == Run(5u, 5u));
    // A gap of up to kMergeGap unchanged slots is uploaded with its neighbours; one more splits the run.
    GpuScene::BuildRuns({0, 1 + GpuScene::kMergeGap}, runs);
    CHECK(runs.size() == 1 && runs[0] == Run(0u, 1 + GpuScene::kMergeGap));
    GpuScene::BuildRuns({0, 2 + GpuScene::kMergeGap, 2 + GpuScene::kMergeGap, 2 * GpuScene::kMergeGap + 3}, runs); // a repeat merges too
    CHECK(runs.size() == 2 && runs[0] == Run(0u, 0u) && runs[1] == Run(2 + GpuScene::kMergeGap, 2 * GpuScene::kMergeGap + 3));
    // Past kMaxRuns runs: one span over all of them.
    std::vector<std::uint32_t> sparse;
    for (std::uint32_t i = 0; i <= GpuScene::kMaxRuns; ++i) sparse.push_back(i * 100);
    GpuScene::BuildRuns(sparse, runs);
    CHECK(runs.size() == 1 && runs[0] == Run(0u, sparse.back()));
    sparse.pop_back();
    GpuScene::BuildRuns(sparse, runs);
    CHECK(runs.size() == GpuScene::kMaxRuns);
    // The layer goes in as its low 16 bits: a negative one is not folded into layer 0.
    SceneSlot s;
    s.Alive = true;
    glm::mat4 xf[2];
    glm::vec4 b[2];
    auto layerBits = [&](int layer) {
        s.Layer = layer;
        GpuScene::Pack(s, xf, b);
        std::uint32_t bits;
        std::memcpy(&bits, &b[1].w, sizeof bits);
        return bits >> 16;
    };
    CHECK(layerBits(3) == 3u);
    CHECK(layerBits(0) == 0u);
    CHECK(layerBits(-1) == 0xFFFFu);
    CHECK(layerBits(-2) != layerBits(0));
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

// GpuCull (G-2): stage A's CPU reference gives GatherVisible's answer for every slot - the same flag tests, Frustum::
// Intersects, the Within box and TooSmallOnScreen - over random boxes, flags, layers and queries (camera, owner view,
// editor view with a layer mask, shadow casters with a Within box). Slots are packed by GpuScene::Pack, so the GPU
// scene's layout is what is tested. The compute is held to this reference.
void GpuCullStageAMatchesCpu() {
    std::mt19937 rng(1225);
    std::uniform_real_distribution<float> pos(-60.0f, 60.0f), size(0.001f, 6.0f), unit(0.0f, 1.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(70.0f), 16.0f / 9.0f, 0.1f, 200.0f);
    const glm::vec3 eye(1.0f, 1.7f, 2.0f);
    const glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0.3f, -0.1f, -1.0f), glm::vec3(0, 1, 0));
    const Frustum frustum = Frustum::FromViewProj(proj * view);
    const AABB within{glm::vec3(-20.0f, -5.0f, -20.0f), glm::vec3(20.0f, 10.0f, 20.0f)};
    constexpr std::uint16_t kSettingBits[] = {kSlotFlagCastOff, kSlotFlagShadowsOnly, kSlotFlagTransparent};
    int mismatches = 0, kept = 0, total = 0;
    for (int qi = 0; qi < 4; ++qi) {
        VisibilityQuery q;
        q.Cull = &frustum;
        if (qi == 0) { q.OwnerView = true; q.SizeCullEye = &eye; q.SizeCullProjScale = proj[1][1]; q.MinScreenHeight = 0.01f; }
        if (qi == 1) { q.SkipViewModels = true; q.SkipPlayerBody = true; q.SkipTransparent = true; }
        if (qi == 2) { q.EditorView = true; q.LayerVisibleMask = 0x0000FF0Fu; }
        if (qi == 3) { q.ShadowCasters = true; q.Within = &within; q.Cull = nullptr; }
        const GpuCull::ViewQuery gq = GpuCull::MakeQuery(q);
        for (int i = 0; i < 6000; ++i) {
            SceneSlot s;
            const glm::vec3 c(pos(rng), pos(rng) * 0.2f, pos(rng)), h(size(rng), size(rng), size(rng));
            s.WorldMin = c - h * 0.5f;
            s.WorldMax = c + h * 0.5f;
            s.Bounded = unit(rng) < 0.9f;
            s.Alive = unit(rng) < 0.95f;
            for (std::uint32_t bit = 0; bit < 8; ++bit)
                if (unit(rng) < 0.06f) s.Flags |= (std::uint16_t)(1u << bit);
            for (std::uint16_t bit : kSettingBits)
                if (unit(rng) < 0.06f) s.Flags |= bit;
            s.Layer = (int)(unit(rng) * 34.0f) - 1; // -1..32: out-of-range layers are never hidden
            const bool lod = unit(rng) < 0.05f;
            glm::mat4 xf[2];
            glm::vec4 bounds[2];
            GpuScene::Pack(s, xf, bounds);
            // GatherVisible's consider(), restated on the slot.
            bool cpu = s.Alive && !(s.Flags & (GpuCull::kSlotInactive | GpuCull::kSlotPoseSource)) && !lod;
            if (cpu && (s.Flags & ((q.ShadowCasters || !q.OwnerView) ? GpuCull::kSlotOwnerViewOnly : GpuCull::kSlotHiddenFromOwner))) cpu = false;
            if (cpu && q.SkipViewModels && (s.Flags & GpuCull::kSlotViewModel)) cpu = false;
            if (cpu && q.SkipPlayerBody && (s.Flags & GpuCull::kSlotPlayerBody)) cpu = false;
            if (cpu && q.EditorView && ((s.Flags & GpuCull::kSlotHiddenInScene) ||
                                        (s.Layer >= 0 && s.Layer < 32 && !((q.LayerVisibleMask >> s.Layer) & 1u)))) cpu = false;
            if (cpu && (s.Flags & (q.ShadowCasters ? kSlotFlagCastOff : kSlotFlagShadowsOnly))) cpu = false;
            if (cpu && q.SkipTransparent && (s.Flags & kSlotFlagTransparent)) cpu = false;
            if (cpu && s.Bounded) {
                if (q.Cull && !q.Cull->Intersects(AABB{s.WorldMin, s.WorldMax})) cpu = false;
                if (cpu && q.Within && !q.Within->Intersects(AABB{s.WorldMin, s.WorldMax})) cpu = false;
                if (cpu && q.SizeCullEye && TooSmallOnScreen(s.WorldMin, s.WorldMax, *q.SizeCullEye, q.SizeCullProjScale, q.MinScreenHeight)) cpu = false;
            }
            const bool gpu = GpuCull::PassesStageA(bounds, gq, lod);
            mismatches += cpu != gpu;
            kept += gpu;
            ++total;
        }
    }
    CHECK(mismatches == 0);
    CHECK(kept > total / 20 && kept < total); // the cases are mixed: some kept, some not
}

// GpuCull caster mode (G4-a): the per-cascade test's CPU twin gives ShadowFrame's castsInto answer for every slot and
// cascade - the cascade's frustum (Frustum::Intersects) then CascadedShadowMap::CasterReachesSlice, gated by the
// render mask - over a fitted 4-cascade map, a held cascade, unbounded slots and boxes along the cascades' reach. The
// slice data goes through CasterSliceOf (what the compute reads), so the upload is what is tested; the compute is
// held to this twin (plus its slack) by the verify mode and the smoke_min check.
void GpuCullCasterCascadesMatchCpu() {
    CascadedShadowMap csm;
    const glm::vec3 eye(3.0f, 1.7f, -2.0f);
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(10.0f, 1.0f, 20.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(70.0f), 16.0f / 9.0f, 0.05f, 500.0f);
    const glm::vec3 casterMin(-150.0f, -5.0f, -150.0f), casterMax(150.0f, 40.0f, 150.0f);
    csm.Update(view, proj, glm::normalize(glm::vec3(-0.4f, -0.8f, 0.3f)), 150.0f, &casterMin, &casterMax);
    CHECK(csm.Count() == 4);
    Frustum cells[CascadedShadowMap::kMaxCascades];
    for (int c = 0; c < csm.Count(); ++c) cells[c] = Frustum::FromViewProj(csm.LightViewProj(c));
    std::mt19937 rng(4401);
    std::uniform_real_distribution<float> along(-0.2f, 1.0f), side(-1.0f, 1.0f), size(0.02f, 8.0f), unit(0.0f, 1.0f);
    const glm::vec3 fwd = glm::normalize(glm::vec3(7.0f, -0.7f, 22.0f)), right = glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
    int mismatches = 0, twinMismatches = 0, pairs = 0, total = 0;
    unsigned seen = 0;
    for (const unsigned renderMask : {0xFu, 0xBu, 0x7u}) { // all, cascade 2 held, cascade 3 held
        for (const float kernel : {0.0f, 2.5f}) {
            const float bias = kernel > 0.0f ? 1.0f : 0.0f;
            const GpuCull::SunCasterBlock block = GpuCull::MakeSunCasterBlock(csm, cells, renderMask, kernel, bias);
            for (int c = 0; c < csm.Count(); ++c) CHECK(((block.Info.x >> c) & 1u) == ((renderMask >> c) & 1u));
            for (int i = 0; i < 4000; ++i) {
                // Boxes spread along the view out past the shadow distance, near the slices and well off to the sides.
                const float d = along(rng) * 200.0f;
                const glm::vec3 c = eye + fwd * d + right * side(rng) * (10.0f + 0.8f * std::abs(d)) +
                                    glm::vec3(0.0f, side(rng) * 15.0f, 0.0f);
                const glm::vec3 h(size(rng), size(rng), size(rng));
                SceneSlot s;
                s.WorldMin = c - h * 0.5f;
                s.WorldMax = c + h * 0.5f;
                s.Bounded = unit(rng) < 0.97f;
                s.Alive = true;
                glm::mat4 xf[2];
                glm::vec4 bounds[2];
                GpuScene::Pack(s, xf, bounds);
                // castsInto, restated on the slot.
                unsigned cpu = 0;
                for (int k = 0; k < csm.Count(); ++k) {
                    if (!((renderMask >> k) & 1u)) continue;
                    if (!s.Bounded || (cells[k].Intersects(AABB{s.WorldMin, s.WorldMax}) &&
                                       csm.CasterReachesSlice(k, s.WorldMin, s.WorldMax, kernel, bias)))
                        cpu |= 1u << k;
                    // The flat slice alone, bit for bit what CasterReachesSlice answers.
                    twinMismatches += CascadedShadowMap::ReachesSlice(block.Cascades[k].Slice, s.WorldMin, s.WorldMax) !=
                                      csm.CasterReachesSlice(k, s.WorldMin, s.WorldMax, kernel, bias);
                }
                const unsigned gpu = GpuCull::CasterCascades(bounds, block);
                mismatches += cpu != gpu;
                for (unsigned m = gpu; m; m &= m - 1) ++pairs;
                seen |= gpu;
                ++total;
            }
        }
    }
    CHECK(mismatches == 0);
    CHECK(twinMismatches == 0);
    CHECK(seen == 0xFu);                          // every cascade gets casters
    CHECK(pairs > total / 4 && pairs < total * 3); // and the cases are mixed: in some cascades, not all
    // The remap word: a slot and its cascade share one uint.
    CHECK(GpuCull::kSlotMask == 0x3FFFFFFFu);
    CHECK(((7u | (3u << GpuCull::kCascadeShift)) & GpuCull::kSlotMask) == 7u);
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

// A ticket whose jobs are all in but whose material the pump's budget left for the next pump still counts as in
// flight (AsyncInFlight), so a frameless drain (CLI tools, impostor bake) keeps pumping until it loads.
void AssetPumpWaitingTickets() {
    struct Job { bool Done = false; bool Complete() const { return Done; } };
    struct Ticket { std::vector<std::shared_ptr<Job>> Jobs; bool Ready = false; };
    auto done = std::make_shared<Job>(Job{true});
    auto running = std::make_shared<Job>(Job{false});
    auto deferred = std::make_shared<Ticket>(Ticket{{done}, false});      // jobs in, material not loaded: waiting
    auto noJobs = std::make_shared<Ticket>(Ticket{{}, false});            // nothing to wait for, not ready yet: waiting
    auto loading = std::make_shared<Ticket>(Ticket{{done, running}, false}); // a job still running: Active counts it
    auto ready = std::make_shared<Ticket>(Ticket{{done}, true});
    std::vector<std::weak_ptr<Ticket>> tickets{deferred, noJobs, loading, ready};
    { auto gone = std::make_shared<Ticket>(); tickets.push_back(gone); } // dropped by its owner
    CHECK(CountWaitingTickets(tickets) == 2);
    deferred->Ready = true; // the next pump loads its material
    CHECK(CountWaitingTickets(tickets) == 1);
    noJobs->Ready = true;
    running->Done = true;   // the last job lands: now waiting on the pump
    CHECK(CountWaitingTickets(tickets) == 1);
    loading->Ready = true;
    CHECK(CountWaitingTickets(tickets) == 0);
}

// A model's pending uploads share the staging ring. The first one waits for ring space that a later one holds (its copy
// is done and only a step retires it): stepping just the first never frees it. Every pending item gets a step.
void ModelUploadStepsFreeTheRing() {
    struct Up { bool Pending = true, Holds = false, CopyDone = false; };
    int freeSlots = 0; // the ring has room for one reservation, and the second upload holds it
    std::vector<Up> ups(2);
    ups[1].Holds = true;
    ups[1].CopyDone = true;
    const auto isPending = [](const Up& u) { return u.Pending; };
    const auto step = [&](Up& u) {
        if (u.Holds && u.CopyDone) { u.Pending = false; u.Holds = false; ++freeSlots; return true; } // upload + retire
        if (!u.Holds) {
            if (freeSlots == 0) return false;                  // the ring is full for now
            --freeSlots; u.Holds = true; u.CopyDone = true;    // reserved; the worker's copy lands
        }
        return false;                                          // copy not in yet: waiting
    };
    CHECK(StepPending(ups, isPending, step) == PendingStep::Progress); // the later upload retires its span...
    int steps = 1;
    while (StepPending(ups, isPending, step) != PendingStep::None && steps < 10) ++steps;
    CHECK(!ups[0].Pending && !ups[1].Pending);                         // ...so the first gets its space and finishes
    CHECK(steps <= 4);
    // Everything waiting is reported as waiting, nothing pending as none.
    std::vector<Up> stuck(2);
    int none = 0;
    CHECK(StepPending(stuck, isPending, [&](Up&) { return none != 0; }) == PendingStep::Waiting);
    std::vector<Up> done(2);
    done[0].Pending = done[1].Pending = false;
    CHECK(StepPending(done, isPending, step) == PendingStep::None);
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
void VolumetricInscatterCap() {
    CHECK(VolumetricFog::CapInscatter(0.3f, 2.0f) == 0.3f && VolumetricFog::CapInscatter(1.0f, 2.0f) == 1.0f); // below the knee: untouched
    CHECK(VolumetricFog::CapInscatter(50.0f, 0.0f) == 50.0f);                                                   // 0 = off
    float prev = 1.0f;
    for (float l = 1.0f; l < 1000.0f; l *= 1.3f) {
        const float c = VolumetricFog::CapInscatter(l, 2.0f);
        CHECK(c >= prev - 1e-6f && c <= 2.0f && c <= l + 1e-6f); // monotone, bounded by the cap, never brighter than the input
        prev = c;
    }
    CHECK(std::fabs(VolumetricFog::CapInscatter(1.0001f, 2.0f) - VolumetricFog::CapInscatter(0.9999f, 2.0f)) < 1e-3f); // continuous at the knee
}
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
    world.Render.AntiAliasing = World::AntiAliasingMode::Taa;
    world.Render.TaaSharpness = 0.6f;
    world.Render.Wetness = 0.8f;
    world.Render.PuddleAmount = 0.4f;
    world.Render.PuddleScale = 7.5f;
    const std::string saved = SceneSerializer::SaveToString(world);
    {
        World reloaded;
        CHECK(SceneSerializer::LoadFromString(reloaded, assets, saved));
        CHECK(reloaded.Render.AntiAliasing == World::AntiAliasingMode::Taa && reloaded.Render.TaaSharpness == 0.6f);
        CHECK(reloaded.Render.TaaEnabled() && reloaded.Render.EffectiveMsaaSamples() == World::kTaaMsaaSamples && !reloaded.Render.EffectiveFxaa());
        CHECK(reloaded.Render.Wetness == 0.8f && reloaded.Render.PuddleAmount == 0.4f && reloaded.Render.PuddleScale == 7.5f);
        CHECK(reloaded.Render.MsaaSamples == world.Render.MsaaSamples); // kept for switching back
    }
    nlohmann::json j = nlohmann::json::parse(saved);
    for (const char* k : {"antiAliasing", "taaSharpness", "wetness", "puddleAmount", "puddleScale"}) CHECK(j.erase(k) == 1);
    j["fxaa"] = true;
    {
        World old;
        CHECK(SceneSerializer::LoadFromString(old, assets, j.dump()));
        CHECK(old.Render.AntiAliasing == World::AntiAliasingMode::Fxaa && old.Render.EffectiveFxaa() && !old.Render.TaaEnabled());
        CHECK(old.Render.EffectiveMsaaSamples() == old.Render.MsaaSamples);
        CHECK(old.Render.Wetness == 0.0f && old.Render.PuddleAmount == 0.0f && old.Render.PuddleScale == 4.0f && old.Render.TaaSharpness == 0.35f);
    }
    {
        const World plain;
        CHECK(plain.Render.AntiAliasing == World::AntiAliasingMode::Msaa && plain.Render.EffectiveMsaaSamples() == plain.Render.MsaaSamples);
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
    world.Render.SkySourceMode = World::SkySource::Atmosphere;
    world.Render.Sky.CloudWindDirectionDegrees = 120.0f;
    world.Render.Sky.WindStrength = 0.6f;
    world.Render.Sky.WindGustiness = 0.3f;
    world.Render.Sky.WindGustSpeed = 6.5f;
    const std::string saved = SceneSerializer::SaveToString(world);
    {
        World reloaded;
        CHECK(SceneSerializer::LoadFromString(reloaded, assets, saved));
        CHECK(reloaded.Render.Sky.CloudWindDirectionDegrees == 120.0f && reloaded.Render.Sky.WindStrength == 0.6f);
        CHECK(reloaded.Render.Sky.WindGustiness == 0.3f && reloaded.Render.Sky.WindGustSpeed == 6.5f);
    }
    nlohmann::json j = nlohmann::json::parse(saved);
    CHECK(j.contains("sky"));
    for (const char* k : {"windStrength", "windGustiness", "windGustSpeed"}) j["sky"].erase(k);
    {
        World old;
        CHECK(SceneSerializer::LoadFromString(old, assets, j.dump()));
        const SkySettings d;
        CHECK(old.Render.Sky.WindStrength == d.WindStrength && old.Render.Sky.WindGustiness == d.WindGustiness);
        CHECK(old.Render.Sky.WindGustSpeed == d.WindGustSpeed && old.Render.Sky.CloudWindDirectionDegrees == 120.0f);
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
    world.Render.VolumetricEnabled = true;
    world.Render.VolumetricDensity = 0.07f;
    world.Render.VolumetricAnisotropy = -0.3f;
    world.Render.VolumetricHeightFalloff = 0.25f;
    world.Render.VolumetricBaseHeight = 2.0f;
    world.Render.VolumetricAlbedo = glm::vec3(0.5f, 0.6f, 0.7f);
    world.Render.VolumetricDistance = 40.0f;
    world.Render.VolumetricQuality = 2;
    const std::string saved = SceneSerializer::SaveToString(world);
    {
        World reloaded;
        CHECK(SceneSerializer::LoadFromString(reloaded, assets, saved));
        CHECK(reloaded.Render.VolumetricEnabled && reloaded.Render.VolumetricQuality == 2);
        CHECK(reloaded.Render.VolumetricDensity == 0.07f && reloaded.Render.VolumetricAnisotropy == -0.3f);
        CHECK(reloaded.Render.VolumetricHeightFalloff == 0.25f && reloaded.Render.VolumetricBaseHeight == 2.0f);
        CHECK(reloaded.Render.VolumetricAlbedo == glm::vec3(0.5f, 0.6f, 0.7f) && reloaded.Render.VolumetricDistance == 40.0f);
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
        CHECK(!old.Render.VolumetricEnabled && old.Render.VolumetricDensity == defaults.Render.VolumetricDensity &&
              old.Render.VolumetricAnisotropy == defaults.Render.VolumetricAnisotropy && old.Render.VolumetricQuality == 1);
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

// The built-in property table: a default material writes only the core keys (v2 and v1 names); every field
// round-trips through both formats; the accessors and the bind resolver agree on kinds, clamps and the emissive
// pre-multiply.
void MaterialBuiltinTable() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / UnitTestSupport::TempName("tartarus_mat_table");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto readJson = [](const std::string& path) {
        std::ifstream in(path);
        return nlohmann::json::parse(in, nullptr, false);
    };
    // Defaults: exactly the always-written keys.
    MaterialAsset def;
    def.Path = (dir / "M_Default.mat").string();
    def.ShaderPath = "engine://Standard.shader";
    CHECK(def.Save());
    const nlohmann::json dj = readJson(def.Path);
    CHECK(dj.contains("properties") && dj["properties"].size() == 14);
    for (const char* k : {"_BaseColor", "_Metallic", "_Roughness", "_EmissiveColor", "_EmissiveStrength", "_Triplanar",
                          "_TriplanarScale", "_AlbedoMap", "_NormalMap", "_MetallicRoughnessMap", "_MetallicMap",
                          "_RoughnessMap", "_AOMap", "_EmissiveMap"})
        CHECK(dj["properties"].contains(k));
    MaterialAsset defV1 = def;
    defV1.ShaderPath.clear();
    defV1.Path = (dir / "M_DefaultV1.mat").string();
    CHECK(defV1.Save());
    const nlohmann::json dj1 = readJson(defV1.Path);
    CHECK(dj1.contains("baseColor") && dj1.contains("emissiveMap") && !dj1.contains("transmission") && !dj1.contains("furEnabled"));

    // Every field away from its default, through v2 and v1.
    for (const bool v2 : {true, false}) {
        MaterialAsset ma;
        ma.Path = (dir / (v2 ? "M_All.mat" : "M_AllV1.mat")).string();
        if (v2) ma.ShaderPath = "engine://Standard.shader";
        Material& m = ma.Mat;
        m.BaseColor = {0.1f, 0.2f, 0.3f}; m.Metallic = 0.7f; m.Roughness = 0.2f;
        m.EmissiveColor = {0.5f, 0.25f, 0.0f}; m.EmissiveStrength = 3.0f;
        m.Triplanar = true; m.TriplanarScale = 2.0f; m.TriplanarBreakup = 0.4f;
        m.ClearCoat = 0.6f; m.ClearCoatRoughness = 0.1f; m.Anisotropy = -0.5f; m.AnisotropyRotation = 0.25f;
        m.Sheen = {0.2f, 0.2f, 0.4f}; m.SheenRoughness = 0.3f; m.SubsurfaceColor = {0.9f, 0.1f, 0.1f}; m.Thickness = 0.8f;
        m.TransmissionStrength = 0.5f; m.IOR = 1.33f; m.SubsurfaceEnabled = true; m.ReflectionProbes = true;
        m.GrungeStrength = 0.5f; m.GrungeScale = 2.0f; m.ReceiveDecals = false; m.NoWetness = true; m.Foliage = true;
        m.WindStrength = 0.3f; m.WindFrequency = 2.0f; m.WindFlutter = 0.1f; m.WindHeight = 3.0f; m.Translucency = 0.2f;
        m.UVTiling = {2.0f, 3.0f}; m.UVOffset = {0.5f, 0.25f}; m.DetailTiling = {8.0f, 6.0f};
        m.NormalStrength = 0.5f; m.NormalFlipY = true; m.DoubleSided = true; m.UseVertexColor = true;
        m.SpecularHighlights = false; m.GlossyReflections = false; m.ParallaxScale = 0.05f;
        m.Fur.Enabled = true; m.Fur.Shells = 8; m.Fur.Tint = glm::vec3(0.5f);
        ma.AlbedoMapPath = "a.png"; ma.ThicknessMapPath = "t.png"; ma.GrungeMapPath = "g.png"; ma.FurMaskPath = "f.png";
        CHECK(ma.Save());
        if (!v2) {
            const nlohmann::json j1 = readJson(ma.Path);
            CHECK(j1.contains("transmission") && j1.contains("subsurface") && j1.contains("vertexColors") && j1["furShells"].is_number_integer());
        }
        const auto back = MaterialAsset::Load(ma.Path);
        CHECK(back != nullptr);
        if (!back) continue;
        const Material& b = back->Mat;
        CHECK(b.BaseColor == m.BaseColor && b.Metallic == m.Metallic && b.Roughness == m.Roughness);
        CHECK(b.EmissiveColor == m.EmissiveColor && b.EmissiveStrength == m.EmissiveStrength);
        CHECK(b.Triplanar && b.TriplanarScale == 2.0f && b.TriplanarBreakup == 0.4f);
        CHECK(b.ClearCoat == 0.6f && b.ClearCoatRoughness == 0.1f && b.Anisotropy == -0.5f && b.AnisotropyRotation == 0.25f);
        CHECK(b.Sheen == m.Sheen && b.SheenRoughness == 0.3f && b.SubsurfaceColor == m.SubsurfaceColor && b.Thickness == 0.8f);
        CHECK(b.TransmissionStrength == 0.5f && b.IOR == 1.33f && b.SubsurfaceEnabled && b.ReflectionProbes);
        CHECK(b.GrungeStrength == 0.5f && b.GrungeScale == 2.0f && !b.ReceiveDecals && b.NoWetness && b.Foliage);
        CHECK(b.WindStrength == 0.3f && b.WindFrequency == 2.0f && b.WindFlutter == 0.1f && b.WindHeight == 3.0f && b.Translucency == 0.2f);
        CHECK(b.UVTiling == m.UVTiling && b.UVOffset == m.UVOffset && b.DetailTiling == m.DetailTiling);
        CHECK(b.NormalStrength == 0.5f && b.NormalFlipY && b.DoubleSided && b.UseVertexColor);
        CHECK(!b.SpecularHighlights && !b.GlossyReflections && b.ParallaxScale == 0.05f);
        CHECK(b.Fur.Enabled && b.Fur.Shells == 8 && b.Fur.Tint == glm::vec3(0.5f));
        CHECK(back->AlbedoMapPath == "a.png" && back->ThicknessMapPath == "t.png" && back->GrungeMapPath == "g.png" && back->FurMaskPath == "f.png");
        CHECK(back->NormalMapPath.empty());
    }

    // Accessors and the bind resolver.
    Material m;
    m.EmissiveColor = glm::vec3(0.5f);
    m.EmissiveStrength = 4.0f;
    CHECK(MaterialAsset::GetColor(m, "_EmissiveColor") == glm::vec3(2.0f));
    CHECK(MaterialAsset::GetAuthoredColor(m, "_EmissiveColor") == glm::vec3(0.5f));
    CHECK(!MaterialAsset::IsBuiltinProp("_SubsurfaceEnabled") && !MaterialAsset::IsBuiltinProp("_CoverStrength"));
    MaterialAsset::SetFloat(m, "_FurShells", 7.6f);
    CHECK(m.Fur.Shells == 8 && MaterialAsset::GetFloat(m, "_FurShells") == 8.0f);
    MaterialAsset::SetFloat(m, "_FurDensity", 0.0f);
    CHECK(m.Fur.Density == 1.0f);
    MaterialAsset::SetVec(m, "_UVTiling", glm::vec4(3.0f, 4.0f, 9.0f, 9.0f));
    CHECK(m.UVTiling == glm::vec2(3.0f, 4.0f) && MaterialAsset::GetVec(m, "_UVTiling") == glm::vec4(3.0f, 4.0f, 0.0f, 0.0f));
    MaterialAsset::SetFloat(m, "_BaseColor", 9.0f); // wrong kind: ignored
    CHECK(m.BaseColor == glm::vec3(1.0f) && MaterialAsset::GetFloat(m, "_BaseColor") == 0.0f);
    const std::shared_ptr<Texture>& (*tex)(const Material&) = nullptr;
    glm::vec4 (*val)(const Material&) = nullptr;
    MaterialAsset::ResolveBuiltin("_EmissiveColor", ShaderPropType::Color, tex, val);
    CHECK(!tex && val && glm::vec3(val(m)) == glm::vec3(2.0f));
    MaterialAsset::ResolveBuiltin("_FurShells", ShaderPropType::Int, tex, val);
    CHECK(val && val(m).x == 8.0f);
    MaterialAsset::ResolveBuiltin("_NormalFlipY", ShaderPropType::Bool, tex, val);
    m.NormalFlipY = true;
    CHECK(val && val(m).x == 1.0f);
    MaterialAsset::ResolveBuiltin("_Metallic", ShaderPropType::Color, tex, val);
    CHECK(!tex && !val);
    MaterialAsset::ResolveBuiltin("_DetailNormalMap", ShaderPropType::Texture2D, tex, val);
    CHECK(tex && !val && !tex(m));
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
    CHECK(!world.Render.PuddleSsr);
    const entt::entity probe = world.Registry.create();
    world.Registry.emplace<TransformComponent>(probe);
    CHECK(world.Registry.emplace<ReflectionProbeComponent>(probe).CaptureScene);
    CHECK(!nlohmann::json::parse(SceneSerializer::SaveToString(world)).contains("puddleSsr"));
    world.Render.PuddleSsr = true;
    world.Registry.get<ReflectionProbeComponent>(probe).CaptureScene = false;
    const std::string saved = SceneSerializer::SaveToString(world);
    World reloaded;
    CHECK(SceneSerializer::LoadFromString(reloaded, assets, saved));
    CHECK(reloaded.Render.PuddleSsr);
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
        CHECK(has(*text, "uniform float uPointShadowWeight[" + n(PointShadowMap::kMaxPoints) + "];"));
        CHECK(has(*text, "uniform float uSpotShadowWeight[" + n(SpotShadowMap::kMaxSpots) + "];"));
    }
    // S2 point PCF: the per-slot kernel array SceneRenderer uploads, and the hard path for Softness 0 kept.
    CHECK(has(shading, "uniform float uPointShadowSoftness[" + n(PointShadowMap::kMaxPoints) + "];"));
    CHECK(has(shading, "float soft = kernel > 0.0 ? smoothstep(0.03, 0.10, nlG) : 0.0;"));
    CHECK(has(shading, "bool PointSpotLight(uint i, vec3 N, vec3 geoN, out vec3 L, out vec3 radiance)"));
    CHECK(has(shading, "float ShadowPlanePCF(int kind, int slot, vec3 dir, float d, vec3 N, float texelW, float kernel, float biasW, float far) {"));
    // Tent PCF: 4x4 grid, binomial weights, spacing <= 1 texel (continuous penumbra: no bands, no grain).
    CHECK(has(shading, "float h = clamp((kernel - 1.0) / 1.5, 0.0, 1.0) * texelW;")); // taps <= 1 texel apart
    CHECK(has(shading, "const float kTentW[4] = float[](1.0, 3.0, 3.0, 1.0);"));
    // fix-3 spot PCF: SpotShadow takes the geometric normal and the shared PCF (kind 1).
    CHECK(has(shading, "float SpotShadow(int slot, vec3 worldPos, vec3 N, vec3 geoN) {"));
    CHECK(has(shading, "ShadowPlanePCF(1, slot, biasedPos - uSpotShadowPos[slot], d, geoN, texelW, kernel, biasW, far)"));
    CHECK(!has(shading, "kPoisson16[k] * radiusW")); // the equal-weight Poisson disk is gone from the point/spot PCF
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
    // A fade per face (local x is world -z, so the +x face is at world z -5 and the -x face at z -1).
    const glm::vec3 half3(0.3f);
    CHECK(box.FadeFaces({2, 1, -3 + 2.0f - 0.15f}, half3, half3) == box.Fade({2, 1, -3 + 2.0f - 0.15f}, 0.3f)); // equal faces = the scalar
    CHECK(box.FadeFaces({2, 1, -3}, half3, half3) == 1.0f && box.FadeFaces({2, 1, -3 + 2.5f}, half3, half3) == 0.0f);
    const glm::vec3 lo(0.3f), hi(1.0f, 0.3f, 0.3f); // a wide fade on the +x face only
    CHECK(std::fabs(box.FadeFaces({2, 1, -3 - 1.85f}, lo, hi) - 0.15f) < 1e-4f); // 0.15 m inside the +x face: 0.15 / 1.0
    CHECK(std::fabs(box.FadeFaces({2, 1, -3 + 1.85f}, lo, hi) - 0.50f) < 1e-4f); // the -x face keeps 0.3: 0.15 / 0.3
    CHECK(box.FadeFaces({2, 1, -3 - 2.05f}, lo, glm::vec3(0.0f)) == 0.0f && box.FadeFaces({2, 1, -3 - 1.99f}, lo, glm::vec3(0.0f)) == 1.0f); // 0 = hard edge
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

    // Face probes (in the wall / ceiling slab) are captured nudged inward (Jacob #5: the ceiling ring around a fixture).
    {
        const glm::ivec3 d(5, 4, 6);
        const glm::vec3 half(2.0f, 1.5f, 2.5f); // cell 1 m on every axis
        const auto near3 = [](const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-6f; };
        CHECK(near3(GI::FaceProbeNudge(glm::ivec3(2, 1, 3), d, half, 0.15f), glm::vec3(0.0f)));            // interior: none
        CHECK(near3(GI::FaceProbeNudge(glm::ivec3(2, 3, 3), d, half, 0.15f), glm::vec3(0.0f, -0.15f, 0.0f))); // ceiling row: down
        CHECK(near3(GI::FaceProbeNudge(glm::ivec3(2, 0, 3), d, half, 0.15f), glm::vec3(0.0f, 0.15f, 0.0f)));  // floor row: up
        CHECK(near3(GI::FaceProbeNudge(glm::ivec3(0, 0, 0), d, half, 0.15f), glm::vec3(0.15f)));              // corner: 3 axes
        CHECK(near3(GI::FaceProbeNudge(glm::ivec3(4, 3, 5), d, half, 0.15f), glm::vec3(-0.15f)));
        CHECK(near3(GI::FaceProbeNudge(glm::ivec3(1, 0, 1), glm::ivec3(3, 1, 3), half, 0.15f), glm::vec3(0.0f))); // 1 probe on an axis: no face
        CHECK(near3(GI::FaceProbeNudge(glm::ivec3(2, 3, 3), d, half, 0.0f), glm::vec3(0.0f)));                 // 0 = off
        CHECK(near3(GI::FaceProbeNudge(glm::ivec3(2, 3, 3), d, half, 1.0f), glm::vec3(0.0f, -0.45f, 0.0f)));  // clamped to 0.45 cell
    }
    // The nudge guard: a face probe is nudged only into open air (Jacob: child's bedroom 2, closet doors).
    {
        const auto S = [](float x0, float y0, float z0, float x1, float y1, float z1) { return GI::SolidBox{glm::vec3(x0, y0, z0), glm::vec3(x1, y1, z1)}; };
        const glm::vec3 face(9.42f, 4.0f, 25.0f), nudged(9.27f, 4.0f, 25.0f);
        CHECK(!GI::NudgeBlocked(face, nudged, {}));                                                  // open air
        CHECK(GI::NudgeBlocked(face, nudged, {S(8.0f, 3.0f, 24.0f, 9.3f, 5.0f, 26.0f)}));             // a wardrobe whose front is 0.12 m behind the face
        CHECK(!GI::NudgeBlocked(face, glm::vec3(9.35f, 4.0f, 25.0f), {S(8.0f, 3.0f, 24.0f, 9.3f, 5.0f, 26.0f)})); // the nudge stops short of it
        CHECK(!GI::NudgeBlocked(face, nudged, {S(8.0f, 3.0f, 20.0f, 9.3f, 5.0f, 21.0f)}));            // a box beside the path
        CHECK(!GI::NudgeBlocked(glm::vec3(9.0f, 4.0f, 25.0f), glm::vec3(8.85f, 4.0f, 25.0f), {S(8.0f, 3.0f, 24.0f, 9.3f, 5.0f, 26.0f)})); // the face point is already inside it (a slab)
        CHECK(!GI::NudgeBlocked(glm::vec3(5.0f, 5.0f, 5.0f), glm::vec3(5.0f, 4.85f, 5.0f), {S(0.0f, 4.9f, 0.0f, 9.0f, 5.1f, 9.0f)})); // a ceiling slab: the nudge leaves it
        CHECK(GI::NudgeBlocked(glm::vec3(5.0f, 5.0f, 5.0f), glm::vec3(5.0f, 4.85f, 5.0f), {S(0.0f, 4.0f, 4.0f, 9.0f, 4.9f, 6.0f)}));   // a fixture hanging 0.1 m below the face
        // A volume's face sits 2-5 cm off the slab it is padded to: a slab within 6 cm of the face point holds it (the nudge leaves it);
        // a box 7 cm or more away still blocks.
        CHECK(!GI::NudgeBlocked(glm::vec3(5.0f, 0.0f, 5.0f), glm::vec3(5.0f, 0.15f, 5.0f), {S(0.0f, 0.03f, 0.0f, 9.0f, 0.20f, 9.0f)}));  // a floor slab 3 cm above the face
        CHECK(!GI::NudgeBlocked(glm::vec3(5.0f, 0.0f, 5.0f), glm::vec3(5.0f, 0.15f, 5.0f), {S(0.0f, 0.05f, 0.0f, 9.0f, 0.20f, 9.0f)}));  // 5 cm
        CHECK(GI::NudgeBlocked(glm::vec3(5.0f, 0.0f, 5.0f), glm::vec3(5.0f, 0.15f, 5.0f), {S(0.0f, 0.07f, 0.0f, 9.0f, 0.20f, 9.0f)}));   // 7 cm: still blocks
        CHECK(!GI::NudgeBlocked(glm::vec3(9.0f, 4.0f, 25.0f), glm::vec3(8.85f, 4.0f, 25.0f), {S(9.04f, 3.0f, 24.0f, 9.5f, 5.0f, 26.0f)})); // a wall piece 4 cm behind the face
        // Whole volume: 5 x 4 x 5 probes, x faces at +-2. Face probes without a wardrobe all nudge; with one against the +x face the ones behind it stay.
        const GI::Box nbox = GI::Box::FromTransform(glm::mat4(1.0f), glm::vec3(4.0f, 3.0f, 4.0f));
        const glm::ivec3 d = GI::GridDims(nbox.Half, 1.0f);
        int ok0 = 0, blk0 = 0, ok1 = 0, blk1 = 0;
        const auto free = GI::PlanFaceNudges(nbox, d, 0.15f, {}, &ok0, &blk0);
        const auto shut = GI::PlanFaceNudges(nbox, d, 0.15f, {S(1.0f, -1.5f, -2.5f, 1.9f, 1.5f, 2.5f)}, &ok1, &blk1); // a wardrobe whose front is 0.1 m inside the +x face
        CHECK(blk0 == 0 && ok0 > 0);
        CHECK(blk1 > 0 && ok1 + blk1 == ok0);                 // every probe it blocked would have nudged
        CHECK(free.size() == shut.size() && free.size() == (std::size_t)d.x * d.y * d.z);
        CHECK(shut[GI::ProbeIndex(glm::ivec3(d.x - 1, 2, 2), d)] == glm::vec3(0.0f));       // a +x face probe in front of the wardrobe stays
        CHECK(free[GI::ProbeIndex(glm::ivec3(d.x - 1, 2, 2), d)] != glm::vec3(0.0f));
        CHECK(shut[GI::ProbeIndex(glm::ivec3(0, 2, 2), d)] != glm::vec3(0.0f));             // the -x face (open air) still nudges
        CHECK(GI::PlanFaceNudges(nbox, d, 0.0f, {}, &ok0, &blk0)[0] == glm::vec3(0.0f) && ok0 == 0); // 0 = off
    }
    // FillInvalid on a face row: a whole invalid ceiling row is filled from the row below, blurred in-plane, so one strongly
    // directional probe under it does not paint a hot spot; and a face probe with a valid in-plane neighbour takes those.
    {
        const glm::ivec3 big(5, 3, 5); // the top plane's inner 3x3 probes lie on ONE face (the ceiling) only
        std::vector<GI::ProbeSH> pr(75);
        std::vector<bool> valid(75, true);
        for (int z = 0; z < 5; ++z)
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 5; ++x) {
                    const int i = GI::ProbeIndex(glm::ivec3(x, y, z), big);
                    valid[i] = y < 2;
                    pr[i].c[0].x = (y == 1 && x == 2 && z == 2) ? 9.0f : 1.0f; // one strongly directional probe under the ceiling
                }
        GI::FillInvalid(pr, valid, big);
        float mean = 0.0f, mx = 0.0f;
        for (int z = 1; z <= 3; ++z)
            for (int x = 1; x <= 3; ++x) {
                const float v = pr[GI::ProbeIndex(glm::ivec3(x, 2, z), big)].c[0].x;
                mean += v / 9.0f;
                mx = std::max(mx, v);
            }
        const float inplaneMean = (9.0f + 8.0f) / 9.0f; // the inner 3x3 of the row below
        CHECK(std::abs(mean - inplaneMean) < 0.25f * inplaneMean);
        CHECK(mx < 4.0f); // the old 6-neighbour average copied the 9 straight up
        const glm::ivec3 d(3, 3, 3);
        std::vector<GI::ProbeSH> q(27);
        std::vector<bool> v2(27, true);
        for (auto& p : q) p.c[0].x = 100.0f;                         // the inward row is hot
        for (int z = 0; z < 3; ++z)
            for (int x = 0; x < 3; ++x) q[GI::ProbeIndex(glm::ivec3(x, 2, z), d)].c[0].x = 5.0f; // the ceiling row is 5
        const int top = GI::ProbeIndex(glm::ivec3(1, 2, 1), d);
        v2[top] = false;
        q[top].c[0].x = -7.0f;
        GI::FillInvalid(q, v2, d);
        CHECK(std::abs(q[top].c[0].x - 5.0f) < 1e-5f); // in-plane first, not the inward neighbour
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
    CHECK(gi.Volumes()[0].FadeLo == glm::vec3(gc.Fade) && gi.Volumes()[0].FadeHi == glm::vec3(gc.Fade)); // no per-face fade: the volume's
    gc.FadeMax = glm::vec3(0.0f, 0.0f, 0.4f);
    gc.FadeMin = glm::vec3(0.25f, 0.0f, 0.0f);
    gi.Gather(world);
    CHECK(gi.Volumes()[0].FadeHi == glm::vec3(gc.Fade, gc.Fade, 0.4f) && gi.Volumes()[0].FadeLo == glm::vec3(0.25f, gc.Fade, gc.Fade));
    gc.FadeMax = gc.FadeMin = glm::vec3(0.0f);
    gi.Gather(world);
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
using UnitTestSupport::ReadAll;

// GI Phase 1 (W3): GPU-driven batches merge only groups the per-draw hook binds alike. The GI volume mask and every bit
// of the probe selection must split them; otherwise a group is shaded with its batch lead's GI volumes (launch-dependent).
void TestOpaqueBatchKey() {
    namespace K = OpaqueBatchKey;
    K::ObjectState a;
    a.LayerBit = 1u;
    a.ProbeKey = (2u << 30) | (3u << 20) | (7u << 10) | 40u;
    a.GIMask = 0x0000000000000003ull;
    const std::uint64_t ka = K::Object(a);
    CHECK(K::Object(a) == ka); // deterministic: equal state, equal key
    K::ObjectState b = a;
    b.GIMask = 0x0000000000000006ull; // another room's volumes
    CHECK(K::Object(b) != ka);
    b = a;
    b.GIMask = a.GIMask | (1ull << 40); // a high volume bit (the second mask word)
    CHECK(K::Object(b) != ka);
    b = a;
    b.ProbeKey = a.ProbeKey + (16u << 20); // primary probe 3 -> 19: only bits 24-31 differ (lost by the old << 40)
    CHECK(K::Object(b) != ka);
    b = a;
    b.ProbeKey = (1u << 30) | (a.ProbeKey & 0x3FFFFFFFu); // one probe instead of two: bits 30-31 only
    CHECK(K::Object(b) != ka);
    for (int f = 0; f < 6; ++f) { // each flag still splits
        b = a;
        bool* flags[6] = {&b.ReceiveShadows, &b.StaticReceiver, &b.NoSsao, &b.DoubleSided, &b.Prepassed, &b.GIFlatL1};
        *flags[f] = !*flags[f];
        CHECK(K::Object(b) != ka);
    }
    b = a;
    b.LayerBit = 2u;
    CHECK(K::Object(b) != ka);
}

// A still spot / point light redraws its static depth only when a caster's signature changes. A colourway swaps one
// material slot in place (same count, same first slot): that must change the signature too.
void TestLocalCasterHashSlots() {
    auto a = std::make_shared<MaterialAsset>();
    auto b = std::make_shared<MaterialAsset>();
    auto c = std::make_shared<MaterialAsset>();
    const glm::mat4 xf(1.0f);
    const std::vector<std::shared_ptr<MaterialAsset>> before{a, b};
    const std::vector<std::shared_ptr<MaterialAsset>> after{a, c}; // slot 1 recoloured
    const std::uint64_t h0 = LocalCasterHash(7, nullptr, before, xf, 0, false);
    CHECK(LocalCasterHash(7, nullptr, before, xf, 0, false) == h0); // stable
    CHECK(LocalCasterHash(7, nullptr, after, xf, 0, false) != h0);
    CHECK(LocalCasterHash(7, nullptr, {b, a}, xf, 0, false) != h0); // slot order counts
    CHECK(LocalCasterHash(7, nullptr, {a}, xf, 0, false) != h0);
    CHECK(LocalCasterHash(7, nullptr, before, xf, 3, false) != h0); // hidden-triangle set
    CHECK(LocalCasterHash(7, nullptr, before, xf, 0, true) != h0);
    CHECK(LocalCasterHash(8, nullptr, before, xf, 0, false) != h0);
    CHECK(LocalCasterHash(7, nullptr, before, glm::translate(xf, glm::vec3(0, 0.01f, 0)), 0, false) != h0);
}

// Material edits written into the same MaterialAsset (the Inspector's Alpha Cutoff slider, Double Sided, a reload's
// copy into the same object) keep every slot pointer, so the signature must also read what the depth draw reads, and
// the edit must report itself (RenderSettingsEpoch) so a quiet light wakes to look.
void TestLocalCasterHashMaterialContent() {
    auto model = Model::CreatePrimitive("terrain", "w2_caster_content.fbx"); // CPU-only stand-in: no GL in unit tests
    CHECK(model && model->MeshCount() >= 1);
    if (!model || model->MeshCount() < 1) return;
    auto leaf = std::make_shared<MaterialAsset>();
    leaf->Mat.Foliage = true; // the depth draw binds this material's own values (sway, cutoff)
    const std::vector<std::shared_ptr<MaterialAsset>> slots{leaf};
    const glm::mat4 xf(1.0f);
    const auto sig = [&]() { Material::NextHashFrame(); return LocalCasterHash(7, model.get(), slots, xf, 0, false); };

    const std::uint64_t h0 = sig();
    const std::uint32_t e0 = RenderSettingsEpoch();
    CHECK(sig() == h0);                      // stable across frames
    CHECK(RenderSettingsEpoch() == e0);      // and no change reported
    leaf->Mat.AlphaCutoff = 0.8f;            // the Inspector slider: in place, no call
    const std::uint64_t h1 = sig();
    CHECK(h1 != h0);
    CHECK(RenderSettingsEpoch() != e0);      // the hash's depth watch reported it
    const std::uint32_t e1 = RenderSettingsEpoch();
    leaf->Mat.BaseColor = glm::vec3(0.2f);   // colour only: not a depth field, nothing reported, no redraw
    CHECK(sig() == h1);
    CHECK(RenderSettingsEpoch() == e1);
    leaf->Mat.BaseColor = glm::vec3(1.0f);

    leaf->Mat.DoubleSided = true;            // the depth draw's cull
    CHECK(sig() != h1);
    leaf->Mat.DoubleSided = false;
    CHECK(sig() == h1);

    MaterialAsset fresh = *leaf;             // a reload: a new object copied INTO the old one
    fresh.Mat.AlphaCutoff = 0.3f;
    const std::uint32_t e2 = RenderSettingsEpoch();
    fresh.Mat.Hash();                        // a copy starts unseen: hashing it reports nothing
    CHECK(RenderSettingsEpoch() == e2);
    *leaf = fresh;
    CHECK(sig() != h1);
    CHECK(RenderSettingsEpoch() != e2);

    leaf->RenderQueue = MaterialAsset::Queue::Transparent; // casts nothing now
    const std::uint64_t ht = sig();
    leaf->RenderQueue = MaterialAsset::Queue::Opaque;
    CHECK(sig() != ht);
}

// GI Phase 1 (W2): room-aware, stable shadow slots that fade instead of popping (ShadowSlotScoring).
void TestShadowSlotScoring() {
    namespace S = ShadowSlotScoring;
    // Room names: the trailing number (and the space before it) goes; nothing else does.
    CHECK(S::RoomKey("GI living room 1") == "GI living room");
    CHECK(S::RoomKey("GI living room 12") == "GI living room");
    CHECK(S::RoomKey("GI_hall_2") == "GI_hall");
    CHECK(S::RoomKey("GI kitchen") == "GI kitchen");
    CHECK(S::RoomKey("Bedroom2") == "Bedroom");
    CHECK(S::RoomKey("42") == "42");

    auto box = [](glm::vec3 c, glm::vec3 size, float yawDeg = 0.0f) {
        const glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1.0f), c), glm::radians(yawDeg), glm::vec3(0, 1, 0));
        return GI::Box::FromTransform(m, size);
    };
    // Touching: within the gap (a 0.2 m wall), not past it; rotated boxes use the full separating-axis test.
    CHECK(S::Touch(box({0, 0, 0}, {4, 3, 4}), box({4.2f, 0, 0}, {4, 3, 4}), S::kNeighbourGap));
    CHECK(!S::Touch(box({0, 0, 0}, {4, 3, 4}), box({4.6f, 0, 0}, {4, 3, 4}), S::kNeighbourGap));
    CHECK(S::Touch(box({0, 0, 0}, {4, 3, 4}), box({4.6f, 0, 0}, {4, 3, 4}, 45.0f), S::kNeighbourGap)); // its corner reaches
    CHECK(!S::Touch(box({0, 0, 0}, {2, 3, 2}), box({2.0f, 0, 2.0f}, {1, 3, 1}, 45.0f), 0.0f));          // diagonal gap

    // A house: the living room is two volumes; the hall is through a wall from it; the bedroom is across the hall.
    //   living 1: x -2..2   living 2: x 2..5   hall: x 5.2..7.2   bedroom: x 7.4..11.4 (touches the hall only)
    S::Rooms rooms;
    rooms.Add("GI living room 1", box({0, 1.5f, 0}, {4, 3, 4}));
    rooms.Add("GI living room 2", box({3.5f, 1.5f, 0}, {3, 3, 4}));
    rooms.Add("GI hall", box({6.2f, 1.5f, 0}, {2, 3, 4}));
    rooms.Add("GI bedroom 1", box({9.4f, 1.5f, 0}, {4, 3, 4}));
    CHECK(rooms.Names.size() == 3 && rooms.Boxes.size() == 4);
    const glm::vec3 view(-1.0f, 1.6f, 0.0f); // in living 1
    rooms.Classify(view);
    CHECK(rooms.ViewRoom >= 0 && rooms.Names[rooms.ViewRoom] == "GI living room");
    CHECK(rooms.TierAt({0.5f, 2.0f, 1.0f}) == S::Tier::Room);       // living 1
    CHECK(rooms.TierAt({4.5f, 2.0f, 1.0f}) == S::Tier::Room);       // living 2: the same room
    CHECK(rooms.TierAt({4.5f, 3.1f, 1.0f}) == S::Tier::Room);       // a ceiling fitting just above the box (kLightPad)
    CHECK(rooms.TierAt({6.2f, 2.0f, 0.0f}) == S::Tier::Neighbour);  // the hall
    CHECK(rooms.TierAt({5.25f, 2.0f, 0.0f}) == S::Tier::Neighbour); // a hall wall lamp, 5 cm from the living room's box
    CHECK(rooms.TierAt({10.0f, 2.0f, 0.0f}) == S::Tier::Other);     // the bedroom: two rooms away
    CHECK(rooms.TierAt({0.0f, 2.0f, 9.0f}) == S::Tier::Other);      // outdoors
    // Overlapping volumes: the smallest one holding the view is its room.
    S::Rooms nested;
    nested.Add("GI house", box({0, 1.5f, 0}, {20, 3, 20}));
    nested.Add("GI closet", box({1, 1.5f, 1}, {1, 3, 1}));
    nested.Classify({1.0f, 1.0f, 1.0f});
    CHECK(nested.Names[nested.ViewRoom] == "GI closet");
    // A doorway gap (in no volume, between living 2 and the hall): the view keeps its last room, so crossing a door is
    // one re-ranking, not two (Talos's #1189 review).
    rooms.Classify({5.1f, 1.6f, 0.0f});
    CHECK(rooms.ViewRoom >= 0 && rooms.Names[rooms.ViewRoom] == "GI living room");
    rooms.Classify({6.2f, 1.6f, 0.0f}); // into the hall: now the hall
    CHECK(rooms.Names[rooms.ViewRoom] == "GI hall" && rooms.TierAt({0.5f, 2.0f, 1.0f}) == S::Tier::Neighbour);
    rooms.Classify({5.1f, 1.6f, 0.0f}); // back in the gap: still the hall
    CHECK(rooms.Names[rooms.ViewRoom] == "GI hall");
    // Outside every volume, and past kStickyPad from the last room, nothing is boosted: the old #110 score.
    rooms.Classify({0.0f, 1.6f, 20.0f});
    CHECK(rooms.ViewRoom == -1 && rooms.StickyRoom.empty() && rooms.TierAt({0.5f, 2.0f, 1.0f}) == S::Tier::Other);
    rooms.Classify(view);
    // The room upstairs, through a 0.2 m floor slab, touches the living room's box but is not its neighbour.
    {
        S::Rooms storeys;
        storeys.Add("GI living room 1", box({0, 1.5f, 0}, {4, 3, 4}));
        storeys.Add("GI den", box({0, 4.7f, 0}, {4, 3, 4}));       // y 3.2..6.2
        storeys.Add("GI stairwell", box({2.5f, 3.1f, 0}, {1, 6.2f, 2})); // both storeys
        CHECK(S::Touch(storeys.Boxes[0].Box, storeys.Boxes[1].Box, S::kNeighbourGap));
        storeys.Classify({-1.0f, 1.6f, 0.0f});
        CHECK(storeys.TierAt({0.0f, 5.0f, 0.0f}) == S::Tier::Other);       // the den lamp upstairs
        CHECK(storeys.TierAt({2.5f, 2.0f, 0.0f}) == S::Tier::Neighbour);   // the stairwell, beside the living room
        storeys.Classify({0.0f, 4.5f, 0.0f});                              // upstairs in the den
        CHECK(storeys.Names[storeys.ViewRoom] == "GI den" && storeys.TierAt({0.0f, 1.0f, 0.0f}) == S::Tier::Other);
        CHECK(storeys.TierAt({2.5f, 5.8f, 0.5f}) == S::Tier::Neighbour);   // the stairwell spans both storeys
    }

    // Scores: the room first, then its neighbours, then the rest; held lights keep a 25% edge.
    const float base = S::Score(10.0f, 6.0f, 2.0f, S::Tier::Other, false);
    CHECK(base == 10.0f * 36.0f);                                         // inside its range: the full score
    CHECK(S::Score(10.0f, 6.0f, 2.0f, S::Tier::Neighbour, false) == base * S::kNeighbourBoost);
    CHECK(S::Score(10.0f, 6.0f, 2.0f, S::Tier::Room, false) == base * S::kRoomBoost);
    CHECK(S::Score(10.0f, 6.0f, 2.0f, S::Tier::Other, true) == base * S::kHoldBonus);
    CHECK(S::Score(10.0f, 6.0f, 9.0f, S::Tier::Other, false) == 10.0f * 36.0f / 10.0f); // 3 m past its range
    // The case the orders are about: a bright hall lamp right by the view, and a dimmer lamp across the living room.
    // Before (#110's distance score) the hall lamp won the one slot; now the lamp in the player's room does.
    const glm::vec3 hallLamp(5.3f, 2.4f, 0.0f), roomLamp(4.5f, 2.4f, 1.5f);
    const float hallOld = S::Score(20.0f, 8.0f, glm::length(hallLamp - view), S::Tier::Other, false);
    const float roomOld = S::Score(8.0f, 8.0f, glm::length(roomLamp - view), S::Tier::Other, false);
    CHECK(hallOld > roomOld);
    const float hallNew = S::Score(20.0f, 8.0f, glm::length(hallLamp - view), rooms.TierAt(hallLamp), false);
    const float roomNew = S::Score(8.0f, 8.0f, glm::length(roomLamp - view), rooms.TierAt(roomLamp), false);
    CHECK(roomNew > hallNew);
    // Hysteresis: a holder keeps its slot against a challenger up to 25% better, and loses it past that.
    CHECK(S::Score(10.0f, 6.0f, 2.0f, S::Tier::Room, true) > S::Score(12.0f, 6.0f, 2.0f, S::Tier::Room, false));
    CHECK(S::Score(10.0f, 6.0f, 2.0f, S::Tier::Room, true) < S::Score(13.0f, 6.0f, 2.0f, S::Tier::Room, false));

    // Fades: one slot, light A holds it, then B is wanted instead. A fades out, B waits, then B fades in.
    using Slots = S::FadingSlots<4>;
    const std::uint32_t A = 1, B = 2, C = 3, T = 99;
    Slots slots;
    const float dt = 1.0f / 60.0f, step = dt / S::kFadeSeconds;
    CHECK(slots.Step({A}, {A, B}, 1, 1, dt, true) == 1); // snap: in at full strength
    CHECK(slots.Owner[0] == A && slots.Weight[0] == 1.0f);
    float shownA = 1.0f, shownB = 0.0f; // what each light's shadow shows (0 when it holds no slot)
    bool bIn = false;
    int frames = 0;
    for (std::uint64_t f = 2; f < 200 && !(bIn && shownB == 1.0f); ++f, ++frames) {
        slots.Step({B}, {A, B}, 1, f, dt, false);
        const float a = slots.Owner[0] == A ? slots.Weight[0] : 0.0f;
        const float b = slots.Owner[0] == B ? slots.Weight[0] : 0.0f;
        CHECK(std::abs(a - shownA) <= step + 1e-5f); // never a jump bigger than one frame of fade
        CHECK(std::abs(b - shownB) <= step + 1e-5f);
        CHECK(a == 0.0f || b == 0.0f);               // one slot: never both
        bIn = bIn || b > 0.0f;
        shownA = a; shownB = b;
    }
    CHECK(bIn && shownB == 1.0f && shownA == 0.0f);
    CHECK(frames <= (int)std::ceil(2.0f / step) + 2); // out, then in: about two fade times
    // B, holding the slot, is switched off (no candidate any more): freed at once, there is no light left to fade.
    // A was lit all along (a candidate last frame), so it is a hand-over and A fades in.
    CHECK(slots.Owner[0] == B);
    slots.Step({A}, {A}, 1, 300, dt, false);
    CHECK(slots.Owner[0] == A && std::abs(slots.Weight[0] - step) < 1e-5f);
    // Switch-on (Talos's #1189 review): a lamp that was NOT a candidate last frame lights at once, so its shadow is
    // full at once - with a free slot, or by taking a leaving holder's - never shining through walls while it fades.
    {
        Slots on;
        on.Step({A}, {A}, 1, 1, dt, true);                  // A holds the one slot; C is off
        on.Step({C}, {A, C}, 1, 2, dt, false);              // C switched on and outscores A
        CHECK(on.Owner[0] == C && on.Weight[0] == 1.0f);    // full the same frame (A, a leaver, gave the slot up)
        Slots free2;
        free2.Step({A}, {A}, 2, 1, dt, true);
        free2.Step({A, C}, {A, C}, 2, 2, dt, false);        // a free slot: C still comes in at full
        int c = free2.Owner[0] == C ? 0 : 1;
        CHECK(free2.Owner[c] == C && free2.Weight[c] == 1.0f);
        // Switched off and on again (off for a frame): it arrives again, full again.
        free2.Step({A}, {A}, 2, 3, dt, false);
        CHECK(free2.Owner[0] != C && free2.Owner[1] != C);
        free2.Step({A, C}, {A, C}, 2, 4, dt, false);
        c = free2.Owner[0] == C ? 0 : 1;
        CHECK(free2.Owner[c] == C && free2.Weight[c] == 1.0f);
        // A lit lamp that was a candidate without a slot still waits and fades (the hand-over case above).
        Slots lit;
        lit.Step({A}, {A, C}, 1, 1, dt, true);
        lit.Step({C}, {A, C}, 1, 2, dt, false);
        CHECK(lit.Owner[0] == A && lit.Weight[0] < 1.0f);    // A fading out, C waiting
        // Reset (a scene load): every holder gone, and everything after arrives.
        lit.Reset();
        CHECK(lit.Owner[0] == Slots::kNone && lit.PrevLit.empty());
        lit.Step({C}, {C}, 1, 3, dt, false);
        CHECK(lit.Owner[0] == C && lit.Weight[0] == 1.0f);
        // Walking into range (Talos's re-check): C was lit but past its DistanceFade range (lit, not a candidate). Its
        // light ramps up from 0, so its shadow is a hand-over: it waits for A to fade out, then fades in - no pop.
        Slots walk;
        walk.StepLit({A}, {A}, {A, C}, 1, 1, dt, true);
        walk.StepLit({C}, {A, C}, {A, C}, 1, 2, dt, false);
        CHECK(walk.Owner[0] == A && walk.Weight[0] < 1.0f); // A fading; C waits
        for (std::uint64_t f = 3; f < 60 && walk.Owner[0] != C; ++f) walk.StepLit({C}, {A, C}, {A, C}, 1, f, dt, false);
        CHECK(walk.Owner[0] == C && std::abs(walk.Weight[0] - step) < 1e-5f); // in, fading from dark
        // ...while the same lamp switched on (not lit last frame) snaps in at once.
        Slots sw;
        sw.StepLit({A}, {A}, {A}, 1, 1, dt, true);
        sw.StepLit({C}, {A, C}, {A, C}, 1, 2, dt, false);
        CHECK(sw.Owner[0] == C && sw.Weight[0] == 1.0f);
    }
    // The urgent key (the flashlight) never waits: it takes the leaving holder's slot at full strength.
    Slots torch;
    torch.Step({A, B}, {A, B, C}, 2, 1, dt, true);           // C lit but slot-less
    torch.Step({T, C}, {A, B, C, T}, 2, 2, dt, false, T);
    int tSlot = -1;
    for (int i = 0; i < 2; ++i) if (torch.Owner[i] == T) tSlot = i;
    CHECK(tSlot >= 0 && torch.Weight[tSlot] == 1.0f);
    CHECK(torch.Owner[1 - tSlot] == A || torch.Owner[1 - tSlot] == B); // the other leaver still fading; C waits
    // Stable: a wanted holder keeps its slot index while others come and go.
    Slots stable;
    stable.Step({A, B, C}, {A, B, C}, 3, 1, dt, true);
    int slotOfC = -1;
    for (int i = 0; i < 3; ++i) if (stable.Owner[i] == C) slotOfC = i;
    for (std::uint64_t f = 2; f < 60; ++f) stable.Step({C, A}, {A, B, C}, 3, f, dt, false);
    CHECK(stable.Owner[slotOfC] == C && stable.Weight[slotOfC] == 1.0f);
    // A smaller budget frees the slots past it at once.
    CHECK(stable.Step({C}, {A, B, C}, 1, 61, dt, false) >= 1);
    for (int i = 1; i < 4; ++i) CHECK(stable.Owner[i] == Slots::kNone);
}

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

// The asset cost table's LOD level of a renderer: the index of the LOD Group child it sits under (at any depth), -1
// outside a group or under a fifth child or later; the nearest group wins when they nest.
void TestAssetCostLodLevel() {
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    auto make = [&](const char* name, entt::entity parent) {
        const entt::entity e = world.CreateEmptyEntity(zero, zero, one, name);
        if (parent != entt::null) CHECK(world.SetParent(e, parent));
        return e;
    };
    auto lod = [&](entt::entity e) { return AssetCost::LodLevelOf(world, (unsigned)entt::to_integral(e)); };
    const entt::entity loose = make("Loose", entt::null);
    const entt::entity group = make("Group", entt::null);
    world.Registry.emplace<LODGroupComponent>(group);
    entt::entity lv[5];
    for (int i = 0; i < 5; ++i) lv[i] = make("LOD", group);
    const entt::entity deep = make("Deep", make("Mid", lv[2]));
    const entt::entity inner = make("Inner", lv[1]);
    world.Registry.emplace<LODGroupComponent>(inner);
    const entt::entity innerLv0 = make("InnerLOD0", inner);
    const entt::entity innerLv1 = make("InnerLOD1", inner);
    CHECK(lod(loose) == -1);
    CHECK(lod(group) == -1);
    CHECK(lod(lv[0]) == 0);
    CHECK(lod(lv[3]) == 3);
    CHECK(lod(lv[4]) == -1); // a fifth child: always drawn, not a level
    CHECK(lod(deep) == 2);
    CHECK(lod(inner) == 1);
    CHECK(lod(innerLv0) == 0); // the nearest group decides
    CHECK(lod(innerLv1) == 1);
}

// A model drawn during a phase and unloaded before the phase is written (Forest's play phase streams outfits) still
// gets its row: the table keeps what it measured at draw time, never a pointer to the model.
void TestAssetCostModelFreedBeforeWrite() {
    const std::filesystem::path csv = UnitTestSupport::TempDir("AssetCost") / "asset_cost_freed.csv";
    std::error_code ec;
    std::filesystem::remove(csv, ec);
    CHECK(_putenv_s("TARTARUS_ASSET_COST", csv.string().c_str()) == 0);
    World world;
    auto model = std::make_unique<Model>("no/such/AssetCostFreed.fbx");
    SceneDrawItem item{};
    item.Ref = model.get();
    item.Tris = 12;
    item.SortId = 0xFFFFFFFFu;
    const std::vector<SceneDrawItem> list{item};
    AssetCost::NoteView(world, false, {&list});
    model.reset();
    AssetCost::WritePhase("freed.json", "play-max");
    CHECK(_putenv_s("TARTARUS_ASSET_COST", "") == 0);
    std::ifstream in(csv);
    std::string line, row;
    while (std::getline(in, line))
        if (line.find("AssetCostFreed.fbx") != std::string::npos) row = line;
    in.close();
    CHECK(row.find("\"freed.json\",play-max,game-view,model,") == 0);
    CHECK(row.find(",-1,1,12.0,12,1.00,12,0.000,") != std::string::npos);
    std::filesystem::remove(csv, ec);
}
} // namespace
// Reads World's LOD records for the structure-patch test's oracle (outside the unnamed namespace: World's friend).
struct WorldLodTestAccess {
    // What the records say a renderer should be: culled when a group above it tags the child on the path down to it
    // (neither the level it shows nor the one fading out); fading out (-1) or in (+1) when exactly one group above it
    // is cross-fading and the path goes through that group's old or new level, 0 when none is; 2 when several are
    // (the last fade written wins, so the test doesn't check those).
    static void Expect(const World& world, entt::entity e, bool& culled, int& fade) {
        culled = false;
        fade = 0;
        int fades = 0;
        entt::entity child = e;
        for (size_t depth = 0; depth <= World::kMaxHierarchyDepth; ++depth) {
            const auto* h = world.Registry.try_get<HierarchyComponent>(child);
            const entt::entity parent = h ? h->Parent : entt::null;
            if (parent == entt::null || !world.Registry.valid(parent)) break;
            const size_t s = (size_t)entt::to_entity(parent);
            if (world.Registry.all_of<LODGroupComponent>(parent) && s < world.m_LodIndex.size() &&
                world.m_LodIndex[s] < world.m_LodState.size() && world.m_LodState[world.m_LodIndex[s]].Group == parent) {
                const World::LodGroupState& st = world.m_LodState[world.m_LodIndex[s]];
                for (int j = 0; j < st.Levels; ++j) {
                    if (st.Kids[j] != child) continue;
                    if (st.Active >= -1 && j != st.Active && j != st.FadeFrom) culled = true;
                    if (st.FadeFrom >= 0 && (j == st.FadeFrom || j == st.Active)) {
                        ++fades;
                        fade = j == st.FadeFrom ? -1 : 1;
                    }
                }
            }
            child = parent;
        }
        if (fades > 1) fade = 2;
    }
    static size_t Fading(const World& world) { return world.m_LodFading.size(); }
};
namespace {

// ApplyLod's structure patch (renderers created, destroyed and re-parented in and around LOD Groups, nested ones
// included) tags exactly what a full pass from scratch tags, frame after frame of random changes; and the small ones
// do take the patch. With LodSmooth on (cross-fades under way while things come and go) every renderer's tag and fade
// match what the groups above it say: a piece added under a level fading out fades out with it.
static void RunLodStructurePatch(bool smooth) {
    World world;
    world.LodSmooth = smooth;
    world.LodClockOverride = smooth ? 10.0 : -1.0;
    const glm::vec3 zero(0.0f), one(1.0f);
    std::mt19937 rng(1107);
    auto pick = [&](size_t n) { return (size_t)std::uniform_int_distribution<size_t>(0, n - 1)(rng); };
    std::vector<entt::entity> places; // what new renderers go under (level children and what's under them, loose ones)
    std::vector<entt::entity> spawned; // what the test creates, destroys and moves
    auto renderer = [&](const glm::vec3& at) {
        const entt::entity e = world.CreateEmptyEntity(at, zero, one, "R");
        world.Registry.emplace<RenderableComponent>(e);
        return e;
    };
    auto makeGroup = [&](entt::entity parent, const glm::vec3& at) {
        const entt::entity g = world.CreateEmptyEntity(at, zero, one, "Group");
        world.Registry.emplace<LODGroupComponent>(g).Size = 2.0f;
        if (parent != entt::null) CHECK(world.SetParent(g, parent));
        for (int i = 0; i < 3; ++i) {
            const entt::entity level = renderer(at);
            CHECK(world.SetParent(level, g));
            places.push_back(level);
            const entt::entity under = renderer(at);
            CHECK(world.SetParent(under, level));
            places.push_back(under);
        }
        return g;
    };
    std::vector<entt::entity> groups;
    for (int i = 0; i < 4; ++i) groups.push_back(makeGroup(entt::null, glm::vec3(4.0f * i, 0.0f, 0.0f)));
    groups.push_back(makeGroup(places[1], glm::vec3(0.0f, 1.0f, 0.0f)));  // inside group 0's LOD 0
    groups.push_back(makeGroup(places[8], glm::vec3(4.0f, 1.0f, 0.0f)));  // inside group 1's LOD 1
    for (int i = 0; i < 3; ++i) places.push_back(renderer(glm::vec3(0.0f, 0.0f, 3.0f * i))); // under no group

    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
    auto tagged = [&]() {
        std::vector<std::uint32_t> out;
        for (entt::entity e : world.Registry.view<LodCulledTag>()) {
            CHECK(world.IsLodCulled(e));
            out.push_back(entt::to_integral(e));
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto alive = [&](std::vector<entt::entity>& v) {
        v.erase(std::remove_if(v.begin(), v.end(), [&](entt::entity e) { return !world.Registry.valid(e); }), v.end());
    };
    // The records' word for every renderer (see WorldLodTestAccess::Expect); false and a message on the first miss.
    int fadingChecked = 0;
    auto matchesRecords = [&](int frame) {
        for (entt::entity e : world.Registry.view<RenderableComponent>()) {
            bool culled = false;
            int fade = 0;
            WorldLodTestAccess::Expect(world, e, culled, fade);
            const float got = world.LodFadeOf(e);
            const bool fadeOk = fade == 2 || (fade == 0 ? got == 0.0f : fade < 0 ? got < 0.0f : got > 0.0f);
            fadingChecked += fade == 1 || fade == -1 ? 1 : 0;
            if (world.IsLodCulled(e) != culled || !fadeOk) {
                std::printf("[LodStructurePatch%s] frame %d: entity %u culled %d (records say %d), fade %.3f (records say %d)\n",
                            smooth ? " smooth" : "", frame, (unsigned)entt::to_integral(e), (int)world.IsLodCulled(e),
                            (int)culled, got, fade);
                return false;
            }
        }
        return true;
    };
    int patched = 0, changedFrames = 0, fadingFrames = 0;
    glm::vec3 view(0.0f, 0.0f, 1.0f);
    for (int frame = 0; frame < 400; ++frame) {
        alive(places);
        alive(spawned);
        const int ops = (int)pick(3);
        for (int op = 0; op < ops; ++op) {
            alive(spawned); // an earlier op may have destroyed some
            const size_t kind = pick(10);
            if (kind < 4 || spawned.empty()) { // a renderer (sometimes with a child) under some place, or under none
                const entt::entity e = renderer(glm::vec3((float)pick(16), 0.0f, (float)pick(8)));
                if (pick(6) != 0) CHECK(world.SetParent(e, places[pick(places.size())]));
                spawned.push_back(e);
                if (pick(3) == 0) {
                    const entt::entity c = renderer(zero);
                    CHECK(world.SetParent(c, e));
                    spawned.push_back(c);
                }
            } else if (kind < 6) { // destroyed: with its children, or through the registry (its children orphaned)
                const entt::entity e = spawned[pick(spawned.size())];
                if (pick(2) == 0) world.DestroyEntityAndChildren(e);
                else world.Registry.destroy(e);
            } else if (kind < 8) { // moved to another place, or to none (a cycle is refused)
                const entt::entity e = spawned[pick(spawned.size())];
                world.SetParent(e, pick(5) == 0 ? entt::entity{entt::null} : places[pick(places.size())]);
            } else if (kind < 9) { // a renderer added in place (the full pass's job)
                const entt::entity e = spawned[pick(spawned.size())];
                if (!world.Registry.all_of<RenderableComponent>(e)) world.Registry.emplace<RenderableComponent>(e);
                else world.Registry.remove<RenderableComponent>(e);
            } else { // only moved
                world.Registry.get<TransformComponent>(spawned[pick(spawned.size())]).Position.y += 1.0f;
            }
        }
        world.RebuildWorldTransformCache();
        // Smooth: the view holds a few frames (a group fades only once it has settled) and the clock runs 0.15 s a
        // frame, so a fade spans about four frames of changes.
        if (!smooth || frame % 6 == 0) view = glm::vec3((float)pick(16), 0.0f, 1.0f + (float)pick(20));
        if (smooth) world.LodClockOverride += 0.15;
        const std::uint64_t fullBefore = world.LodFullPasses();
        world.ApplyLod(view, proj);
        const auto& d = world.TransformStructureChanges();
        if (ops > 0 && d.Valid && (!d.Added.empty() || !d.Reparented.empty() || !d.Dropped.empty())) {
            ++changedFrames;
            patched += world.LodFullPasses() == fullBefore ? 1 : 0;
        }
        fadingFrames += WorldLodTestAccess::Fading(world) > 0 ? 1 : 0;
        const bool ok = matchesRecords(frame);
        CHECK(ok);
        if (!ok) break;
        if (smooth) continue; // a pass from scratch would end the fades under way
        const auto incremental = tagged();
        world.ClearLod();
        world.ApplyLod(view, proj); // from scratch
        const auto fromScratch = tagged();
        CHECK(incremental == fromScratch);
        if (incremental != fromScratch) {
            std::printf("[LodStructurePatch] frame %d: %zu tagged incrementally, %zu from scratch\n", frame,
                        incremental.size(), fromScratch.size());
            break;
        }
    }
    std::printf("[LodStructurePatch%s] %d of %d structure-change frames patched; %d frames fading, %d fading renderers checked\n",
                smooth ? " smooth" : "", patched, changedFrames, fadingFrames, fadingChecked);
    CHECK(changedFrames > 100);
    CHECK(patched * 2 > changedFrames);
    if (smooth) CHECK(fadingFrames > 40 && fadingChecked > 100);
}

void TestLodStructurePatch() {
    RunLodStructurePatch(false);
    RunLodStructurePatch(true);
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

    // --bake-gi pins the clocks a GI capture sees (SkyAtmosphere::SetBakeClock, Material::PinWindTime,
    // RenderFrameContext::GIBakePinClocks). All three are OFF by default, so Play, the editor and --gi-tour keep their
    // drifting clouds, running clock and swaying leaves; ON, a second of frames moves nothing.
    {
        SkySettings w;
        w.CloudWindSpeed = 4.0f;
        w.TimeOfDayHours = 18.8f;
        w.DayLengthMinutes = 1.0f; // 24 h a minute: 1 s moves the clock 0.4 h
        w.AnimateInEditor = true;
        auto run = [&](SkyAtmosphere& sky) {
            for (int i = 0; i < 60; ++i) sky.Tick(w, 1.0f / 60.0f, false);
            return sky.ResolveLighting(w, glm::vec3(0, -1, 0), glm::vec3(5.0f), 0.53f, glm::vec3(0.0f)).ClockHours;
        };
        SkyAtmosphere live;
        CHECK(!live.BakeClock());
        const glm::vec2 liveStart = live.Clouds().WeatherOffset();
        const float liveHours = run(live);
        CHECK(glm::length(live.Clouds().WeatherOffset() - liveStart) > 1e-3f); // 4 m/s for 1 s = 0.004 km
        CHECK(near(liveHours, 19.2f, 1e-3f));
        SkyAtmosphere baking;
        baking.SetBakeClock(true);
        const glm::vec2 bakeStart = baking.Clouds().WeatherOffset();
        const float bakeHours = run(baking);
        CHECK(baking.Clouds().WeatherOffset() == bakeStart);
        CHECK(near(bakeHours, 18.8f, 1e-5f));

        CHECK(!RenderFrameContext{}.GIBakePinClocks);
        Material::NextHashFrame();
        CHECK(Material::WindTime() == (float)Time::UnscaledTimeSinceStartup());
        Material::PinWindTime(true);
        Material::NextHashFrame();
        CHECK(Material::WindTime() == 0.0f && Material::PrevWindTime() == 0.0f);
        Material::PinWindTime(false);
        Material::NextHashFrame();
        CHECK(Material::WindTime() == (float)Time::UnscaledTimeSinceStartup());
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
    w.Render.SkySourceMode = World::SkySource::Atmosphere;
    w.Render.Sky.CloudCoverage = 0.77f;
    w.Render.Sky.RayleighTint = glm::vec3(0.5f, 1.0f, 0.625f);
    w.Render.Sky.CloudQuality = 2;
    w.Render.Sky.MoonEnabled = false;
    w.Render.Sky.NightBrightness = 2.5f;
    w.Render.Sky.TimeOfDayHours = 17.25f;
    AssetLibrary assets;
    const std::string saved = SceneSerializer::SaveToString(w, assets);
    World r;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(r, assets2, saved));
    CHECK(r.Render.SkySourceMode == World::SkySource::Atmosphere);
    CHECK(r.Render.Sky == w.Render.Sky);
    World old;
    AssetLibrary assets3;
    CHECK(SceneSerializer::LoadFromString(old, assets3, R"({"formatVersion":4,"skySource":2})"));
    CHECK(old.Render.SkySourceMode == World::SkySource::Atmosphere && old.Render.Sky == SkySettings{});
    World clamped;
    AssetLibrary assets4;
    CHECK(SceneSerializer::LoadFromString(clamped, assets4,
        R"({"formatVersion":4,"skySource":9,"sky":{"cloudCoverage":7,"cloudQuality":9,"latitude":-400,"clouds":"yes"}})"));
    CHECK(clamped.Render.SkySourceMode == World::SkySource::Atmosphere);
    CHECK(clamped.Render.Sky.CloudCoverage == 1.0f && clamped.Render.Sky.CloudQuality == 3 && clamped.Render.Sky.LatitudeDegrees == -90.0f);
    CHECK(clamped.Render.Sky.CloudsEnabled); // a non-boolean is ignored, keeping the default
}

// --- TextureCache. P-53: the size cap never evicts this session's working set (loaded, stored or requested), grows with
// it, and keeps 20 GB of disk free.
void TestTextureCachePinEvict() {
    constexpr uint64_t GiB = 1024ull * 1024 * 1024;
    constexpr uint64_t unknownFree = std::numeric_limits<uint64_t>::max();
    CHECK(TextureCache::CapBytes(0) == 8 * GiB);
    CHECK(TextureCache::CapBytes(4 * GiB) == 8 * GiB);
    CHECK(TextureCache::CapBytes(8 * GiB) == 10 * GiB);
    using E = TextureCache::EntryInfo;
    // The Forest case: a 4.47 GB working set fits the 8 GB floor, nothing goes.
    std::vector<E> forest;
    for (int i = 0; i < 45; ++i) forest.push_back({"f" + std::to_string(i), GiB / 10, i, false});
    CHECK(TextureCache::ChooseEvictions(forest, unknownFree).empty());
    // 12 GB on disk, 9 GB of it pinned (cap 11.25 GB): only unpinned, oldest first, until it fits.
    std::vector<E> e = {{"old", 1 * GiB, 1, false}, {"pinOld", 9 * GiB, 0, true}, {"mid", 1 * GiB, 2, false},
                        {"new", 1 * GiB, 3, false}};
    std::vector<size_t> v = TextureCache::ChooseEvictions(e, unknownFree);
    CHECK(v.size() == 1 && v[0] == 0);
    // Disk nearly full: cap clamps to (total + free) - 20 GB, but pinned entries still stay.
    v = TextureCache::ChooseEvictions(e, 10 * GiB); // room 22 GB -> cap 2 GB
    CHECK(v.size() == 3);
    for (size_t i : v) CHECK(!e[i].Pinned);
    // Under the cap with plenty of disk: nothing.
    CHECK(TextureCache::ChooseEvictions({{"a", GiB, 0, false}}, 500 * GiB).empty());
    // Pinned at request time (the async loader, before its decode): Prune keeps it, and the pin check doesn't expect it
    // on disk yet.
    const std::string queued = (std::filesystem::temp_directory_path() / "tt_cache_pin_request_test.png").string();
    TextureImportSettings s;
    s.MaxTextureSize = 333; // a variant nothing else uses
    const int missingBefore = TextureCache::SessionEntriesMissing();
    CHECK(!TextureCache::Pinned(queued, s));
    TextureCache::Pin(queued, s);
    CHECK(TextureCache::Pinned(queued, s));
    CHECK(TextureCache::SessionEntriesMissing() == missingBefore);
}

// One texture loaded two ways (a model's own material at the defaults, a material asset at its .meta's) keeps both
// entries - they used to share one and evict each other every session, re-decoding (and re-compressing) both.
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

// A-2: the linked-program cache's key and entry file. A changed source, a stage boundary moved, another driver, a
// corrupt or truncated file are all misses; a clean entry reads back byte-exact; Prune keeps a fresh entry.
void ProgramBinaryCacheKeyAndEntry() {
    namespace PBC = ProgramBinaryCache;
    const PBC::Key a = PBC::MakeKeyFor({"#version 460\nvoid main(){}", "#version 460\nout vec4 c; void main(){c=vec4(1);}"});
    CHECK(a.Valid);
    CHECK(a.Source == PBC::MakeKeyFor({"#version 460\nvoid main(){}", "#version 460\nout vec4 c; void main(){c=vec4(1);}"}).Source);
    // One character changed (a variant #define) -> a different key.
    CHECK(a.Source != PBC::MakeKeyFor({"#version 460\nvoid main(){}", "#version 460\nout vec4 c; void main(){c=vec4(0);}"}).Source);
    // The same text split differently between stages -> a different key.
    CHECK(PBC::MakeKeyFor({"ab", "c"}).Source != PBC::MakeKeyFor({"a", "bc"}).Source);
    // A compute program with the vertex stage's text is not the vertex+fragment program.
    CHECK(PBC::MakeKeyFor({"x"}).Source != PBC::MakeKeyFor({"x", ""}).Source);
    CHECK(a.SourceBytes == 26 + 48);

    const std::string driver = "NVIDIA Corporation | GeForce Test | 4.6.0 NVIDIA 999.99";
    std::string blob(4096, '\0');
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = (char)(i * 31 + 7);
    std::uint32_t format = 0;
    std::string out;

    const std::string entry = PBC::EncodeEntry(a, driver, 0x8E21u, blob);
    CHECK(PBC::DecodeEntry(entry, a, driver, format, out) && format == 0x8E21u && out == blob);
    CHECK(!PBC::DecodeEntry(entry, a, driver + " (updated)", format, out)); // another driver
    PBC::Key other = a;
    other.Source ^= 1;
    CHECK(!PBC::DecodeEntry(entry, other, driver, format, out)); // another program's entry
    other = a;
    other.SourceBytes += 1;
    CHECK(!PBC::DecodeEntry(entry, other, driver, format, out)); // a hash collision with a different length
    std::string corrupt = entry;
    corrupt[corrupt.size() - 100] ^= 0x40;
    CHECK(!PBC::DecodeEntry(corrupt, a, driver, format, out)); // a flipped bit in the binary
    CHECK(!PBC::DecodeEntry(std::string_view(entry).substr(0, entry.size() - 1), a, driver, format, out)); // truncated
    CHECK(!PBC::DecodeEntry(entry + "x", a, driver, format, out));                                        // trailing bytes
    CHECK(!PBC::DecodeEntry({}, a, driver, format, out));
    CHECK(!PBC::DecodeEntry(PBC::EncodeEntry(a, driver, 1, {}), a, driver, format, out)); // an empty binary

    // Through the folder: write, read back, a miss for another driver, Prune keeps it, Clear removes it.
    const std::filesystem::path dir = UnitTestSupport::TempDir("program_cache");
    PBC::SetDirectoryForTests((dir / "ShaderCache").string());
    struct Restore { ~Restore() { ProgramBinaryCache::SetDirectoryForTests(""); } } restore;
    PBC::Clear();
    CHECK(PBC::DiskUsage().Entries == 0);
    CHECK(!PBC::ReadEntryForTests(a, driver, format, out)); // nothing stored yet
    CHECK(PBC::WriteEntryForTests(a, driver, 0x8E21u, blob));
    CHECK(PBC::DiskUsage().Entries == 1);
    out.clear();
    CHECK(PBC::ReadEntryForTests(a, driver, format, out) && out == blob);
    CHECK(!PBC::ReadEntryForTests(a, "AMD | Radeon | 4.6", format, out));
    CHECK(PBC::WriteEntryForTests(a, driver, 0x8E21u, blob + "more")); // a re-store replaces the entry in place
    CHECK(PBC::DiskUsage().Entries == 1);
    CHECK(PBC::ReadEntryForTests(a, driver, format, out) && out == blob + "more");
    PBC::Prune();
    CHECK(PBC::DiskUsage().Entries == 1);
    CHECK(PBC::Clear() == 1 && PBC::DiskUsage().Entries == 0);
}

void RegisterRenderTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Program binary cache: key, entry checks, folder round trip", ProgramBinaryCacheKeyAndEntry});
    tests.push_back({"GI: L1 SH projection of known environments", GISphericalHarmonics});
    tests.push_back({"GI: volume lookup, bake file, weights follow circuits", GIVolumeLookup});
    tests.push_back({"Shader constants match their C++ sources", ShaderConstantsMatch});
    tests.push_back({"Reflection probe captures + puddle SSR setting", ProbeCaptureAndPuddleSsr});
    tests.push_back({"Small-object cull: projected bounding-sphere height", SmallObjectCull});
    tests.push_back({"Light distance fade: by projected size, smooth, then dropped", LightDistanceFade});
    tests.push_back({"Light bounds: per-face fade (Bounds Fade - / +), uniform = the old single fade", LightBoundsFaceFade});
    tests.push_back({"GatherVisible: per-frame candidates, per-query filters, invalidation", GatherCache});
    tests.push_back({"GpuScene: the mirror matches the candidate list slot by slot, frame after frame", GpuSceneMirror});
    tests.push_back({"GpuScene: in-place render settings reach the slots (reported: next sync; unreported: within a sweep lap)", GpuSceneLiveEpoch});
    tests.push_back({"GpuScene: SyncOnce's key follows every Sync (another world in between syncs again)", GpuSceneSyncOnceKey});
    tests.push_back({"GpuScene: slot object state = what the per-draw hook binds (probes, blends, GI mask)", GpuSceneObjectState});
    tests.push_back({"GpuScene: slot state needs 9 fragment SSBO blocks; below that a global _NO_SLOT_STATE define", GpuSceneSlotStateBlockBudget});
    tests.push_back({"GpuScene: the change log lists every changed slot with where it was and is (300 random frames)", GpuSceneChangeLog});
    tests.push_back({"Local shadows (G4-c): a frame's changes wake the quiet lights they reach, and only those", LocalShadowQuietWake});
    tests.push_back({"Local shadows (G4-c): a LOD level change wakes the lights around the renderers it swapped", LocalShadowLodToggles});
    tests.push_back({"GpuScene: upload runs (merge, cap, whole list) and layer packing", GpuSceneRuns});
    tests.push_back({"GatherPatch", GatherPatch});
    tests.push_back({"Model import: mergeByMaterial keeps geometry, one mesh per material", MergeByMaterial});
    tests.push_back({"GPU instancing: grouping keeps first-appearance order, kNoBatch stands alone", InstanceGrouping});
    tests.push_back({"GPU instancing: material slots batch by asset or identical values", InstanceSlots});
    tests.push_back({"Async uploads: staging ring placement wraps and never overlaps", UploadStagingRing});
    tests.push_back({"Async uploads: the pump budget follows the frame headroom in Play", AssetPumpBudget});
    tests.push_back({"Async uploads: a ticket the budget deferred still counts as in flight", AssetPumpWaitingTickets});
    tests.push_back({"Async uploads: a model's later upload that holds ring space is stepped too", ModelUploadStepsFreeTheRing});
    tests.push_back({"GPU-driven draws: indirect batches by key, in first-appearance order", IndirectBuilder});
    tests.push_back({"GPU-driven draws: mesh arena free list is first-fit and coalesces", MeshArenaFreeList});
    tests.push_back({"GPU occlusion culling: box padding", HiZPieces});
    tests.push_back({"GPU culling: stage A matches the CPU gather", GpuCullStageAMatchesCpu});
    tests.push_back({"GPU culling: sun caster cascades match the CPU", GpuCullCasterCascadesMatchCpu});
    tests.push_back({"Scene wind: settings round-trip, old-scene defaults, one bearing convention", SceneWindSettings});
    tests.push_back({"GPU instancing: placed copies share one import id", InstanceImportId});
    tests.push_back({"Volumetric fog: blue-noise jitter ranks are a permutation, evenly spread", VolumetricBlueNoise});
    tests.push_back({"Volumetric fog: the in-scatter soft cap is identity below the knee, monotone and bounded", VolumetricInscatterCap});
    tests.push_back({"Volumetric fog: settings and Volumetric Intensity round-trip; old scenes default off / 1", VolumetricSettingsRoundTrip});
    tests.push_back({"TAA mode + rain wetness round-trip; old scenes keep MSAA/FXAA and stay dry; jitter un-jitters", TaaAndWetnessRoundTrip});
    tests.push_back({"Fur: shell count follows the hair's size on screen", FurShellLod});
    tests.push_back({"Fur: material properties round-trip through a .mat and the accessors", FurMaterialRoundTrip});
    tests.push_back({"Materials: built-in property table - default keys, v1/v2 round-trip, accessors and bind resolver", MaterialBuiltinTable});
    tests.push_back({"LodGroup", TestLodGroup});
    tests.push_back({"AssetCostLodLevel", TestAssetCostLodLevel});
    tests.push_back({"AssetCostModelFreedBeforeWrite", TestAssetCostModelFreedBeforeWrite});
    tests.push_back({"LodStructurePatch", TestLodStructurePatch});
    tests.push_back({"LodImpostor", TestLodImpostor});
    tests.push_back({"LodImpostorSceneSwitch", TestLodImpostorSceneSwitch});
    tests.push_back({"LodFadeCompletes", TestLodFadeCompletes});
    tests.push_back({"PhysicalSky", TestPhysicalSky});
    tests.push_back({"MaterialRobustness", TestMaterialRobustness});
    tests.push_back({"MaterialTextureDefaults", TestMaterialTextureDefaults});
    tests.push_back({"TextureCacheVariants", TestTextureCacheVariants});
    tests.push_back({"TextureCachePinEvict", TestTextureCachePinEvict});
    tests.push_back({"TextureCacheHash", TestTextureCacheHash});
    tests.push_back({"CameraFrustumValidation", TestCameraFrustumValidation});
    tests.push_back({"CameraRoll", TestCameraRoll});
    tests.push_back({"OpaqueBatchKey", TestOpaqueBatchKey});
    tests.push_back({"LocalCasterHashSlots", TestLocalCasterHashSlots});
    tests.push_back({"LocalCasterHashMaterialContent", TestLocalCasterHashMaterialContent});
    tests.push_back({"ShadowSlotScoring", TestShadowSlotScoring});
}
