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

int GaitSpeedTable::GroupOf(int style) {
    switch (style) {
        case MotionLabel::StyleWalk: return 0;
        case MotionLabel::StyleJog: return 1;
        case MotionLabel::StyleRun: return 2;
        case MotionLabel::StyleCrouch: return 3;
        case MotionLabel::StyleStroll: return 4;
        case MotionLabel::StyleSprint: return 5;
        default: return -1;
    }
}

int GaitSpeedTable::StyleOf(int group) {
    static const int k[kGroups] = {MotionLabel::StyleWalk, MotionLabel::StyleJog, MotionLabel::StyleRun, MotionLabel::StyleCrouch,
                                   MotionLabel::StyleStroll, MotionLabel::StyleSprint};
    return group >= 0 && group < kGroups ? k[group] : MotionLabel::StyleStand;
}

GaitSpeedTable GaitSpeedTable::Measure(const MotionDatabase& db) {
    GaitSpeedTable t;
    if (!db.Loaded()) return t;
    std::vector<float> plateaus[kGroups], strafes[kGroups], backs[kGroups];
    const float backCos = std::cos(kBackDeg * MotionMath::kPi / 180.0f);
    const float fps = db.Fps();
    const int h = 3;                                          // speed over +-3 frames
    const int w = std::max(1, (int)std::lround(0.25f * fps)); // yaw rate over +-0.25 s
    const int minLeg = (int)std::lround(kLegSeconds * fps);
    const int win = std::max(1, (int)std::lround(kWindowSeconds * fps));
    const float maxRate = kLegYawRateDeg * MotionMath::kPi / 180.0f;
    for (const auto& clip : db.Stream().Clips()) {
        const int n = (int)clip.Count;
        if (n < 2 * w + minLeg) continue;
        for (int g = 0; g < kGroups; ++g) {
        const int style = StyleOf(g);
        std::vector<float> speed((size_t)n, 0.0f);
        std::vector<char> ok((size_t)n, 0), side((size_t)n, 0), back((size_t)n, 0);
        const auto root = [&](int i) { return db.Stream().Root(clip.First + (std::uint32_t)i); };
        bool any = false;
        for (int i = std::max(h, w); i + std::max(h, w) < n; ++i) {
            const std::uint32_t at = clip.First + (std::uint32_t)i;
            if (db.Style(at) != style) continue;
            speed[(size_t)i] = glm::length(root(i + h).Position - root(i - h).Position) * fps / (2.0f * (float)h);
            const float rate = std::fabs(root(i + w).Yaw - root(i - w).Yaw) * fps / (2.0f * (float)w);
            const std::uint8_t f = db.Flags(at);
            const bool crouched = (f & MotionLabel::Crouch) != 0;
            const bool base = rate < maxRate && speed[(size_t)i] > 0.5f && !(f & MotionLabel::Airborne) && crouched == (g == 3);
            ok[(size_t)i] = base && !(f & MotionLabel::Strafe);
            glm::vec3 travel = root(i + h).Position - root(i - h).Position;
            travel.y = 0.0f;
            const bool backwards = glm::length(travel) > 1e-5f && glm::dot(glm::normalize(travel), MotionMath::YawDir(root(i).Yaw)) <= backCos;
            side[(size_t)i] = base && (f & MotionLabel::Strafe) && !backwards;
            back[(size_t)i] = base && (f & MotionLabel::Strafe) && backwards;
            any = any || base;
        }
        if (!any) continue;
        const auto legs = [&](const std::vector<char>& in, std::vector<float>* out) {
        for (int i = 0; i < n;) {
            if (!in[(size_t)i]) { ++i; continue; }
            int j = i;
            while (j < n && in[(size_t)j]) ++j;
            if (j - i >= minLeg) {
                float best = 0.0f, sum = 0.0f;
                for (int k = i; k < j; ++k) {
                    sum += speed[(size_t)k];
                    if (k - i >= win) sum -= speed[(size_t)(k - win)];
                    if (k - i + 1 >= win) best = std::max(best, sum / (float)win);
                }
                out[g].push_back(best);
            }
            i = j;
        }
        };
        legs(ok, plateaus);
        legs(side, strafes);
        legs(back, backs);
        }
    }
    for (int g = 0; g < kGroups; ++g) {
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
    if (t.Legs[4]) t.Stroll = t.P50[4];
    if (t.Legs[5]) t.Sprint = t.P50[5];
    for (int g = 0; g < kGroups; ++g) {
        t.StrafeLegs[g] = (int)strafes[g].size();
        t.Strafe[g] = Percentile(strafes[g], 0.5f);
        t.BackLegs[g] = (int)backs[g].size();
        t.Back[g] = Percentile(backs[g], 0.5f);
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

float GaitSpeedTable::StyleSpeed(int style) const {
    switch (style) {
        case MotionLabel::StyleStroll: return Stroll;
        case MotionLabel::StyleWalk: return Walk;
        case MotionLabel::StyleJog: return Jog;
        case MotionLabel::StyleRun: return Run;
        case MotionLabel::StyleCrouch: return CrouchWalk;
        case MotionLabel::StyleSprint: return Sprint;
        default: return 0.0f;
    }
}

float GaitSpeedTable::StyleStrafe(int style) const {
    const int g = GroupOf(style);
    if (g < 0) return 0.0f;
    if (Strafe[g] > 0.0f) return Strafe[g];
    if (style == MotionLabel::StyleCrouch) return StrafeFor(MotionLabel::Walk, true);
    if (style == MotionLabel::StyleStroll) return 0.75f * Stroll;
    return StrafeFor(style == MotionLabel::StyleWalk ? MotionLabel::Walk : style == MotionLabel::StyleJog ? MotionLabel::Jog : MotionLabel::Run, false);
}

float GaitSpeedTable::StyleBack(int style) const {
    const int g = GroupOf(style);
    if (g < 0) return 0.0f;
    return Back[g] > 0.0f ? Back[g] : StyleStrafe(style);
}

std::string GaitSpeedTable::Describe() const {
    char b[1400];
    std::snprintf(b, sizeof b,
                  "stroll %.2f (legs %d, p25/50/75 %.2f/%.2f/%.2f, max %.2f), walk %.2f (%d, %.2f/%.2f/%.2f, %.2f), "
                  "jog %.2f (%d, %.2f/%.2f/%.2f, %.2f), run %.2f (%d, %.2f/%.2f/%.2f, %.2f), crouch %.2f (%d, %.2f/%.2f/%.2f, %.2f) m/s; "
                  "sideways stroll %.2f (%d legs), walk %.2f (%d), jog %.2f (%d), run %.2f (%d), crouch %.2f (%d) m/s; "
                  "backwards stroll %.2f (%d), walk %.2f (%d), jog %.2f (%d), run %.2f (%d), crouch %.2f (%d) m/s",
                  Stroll, Legs[4], P25[4], P50[4], P75[4], Max[4], Walk, Legs[0], P25[0], P50[0], P75[0], Max[0], Jog, Legs[1], P25[1],
                  P50[1], P75[1], Max[1], Run, Legs[2], P25[2], P50[2], P75[2], Max[2], CrouchWalk, Legs[3], P25[3], P50[3], P75[3], Max[3],
                  Strafe[4], StrafeLegs[4], Strafe[0], StrafeLegs[0], Strafe[1], StrafeLegs[1], Strafe[2], StrafeLegs[2], Strafe[3],
                  StrafeLegs[3], Back[4], BackLegs[4], Back[0], BackLegs[0], Back[1], BackLegs[1], Back[2], BackLegs[2], Back[3], BackLegs[3]);
    char sprint[160];
    std::snprintf(sprint, sizeof sprint, "; sprint %.2f (%d legs, p25/50/75 %.2f/%.2f/%.2f), sideways %.2f (%d), backwards %.2f (%d) m/s", Sprint,
                  Legs[5], P25[5], P50[5], P75[5], Strafe[5], StrafeLegs[5], Back[5], BackLegs[5]);
    return std::string(b) + sprint;
}
