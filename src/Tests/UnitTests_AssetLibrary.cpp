// The Asset Library (docs/PROJECT_ASSETS.md): asset_library.json's category rules and kit table
// (AssetLibraryIndex) and the modular kit snap maths (ModularSnap.h). Pure, no scene or ImGui.

#include "UnitTestSupport.h"

#include "AssetLibraryIndex.h"
#include "ModularSnap.h"
#include "ThumbnailCache.h"
#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "Core/Paths.h"
#include "Model.h"
#include "ModelCache.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <cmath>
#include <vector>

namespace {

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void TestIndexRules() {
    AssetLibraryIndex idx;
    idx.LoadFromText(R"({
        "categories": [{"id": "arch", "name": "Architecture"}, {"id": "arch.walls", "name": "Walls", "parent": "arch"}],
        "rules": [{"glob": "assets/Environments/*/Prefabs/*Wall*", "category": "arch.walls"},
                  {"glob": "assets/Environments/**", "category": "arch"}],
        "kits": {"assets/Environments/Kit": 2.0}})");
    CHECK(idx.Categories().size() == 2);
    CHECK(idx.Categories()[1].Parent == "arch");
    // First match wins, case-insensitively; no match is "other".
    CHECK(idx.CategoryOf("assets/Environments/Kit/Prefabs/brick_wall_2m.prefab") == "arch.walls");
    CHECK(idx.CategoryOf("assets/Environments/Kit/Prefabs/Floor.prefab") == "arch");
    CHECK(idx.CategoryOf("assets/Props/Knife.prefab") == "other");
    // A kit owns paths under its folder only, not a sibling sharing the prefix.
    CHECK(Near(idx.KitModuleOf("assets/Environments/Kit/Prefabs/Floor.prefab"), 2.0f));
    CHECK(Near(idx.KitModuleOf("assets/Environments/KitBash/Floor.prefab"), 0.0f));

    AssetLibraryIndex broken;
    broken.LoadFromText("{not json");
    CHECK(broken.Empty());
    CHECK(broken.CategoryOf("assets/x.prefab") == "other");
}

AABB Box(glm::vec3 mn, glm::vec3 mx) { return AABB{mn, mx}; }

void TestSnapFlush() {
    using namespace ModularSnap;
    const AABB wall = Box({0, 0, 0}, {2, 3, 0.2f});
    // 0.3 m short of the neighbour's right face (reach is 0.25 x 2 = 0.5): pulled flush.
    const AABB moving = Box({2.3f, 0, 0.05f}, {4.3f, 3, 0.25f});
    const Result r = Solve(moving, 2.0f, {wall});
    CHECK(r.Snapped);
    CHECK(Near(moving.Min.x + r.Delta.x, wall.Max.x));
    // Fronts lined up in a row (same-side faces), and the height left alone.
    CHECK(Near(moving.Min.z + r.Delta.z, wall.Min.z));
    CHECK(Near(r.Delta.y, 0.0f));
    // The highlighted face is the shared plane x = 2.
    CHECK(Near(r.Face.Min.x, 2.0f) && Near(r.Face.Max.x, 2.0f));
}

void TestSnapGridAndFree() {
    using namespace ModularSnap;
    // Nothing nearby: X/Z land on the module grid, Y stays where the drop put it.
    const AABB lone = Box({1.3f, 0.37f, -2.9f}, {3.3f, 3.37f, -0.9f});
    const Result r = Solve(lone, 2.0f, {});
    CHECK(!r.Snapped);
    CHECK(Near(lone.Min.x + r.Delta.x, 2.0f));
    CHECK(Near(lone.Min.z + r.Delta.z, -2.0f));
    CHECK(Near(r.Delta.y, 0.0f));
    // A neighbour too far round the side (no overlap on the other axes) doesn't pull.
    const AABB far = Box({0, 0, 10}, {2, 3, 10.2f});
    CHECK(!Solve(Box({2.1f, 0, 0}, {4.1f, 3, 0.2f}), 2.0f, {far}).Snapped);
    // Dropped half inside a neighbour on the ground (the CI case): lining the fronts up must not
    // stack it into the neighbour's box - it ends flush beside it, one module over.
    {
        const AABB cube = Box({0, 0, 0}, {1, 1, 1});
        const AABB half = Box({0.3f, 0, 0.1f}, {1.3f, 1, 1.1f});
        const Result s = Solve(half, 1.0f, {cube});
        const glm::vec3 mn = half.Min + s.Delta;
        CHECK(s.Snapped);
        CHECK(Near(mn.x, 1.0f) || Near(mn.z, 1.0f)); // out on the side it came from (+x / +z)
        CHECK(Near(mn.y, 0.0f));
    }
    // No module: no snap at all.
    CHECK(Near(Solve(lone, 0.0f, {}).Delta.x, 0.0f));
    CHECK(Near(SnapYawDegrees(44.0f), 0.0f) && Near(SnapYawDegrees(46.0f), 90.0f) && Near(SnapYawDegrees(-170.0f), -180.0f));
}

