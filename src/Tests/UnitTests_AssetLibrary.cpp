// The Asset Library (docs/PROJECT_ASSETS.md): asset_library.json's category rules and kit table
// (AssetLibraryIndex) and the modular kit snap maths (ModularSnap.h). Pure, no scene or ImGui.

#include "UnitTestSupport.h"

#include "AssetLibraryIndex.h"
#include "ModularSnap.h"

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
    // No module: no snap at all.
    CHECK(Near(Solve(lone, 0.0f, {}).Delta.x, 0.0f));
    CHECK(Near(SnapYawDegrees(44.0f), 0.0f) && Near(SnapYawDegrees(46.0f), 90.0f) && Near(SnapYawDegrees(-170.0f), -180.0f));
}

} // namespace

void RegisterAssetLibraryTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("Asset Library: category rules and kit table", TestIndexRules);
    tests.emplace_back("Asset Library: kit pieces snap flush and fronts line up", TestSnapFlush);
    tests.emplace_back("Asset Library: lone pieces land on the kit grid", TestSnapGridAndFree);
}
