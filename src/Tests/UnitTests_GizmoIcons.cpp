// The Scene view's entity-icon maths (GizmoIconMath.h): the distance fade / shrink curve and the
// overlap declutter. Pure functions, no ImGui or scene.

#include "UnitTestSupport.h"

#include "GizmoIconMath.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void TestFadeAndSize() {
    using namespace GizmoIconMath;
    // Full strength inside the near range, gone past the far range.
    CHECK(Near(DistanceAlpha(0.0f, false), 1.0f));
    CHECK(Near(DistanceAlpha(kFadeNear, false), 1.0f));
    CHECK(Near(DistanceAlpha(kFadeFar, false), 0.0f));
    CHECK(Near(DistanceAlpha(500.0f, false), 0.0f));
    CHECK(Near(DistanceScale(kFadeNear, false), 1.0f));
    CHECK(Near(DistanceScale(kFadeFar, false), kMinSize));
    CHECK(Near(DistanceScale(500.0f, false), kMinSize));

    // Monotonic, and smoothstep puts the midpoint at exactly half.
    const float mid = 0.5f * (kFadeNear + kFadeFar);
    CHECK(Near(DistanceAlpha(mid, false), 0.5f));
    float prevA = 1.0f, prevS = 1.0f;
    for (float d = kFadeNear; d <= kFadeFar; d += 1.0f) {
        const float a = DistanceAlpha(d, false), s = DistanceScale(d, false);
        CHECK(a <= prevA + 1e-6f);
        CHECK(s <= prevS + 1e-6f);
        prevA = a;
        prevS = s;
    }

    // A selected icon never fades or shrinks.
    CHECK(Near(DistanceAlpha(500.0f, true), 1.0f));
    CHECK(Near(DistanceScale(500.0f, true), 1.0f));
}

void TestDeclutter() {
    using namespace GizmoIconMath;
    auto make = [](int id, float x, float y, int prio, float dist) {
        IconCandidate c;
        c.Id = id; c.X = x; c.Y = y; c.Priority = prio; c.Distance = dist;
        return c;
    };
    auto kept = [](const std::vector<IconCandidate>& v, int id) {
        for (const auto& c : v) if (c.Id == id) return c.Keep;
        return false;
    };

    // Two icons on the same pixel: the better priority wins regardless of input order.
    {
        std::vector<IconCandidate> v = {make(0, 100, 100, 3, 1.0f), make(1, 101, 100, 1, 30.0f)};
        Declutter(v, 10.0f);
        CHECK(kept(v, 1));
        CHECK(!kept(v, 0));
    }
    // Same priority: the nearer one wins.
    {
        std::vector<IconCandidate> v = {make(0, 50, 50, 3, 20.0f), make(1, 52, 50, 3, 5.0f)};
        Declutter(v, 10.0f);
        CHECK(kept(v, 1));
        CHECK(!kept(v, 0));
    }
    // Selected beats a light.
    {
        std::vector<IconCandidate> v = {make(0, 10, 10, 1, 1.0f), make(1, 10, 10, 0, 40.0f)};
        Declutter(v, 10.0f);
        CHECK(kept(v, 1));
        CHECK(!kept(v, 0));
    }
    // Far apart icons all stay; the boundary distance itself is kept (strictly-inside is dropped).
    {
        std::vector<IconCandidate> v = {make(0, 0, 0, 3, 1.0f), make(1, 10, 0, 3, 2.0f), make(2, 100, 100, 3, 3.0f)};
        Declutter(v, 10.0f);
        CHECK(kept(v, 0) && kept(v, 1) && kept(v, 2));
    }
    // A dropped icon does not suppress its neighbours (only kept ones count).
    {
        std::vector<IconCandidate> v = {make(0, 0, 0, 1, 1.0f), make(1, 8, 0, 3, 2.0f), make(2, 16, 0, 3, 3.0f)};
        Declutter(v, 10.0f);
        CHECK(kept(v, 0));
        CHECK(!kept(v, 1));
        CHECK(kept(v, 2));
    }
}

// The grid-bucketed Declutter gives exactly the brute-force answer (every earlier kept icon tested),
// on clustered, spread, negative and on-cell-edge positions, and with no spacing at all.
void TestDeclutterMatchesBruteForce() {
    using namespace GizmoIconMath;
    auto brute = [](std::vector<IconCandidate> v, float minSpacing) {
        std::stable_sort(v.begin(), v.end(), [](const IconCandidate& a, const IconCandidate& b) {
            if (a.Priority != b.Priority) return a.Priority < b.Priority;
            return a.Distance < b.Distance;
        });
        const float minSq = minSpacing * minSpacing;
        for (size_t i = 0; i < v.size(); ++i) {
            bool ok = true;
            for (size_t j = 0; j < i && ok; ++j) {
                if (!v[j].Keep) continue;
                const float dx = v[i].X - v[j].X, dy = v[i].Y - v[j].Y;
                if (dx * dx + dy * dy < minSq) ok = false;
            }
            v[i].Keep = ok;
        }
        return v;
    };
    unsigned state = 12345u;
    auto rnd = [&state](float lo, float hi) {
        state = state * 1664525u + 1013904223u;
        return lo + (hi - lo) * (float)(state >> 8) / 16777216.0f;
    };
    const float spacings[] = {0.0f, 9.6f, 10.0f, 37.5f};
    for (int round = 0; round < 40; ++round) {
        std::vector<IconCandidate> v;
        const int n = 50 + round * 25;
        const float span = round % 3 == 0 ? 60.0f : round % 3 == 1 ? 800.0f : 4000.0f; // clustered .. spread
        for (int i = 0; i < n; ++i) {
            IconCandidate c;
            c.Id = i;
            c.X = round % 4 == 0 ? std::floor(rnd(-span, span) / 10.0f) * 10.0f : rnd(-span, span); // some exactly on cell edges
            c.Y = rnd(-span, span);
            c.Priority = (int)rnd(0.0f, 4.0f);
            c.Distance = rnd(0.0f, 45.0f);
            v.push_back(c);
        }
        const float spacing = spacings[round % 4];
        const std::vector<IconCandidate> want = brute(v, spacing);
        Declutter(v, spacing);
        bool same = v.size() == want.size();
        for (size_t i = 0; same && i < v.size(); ++i) same = v[i].Id == want[i].Id && v[i].Keep == want[i].Keep;
        CHECK(same);
    }
}

} // namespace

void RegisterGizmoIconTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("Gizmo icons: distance fade and shrink curve", TestFadeAndSize);
    tests.emplace_back("Gizmo icons: declutter keeps the highest priority, nearest icon", TestDeclutter);
    tests.emplace_back("Gizmo icons: grid declutter matches the brute-force answer", TestDeclutterMatchesBruteForce);
}
