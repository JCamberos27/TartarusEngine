// --npc-test npcperf: what the dummies cost (docs/MOTION_MATCHING_HANDOFF.md, Phase 5: "perf compared against the baseline").
// LocomotionLab's open floor (a light test scene); the player stands still at its south end looking north (+Z), and
// kPerfDummies dummies roam round posts spread in front of it (near, 25 m off, past 50 m) and behind it, so every animation LOD
// tier runs: every frame (near and in view), every second frame (past 25 m), every fourth (past 50 m or out of view).
// Reported: the director's think (the AI) and late (the pose: the matcher, foot pass, look) per frame and per dummy, and the
// frame's wall time. (Phase 5 measured it against the legacy controller too: docs/MOTION_MATCHING_HANDOFF.md.) Logged as [NpcPerf]; checked: every dummy spawned and roaming, and every
// dummy's matcher stepping as its LOD tier says.
#include "NpcTest.h"
#include "NpcDirector.h"
#include "Player.h"
#include "PhysicsWorld.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

namespace {
// The posts (flat x, z), the player at (0, -25) looking +Z: in front near (in view, LOD 1), mid (in view past 25 m: LOD 2), and
// behind (out of view: LOD 4). Clear of the ledge (-6, 8) and the stairs (8, 4..11); each dummy roams 7 m round its post.
const glm::vec2 kPosts[] = {
    {-6.0f, -18.0f}, {6.0f, -18.0f}, {-14.0f, -12.0f}, {14.0f, -12.0f}, // near
    {-16.0f, 4.0f},  {18.0f, 2.0f},  {-14.0f, 20.0f},  {14.0f, 22.0f},  // 25-50 m
    {0.0f, 26.0f},   {-22.0f, 26.0f},                                   // the far edge
    {-20.0f, -28.0f}, {20.0f, -28.0f},                                  // behind
};
constexpr int kPerfDummies = (int)(sizeof(kPosts) / sizeof(kPosts[0]));
constexpr float kPerfSeconds = 40.0f, kMeasureFrom = 8.0f; // the first seconds: spawns, the navigation mesh, shader warm-up
const glm::vec3 kPerfPlayer(0.0f, 0.05f, -25.0f);

struct PerfState {
    std::vector<std::string> Names;
    bool Spawned = false;
    std::chrono::steady_clock::time_point Last{};
    bool HaveLast = false;
    std::vector<float> FrameMs;
    long long LodFrames[5] = {0, 0, 0, 0, 0}; // dummy-frames per LOD tier (1, 2, 4)
    long long DummyFrames = 0, MovingFrames = 0;
    int UpdatesAt[32] = {}, SearchesAt[32] = {};
    float MeasureStart = -1.0f;
    bool Mm = false;
};
PerfState g_P;

float Percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return v[(size_t)(p * (float)(v.size() - 1))];
}
} // namespace

float NpcTest::NpcPerfSeconds() { return kPerfSeconds + 1.0f; } // (the report at kPerfSeconds, a second before the run ends)

