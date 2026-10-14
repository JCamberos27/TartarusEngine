#include "Tests/SmokeChecks.h"

#include "AssetDatabase.h"
#include "AssetGuid.h"
#include "AssetLibrary.h"
#include "AtomicFile.h"
#include "Components.h"
#include "EditorLayer.h"
#include "Log.h"
#include "MaterialAsset.h"
#include "MaterialPreviewRenderer.h"
#include "ProjectPaths.h"
#include "SceneSerializer.h"
#include "World.h"
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

} // namespace SmokeChecks
