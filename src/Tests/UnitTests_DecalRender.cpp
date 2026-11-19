#include "UnitTestSupport.h"
#include "../Renderer/DecalSystem.h"
#include "../Renderer/DecalTextureArrays.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

// The clustered decal renderer's CPU halves (docs/DECALS.md): the box bounds, the deterministic draw order,
// the texture-array quadtree and the seeded variation. Run with --unit-tests "Decal render".
namespace {

// The draw order the decal lists are sorted by: key, then seed, then gather order - a strict total order, so a
// shuffled list sorts the same way every time, ties on key and seed keep their gather order.
void DrawOrderIsDeterministic() {
    struct Item { std::uint64_t Key; std::uint32_t Seed, Seq; };
    std::vector<Item> v;
    std::mt19937 rng(7);
    for (std::uint32_t i = 0; i < 300; ++i) v.push_back({(std::uint64_t)(rng() % 12), (std::uint32_t)(rng() % 3), i}); // plenty of ties
    auto less = [](const Item& a, const Item& b) { return DecalSystem::DrawOrderLess(a.Key, a.Seed, a.Seq, b.Key, b.Seed, b.Seq); };
    std::vector<Item> a = v, b = v;
    std::shuffle(b.begin(), b.end(), rng);
    std::sort(a.begin(), a.end(), less);
    std::sort(b.begin(), b.end(), less);
    bool same = true, ordered = true;
    for (size_t i = 0; i < a.size(); ++i) {
        same &= a[i].Seq == b[i].Seq;
        if (i > 0) {
            const Item &p = a[i - 1], &q = a[i];
            ordered &= p.Key < q.Key || (p.Key == q.Key && (p.Seed < q.Seed || (p.Seed == q.Seed && p.Seq < q.Seq)));
        }
    }
    CHECK(same);
    CHECK(ordered);
    CHECK(!DecalSystem::DrawOrderLess(1, 2, 3, 1, 2, 3)); // irreflexive
    // A component decal's key: SortOrder first, then the entity.
    CHECK(DecalSystem::SortKeyFor(-5, 900) < DecalSystem::SortKeyFor(0, 1));
    CHECK(DecalSystem::SortKeyFor(0, 1) < DecalSystem::SortKeyFor(0, 2));
}

void BoundsFromBox() {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3));
    m = glm::scale(m, glm::vec3(2.0f, 4.0f, 0.5f));
    const Decals::GpuDecalBounds b = DecalSystem::BoundsFor(m);
    CHECK(glm::length(glm::vec3(b.Centre) - glm::vec3(1, 2, 3)) < 1e-5f);
    CHECK(std::abs(glm::length(glm::vec3(b.AxisX)) - 1.0f) < 1e-5f);
    CHECK(std::abs(glm::length(glm::vec3(b.AxisY)) - 2.0f) < 1e-5f);
    CHECK(std::abs(glm::length(glm::vec3(b.AxisZ)) - 0.25f) < 1e-5f);
}

bool Disjoint(const DecalPageAllocator::Alloc& a, const DecalPageAllocator::Alloc& b) {
    if (a.Layer != b.Layer) return true;
    return a.X + a.Size <= b.X || b.X + b.Size <= a.X || a.Y + a.Size <= b.Y || b.Y + b.Size <= a.Y;
}

