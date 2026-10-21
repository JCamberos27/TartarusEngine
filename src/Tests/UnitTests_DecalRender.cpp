#include "UnitTestSupport.h"
#include "../Renderer/DecalSystem.h"
#include "../Renderer/DecalTextureArrays.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

// The clustered decal renderer's CPU halves (docs/DECALS.md): the cull test the compute shader
// mirrors, the deterministic sort, the per-cluster cap, the texture-array quadtree and the seeded
// variation. Run with --unit-tests "Decal render".
namespace {

Decals::GpuDecalBounds RandomBox(std::mt19937& rng) {
    std::uniform_real_distribution<float> pos(-1.5f, 1.5f), ext(0.05f, 1.5f), ang(-3.14159f, 3.14159f);
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(pos(rng), pos(rng), pos(rng)));
    m = glm::rotate(m, ang(rng), glm::normalize(glm::vec3(pos(rng), pos(rng), pos(rng)) + glm::vec3(1e-3f)));
    m = glm::scale(m, glm::vec3(ext(rng), ext(rng), ext(rng) * 0.3f)); // decals are flat-ish
    return DecalSystem::BoundsFor(m);
}

// Exact OBB-vs-AABB: all 15 separating axes.
bool ExactOverlap(const Decals::GpuDecalBounds& b, const glm::vec3& mn, const glm::vec3& mx) {
    const glm::vec3 bc = 0.5f * (mn + mx), bh = 0.5f * (mx - mn);
    const glm::vec3 a[3] = {glm::vec3(b.AxisX), glm::vec3(b.AxisY), glm::vec3(b.AxisZ)};
    const glm::vec3 d = glm::vec3(b.Centre) - bc;
    const glm::vec3 e[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::vector<glm::vec3> axes = {e[0], e[1], e[2], a[0], a[1], a[2]};
    for (const glm::vec3& u : e)
        for (const glm::vec3& v : a) axes.push_back(glm::cross(u, v));
    for (const glm::vec3& n : axes) {
        if (glm::dot(n, n) < 1e-10f) continue;
        const float rb = std::abs(glm::dot(a[0], n)) + std::abs(glm::dot(a[1], n)) + std::abs(glm::dot(a[2], n));
        const float ra = glm::dot(bh, glm::abs(n));
        if (std::abs(glm::dot(d, n)) > ra + rb + 1e-5f) return false;
    }
    return true;
}

// Brute force: any of a grid of points through the box inside the AABB.
bool SampledOverlap(const Decals::GpuDecalBounds& b, const glm::vec3& mn, const glm::vec3& mx) {
    const int n = 8;
    for (int i = 0; i <= n; ++i)
        for (int j = 0; j <= n; ++j)
            for (int k = 0; k <= n; ++k) {
                const glm::vec3 t = glm::vec3((float)i, (float)j, (float)k) / (float)n * 2.0f - 1.0f;
                const glm::vec3 p = glm::vec3(b.Centre) + t.x * glm::vec3(b.AxisX) + t.y * glm::vec3(b.AxisY) +
                                    t.z * glm::vec3(b.AxisZ);
                if (glm::all(glm::greaterThanEqual(p, mn)) && glm::all(glm::lessThanEqual(p, mx))) return true;
            }
    return false;
}

void SatMatchesReference() {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> pos(-1.5f, 1.5f), size(0.1f, 2.0f);
    int falseNeg = 0, exactMiss = 0, conservative = 0, hits = 0;
    for (int it = 0; it < 4000; ++it) {
        const Decals::GpuDecalBounds b = RandomBox(rng);
        const glm::vec3 mn(pos(rng), pos(rng), pos(rng));
        const glm::vec3 mx = mn + glm::vec3(size(rng), size(rng), size(rng));
        const bool six = DecalSystem::OverlapsAabb(b, mn, mx);
        const bool exact = ExactOverlap(b, mn, mx);
        if (SampledOverlap(b, mn, mx) && !six) ++falseNeg; // a point of the box inside, yet rejected
        if (exact && !six) ++exactMiss;
        if (six && !exact) ++conservative;
        hits += six ? 1 : 0;
    }
    CHECK(falseNeg == 0);
    CHECK(exactMiss == 0);
    CHECK(hits > 200);             // the cases aren't all trivial misses
    CHECK(conservative < hits / 4); // the skipped edge axes cost little
    // Axis-aligned boxes: the test is exact.
    const glm::mat4 m = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f)), glm::vec3(1.0f));
    const Decals::GpuDecalBounds aligned = DecalSystem::BoundsFor(m);
    CHECK(DecalSystem::OverlapsAabb(aligned, glm::vec3(0.49f), glm::vec3(1.0f)));
    CHECK(!DecalSystem::OverlapsAabb(aligned, glm::vec3(0.51f), glm::vec3(1.0f)));
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