// SaveAsync writes on a worker; after FlushAsync the entry loads back with its key, a different
// key misses, and a second save replaces the first (temp file + rename, no stray temp files).
void TestThumbnailAsyncSave() {
    namespace fs = std::filesystem;
    const std::string asset = ProjectPaths::Resolve("assets/__thumb_async_ut.bin");
    { std::ofstream(fs::u8path(asset), std::ios::binary) << "x"; }
    const int size = 4;
    std::vector<unsigned char> a((size_t)size * size * 4, 10), b((size_t)size * size * 4, 200);
    ThumbnailCache::SaveAsync(asset, size, 111, a);
    ThumbnailCache::SaveAsync(asset, size, 222, b);
    ThumbnailCache::FlushAsync();
    std::vector<unsigned char> got;
    CHECK(ThumbnailCache::Load(asset, size, 222, got));
    CHECK(got == b);
    CHECK(!ThumbnailCache::Load(asset, size, 111, got));
    const std::string cache = ThumbnailCache::CacheFilePath(asset);
    int strays = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(cache).parent_path(), ec))
        if (e.path().filename().string().find(".tmp-") != std::string::npos) ++strays;
    CHECK(strays == 0);
    fs::remove(fs::u8path(cache), ec);
    fs::remove(fs::u8path(cache + ".key"), ec);
    AssetDatabase::ForgetPath(asset);
    fs::remove(fs::u8path(asset), ec);
    fs::remove(fs::u8path(asset + ".meta"), ec);
}

// A model's material remap is cached after the first read. Renaming or deleting the model must not leave the old
// path answering from that cache (W5-103): the old path reads its (now missing) .meta, the new one reads the moved one.
void TestRemapCacheFollowsRenameAndRemoval() {
    namespace fs = std::filesystem;
    const std::string a = "assets/__remap_ut_a.obj", b = "assets/__remap_ut_b.obj";
    std::error_code ec;
    for (const std::string& p : {a, b}) {
        fs::remove(fs::u8path(ProjectPaths::Resolve(p)), ec);
        fs::remove(fs::u8path(ProjectPaths::Resolve(p) + ".meta"), ec);
    }
    { std::ofstream(fs::u8path(ProjectPaths::Resolve(a)), std::ios::binary) << "o x\n"; }
    AssetLibrary assets;
    assets.SetMaterialRemap(a, "Body", "assets/__remap_ut.mat");
    CHECK(assets.MaterialRemap(a).count("Body") == 1);

    fs::rename(fs::u8path(ProjectPaths::Resolve(a)), fs::u8path(ProjectPaths::Resolve(b)), ec);
    fs::rename(fs::u8path(ProjectPaths::Resolve(a) + ".meta"), fs::u8path(ProjectPaths::Resolve(b) + ".meta"), ec);
    AssetDatabase::ForgetPath(a);
    assets.RenamePath(a, b);
    CHECK(assets.MaterialRemap(a).empty());
    CHECK(assets.MaterialRemap(b).count("Body") == 1);

    fs::remove(fs::u8path(ProjectPaths::Resolve(b)), ec);
    fs::remove(fs::u8path(ProjectPaths::Resolve(b) + ".meta"), ec);
    AssetDatabase::ForgetPath(b);
    assets.ForgetRemoved(b);
    CHECK(assets.MaterialRemap(b).empty());
}