void NpcTest::NpcPerfDrive(Player& player, float& wantYaw, float& wantPitch) {
    static bool placed = false;
    if (!placed) {
        PhysicsWorld::SetCharacterFootPosition(&kPerfPlayer.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = kPerfPlayer + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        placed = true;
    }
    wantYaw = player.Cam.Yaw = 90.0f; // +Z, over the floor
    wantPitch = -4.0f;
}

void NpcTest::NpcPerf(World& world, NpcDirector& npcs, float now) {
    PerfState& s = g_P;
    if (!s.Spawned && now > 0.5f && Assets) {
        for (int i = 0; i < kPerfDummies; ++i) {
            const glm::vec2 p = kPosts[i];
            const int index = npcs.SpawnAt(world, *Assets, glm::vec3(p.x, 0.0f, p.y), (float)(i * 47 % 360), true);
            if (Npc* n = index >= 0 ? npcs.Find(index) : nullptr) s.Names.push_back(n->Name);
        }
        s.Spawned = true;
        std::printf("[NpcPerf] %zu of %d dummies spawned\n", s.Names.size(), kPerfDummies);
    }
    // The frame's wall time (unlocked FPS: the real cost), from kMeasureFrom on.
    const auto t = std::chrono::steady_clock::now();
    if (now >= kMeasureFrom) {
        if (s.MeasureStart < 0.0f) {
            s.MeasureStart = now;
            int k = 0;
            for (const auto& up : npcs.Npcs())
                if (up && !up->Dead && k < 32) {
                    s.UpdatesAt[k] = up->Body.MmUpdates();
                    s.SearchesAt[k++] = up->Body.MmDebug().Searches;
                }
        }
        if (s.HaveLast) s.FrameMs.push_back(std::chrono::duration<float, std::milli>(t - s.Last).count());
        for (const auto& up : npcs.Npcs()) {
            if (!up || up->Dead) continue;
            ++s.DummyFrames;
            s.LodFrames[std::clamp(up->Lod, 1, 4)] += 1;
            s.MovingFrames += glm::length(glm::vec2(up->Velocity.x, up->Velocity.z)) > 0.3f ? 1 : 0;
            s.Mm = s.Mm || up->Body.MotionMatching();
        }
    }
    s.Last = t;
    s.HaveLast = true;
    if (now < kPerfSeconds || m_Done) return;

    // The report.
    m_Done = true;
    const float measured = now - s.MeasureStart;
    int alive = 0, k = 0;
    long long updates = 0, searches = 0;
    for (const auto& up : npcs.Npcs()) {
        if (!up || up->Dead) continue;
        ++alive;
        if (k < 32) {
            updates += up->Body.MmUpdates() - s.UpdatesAt[k];
            searches += up->Body.MmDebug().Searches - s.SearchesAt[k];
        }
        ++k;
    }
    const auto& think = npcs.ThinkCost();
    const auto& late = npcs.LateCost();
    const double frames = (double)s.FrameMs.size();
    const double perDummyLod = s.DummyFrames ? 1.0 / (double)s.DummyFrames : 0.0;
    std::printf("[NpcPerf] source %s; %d dummies alive (all scene spawns included), %.1f s measured, %.0f frames\n",
                s.Mm ? "motion matching" : "legacy", alive, measured, frames);
    std::printf("[NpcPerf] LOD tiers (share of dummy-frames): every frame %.0f %%, every 2nd %.0f %%, every 4th %.0f %%; moving %.0f %%\n",
                100.0 * s.LodFrames[1] * perDummyLod, 100.0 * s.LodFrames[2] * perDummyLod, 100.0 * s.LodFrames[4] * perDummyLod,
                100.0 * s.MovingFrames * perDummyLod);
    std::printf("[NpcPerf] frame ms: avg %.3f  p50 %.3f  p95 %.3f  p99 %.3f  max %.3f\n",
                frames > 0 ? (double)std::accumulate(s.FrameMs.begin(), s.FrameMs.end(), 0.0) / frames : 0.0, Percentile(s.FrameMs, 0.5f),
                Percentile(s.FrameMs, 0.95f), Percentile(s.FrameMs, 0.99f), Percentile(s.FrameMs, 1.0f));
    std::printf("[NpcPerf] director ms a frame: think %.3f (max %.3f), late %.3f (max %.3f), both %.3f = %.4f per dummy\n", think.Avg(),
                think.Max, late.Avg(), late.Max, think.Avg() + late.Avg(), alive ? (think.Avg() + late.Avg()) / alive : 0.0f);
    PrintCosts(npcs);
    Check(s.Spawned && (int)s.Names.size() == kPerfDummies, "every dummy spawned (" + std::to_string(s.Names.size()) + " of " +
                                                                   std::to_string(kPerfDummies) + ")");
    Check(s.MovingFrames > 0, "the dummies roam (moving on " + std::to_string(100.0 * s.MovingFrames * perDummyLod) + " % of dummy-frames)");
    Check(s.LodFrames[1] > 0 && s.LodFrames[2] > 0 && s.LodFrames[4] > 0, "every LOD tier ran");
    if (s.Mm) {
        // A dummy steps its matcher on the frames it's posed: about frames / its tier. Over all of them, the steps a second
        // against the frame rate those tiers give (within 15 %: busy dummies - just spawned, just hurt - run every frame).
        const double fps = measured > 0.0f ? frames / measured : 0.0;
        const double want = fps * measured * (s.LodFrames[1] + s.LodFrames[2] / 2.0 + s.LodFrames[4] / 4.0) * perDummyLod * alive;
        std::printf("[NpcPerf] matcher steps %lld (the LOD tiers give %.0f), searches %lld (%.2f a dummy a second)\n", updates, want,
                    searches, alive && measured > 0.0f ? searches / (double)alive / measured : 0.0);
        Check(want > 0.0 && std::fabs((double)updates - want) <= 0.15 * want,
              "each dummy's matcher steps as its LOD tier says (" + std::to_string(updates) + " steps, " + std::to_string((long long)want) +
                  " expected)");
    }
}