void QuadtreeInvariants() {
    CHECK(DecalPageAllocator::SlotSizeFor(100) == 512 && DecalPageAllocator::SlotSizeFor(1024) == 1024 &&
          DecalPageAllocator::SlotSizeFor(1500) == 2048 && DecalPageAllocator::SlotSizeFor(9000) == 4096);
    {
        DecalPageAllocator q(1);
        std::vector<DecalPageAllocator::Alloc> twoK;
        for (int i = 0; i < 4; ++i) twoK.push_back(q.Allocate(2048));
        bool valid = true;
        for (const auto& a : twoK) valid &= a.Valid() && a.Size == 2048 && a.X % 2048 == 0 && a.Y % 2048 == 0;
        CHECK(valid);
        CHECK(!q.Allocate(512).Valid()); // one layer, full
        CHECK(q.UsedTexels() == 4096LL * 4096);
        q.Free(twoK[2]);
        int oneK = 0;
        while (q.Allocate(1024).Valid()) ++oneK;
        CHECK(oneK == 4); // the freed quadrant splits into four
        CHECK(q.CheckInvariants());
    }
    {
        // A 4K fills a layer; freeing everything merges back so a 4K fits again.
        DecalPageAllocator q(2);
        const auto big = q.Allocate(4096);
        CHECK(big.Valid() && big.Layer == 0 && big.X == 0 && big.Y == 0);
        std::vector<DecalPageAllocator::Alloc> small;
        for (int i = 0; i < 16; ++i) small.push_back(q.Allocate(1024));
        CHECK(small.back().Valid() && small.back().Layer == 1);
        CHECK(!q.Allocate(512).Valid());
        for (const auto& a : small) q.Free(a);
        CHECK(q.UsedTexels() == 4096LL * 4096);
        const auto again = q.Allocate(4096);
        CHECK(again.Valid() && again.Layer == 1);
        CHECK(q.CheckInvariants());
    }
    {
        // Random churn: nothing overlaps, everything stays inside its page, the books balance.
        DecalPageAllocator q(3);
        std::mt19937 rng(99);
        const int sizes[] = {512, 512, 512, 1024, 1024, 2048, 4096};
        std::vector<DecalPageAllocator::Alloc> live;
        bool ok = true;
        for (int step = 0; step < 3000; ++step) {
            if (!live.empty() && (rng() % 3 == 0)) {
                const size_t k = rng() % live.size();
                q.Free(live[k]);
                live.erase(live.begin() + (long)k);
            } else {
                const auto a = q.Allocate(sizes[rng() % 7]);
                if (!a.Valid()) continue;
                ok &= a.X >= 0 && a.Y >= 0 && a.X + a.Size <= Decals::PAGE_SIZE && a.Y + a.Size <= Decals::PAGE_SIZE;
                ok &= a.Layer < q.MaxLayers();
                for (const auto& b : live) ok &= Disjoint(a, b);
                live.push_back(a);
            }
            if (step % 50 == 0) ok &= q.CheckInvariants();
        }
        long long used = 0;
        for (const auto& a : live) used += (long long)a.Size * a.Size;
        CHECK(ok);
        CHECK(used == q.UsedTexels());
        CHECK(q.CheckInvariants());
        for (const auto& a : live) q.Free(a);
        CHECK(q.UsedTexels() == 0 && q.CheckInvariants());
    }
}

void SeededVariation() {
    const glm::mat4 a = DecalSystem::VariationFor(42, 30.0f, 0.2f, true);
    const glm::mat4 b = DecalSystem::VariationFor(42, 30.0f, 0.2f, true);
    CHECK(a == b);
    CHECK(DecalSystem::VariationFor(42, 0.0f, 0.0f, false) == glm::mat4(1.0f));
    int differ = 0, flipped = 0;
    bool bounded = true;
    for (int s = 0; s < 200; ++s) {
        const glm::mat4 m = DecalSystem::VariationFor(s, 30.0f, 0.2f, true);
        if (m != DecalSystem::VariationFor(s + 1, 30.0f, 0.2f, true)) ++differ;
        const float det = glm::determinant(glm::mat3(m));
        if (det < 0.0f) ++flipped;
        const float scale = glm::length(glm::vec3(m[1]));
        const float angle = std::atan2(m[1][0], m[1][1]); // the Y axis isn't flipped
        bounded &= scale >= 0.8f - 1e-4f && scale <= 1.2f + 1e-4f && std::abs(angle) <= glm::radians(30.0f) + 1e-4f;
        bounded &= std::abs(m[2][2] - 1.0f) < 1e-5f; // the projection depth is never jittered
    }
    CHECK(differ > 190);
    CHECK(flipped > 60 && flipped < 140);
    CHECK(bounded);
    bool frames = true;
    for (int s = 0; s < 100; ++s) {
        const int f = DecalSystem::FrameFor(s, 16);
        frames &= f >= 0 && f < 16 && f == DecalSystem::FrameFor(s, 16);
    }
    CHECK(frames);
    CHECK(DecalSystem::FrameFor(5, 1) == 0);
}

} // namespace

void RegisterDecalRenderTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Decal render: bounds from the box transform", BoundsFromBox});
    tests.push_back({"Decal render: deterministic draw order", DrawOrderIsDeterministic});
    tests.push_back({"Decal render: quadtree allocator invariants", QuadtreeInvariants});
    tests.push_back({"Decal render: seed-reproducible variation", SeededVariation});
}