void SortIsDeterministic() {
    std::vector<DecalInstance> v;
    std::mt19937 rng(7);
    for (uint32_t i = 0; i < 200; ++i) {
        DecalInstance d;
        d.SortKey = (uint64_t)(rng() % 12); // plenty of ties
        d.Gpu.Ids.w = i;                    // insertion order
        v.push_back(d);
    }
    std::vector<DecalInstance> a = v, b = v;
    DecalSystem::SortDeterministic(a);
    DecalSystem::SortDeterministic(b);
    bool same = true, ordered = true;
    for (size_t i = 0; i < a.size(); ++i) {
        same &= a[i].SortKey == b[i].SortKey && a[i].Gpu.Ids.w == b[i].Gpu.Ids.w;
        if (i > 0) {
            ordered &= a[i - 1].SortKey <= a[i].SortKey;
            if (a[i - 1].SortKey == a[i].SortKey) ordered &= a[i - 1].Gpu.Ids.w < a[i].Gpu.Ids.w; // ties keep insertion
        }
    }
    CHECK(same);
    CHECK(ordered);
}

void CapKeepsTopMost() {
    std::vector<Decals::GpuDecalBounds> bounds;
    const glm::mat4 box = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, 0.2f));
    const glm::mat4 away = glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, 0.0f, 0.0f));
    for (int i = 0; i < 300; ++i) bounds.push_back(DecalSystem::BoundsFor(i % 10 == 9 ? away * box : box));
    std::vector<uint32_t> out;
    DecalSystem::BinCluster(bounds, glm::vec3(-0.1f), glm::vec3(0.1f), Decals::MAX_PER_CLUSTER, out);
    // 270 overlap; the bottom 14 are dropped and the rest stay in sort order, ending with the top one.
    CHECK((int)out.size() == Decals::MAX_PER_CLUSTER);
    CHECK(std::is_sorted(out.begin(), out.end()));
    CHECK(!out.empty() && out.back() == 298u);
    bool noneAway = true;
    for (uint32_t i : out) noneAway &= i % 10 != 9;
    CHECK(noneAway);
    std::vector<uint32_t> all;
    DecalSystem::BinCluster(bounds, glm::vec3(-0.1f), glm::vec3(0.1f), 100000, all);
    CHECK(all.size() == 270u);
    CHECK(std::equal(out.begin(), out.end(), all.end() - Decals::MAX_PER_CLUSTER));
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
    tests.push_back({"Decal render: OBB-vs-froxel SAT against a brute-force reference", SatMatchesReference});
    tests.push_back({"Decal render: bounds from the box transform", BoundsFromBox});
    tests.push_back({"Decal render: deterministic stable sort", SortIsDeterministic});
    tests.push_back({"Decal render: per-cluster cap keeps the top-most", CapKeepsTopMost});
    tests.push_back({"Decal render: quadtree allocator invariants", QuadtreeInvariants});
    tests.push_back({"Decal render: seed-reproducible variation", SeededVariation});
}
