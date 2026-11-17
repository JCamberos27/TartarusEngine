#include "MotionGait.h"

#include "MotionDatabase.h"
#include "MotionMath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
float Percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const float x = p * (float)(v.size() - 1);
    const size_t i = (size_t)x;
    const float t = x - (float)i;
    return i + 1 < v.size() ? v[i] * (1.0f - t) + v[i + 1] * t : v[i];
}
} // namespace

GaitSpeedTable GaitSpeedTable::Measure(const MotionDatabase& db) {
    GaitSpeedTable t;
    if (!db.Loaded()) return t;
    static const char* const kGroup[4] = {"walk", "jog", "run", "crouch"};
    std::vector<float> plateaus[4], strafes[4];
    const float fps = db.Fps();
    const int h = 3;                                          // speed over +-3 frames
    const int w = std::max(1, (int)std::lround(0.25f * fps)); // yaw rate over +-0.25 s
    const int minLeg = (int)std::lround(kLegSeconds * fps);
    const int win = std::max(1, (int)std::lround(kWindowSeconds * fps));
    const float maxRate = kLegYawRateDeg * MotionMath::kPi / 180.0f;
    for (const auto& clip : db.Stream().Clips()) {
        const std::string first = clip.Tags.substr(0, clip.Tags.find(';'));
        int g = -1;
        for (int k = 0; k < 4; ++k)
            if (first == kGroup[k]) g = k;
        const int n = (int)clip.Count;
        if (g < 0 || n < 2 * w + minLeg) continue;
        std::vector<float> speed((size_t)n, 0.0f);
        std::vector<char> ok((size_t)n, 0), side((size_t)n, 0);
        const auto root = [&](int i) { return db.Stream().Root(clip.First + (std::uint32_t)i); };
        for (int i = std::max(h, w); i + std::max(h, w) < n; ++i) {
            speed[(size_t)i] = glm::length(root(i + h).Position - root(i - h).Position) * fps / (2.0f * (float)h);
            const float rate = std::fabs(root(i + w).Yaw - root(i - w).Yaw) * fps / (2.0f * (float)w);
            const std::uint8_t f = db.Flags(clip.First + (std::uint32_t)i);
            const bool crouched = (f & MotionLabel::Crouch) != 0;
            const bool base = rate < maxRate && speed[(size_t)i] > 0.5f && !(f & MotionLabel::Airborne) && crouched == (g == 3);
            ok[(size_t)i] = base && !(f & MotionLabel::Strafe);
            side[(size_t)i] = base && (f & MotionLabel::Strafe);
        }
        const auto legs = [&](const std::vector<char>& in, bool strafe) {
        for (int i = 0; i < n;) {
            if (!in[(size_t)i]) { ++i; continue; }
            int j = i;
            while (j < n && in[(size_t)j]) ++j;
            if (j - i >= minLeg) {
                float best = 0.0f, sum = 0.0f;
                int bestAt = i;
                for (int k = i; k < j; ++k) {
                    sum += speed[(size_t)k];
                    if (k - i >= win) sum -= speed[(size_t)(k - win)];
                    if (k - i + 1 >= win && sum / (float)win > best) {
                        best = sum / (float)win;
                        bestAt = k - win / 2;
                    }
                }
                // A run take's walk-backs between sprints are not runs.
                if (strafe) strafes[g].push_back(best);
                else if (g != 2 || db.Gait(clip.First + (std::uint32_t)bestAt) == MotionLabel::Run) plateaus[g].push_back(best);
            }
            i = j;
        }
        };
        legs(ok, false);
        legs(side, true);
    }
    for (int g = 0; g < 4; ++g) {
        t.Legs[g] = (int)plateaus[g].size();
        t.P25[g] = Percentile(plateaus[g], 0.25f);
        t.P50[g] = Percentile(plateaus[g], 0.50f);
        t.P75[g] = Percentile(plateaus[g], 0.75f);
        t.Max[g] = plateaus[g].empty() ? 0.0f : *std::max_element(plateaus[g].begin(), plateaus[g].end());
    }
    if (t.Legs[0]) t.Walk = t.P50[0];
    if (t.Legs[1]) t.Jog = t.P50[1];
    if (t.Legs[2]) t.Run = t.P50[2];
    if (t.Legs[3]) t.CrouchWalk = t.P50[3];
    for (int g = 0; g < 4; ++g) {
        t.StrafeLegs[g] = (int)strafes[g].size();
        t.Strafe[g] = Percentile(strafes[g], 0.5f);
    }
    return t;
}

float GaitSpeedTable::SpeedFor(int gait, bool crouch) const {
    if (gait <= MotionLabel::Idle) return 0.0f;
    if (crouch) return CrouchWalk;
    return gait == MotionLabel::Walk ? Walk : gait == MotionLabel::Jog ? Jog : Run;
}

float GaitSpeedTable::StrafeFor(int gait, bool crouch) const {
    if (gait <= MotionLabel::Idle) return 0.0f;
    if (crouch) return Strafe[3] > 0.0f ? Strafe[3] : 0.75f * CrouchWalk;
    for (int g = std::min(gait, (int)MotionLabel::Run) - 1; g >= 0; --g)
        if (Strafe[g] > 0.0f) return Strafe[g];
    return 0.75f * SpeedFor(gait, false);
}

int GaitSpeedTable::GaitFor(float speed, bool crouch) const {
    if (speed < 0.5f * (crouch ? CrouchWalk : Walk)) return MotionLabel::Idle;
    if (crouch) return MotionLabel::Walk;
    if (speed < 0.5f * (Walk + Jog)) return MotionLabel::Walk;
    if (speed < 0.5f * (Jog + Run)) return MotionLabel::Jog;
    return MotionLabel::Run;
}

std::string GaitSpeedTable::Describe() const {
    char b[768];
    std::snprintf(b, sizeof b,
                  "walk %.2f (legs %d, p25/50/75 %.2f/%.2f/%.2f, max %.2f), jog %.2f (%d, %.2f/%.2f/%.2f, %.2f), "
                  "run %.2f (%d, %.2f/%.2f/%.2f, %.2f), crouch %.2f (%d, %.2f/%.2f/%.2f, %.2f) m/s; strafe walk %.2f (%d legs), "
                  "jog %.2f (%d), run %.2f (%d), crouch %.2f (%d) m/s",
                  Walk, Legs[0], P25[0], P50[0], P75[0], Max[0], Jog, Legs[1], P25[1], P50[1], P75[1], Max[1], Run, Legs[2], P25[2],
                  P50[2], P75[2], Max[2], CrouchWalk, Legs[3], P25[3], P50[3], P75[3], Max[3], Strafe[0], StrafeLegs[0], Strafe[1],
                  StrafeLegs[1], Strafe[2], StrafeLegs[2], Strafe[3], StrafeLegs[3]);
    return b;
}
