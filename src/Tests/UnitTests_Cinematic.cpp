#include "UnitTestSupport.h"
#include "Cinematic.h"

#include <cmath>

namespace {

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// The camera path: through every key, still at a shot's ends, the short way round in yaw, and a cut jumps.
void CinematicPath() {
    using Cinematic::Key;
    std::vector<Key> keys(5);
    keys[0].T = 0.0f; keys[0].Pos = {0, 0, 0}; keys[0].Yaw = 170.0f;
    keys[1].T = 2.0f; keys[1].Pos = {2, 0, 0}; keys[1].Yaw = -170.0f;   // 20 degrees on, across the wrap
    keys[2].T = 4.0f; keys[2].Pos = {4, 1, 0}; keys[2].Yaw = -150.0f;
    keys[3].T = 5.0f; keys[3].Pos = {9, 9, 9}; keys[3].Cut = true;
    keys[4].T = 7.0f; keys[4].Pos = {9, 9, 13};
    for (const Key& k : keys) {
        const Cinematic::Pose p = Cinematic::Evaluate(keys, k.T);
        CHECK(Near(p.Pos.x, k.Pos.x) && Near(p.Pos.y, k.Pos.y) && Near(p.Pos.z, k.Pos.z));
    }
    // between 170 and -170 the yaw passes 180, not 0
    const float mid = Cinematic::Evaluate(keys, 1.0f).Yaw;
    CHECK(mid > 170.0f && mid < 190.0f);
    // eased: barely moving just after the start and just before the shot's last key
    CHECK(Cinematic::Evaluate(keys, 0.05f).Pos.x < 0.01f);
    CHECK(Cinematic::Evaluate(keys, 3.95f).Pos.x > 3.99f);
    // holds the shot's last key until the cut, then the new shot
    CHECK(Near(Cinematic::Evaluate(keys, 4.9f).Pos.x, 4.0f));
    CHECK(Near(Cinematic::Evaluate(keys, 5.0f).Pos.x, 9.0f));
    CHECK(Near(Cinematic::Evaluate(keys, 6.0f).Pos.z, 11.0f)); // a one-segment shot, eased both ends: halfway at half time
}

} // namespace

void RegisterCinematicTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Cinematic: the camera path runs through its keys, eased, the short way round, with cuts", CinematicPath});
}