// A one-triangle binary glTF (.glb, a cooked format): positions + indices in the BIN chunk, one named material.
void WriteTriangleGlb(const std::filesystem::path& file, float x) {
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"Tri","mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],)"
        R"("materials":[{"name":"CacheMat","pbrMetallicRoughness":{"baseColorFactor":[0.5,0.25,1,1],"metallicFactor":0.2}}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[2,1,0]},)"
        R"({"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":12}],)"
        R"("buffers":[{"byteLength":48}]})";
    std::string jsonChunk = json;
    while (jsonChunk.size() % 4) jsonChunk += ' ';
    const float pos[9] = {0, 0, 0, x, 0, 0, 0, 1, 0};
    const std::uint32_t idx[3] = {0, 1, 2};
    std::string bin(48, '\0');
    std::memcpy(bin.data(), pos, 36);
    std::memcpy(bin.data() + 36, idx, 12);
    auto u32 = [](std::string& out, std::uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    std::string glb;
    u32(glb, 0x46546C67u); // "glTF"
    u32(glb, 2);
    u32(glb, (std::uint32_t)(12 + 8 + jsonChunk.size() + 8 + bin.size()));
    u32(glb, (std::uint32_t)jsonChunk.size());
    u32(glb, 0x4E4F534Au); // JSON
    glb += jsonChunk;
    u32(glb, (std::uint32_t)bin.size());
    u32(glb, 0x004E4942u); // BIN
    glb += bin;
    std::ofstream(file, std::ios::binary | std::ios::trunc) << glb;
}

// The cooked model cache: a cooked import equals a fresh one; a changed file, changed settings or a damaged entry is
// never read (the file is imported again and re-cooked); the bypass and the size / clear calls work.
void TestModelCacheCooksAndReadsBack() {
    const std::filesystem::path dir = UnitTestSupport::TempDir("model_cache");
    ModelCache::SetDirectoryForTests((dir / "Library").string());
    ModelCache::Clear();
    struct Restore { ~Restore() { ModelCache::SetBypass(false); ModelCache::SetDirectoryForTests(""); } } restore;
    const std::filesystem::path file = dir / "tri.glb";
    WriteTriangleGlb(file, 2.0f);
    const ModelImportSettings settings;

    ModelCache::SetBypass(true);
    const auto fresh = Model::ImportDeferred(file.string(), settings);
    ModelCache::SetBypass(false);
    CHECK(fresh && fresh->MeshCount() == 1);
    if (!fresh || fresh->MeshCount() != 1) return;
    CHECK(ModelCache::DiskUsage().Entries == 0); // bypassed: nothing cooked

    const ModelCache::Stats s0 = ModelCache::GetStats();
    const auto cooked = Model::ImportDeferred(file.string(), settings);
    const ModelCache::Stats s1 = ModelCache::GetStats();
    CHECK(s1.Cooked == s0.Cooked + 1 && s1.Hits == s0.Hits);
    CHECK(ModelCache::DiskUsage().Entries == 1);
    const auto read = Model::ImportDeferred(file.string(), settings);
    const ModelCache::Stats s2 = ModelCache::GetStats();
    CHECK(s2.Hits == s1.Hits + 1 && s2.Cooked == s1.Cooked);
    CHECK(cooked && read);
    if (!cooked || !read) return;
    CHECK(cooked->ImportDigest() == fresh->ImportDigest());
    CHECK(read->ImportDigest() == fresh->ImportDigest());
    CHECK(read->MeshMaterial(0).Name == "CacheMat");
    CHECK(std::abs(read->BoundsMax().x - fresh->BoundsMax().x) < 1e-6f);

    // Other settings: their own entry, and the import they make.
    ModelImportSettings scaled;
    scaled.GlobalScale = 2.0f;
    CHECK(ModelCache::HashSettings(scaled) != ModelCache::HashSettings(settings));
    ModelImportSettings excluded;
    excluded.ExcludeNodes = {"Other"};
    CHECK(ModelCache::HashSettings(excluded) != ModelCache::HashSettings(settings));
    const auto big = Model::ImportDeferred(file.string(), scaled);
    CHECK(ModelCache::GetStats().Cooked == s2.Cooked + 1);
    CHECK(big && big->ImportDigest() != fresh->ImportDigest());

    // A damaged entry is a miss: imported from the file and cooked again, the same result.
    for (const auto& e : std::filesystem::directory_iterator(dir / "Library")) {
        std::fstream f(e.path(), std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(-5, std::ios::end);
        f.put('\x7f');
    }
    const ModelCache::Stats s3 = ModelCache::GetStats();
    const auto again = Model::ImportDeferred(file.string(), settings);
    const ModelCache::Stats s4 = ModelCache::GetStats();
    CHECK(s4.Hits == s3.Hits && s4.Cooked == s3.Cooked + 1);
    CHECK(again && again->ImportDigest() == fresh->ImportDigest());

    // The same path with other content: a new key, never the old entry.
    WriteTriangleGlb(file, 3.0f);
    const auto changed = Model::ImportDeferred(file.string(), settings);
    CHECK(ModelCache::GetStats().Cooked == s4.Cooked + 1);
    CHECK(changed && std::abs(changed->BoundsMax().x - 3.0f) < 1e-5f);

    CHECK(ModelCache::Clear() >= 3);
    CHECK(ModelCache::DiskUsage().Entries == 0);
}

} // namespace

void RegisterAssetLibraryTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("Asset Library: category rules and kit table", TestIndexRules);
    tests.emplace_back("Asset Library: kit pieces snap flush and fronts line up", TestSnapFlush);
    tests.emplace_back("Asset Library: lone pieces land on the kit grid", TestSnapGridAndFree);
    tests.emplace_back("Thumbnail cache: async save lands, replaces, and leaves no temp files", TestThumbnailAsyncSave);
    tests.emplace_back("Asset Library: material remap cache follows a rename and a removal", TestRemapCacheFollowsRenameAndRemoval);
    tests.emplace_back("Model cache: cooked import equals a fresh one; changed file, settings or entry re-cooks", TestModelCacheCooksAndReadsBack);
}
