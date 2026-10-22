#include "Tests/SmokeChecks.h"

#include "AssetDatabase.h"
#include "AssetGuid.h"
#include "AssetLibrary.h"
#include "AtomicFile.h"
#include "ClusterGrid.h"
#include "Components.h"
#include "DecalTypes.h"
#include "EditorLayer.h"
#include "Log.h"
#include "MaterialAsset.h"
#include "MaterialPreviewRenderer.h"
#include "ProjectPaths.h"
#include "SceneSerializer.h"
#include "World.h"
#include "Glass/GlassFeedbackMotion.h"
#include "Glass/GlassMotion.h"
#include "gl.h"

// stb_image_write.h's own sprintf() use, and entt's deprecated storage::in_use() (the entity counts, as main.cpp reads them).
#pragma warning(disable: 4996)
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace SmokeChecks {

// #132 - a smoke_project_sync* scene checks the project watcher end to end: files
// added, moved (the .meta left behind) and deleted outside the editor, applied by
// SyncProjectChanges. Each step pumps the watcher (inside this one frame) until
// it lands or 3 s pass.
void ProjectSync(EditorLayer& editor, World& world, AssetLibrary& assets) {
    namespace fs = std::filesystem;
    std::error_code se;
    const fs::path dir = ProjectPaths::Resolve("__smoke_sync");
    fs::remove_all(dir, se);
    fs::create_directories(dir, se);
    auto pump = [&](auto done) {
        for (int i = 0; i < 60 && !done(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            editor.SyncProjectChanges(world, assets);
        }
        return (bool)done();
    };
    const std::string a = (dir / "a.png").string();
    const std::string m = (dir / "m.mat").string();
    const std::string c = (dir / "c.png").string();      // renamed in place
    const std::string b = (dir / "sub" / "c.png").string(); // then moved to a subfolder
    unsigned char px[4 * 4 * 4];
    for (int i = 0; i < 64; ++i) px[i] = (unsigned char)(i * 4);
    stbi_write_png(a.c_str(), 4, 4, 4, px, 16);
    const bool added = pump([&] { return !assets.FindListed(a).empty(); });
    const AssetGuid guidA = AssetDatabase::GuidForPath(a);
    const bool metaA = fs::exists(a + ".meta", se);
    // A material using it, and a scene reference (the sky path string).
    AtomicFile::WriteBytes(m, "{\"matVersion\":1,\"albedoMap\":\"" + fs::path(a).generic_string() + "\"}");
    std::this_thread::sleep_for(std::chrono::milliseconds(3100)); // past the self-write window
    std::ofstream(m, std::ios::app) << "\n";                        // an outside edit: re-import it
    const bool matAdded = pump([&] { return !assets.FindListed(m).empty(); });
    const std::string savedSky = world.SkyHdriPath;
    world.SkyHdriPath = a;
    // Rename it in place, alone: its .meta stays behind and must be carried over.
    fs::rename(a, c, se);
    const bool renamed = pump([&] { return !assets.FindListed(c).empty() && assets.FindListed(a).empty(); });
    const bool renamedGuid = guidA.IsValid() && AssetDatabase::GuidForPath(c) == guidA &&
                             fs::exists(c + ".meta", se) && !fs::exists(a + ".meta", se);
    // Then move it, with its .meta (what Explorer / git do), into a subfolder.
    fs::create_directories(dir / "sub", se);
    fs::rename(c + ".meta", b + ".meta", se);
    fs::rename(c, b, se);
    const bool moved = pump([&] { return !assets.FindListed(b).empty() && assets.FindListed(c).empty(); });
    const bool sameGuid = renamedGuid && AssetDatabase::GuidForPath(b) == guidA;
    const bool metaMoved = fs::exists(b + ".meta", se) && !fs::exists(c + ".meta", se);
    const bool skyFollowed = AssetDatabase::PathKey(world.SkyHdriPath) == AssetDatabase::PathKey(b);
    std::shared_ptr<MaterialAsset> mat = assets.LoadMaterial(assets.FindListed(m));
    const bool matFollowed = mat && mat->Mat.AlbedoMap &&
        AssetDatabase::PathKey(mat->AlbedoMapPath) == AssetDatabase::PathKey(b);
    world.SkyHdriPath = savedSky;
    // Delete it; its .meta goes with it.
    fs::remove(b, se);
    const bool removed = pump([&] { return assets.FindListed(b).empty(); });
    const bool metaCleaned = !fs::exists(b + ".meta", se);
    std::cout << "[SmokeTest]   project sync added=" << added << " meta=" << metaA << " matAdded=" << matAdded
              << " renamed=" << renamed << " moved=" << moved << " sameGuid=" << sameGuid << " metaMoved=" << metaMoved
              << " skyFollowed=" << skyFollowed << " matFollowed=" << matFollowed
              << " removed=" << removed << " metaCleaned=" << metaCleaned << "\n";
    if (!(added && metaA && matAdded && renamed && moved && sameGuid && metaMoved && skyFollowed && matFollowed && removed && metaCleaned))
        Log::Error("[SmokeTest] project sync check failed.");
    fs::remove_all(dir, se);
    pump([] { return false; }); // let the clean-up events drain before the next scene
}

// #107 - a smoke_materials* scene renders every loaded material, plus a chrome, a
// see-through and a glass one, through the material preview. Each must draw
// something distinct from the backdrop, lit from above (so the Inspector's flipped
// UVs are right), and ContentKey must follow edits. PNGs land in %TEMP% for a look.
void MaterialPreviews(World& world) {
    namespace fs = std::filesystem;
    std::error_code pe;
    const fs::path outDir = fs::temp_directory_path(pe) / "TartarusSmokeMaterialPreview";
    fs::create_directories(outDir, pe);
    // The scene's own materials: whatever else is still loaded (the startup scene's - the Sandbox
    // player's black clothing, near the backdrop's colour) isn't what this scene tests.
    std::vector<std::shared_ptr<MaterialAsset>> mats;
    for (auto [e, rc] : world.Registry.view<RenderableComponent>().each())
        for (const auto& m : rc.Materials)
            if (m && std::find(mats.begin(), mats.end(), m) == mats.end()) mats.push_back(m);
    auto extra = [&](const char* name, auto setup) {
        auto m = std::make_shared<MaterialAsset>();
        m->Name = name;
        setup(*m);
        mats.push_back(m);
    };
    extra("chrome", [](MaterialAsset& m) { m.Mat.BaseColor = glm::vec3(0.95f); m.Mat.Metallic = 1.0f; m.Mat.Roughness = 0.08f; });
    extra("red_plastic", [](MaterialAsset& m) { m.Mat.BaseColor = glm::vec3(0.8f, 0.05f, 0.05f); m.Mat.Roughness = 0.45f; });
    extra("see_through", [](MaterialAsset& m) { m.Mat.BaseColor = glm::vec3(0.1f, 0.3f, 0.9f);
                                                m.RenderQueue = MaterialAsset::Queue::Transparent; m.Opacity = 0.35f; });
    MaterialPreviewRenderer preview;
    const int kPx = 128;
    unsigned int readFbo = 0;
    glGenFramebuffers(1, &readFbo);
    int bad = 0, drawn = 0;
    std::string names;
    for (const auto& m : mats) {
        if (!m || m->Missing) continue;
        const unsigned int tex = preview.Render(m, MaterialPreviewRenderer::Shape::Sphere, 0.5f, 0.3f, kPx, kPx);
        std::vector<unsigned char> px((size_t)kPx * kPx * 4);
        GLint prevRead = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (tex) glReadPixels(0, 0, kPx, kPx, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int)prevRead);
        auto at = [&](int x, int y) { const unsigned char* p = &px[((size_t)y * kPx + x) * 4]; return glm::vec3(p[0], p[1], p[2]); };
        // Average over a band: GL rows run bottom-up, so y > kPx/2 is the top of the image.
        auto band = [&](int y0, int y1) {
            glm::vec3 sum(0.0f); int n = 0;
            for (int y = y0; y < y1; ++y) for (int x = kPx * 3 / 8; x < kPx * 5 / 8; ++x) { sum += at(x, y); ++n; }
            return sum / (float)std::max(n, 1);
        };
        const glm::vec3 corner = at(2, 2), centre = band(kPx * 7 / 16, kPx * 9 / 16);
        const glm::vec3 top = band(kPx * 5 / 8, kPx * 3 / 4), bottom = band(kPx / 4, kPx * 3 / 8);
        const float lum = [](glm::vec3 c) { return c.r * 0.3f + c.g * 0.59f + c.b * 0.11f; }(top);
        const float lumB = bottom.r * 0.3f + bottom.g * 0.59f + bottom.b * 0.11f;
        const bool distinct = glm::length(centre - corner) > 12.0f;
        const bool opaque = m->RenderQueue == MaterialAsset::Queue::Opaque && m->Mat.TransmissionStrength <= 0.0f;
        const bool litFromAbove = !opaque || lum >= lumB - 2.0f; // not meaningful through glass
        ++drawn;
        const std::string name = m->Name.empty() ? fs::path(m->Path).stem().string() : m->Name;
        names += " " + name;
        if (!tex || !distinct || !litFromAbove) {
            ++bad;
            Log::Error("[SmokeTest] material preview '" + name + "' tex=" + std::to_string(tex) +
                       " distinct=" + std::to_string(distinct) + " litFromAbove=" + std::to_string(litFromAbove));
        }
        // Flip to top-down rows for the PNG.
        std::vector<unsigned char> flipped(px.size());
        for (int y = 0; y < kPx; ++y)
            std::memcpy(&flipped[(size_t)y * kPx * 4], &px[(size_t)(kPx - 1 - y) * kPx * 4], (size_t)kPx * 4);
        stbi_write_png((outDir / (name + ".png")).string().c_str(), kPx, kPx, 4, flipped.data(), kPx * 4);
    }
    glDeleteFramebuffers(1, &readFbo);
    // ContentKey: stable, and follows a value edit and a queue change.
    bool keyOk = !mats.empty();
    if (keyOk) {
        MaterialAsset& m = *mats.back();
        const std::uint64_t k0 = MaterialPreviewRenderer::ContentKey(m);
        keyOk = k0 == MaterialPreviewRenderer::ContentKey(m);
        m.Mat.BaseColor.r += 0.1f;
        const std::uint64_t k1 = MaterialPreviewRenderer::ContentKey(m);
        m.RenderQueue = MaterialAsset::Queue::Opaque;
        const std::uint64_t k2 = MaterialPreviewRenderer::ContentKey(m);
        keyOk = keyOk && k1 != k0 && k2 != k1;
    }
    std::cout << "[SmokeTest]   material previews drawn=" << drawn << " bad=" << bad << " keyOk=" << keyOk
              << " (" << outDir.string() << ":" << names << ")" << std::endl;
    if (bad || !keyOk || drawn < 4) Log::Error("[SmokeTest] material preview check failed.");
}

// #176 - a smoke_prefab* scene round-trips Prefab Mode: save a box as a temp
// prefab, place two instances, open the prefab, add a child, go back to the
// scene, and check both instances picked the child up and the scene's own
// objects came back.
void PrefabMode(EditorLayer& editor, World& world, AssetLibrary& assets) {
    std::error_code pe;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(pe) / "TartarusSmokePrefab";
    std::filesystem::create_directories(dir, pe);
    const std::string prefabPath = (dir / "crate.prefab").string();
    const size_t sceneEntities = world.Registry.storage<entt::entity>().in_use();
    entt::entity src = world.CreateBox(glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(1.0f), glm::vec3(0.8f, 0.5f, 0.2f),
                                       glm::vec3(0.0f), "Crate");
    SceneSerializer::SavePrefab(world, src, prefabPath);
    world.DestroyEntityAndChildren(src);
    auto countChildren = [&](entt::entity e) {
        int n = 0;
        for (auto [c, h] : world.Registry.view<HierarchyComponent>().each()) if (h.Parent == e) ++n;
        return n;
    };
    for (float x : {-2.0f, 2.0f}) {
        entt::entity inst = SceneSerializer::InstantiatePrefab(world, assets, prefabPath);
        world.Registry.get<TransformComponent>(inst).Position.x = x;
    }
    editor.EnterPrefabMode(world, assets, prefabPath);
    const bool entered = editor.InPrefabMode();
    int rootsInPrefab = 0;
    entt::entity proot = entt::null;
    for (auto [e, t] : world.Registry.view<TransformComponent>().each()) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        if (!h || h->Parent == entt::null) { ++rootsInPrefab; proot = e; }
    }
    if (proot != entt::null) {
        entt::entity child = world.CreateEmptyEntity(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Lid");
        world.SetParent(child, proot);
        // Imported meshes are initially top-level, including on an edit path
        // that hasn't marked the prefab dirty. Closing must save them too.
        world.CreateEmptyEntity(glm::vec3(2.0f, 1.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Dropped Mesh");
    }
    editor.ExitPrefabMode(world, assets);
    const bool saved = !editor.InPrefabMode();
    int instances = 0, withChild = 0;
    for (auto [e, pi] : world.Registry.view<PrefabInstanceComponent>().each()) {
        ++instances;
        if (countChildren(e) == 2) ++withChild;
    }
    const size_t after = world.Registry.storage<entt::entity>().in_use();
    std::cout << "[SmokeTest]   prefab mode entered=" << entered << " rootsInPrefab=" << rootsInPrefab
              << " saved=" << saved << " exited=" << !editor.InPrefabMode() << " instances=" << instances
              << " withChild=" << withChild << " sceneEntities=" << sceneEntities << "->" << after << "\n";
    if (!entered || !saved || editor.InPrefabMode() || instances != 2 || withChild != 2)
        Log::Error("[SmokeTest] Prefab Mode round trip failed.");
    std::filesystem::remove_all(dir, pe);
}

// The Glass theme: on at frame 15 (applied at the next BeginFrame), checked at 70 after ~55 frames
// of drawing every Glass surface over the scene, off at 70, and its memory checked at 80.
void GlassTheme(EditorLayer& editor, World& world, int frame) {
    if (frame == 15) {
        // Something selected, so the Properties panel and the selection HUD have content.
        for (entt::entity e : world.Registry.view<const NameComponent>()) {
            const auto* order = world.Registry.try_get<OrderComponent>(e);
            if (order && world.Registry.get<const NameComponent>(e).Name == "Cube") { editor.SelectEntityByOrder(world, order->Value); break; }
        }
        editor.SetGlassThemeForTest(true);
        std::cout << "[SmokeTest]   glass: theme on" << std::endl;
    } else if (frame == 20) {
        editor.GlassNotifySnap(); // the snap ring, drawn for the next ~25 frames
        Log::Warn("[SmokeTest] glass: a warning (not a compile error) must not raise a toast");
        Glass::PlaySaveCheck();   // the save check's keyframe clip (~1 s)
    } else if (frame == 21) {
        // The very next frame: the clip runs ~1 s of wall clock, which 10 llvmpipe frames on CI outlast.
        if (!Glass::Motion().ReduceMotion() && !Glass::SaveCheckPlaying())
            Log::Error("[SmokeTest] glass: the save check's keyframe clip didn't play");
        else std::cout << "[SmokeTest]   glass: save check clip playing" << std::endl;
    } else if (frame == 70) {
        if (!editor.GlassThemeActive()) {
            Log::Error("[SmokeTest] glass: the theme never switched on");
        } else if (editor.IsSceneViewportVisible() && editor.GlassBackdropTextureForTest() == 0) {
            Log::Error("[SmokeTest] glass: the frosted HUDs produced no blurred backdrop");
        } else if (editor.IsSceneViewportVisible() && editor.GlassSurfacesForTest() == 0) {
            // The viewport toolbar and tool rail float over the Scene view: they must be glass.
            Log::Error("[SmokeTest] glass: no floating window was composited as glass");
        } else if (editor.GlassSurfacesForTest() > 0 && editor.GlassCapturesForTest() == 0) {
            Log::Error("[SmokeTest] glass: glass surfaces were drawn without blurring what's behind them");
        } else {
            std::cout << "[SmokeTest]   glass: panels and HUDs drawn, backdrop texture "
                      << editor.GlassBackdropTextureForTest() << ", " << editor.GlassSurfacesForTest()
                      << " glass surface(s) from " << editor.GlassCapturesForTest() << " capture(s), "
                      << Glass::Motion().LiveCount() << " animation(s) live" << std::endl;
        }
        editor.SetGlassThemeForTest(false);
    } else if (frame >= 80 && frame <= 99) {
        // Motion v2: a few moments (a mood, a toolbar fold, the focus rim) are still landing when the
        // theme goes off. Pass the first frame everything is released; fail if it never is.
        static bool s_Reported = false;
        if (frame == 80) s_Reported = false;
        if (s_Reported) return;
        const bool released = Glass::Motion().LiveCount() == 0 && Glass::Motion().HeldBytes() == 0;
        if (!released && frame < 99 && !editor.GlassThemeActive()) return;
        s_Reported = true;
        if (editor.GlassThemeActive()) Log::Error("[SmokeTest] glass: the theme never switched back off");
        else if (!released) {
            std::string live;
            for (int i = 0; i < Glass::Motion().LiveCount() && i < 8; ++i) {
                char b[40];
                std::snprintf(b, sizeof(b), " %08X/%d", Glass::Motion().LiveId(i), Glass::Motion().LiveKind(i));
                live += b;
            }
            Log::Error("[SmokeTest] glass: animation state still holds " + std::to_string(Glass::Motion().HeldBytes()) +
                       " bytes after the theme was switched off; live (id/kind):" + live);
        }
        else std::cout << "[SmokeTest]   glass: theme off, animation memory released by frame " << frame << std::endl;
    }
}

// smoke_decals_stress: 4096 decals piled on one wall, far past the 256-per-cluster cap in hundreds of
// clusters. Two consecutive frames (a still camera) must bin identically - the old shared list ran out
// and blanked a different set of clusters every frame - and each cluster must hold what the CPU's copy
// of the cull's test gives it (its top-most 256 where more touch it), in sort order, with the clamped
// clusters counted in the overflow word.
void DecalStress(const ClusterGrid& grid, int visible, int frame) {
    static ClusterGrid::DecalSnapshot first;
    static int firstFrame = -1;
    ClusterGrid::DecalSnapshot snap;
    if (!grid.ReadDecalSnapshot(snap)) {
        Log::Error("[SmokeTest] decal stress: no decal cull to read back");
        return;
    }
    if (firstFrame < 0 || frame != firstFrame + 1) {
        first = std::move(snap);
        firstFrame = frame;
        return;
    }
    std::string why;
    if (visible != Decals::MAX_VISIBLE || snap.DecalCount != Decals::MAX_VISIBLE)
        why = "expected " + std::to_string(Decals::MAX_VISIBLE) + " decals binned, got " + std::to_string(snap.DecalCount) +
              " (" + std::to_string(visible) + " visible)";
    // Each cluster's list (the blocks' unused tails hold whatever an earlier cull left).
    for (int c = 0; c < ClusterGrid::CLUSTERS && why.empty(); ++c) {
        const uint32_t count = snap.Ranges[c * 2 + 1], offset = snap.Ranges[c * 2], offset0 = first.Ranges[c * 2];
        bool same = count == first.Ranges[c * 2 + 1];
        for (uint32_t k = 0; same && k < count; ++k) same = snap.Indices[2 + offset + k] == first.Indices[2 + offset0 + k];
        if (!same) why = "cluster " + std::to_string(c) + "'s list changed between two frames of a still view";
    }
    if (why.empty() && (snap.Indices[1] & 2u)) why = "the index list overflowed";
    // Each cluster against the CPU test, bracketed by a millimetre either way for rounding.
    const uint32_t cap = (uint32_t)Decals::MAX_PER_CLUSTER;
    unsigned clampedLo = 0, clampedHi = 0, blank = 0, full = 0;
    uint64_t total = 0;
    for (int c = 0; c < ClusterGrid::CLUSTERS && why.empty(); ++c) {
        const glm::vec3 mn(snap.Clusters[c * 2]), mx(snap.Clusters[c * 2 + 1]);
        uint32_t lo = 0, hi = 0;
        for (int i = 0; i < snap.DecalCount; ++i) {
            const glm::vec4* box = &snap.ViewBoxes[(size_t)i * ClusterGrid::kDecalViewBoxVec4s];
            if (!ClusterGrid::DecalBoxOverlaps(box, mn, mx, 1e-3f)) continue; // the narrow test can't pass either
            ++hi;
            lo += ClusterGrid::DecalBoxOverlaps(box, mn, mx, -1e-3f);
        }
        const uint32_t offset = snap.Ranges[c * 2], count = snap.Ranges[c * 2 + 1];
        total += count;
        clampedLo += lo > cap;
        clampedHi += hi > cap;
        blank += lo > 0 && count == 0;
        full += count == cap;
        if (count < std::min(lo, cap) || count > std::min(hi, cap)) {
            why = "cluster " + std::to_string(c) + " holds " + std::to_string(count) + " decals, the CPU test gives " +
                  std::to_string(lo) + ".." + std::to_string(hi);
            break;
        }
        if ((size_t)offset + count + 2 > snap.Indices.size()) {
            why = "cluster " + std::to_string(c) + "'s range runs past the list";
            break;
        }
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t idx = snap.Indices[2 + offset + k];
            const bool ordered = k == 0 || idx > snap.Indices[2 + offset + k - 1];
            if (idx >= (uint32_t)snap.DecalCount || !ordered ||
                !ClusterGrid::DecalBoxOverlaps(&snap.ViewBoxes[(size_t)idx * ClusterGrid::kDecalViewBoxVec4s], mn, mx, 1e-3f)) {
                why = "cluster " + std::to_string(c) + " lists decal " + std::to_string(idx) + " out of order or off it";
                break;
            }
        }
        // Clamped: the top of the stack is kept, so its last entry is the highest-sorted decal touching it.
        if (why.empty() && lo > cap && hi == lo && count > 0) {
            uint32_t top = 0;
            for (int i = snap.DecalCount - 1; i >= 0; --i)
                if (ClusterGrid::DecalBoxOverlaps(&snap.ViewBoxes[(size_t)i * ClusterGrid::kDecalViewBoxVec4s], mn, mx, 0.0f)) {
                    top = (uint32_t)i;
                    break;
                }
            if (snap.Indices[2 + offset + count - 1] != top)
                why = "clamped cluster " + std::to_string(c) + " dropped its top-most decal";
        }
    }
    if (why.empty() && total != snap.Indices[0]) why = "the header's entry count doesn't match the clusters' counts";
    const unsigned counted = snap.Indices[1] >> 8;
    if (why.empty() && (counted < clampedLo || counted > clampedHi))
        why = "the overflow word counts " + std::to_string(counted) + " clamped clusters, the CPU test " +
              std::to_string(clampedLo) + ".." + std::to_string(clampedHi);
    const unsigned landed = grid.DecalOverflowClusters(snap.BuildSlot);
    if (why.empty() && landed != counted)
        why = "the fenced readback reports " + std::to_string(landed) + " clamped clusters, this frame's cull " +
              std::to_string(counted);
    // More entries than the old shared list held (131072), with hundreds of clusters at the cap.
    if (why.empty() && (clampedLo < 300 || total <= 131072u))
        why = "only " + std::to_string(clampedLo) + " clusters over the cap, " + std::to_string(total) + " entries: not a stress";
    if (!why.empty()) {
        Log::Error("[SmokeTest] decal stress: " + why);
        return;
    }
    std::cout << "[SmokeTest]   decal stress OK (" << snap.DecalCount << " decals, " << counted << " clusters clamped, " << full
              << " full, " << blank << " blank, " << total << " list entries)" << std::endl;
}

} // namespace SmokeChecks
