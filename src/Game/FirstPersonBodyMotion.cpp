#include "FirstPersonBodyMotion.h"

#include "Animation.h"
#include "Core/Env.h"
#include "Core/Paths.h"
#include "Model.h"
#include "MotionMatching/MotionBake.h"
#include "MotionMatching/MotionDebugDraw.h"
#include "MotionMatching/MotionMath.h"
#include "MotionMatching/MotionSearch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>

float MmTune(const char* key, float fallback) {
    static const std::map<std::string, float> values = [] {
        std::map<std::string, float> out;
        if (!Env::IsSet(Env::Var::MmTune)) return out;
        const std::string list = Env::Str(Env::Var::MmTune);
        for (size_t at = 0; at <= list.size();) {
            const size_t end = std::min(list.find(',', at), list.size());
            const std::string item = list.substr(at, end - at);
            const size_t eq = item.find('=');
            if (eq != std::string::npos) out[item.substr(0, eq)] = (float)std::atof(item.c_str() + eq + 1);
            at = end + 1;
        }
        for (const auto& [k, v] : out) std::printf("[MmTune] %s = %g\n", k.c_str(), v);
        return out;
    }();
    const auto it = values.find(key);
    return it != values.end() ? it->second : fallback;
}

namespace {
constexpr float kPi = 3.14159265358979f;
// A turn holds its side until the wanted facing is this far past straight behind (HoldTurnSide): a flick of up to 210 deg
// carries on round the way the mouse went.
constexpr float kTurnSideHold = 30.0f * kPi / 180.0f;
float Wrap(float a) { return MotionMath::WrapPi(a); }
// The share of the way a spring with half-life `h` closes in `dt`.
float Toward(float dt, float h) { return h > 1e-5f ? 1.0f - std::exp(-0.69314718f * dt / h) : 1.0f; }
} // namespace

// --- the shared database ---------------------------------------------------------------------------------------------

struct SharedMotionDatabase::RigShape {
    std::string Path; // (for the log only: the shape, not the file, decides)
    std::vector<std::string> Names;
    std::vector<int> Parents;
    std::vector<LocalTRS> Bind;
    std::vector<glm::mat4> Globals; // MotionBake::RootSpaceGlobals of Bind, root -1
    int Pelvis = -1, Chest = -1, Hand[2] = {-1, -1}, Upper[2] = {-1, -1}; // Model::NodeIndex, as the measuring looked them up
    int NodeCount() const { return (int)Names.size(); }
};

std::shared_ptr<const SharedMotionDatabase::RigShape> SharedMotionDatabase::ShapeOf(const Model& rig) {
    auto s = std::make_shared<RigShape>();
    s->Path = rig.Path();
    const int n = rig.NodeCount();
    s->Names.resize((size_t)n);
    s->Parents.resize((size_t)n);
    for (int i = 0; i < n; ++i) {
        s->Names[(size_t)i] = rig.NodeName(i);
        s->Parents[(size_t)i] = rig.NodeParent(i);
    }
    rig.BindLocalPose(s->Bind);
    MotionBake::RootSpaceGlobals(rig, s->Bind, -1, s->Globals);
    s->Pelvis = rig.NodeIndex("pelvis");
    s->Chest = rig.NodeIndex("spine_05");
    s->Hand[0] = rig.NodeIndex("hand_l");
    s->Hand[1] = rig.NodeIndex("hand_r");
    s->Upper[0] = rig.NodeIndex("upperarm_l");
    s->Upper[1] = rig.NodeIndex("upperarm_r");
    return s;
}

bool SharedMotionDatabase::SameShape(const RigShape& a, const RigShape& b) {
    if (a.Names != b.Names || a.Parents != b.Parents || a.Bind.size() != b.Bind.size() || a.Pelvis != b.Pelvis ||
        a.Chest != b.Chest || a.Hand[0] != b.Hand[0] || a.Hand[1] != b.Hand[1] || a.Upper[0] != b.Upper[0] || a.Upper[1] != b.Upper[1])
        return false;
    for (size_t i = 0; i < a.Bind.size(); ++i) // bit for bit (the globals follow from these)
        if (std::memcmp(&a.Bind[i].T, &b.Bind[i].T, sizeof(a.Bind[i].T)) || std::memcmp(&a.Bind[i].R, &b.Bind[i].R, sizeof(a.Bind[i].R)) ||
            std::memcmp(&a.Bind[i].S, &b.Bind[i].S, sizeof(a.Bind[i].S)))
            return false;
    return true;
}

std::vector<float> SharedMotionDatabase::HandOverShoulder(const MotionDatabase& db, const Model& rig, std::vector<float>* chestTilt) {
    return HandOverShoulder(db, *ShapeOf(rig), chestTilt);
}

std::vector<float> SharedMotionDatabase::HandOverShoulder(const MotionDatabase& db, const RigShape& rig, std::vector<float>* chestTilt,
                                                          const std::function<bool()>& go) {
    const MotionClipStream& stream = db.Stream();
    std::vector<float> out(db.FrameCount(), -1.0f);
    const std::vector<LocalTRS>& bind = rig.Bind;
    const std::vector<glm::mat4>& g = rig.Globals;
    const auto binding = stream.BindNames(rig.Names, rig.Pelvis, 0.0f);
    // The nodes from the hands up to the top: posed per frame, parents first (a node's index is above its parent's).
    std::vector<int> nodes;
    std::vector<char> in((size_t)rig.NodeCount(), 0);
    const int hand[2] = {rig.Hand[0], rig.Hand[1]}, upper[2] = {rig.Upper[0], rig.Upper[1]};
    if (hand[0] < 0 || hand[1] < 0 || upper[0] < 0 || upper[1] < 0) return out;
    for (int h : hand)
        for (int i = h; i >= 0 && !in[(size_t)i]; i = rig.Parents[(size_t)i]) in[(size_t)i] = 1;
    const int chest = rig.Chest;
    if (chestTilt) chestTilt->assign(db.FrameCount(), 0.0f);
    const glm::mat3 chestBindInv = chest >= 0 ? glm::inverse(glm::mat3(g[(size_t)chest])) : glm::mat3(1.0f);
    for (int i = 0; i < rig.NodeCount(); ++i)
        if (in[(size_t)i]) nodes.push_back(i);
    std::vector<int> streamBone((size_t)rig.NodeCount(), -1);
    for (size_t b = 0; b < binding.Nodes.size(); ++b)
        if (binding.Nodes[b] >= 0) streamBone[(size_t)binding.Nodes[b]] = (int)b;
    std::vector<glm::mat4> pose = g;
    for (std::uint32_t f = 0; f < db.FrameCount(); ++f) {
        if ((f & 1023) == 0 && go && !go()) return {}; // cancelled (the worker)
        for (int i : nodes) {
            LocalTRS l = bind[(size_t)i];
            if (streamBone[(size_t)i] >= 0) l.R = stream.Rotation(f, streamBone[(size_t)i]);
            const int p = rig.Parents[(size_t)i];
            pose[(size_t)i] = p >= 0 ? pose[(size_t)p] * l.ToMatrix() : l.ToMatrix();
        }
        if (chestTilt && chest >= 0 && in[(size_t)chest]) {
            glm::vec3 up = glm::mat3(pose[(size_t)chest]) * (chestBindInv * glm::vec3(0.0f, 1.0f, 0.0f));
            up = glm::normalize(up);
            (*chestTilt)[f] = glm::degrees(std::acos(std::clamp(up.y, -1.0f, 1.0f)));
        }
        out[f] = std::max(pose[(size_t)hand[0]][3].y - pose[(size_t)upper[0]][3].y, pose[(size_t)hand[1]][3].y - pose[(size_t)upper[1]][3].y);
    }
    return out;
}

std::string SharedMotionDatabase::CacheKey(const std::string& base, bool hasRig, const std::string& rigPath) {
    return base + "|" + (hasRig ? "rig:" + rigPath : std::string("no rig"));
}

namespace {
std::mutex s_SharedLock;
std::map<std::string, std::weak_ptr<const SharedMotionDatabase>> s_SharedLoaded; // CacheKey -> load
std::atomic<std::uint64_t> s_SharedGeneration{0};

// The last loads stay alive after their bodies stop, so a later Play in the same session reuses them (the database is
// ~2 s to load and measure). They are kept across scene switches until shutdown (ReleaseKept): every scene's body uses
// the same database. Each remembers its files' write times: a rewritten database, rig file or rig .meta loads afresh.
struct KeptLoad {
    std::shared_ptr<const SharedMotionDatabase> Db;
    std::vector<std::filesystem::file_time_type> Stamps;
    std::uint64_t LastUse = 0;
};
std::map<std::string, KeptLoad> s_SharedKept; // CacheKey -> load
std::uint64_t s_SharedKeptClock = 0;

std::vector<std::filesystem::file_time_type> FileStamps(const std::string& base, const std::string& rigPath) {
    std::vector<std::filesystem::file_time_type> out;
    for (const std::string& f : SharedMotionDatabase::WatchedFiles(base, rigPath)) {
        std::error_code ec;
        out.push_back(std::filesystem::last_write_time(std::filesystem::u8path(f), ec));
        if (ec) out.back() = std::filesystem::file_time_type{};
    }
    return out;
}

// One spelling per file for comparing bases: absolute, normalized, forward slashes, lower case (Windows paths).
std::string BaseSpelling(const std::string& base) {
    std::error_code ec;
    std::string s = std::filesystem::absolute(std::filesystem::u8path(base), ec).lexically_normal().generic_u8string();
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// The prefetch (Prefetch): at most one, of one base. Lock order: s_SharedLock, then s_PrefetchLock (the job takes only
// the second, so Get may wait on it holding the first).
struct PrefetchLoad {
    enum class State { Queued, Running, Done, Taken };
    std::string Base, Spelling;
    std::vector<std::filesystem::file_time_type> Stamps; // the database's two files, when it was queued
    State St = State::Queued;
    std::atomic<bool> Cancel{false};
    std::shared_ptr<SharedMotionDatabase> Result; // Done: the loaded database (null: failed or cancelled)
    // The driver rig the job measured Result on (Measure: Result is then final for that shape); null: only read and built.
    std::shared_ptr<const SharedMotionDatabase::RigShape> Rig;
    double ReadMs = 0.0, BuildMs = 0.0, MeasureMs = 0.0;
};
std::mutex s_PrefetchLock;
std::condition_variable s_PrefetchDone;
std::shared_ptr<PrefetchLoad> s_Prefetch;
std::function<void(SharedMotionDatabase::PrefetchJob)> s_PrefetchRun;
std::function<void(bool)> s_PrefetchOnWait;

std::vector<std::filesystem::file_time_type> DatabaseStamps(const std::string& base) { return FileStamps(base, std::string()); }

void DropPrefetchLocked() { // s_PrefetchLock held
    if (!s_Prefetch) return;
    s_Prefetch->Cancel = true;
    if (s_Prefetch->St == PrefetchLoad::State::Queued) s_Prefetch->St = PrefetchLoad::State::Taken; // never runs
    s_Prefetch.reset();
}

// Get's load of `base`, if a prefetch has it (s_SharedLock held): waits while it runs; null when there is none, it was
// still queued (Get reads, the job then does nothing), it failed, the files changed since it was queued, or the job
// measured it on a rig whose shape isn't `rig`'s (its exclusions would be another rig's). `measured`: the job measured it
// on `rig` (Get skips Measure); else it was only read and built.
std::shared_ptr<SharedMotionDatabase> TakePrefetch(const std::string& base, const Model* rig, bool& measured) {
    measured = false;
    std::unique_lock<std::mutex> lock(s_PrefetchLock);
    std::shared_ptr<PrefetchLoad> p = s_Prefetch;
    if (!p || p->Spelling != BaseSpelling(base)) return nullptr;
    s_Prefetch.reset();
    if (p->St == PrefetchLoad::State::Queued) {
        p->St = PrefetchLoad::State::Taken;
        std::printf("[MotionMatching] prefetch still queued at the first Get: loaded here instead\n");
        return nullptr;
    }
    double waitMs = 0.0;
    if (p->St == PrefetchLoad::State::Running) {
        const auto t0 = std::chrono::steady_clock::now();
        lock.unlock();
        if (s_PrefetchOnWait) s_PrefetchOnWait(true);
        lock.lock();
        s_PrefetchDone.wait(lock, [&] { return p->St == PrefetchLoad::State::Done; });
        lock.unlock();
        if (s_PrefetchOnWait) s_PrefetchOnWait(false);
        lock.lock();
        waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    p->St = PrefetchLoad::State::Taken;
    const bool current = p->Stamps == DatabaseStamps(base);
    // A job measured on another shape (an outfit landing changed the driver, or Get has no rig): read afresh.
    const bool sameRig = !p->Rig || (rig && SharedMotionDatabase::SameShape(*p->Rig, *SharedMotionDatabase::ShapeOf(*rig)));
    std::printf("[MotionMatching] prefetched database %s (read %.0f ms + build %.0f ms + measure %.0f ms on the worker; Get waited %.0f ms)\n",
                !p->Result ? "failed: loaded here" : !current ? "stale (files rewritten): loaded here"
                : !sameRig ? "measured on another rig: loaded here" : p->Rig ? "taken, measured" : "taken",
                p->ReadMs, p->BuildMs, p->MeasureMs, waitMs);
    if (!current || !sameRig) return nullptr;
    measured = p->Result && p->Rig;
    return p->Result;
}
}

void SharedMotionDatabase::SetPrefetchRunner(std::function<void(PrefetchJob)> run, std::function<void(bool)> onWait) {
    std::lock_guard<std::mutex> lock(s_PrefetchLock);
    s_PrefetchRun = std::move(run);
    s_PrefetchOnWait = std::move(onWait);
}

void SharedMotionDatabase::Prefetch(const std::string& base, std::shared_ptr<const RigShape> rig) {
    const std::string spelling = BaseSpelling(base);
    const std::vector<std::filesystem::file_time_type> stamps = DatabaseStamps(base);
    std::lock_guard<std::mutex> shared(s_SharedLock);
    for (const auto& [key, kept] : s_SharedKept) // already loaded and current: a later Play reuses it
        if (BaseSpelling(key.substr(0, key.rfind('|'))) == spelling && kept.Stamps.size() >= 2 &&
            std::equal(stamps.begin(), stamps.end(), kept.Stamps.begin()))
            return;
    std::lock_guard<std::mutex> lock(s_PrefetchLock);
    if (!s_PrefetchRun) return;
    if (s_Prefetch && s_Prefetch->Spelling == spelling &&
        (s_Prefetch->Rig == rig || (s_Prefetch->Rig && rig && SameShape(*s_Prefetch->Rig, *rig))))
        return; // pending or held: the same files, the same rig
    DropPrefetchLocked();
    auto p = std::make_shared<PrefetchLoad>();
    p->Base = base;
    p->Spelling = spelling;
    p->Stamps = stamps;
    p->Rig = std::move(rig);
    s_Prefetch = p;
    s_PrefetchRun([p](const std::atomic<bool>& stop) {
        {
            std::lock_guard<std::mutex> lock(s_PrefetchLock);
            if (p->St != PrefetchLoad::State::Queued || p->Cancel) return;
            p->St = PrefetchLoad::State::Running;
        }
        auto db = std::make_shared<SharedMotionDatabase>();
        std::string err;
        // Asked after each file, through the build and the measuring: the place to stop (a cancel, shutdown), and a 1 ms
        // breather every kBreatherMs of work (the worker's frames-first rule).
        constexpr auto kBreatherMs = std::chrono::milliseconds(16);
        auto worked = std::chrono::steady_clock::now();
        const std::function<bool()> go = [&] {
            if (p->Cancel || stop) return false;
            if (std::chrono::steady_clock::now() - worked >= kBreatherMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                worked = std::chrono::steady_clock::now();
            }
            return !p->Cancel && !stop;
        };
        bool ok = db->Db.Load(p->Base, &err, go);
        const auto measured0 = std::chrono::steady_clock::now();
        if (ok && p->Rig) ok = Measure(*db, p->Base, p->Rig.get(), go);
        const double measureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - measured0).count();
        {
            std::lock_guard<std::mutex> lock(s_PrefetchLock);
            p->ReadMs = db->Db.ReadSeconds() * 1000.0;
            p->BuildMs = (db->Db.LoadSeconds() - db->Db.ReadSeconds()) * 1000.0;
            p->MeasureMs = p->Rig ? measureMs : 0.0;
            if (ok && !p->Cancel) p->Result = std::move(db);
            p->St = PrefetchLoad::State::Done;
        }
        s_PrefetchDone.notify_all();
        if (ok)
            std::printf("[MotionMatching] prefetch: %s read in %.0f ms, built in %.0f ms, measured in %.0f ms%s (worker)\n", p->Base.c_str(),
                        p->ReadMs, p->BuildMs, p->MeasureMs, p->Rig ? "" : " (no rig: Get measures)");
    });
}

void SharedMotionDatabase::CancelPrefetch() {
    std::lock_guard<std::mutex> lock(s_PrefetchLock);
    DropPrefetchLocked();
}

bool SharedMotionDatabase::PrefetchPending() {
    std::lock_guard<std::mutex> lock(s_PrefetchLock);
    return s_Prefetch != nullptr;
}

void SharedMotionDatabase::Invalidate(const std::string& base) {
    const std::string want = BaseSpelling(base);
    std::lock_guard<std::mutex> lock(s_SharedLock);
    for (auto it = s_SharedLoaded.begin(); it != s_SharedLoaded.end();) {
        const size_t bar = it->first.rfind('|');
        if (BaseSpelling(it->first.substr(0, bar)) == want) it = s_SharedLoaded.erase(it);
        else ++it;
    }
    for (auto it = s_SharedKept.begin(); it != s_SharedKept.end();) {
        const size_t bar = it->first.rfind('|');
        if (BaseSpelling(it->first.substr(0, bar)) == want) it = s_SharedKept.erase(it);
        else ++it;
    }
    {
        std::lock_guard<std::mutex> prefetch(s_PrefetchLock); // read before the rebake (or during it): the next Get reads
        if (s_Prefetch && s_Prefetch->Spelling == want) DropPrefetchLocked();
    }
    ++s_SharedGeneration;
}

std::uint64_t SharedMotionDatabase::Generation() { return s_SharedGeneration.load(); }

// The rest of a first Get after the files are read and built: the rig's exclusions (with a rig), the gaits, the turn on
// the spot and the start frame. `go` (the worker) is asked between the steps: false stops it (the load is then thrown away).
bool SharedMotionDatabase::Measure(SharedMotionDatabase& measured, const std::string& base, const RigShape* rig,
                                   const std::function<bool()>& go) {
    SharedMotionDatabase* shared = &measured;
    std::vector<float> tilt;
    if (rig) {
        const std::vector<float> over = HandOverShoulder(shared->Db, *rig, &tilt, go);
        if (over.size() != shared->Db.FrameCount()) return false; // cancelled
        const MotionDatabase& d = shared->Db;
        std::vector<char> bad(d.FrameCount(), 0);
        const auto med = [](std::vector<float> v) {
            if (v.empty()) return 0.0f;
            std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)(v.size() / 2), v.end());
            return v[v.size() / 2];
        };
        int raw = 0, searchable = 0, hist[5] = {0, 0, 0, 0, 0};
        const float edges[5] = {-0.15f, -0.10f, -0.05f, 0.0f, 0.05f};
        std::vector<std::uint8_t> why(d.FrameCount(), 0); // per frame: 1 a hand over its shoulder, 2 bent over
        for (std::uint32_t f = 0; f < d.FrameCount(); ++f) {
            if (d.Flags(f) & (MotionSearch::kAlwaysForbid | MotionLabel::Airborne)) continue;
            ++searchable;
            for (int k = 0; k < 5; ++k) hist[k] += over[f] > edges[k];
            const bool bent = !(d.Flags(f) & MotionLabel::Crouch) && tilt[f] > kBentOverDeg &&
                              FirstPersonBodyMotion::RootSpeed(d, f) < kBentOverSpeed;
            if (bent) ++shared->BentOverFrames;
            why[f] = (std::uint8_t)((over[f] > kHandsUpMargin ? 1 : 0) | (bent ? 2 : 0));
            raw += why[f] != 0;
        }
        // Each run of such frames is excluded as a whole gesture: widened back to where its measure (the higher hand's height
        // for a hand, the chest's tilt bent over) was last at the take's rest level - where the gesture began - and on to where
        // it is back there (at most kGestureMax frames either way), then padded. Excluding only the frames over the line cut an
        // idle's gesture in the middle: the take played the hand's rise, ran into the excluded frames, was forced to search
        // and jumped back to the arms at rest (the owner, 2026-11-17: "they play half way through and snap back").
        std::vector<float> handRest((int)d.Stream().Clips().size(), 0.0f), tiltRest((int)d.Stream().Clips().size(), 0.0f);
        for (int c = 0; c < (int)d.Stream().Clips().size(); ++c) {
            const auto& clip = d.Clip(c);
            std::vector<float> h(over.begin() + clip.First, over.begin() + clip.First + clip.Count);
            std::vector<float> t(tilt.begin() + clip.First, tilt.begin() + clip.First + clip.Count);
            handRest[(size_t)c] = med(h);
            tiltRest[(size_t)c] = med(t);
        }
        const auto exclude = [&](bool widen, std::vector<char>& out) {
            out.assign(d.FrameCount(), 0);
            for (std::uint32_t f = 0; f < d.FrameCount();) {
                if (!why[f]) { ++f; continue; }
                const int c = d.ClipOf(f);
                const auto& clip = d.Clip(c);
                const std::uint32_t last = clip.First + clip.Count - 1;
                std::uint32_t a = f, b = f;
                while (b + 1 <= last && why[b + 1]) ++b;
                std::uint32_t from = (std::uint32_t)std::max<std::int64_t>(clip.First, (std::int64_t)a - kHandsUpPad);
                std::uint32_t to = std::min<std::uint32_t>(last, b + (std::uint32_t)kHandsUpPad);
                if (widen) {
                    // The whole gesture - the hand and the chest together: the stretch's ends (padded) walk out until the frame
                    // beyond each is at the take's rest level on both measures (at most kGestureMax frames further). Widening
                    // each measure from the line only, the pad could land the end in an earlier lean of the chest: entered
                    // mid-gesture.
                    const auto elevated = [&](std::uint32_t i) {
                        return over[i] > handRest[(size_t)c] + kGestureHandRest || tilt[i] > tiltRest[(size_t)c] + kGestureTiltRest;
                    };
                    const std::uint32_t reach = (std::uint32_t)(kGestureMax + kHandsUpPad);
                    while (from > clip.First && a - from < reach && elevated(from - 1)) --from;
                    while (to < last && to - b < reach && elevated(to + 1)) ++to;
                }
                for (std::uint32_t j = from; j <= to; ++j) out[j] = 1;
                f = b + 1;
            }
            for (std::uint32_t g = 0; g < d.FrameCount(); ++g) // (airborne frames stay: the jumps play locked)
                if (d.Flags(g) & MotionLabel::Airborne) out[g] = 0;
        };
        std::vector<char> narrow;
        exclude(false, narrow);
        exclude(true, bad);
        // The idle takes, measured both ways: an excluded stretch entered mid-gesture is one whose frame before it has the hand
        // over kGestureHandRest above the take's rest (or the chest over kGestureTiltRest past it) - a forced jump from there is
        // a visible snap back.
        const auto cuts = [&](const std::vector<char>& ex, int& runs, int& mid, std::uint32_t& frames) {
            runs = mid = 0;
            frames = 0;
            for (int c = 0; c < (int)d.Stream().Clips().size(); ++c) {
                const auto& clip = d.Clip(c);
                if (!(d.Flags(clip.First + clip.Count / 2) & MotionLabel::IdleTake)) continue;
                for (std::uint32_t f = clip.First + 1; f < clip.First + clip.Count; ++f) {
                    frames += ex[f] != 0;
                    if (!ex[f] || ex[f - 1] || (d.Flags(f - 1) & MotionSearch::kAlwaysForbid)) continue;
                    ++runs;
                    mid += over[f - 1] > handRest[(size_t)c] + kGestureHandRest || tilt[f - 1] > tiltRest[(size_t)c] + kGestureTiltRest;
                }
            }
        };
        int runsN, midN, runsW, midW;
        std::uint32_t framesN, framesW;
        cuts(narrow, runsN, midN, framesN);
        cuts(bad, runsW, midW, framesW);
        shared->IdleGestureCuts = midW;
        std::printf("[MotionMatching] idle takes: %d excluded stretches, %d entered mid-gesture, %u frames (the frames over the line only); "
                    "%d, %d, %u (the whole gesture)\n", runsN, midN, framesN, runsW, midW, framesW);
        for (int c = 0; c < (int)d.Stream().Clips().size(); ++c) {
            const auto& clip = d.Clip(c);
            if (!(d.Flags(clip.First + clip.Count / 2) & MotionLabel::IdleTake)) continue;
            std::string line;
            for (std::uint32_t f = clip.First; f < clip.First + clip.Count; ++f) {
                if (!bad[f] || (f > clip.First && bad[f - 1])) continue;
                std::uint32_t e = f;
                std::uint8_t kind = 0;
                while (e + 1 < clip.First + clip.Count && bad[e + 1]) kind |= why[e++];
                kind |= why[e];
                char s[256];
                const bool mid = f > clip.First && !(d.Flags(f - 1) & MotionSearch::kAlwaysForbid) &&
                                 (over[f - 1] > handRest[(size_t)c] + kGestureHandRest || tilt[f - 1] > tiltRest[(size_t)c] + kGestureTiltRest);
                char note[160] = "";
                if (mid)
                    std::snprintf(note, sizeof note, ", entered mid-gesture: before it the hand %+.3f m (rest %+.3f), the chest %.1f deg (rest %.1f)",
                                  over[f - 1], handRest[(size_t)c], tilt[f - 1], tiltRest[(size_t)c]);
                std::snprintf(s, sizeof s, " %.2f-%.2f s (%s%s)", d.SecondsInClip(f), d.SecondsInClip(e),
                              (kind & 1) && (kind & 2) ? "hand, bent" : (kind & 1) ? "hand" : (kind & 2) ? "bent" : "pad", note);
                line += s;
                f = e;
            }
            if (!line.empty()) std::printf("[MotionMatching]   %s (%.1f s):%s\n", clip.Name.c_str(), clip.Count / d.Fps(), line.c_str());
        }
        shared->HandsUpFrames = shared->Db.ExcludeFrames(bad);
        std::printf("[MotionMatching] hands over the shoulders or bent over: %d of %d searchable frames (%u bent over %.0f deg; %u excluded with the pads); "
                    "share over -0.15/-0.10/-0.05/0/+0.05 m: %.1f/%.1f/%.1f/%.1f/%.1f %%\n", raw, searchable, shared->BentOverFrames, kBentOverDeg,
                    shared->HandsUpFrames, 100.0f * hist[0] / std::max(1, searchable), 100.0f * hist[1] / std::max(1, searchable),
                    100.0f * hist[2] / std::max(1, searchable), 100.0f * hist[3] / std::max(1, searchable), 100.0f * hist[4] / std::max(1, searchable));
    }
    if (go && !go()) return false;
    // Stretches of takes cut by hand (take, seconds in): an interrupted take's stop lands in the arm swing of the start it
    // cut short, and a stop that jumped there threw the arms (C stop on steep platform: the pose 9.7 deg a frame at p99,
    // 4.2 elsewhere). Only that stretch: the take's other stops and its starts stay (forbidding the whole take moved 29 legs).
    {
        struct Cut { const char* Take; float From, To; };
        static constexpr Cut kCuts[] = {{"Stand_to_Jog_Interrupted_360_Take001", 11.10f, 11.35f}};
        std::vector<char> cut(shared->Db.FrameCount(), 0);
        for (const Cut& c : kCuts)
            for (int k = 0; k < (int)shared->Db.Stream().Clips().size(); ++k) {
                const auto& clip = shared->Db.Clip(k);
                if (clip.Name != c.Take) continue;
                const float fps = shared->Db.Fps();
                const std::uint32_t a = (std::uint32_t)std::max(0L, std::lround(c.From * fps));
                const std::uint32_t b = std::min<std::uint32_t>(clip.Count - 1, (std::uint32_t)std::lround(c.To * fps));
                for (std::uint32_t f = a; f <= b && f < clip.Count; ++f) cut[clip.First + f] = 1;
            }
        shared->CutFrames = shared->Db.ExcludeFrames(cut);
        std::printf("[MotionMatching] stretches cut by hand: %u frames\n", shared->CutFrames);
    }
    const MotionDatabase& db = shared->Db;
    shared->Gaits = GaitSpeedTable::Measure(db);
    std::printf("[MotionMatching] %s: %u frames, %u excluded as capture glitches (a bone over %.0f deg in a frame, the head and neck "
                "over %.0f, %d either side), %u as the way to and from prone (%.1f s)\n", base.c_str(), db.FrameCount(), db.GlitchFrames(),
                MotionDatabase::kGlitchDegrees, MotionDatabase::kHeadGlitchDegrees, MotionDatabase::kGlitchPad, db.ProneApproachFrames(),
                MotionDatabase::kProneApproachSeconds);
    // The turn on the spot: the idle frames whose root turns (over 30 deg/s), standing, searchable; their p75 rate.
    std::vector<float> rates;
    for (std::uint32_t f = 0; f < db.FrameCount(); ++f) {
        const std::uint8_t fl = db.Flags(f);
        if (db.Gait(f) != MotionLabel::Idle || (fl & (MotionLabel::Crouch | MotionLabel::Airborne | MotionSearch::kAlwaysForbid))) continue;
        const float r = std::fabs(FirstPersonBodyMotion::RootYawRate(db, f));
        if (r > 30.0f * kPi / 180.0f && FirstPersonBodyMotion::RootSpeed(db, f) < 0.5f) rates.push_back(r);
    }
    if (!rates.empty()) {
        std::sort(rates.begin(), rates.end());
        shared->TurnInPlaceRate = rates[(size_t)(0.75f * (float)(rates.size() - 1))];
    }
    // The start: the first searchable standing frame of an idle take (MotionLabel::IdleTake), as captured.
    for (std::uint32_t f = 0; f < db.FrameCount(); ++f) {
        const std::uint8_t fl = db.Flags(f);
        if (!(fl & MotionLabel::IdleTake) || db.Gait(f) != MotionLabel::Idle ||
            (fl & (MotionSearch::kAlwaysForbid | MotionLabel::Crouch | MotionLabel::Airborne)))
            continue;
        shared->StartFrame = f;
        std::printf("[MotionMatching] start frame: %s @ %.2f s\n", db.Clip(db.ClipOf(f)).Name.c_str(), db.SecondsInClip(f));
        break;
    }
    return true;
}

std::shared_ptr<const SharedMotionDatabase> SharedMotionDatabase::Get(const std::string& base, std::string* error, const Model* rig) {
    std::lock_guard<std::mutex> lock(s_SharedLock);
    auto& s_Loaded = s_SharedLoaded;
    const std::string key = CacheKey(base, rig != nullptr, rig ? rig->Path() : std::string());
    const std::vector<std::filesystem::file_time_type> stamps = FileStamps(base, rig ? ProjectPaths::Resolve(rig->Path()) : std::string());
    const auto keep = [&](const std::shared_ptr<const SharedMotionDatabase>& db) {
        s_SharedKept[key] = KeptLoad{db, stamps, ++s_SharedKeptClock};
        while (s_SharedKept.size() > kKeptLoads) { // the least recently used goes
            auto oldest = s_SharedKept.begin();
            for (auto it = s_SharedKept.begin(); it != s_SharedKept.end(); ++it)
                if (it->second.LastUse < oldest->second.LastUse) oldest = it;
            s_SharedKept.erase(oldest);
        }
    };
    const auto kept = s_SharedKept.find(key);
    if (auto live = s_Loaded[key].lock()) {
        // Held by a body (beyond `live` here and the kept copy): that load until Invalidate, as before.
        const bool isKept = kept != s_SharedKept.end() && kept->second.Db == live;
        if (live.use_count() > (isKept ? 2 : 1)) {
            if (isKept) kept->second.LastUse = ++s_SharedKeptClock;
            return live;
        }
    }
    // Nothing holds it: the kept load, unless its files were rewritten since it was read.
    if (kept != s_SharedKept.end()) {
        if (kept->second.Stamps == stamps) {
            kept->second.LastUse = ++s_SharedKeptClock;
            s_Loaded[key] = kept->second.Db;
            return kept->second.Db;
        }
        s_SharedKept.erase(kept);
    }
    bool measured = false;
    std::shared_ptr<SharedMotionDatabase> shared = TakePrefetch(base, rig, measured); // read ahead of Play (Prefetch)
    if (!shared) {
        shared = std::make_shared<SharedMotionDatabase>();
        if (!shared->Db.Load(base, error)) return nullptr;
    }
    if (!measured) Measure(*shared, base, rig ? ShapeOf(*rig).get() : nullptr);
    s_Loaded[key] = shared;
    keep(shared);
    return shared;
}

std::vector<std::string> SharedMotionDatabase::WatchedFiles(const std::string& base, const std::string& rigPath) {
    std::vector<std::string> out{base + ".mmanim", base + ".mmdb"};
    if (!rigPath.empty()) {
        out.push_back(rigPath);
        out.push_back(rigPath + ".meta"); // its import settings (scale, axes) change the measured frames, not the model file
    }
    return out;
}

void SharedMotionDatabase::ReleaseKept() {
    std::lock_guard<std::mutex> lock(s_SharedLock);
    s_SharedKept.clear();
    std::lock_guard<std::mutex> prefetch(s_PrefetchLock);
    DropPrefetchLocked();
}

// --- the body's motion -----------------------------------------------------------------------------------------------

float FirstPersonBodyMotion::StepCharacterFacing(float yaw, float clipTurn, float simYaw, const Settings& cfg, float dt) {
    yaw += clipTurn;
    yaw += Wrap(simYaw - yaw) * Toward(dt, cfg.CharacterFacingHalflife);
    const float off = Wrap(yaw - simYaw);
    if (std::fabs(off) > cfg.MaxFacingOffset) yaw = simYaw + std::copysign(cfg.MaxFacingOffset, off);
    return Wrap(yaw);
}

float FirstPersonBodyMotion::HoldTurnSide(float delta, float& side, float hold) {
    const float deg = kPi / 180.0f;
    if (side != 0.0f && delta * side < 0.0f && std::fabs(delta) > kPi - hold) delta = side * (kPi - deg); // just short of behind
    side = std::fabs(delta) > deg ? std::copysign(1.0f, delta) : 0.0f;
    return delta;
}

float FirstPersonBodyMotion::RootSpeed(const MotionDatabase& db, std::uint32_t frame) {
    const auto& c = db.Clip(db.ClipOf(frame));
    const std::uint32_t a = std::max(c.First, frame >= 5 ? frame - 5 : 0u), b = std::min(c.First + c.Count - 1, frame + 5);
    if (b <= a) return 0.0f;
    glm::vec3 d = db.Stream().Root(b).Position - db.Stream().Root(a).Position;
    d.y = 0.0f;
    return glm::length(d) * db.Fps() / (float)(b - a);
}


float FirstPersonBodyMotion::RootYawRate(const MotionDatabase& db, std::uint32_t frame) {
    const auto& c = db.Clip(db.ClipOf(frame));
    const std::uint32_t a = std::max(c.First, frame >= 5 ? frame - 5 : 0u), b = std::min(c.First + c.Count - 1, frame + 5);
    if (b <= a) return 0.0f;
    return Wrap(db.Stream().Root(b).Yaw - db.Stream().Root(a).Yaw) * db.Fps() / (float)(b - a);
}

std::string FirstPersonBodyMotion::PlayerDatabase() { return ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny"); }

bool FirstPersonBodyMotion::Start(const std::vector<std::shared_ptr<Model>>& models, size_t driver, std::string& error,
                                  const FirstPersonBodyMotion* carry) {
    Stop();
    std::string err;
    const std::uint64_t generation = SharedMotionDatabase::Generation(); // before the load: a rebake during it reads stale
    auto shared = SharedMotionDatabase::Get(PlayerDatabase(), &err,
                                            driver < models.size() ? models[driver].get() : nullptr);
    if (!shared) { error = "the motion matching database isn't baked (" + err + ")"; return false; }
    const MotionClipStream& stream = shared->Db.Stream();
    // The driver's pelvis height sets the translation scale; every piece shares it (one skeleton).
    float pelvisScale = 1.0f;
    if (driver < models.size() && models[driver]) {
        std::vector<LocalTRS> bind;
        models[driver]->BindLocalPose(bind);
        std::vector<glm::mat4> g;
        MotionBake::RootSpaceGlobals(*models[driver], bind, -1, g);
        const int p = models[driver]->NodeIndex("pelvis");
        const auto b = stream.Bind(*models[driver], p >= 0 ? g[(size_t)p][3].y : 0.0f);
        if (b.Pelvis < 0 || b.Bound < (int)stream.Bones().size() * 9 / 10) {
            error = "the body's driver doesn't have the stream's bones (" + std::to_string(b.Bound) + " of " +
                    std::to_string(stream.Bones().size()) + " bound)";
            return false;
        }
        pelvisScale = b.PelvisScale;
    }
    m_Bind.clear();
    m_BindPose.clear();
    for (const auto& m : models) {
        MotionClipStream::Binding b;
        std::vector<LocalTRS> bind;
        if (m) {
            m->BindLocalPose(bind);
            b = stream.Bind(*m, 0.0f);
            b.PelvisScale = pelvisScale;
        }
        m_Bind.push_back(std::move(b));
        m_BindPose.push_back(std::move(bind));
    }
    m_Shared = shared;
    m_Generation = generation;
    // (Another driver rig is another cached instance - its own excluded stretches - of the same frames: the frame numbers hold.)
    const bool carried = carry && carry != this && carry->m_Shared &&
                         (carry->m_Shared == shared || (carry->m_Shared->Db.FrameCount() == shared->Db.FrameCount() &&
                                                         carry->m_Shared->StartFrame == shared->StartFrame));
    if (carried) {
        m_Matcher = carry->m_Matcher;
        m_Matcher.Rebind(&shared->Db);
        m_Predictor = carry->m_Predictor;
        m_DesiredYaw = carry->m_DesiredYaw;
        m_YawRate = carry->m_YawRate;
        m_SimYaw = carry->m_SimYaw;
        m_SimYawRate = carry->m_SimYawRate;
        m_TurnSide = carry->m_TurnSide;
        m_HaveYaw = carry->m_HaveYaw;
        m_Still = carry->m_Still;
        m_Turning = carry->m_Turning;
        m_Air = carry->m_Air;
        m_AirTime = carry->m_AirTime;
        m_LandTimer = carry->m_LandTimer;
        m_AirRate = carry->m_AirRate;
        m_AirLast = carry->m_AirLast;
        m_WarpRate = carry->m_WarpRate;
        m_AnimSpeed = carry->m_AnimSpeed;
        m_CapsuleSpeed = carry->m_CapsuleSpeed;
        m_LastOut = carry->m_LastOut;
    } else {
        m_Matcher.Reset(&m_Shared->Db, m_Shared->StartFrame);
    }
    // Experiment knobs (MmTune; keys as named here).
    {
        Settings& c = Config;
        c.MinWarp = MmTune("MinWarp", c.MinWarp);
        c.MaxWarp = MmTune("MaxWarp", c.MaxWarp);
        c.SimFacingHalflife = MmTune("SimFacingHalflife", c.SimFacingHalflife);
        c.CharacterFacingHalflife = MmTune("CharacterFacingHalflife", c.CharacterFacingHalflife);
        c.SprintCharacterFacingHalflife = MmTune("SprintCharacterFacingHalflife", c.SprintCharacterFacingHalflife);
        c.SprintTrajPosWeight = MmTune("SprintTrajPosWeight", c.SprintTrajPosWeight);
        c.MaxFacingOffset = MmTune("MaxFacingOffset", c.MaxFacingOffset);
        c.TransitionCost = MmTune("TransitionCost", c.TransitionCost);
        c.MinRunway = (int)MmTune("MinRunway", (float)c.MinRunway);
        c.ForcedPoseWeight = MmTune("ForcedPoseWeight", c.ForcedPoseWeight);
        auto& ip = m_Matcher.Inertializer.Params;
        ip.BlendTime = MmTune("BlendTime", ip.BlendTime);
        ip.DecayHalflife = MmTune("DecayHalflife", ip.DecayHalflife);
        c.IdleEntryBlendTime = MmTune("IdleEntryBlendTime", c.IdleEntryBlendTime);
        c.CrouchBlendTime = MmTune("CrouchBlendTime", c.CrouchBlendTime);
        static const char* const kW[MotionFeature::GroupCount] = {"W.FootPos", "W.FootVel", "W.PelvisVel", "W.TrajPos", "W.TrajDir"};
        const MotionSearchParams defaults;
        for (int g = 0; g < MotionFeature::GroupCount; ++g) m_GroupWeight[g] = MmTune(kW[g], defaults.GroupWeight[g]);
    }
    if (!carried) {
        m_HaveYaw = false;
        m_Air = Air::None;
        m_WarpRate = 1.0f;
    }
    if (m_Debug) *m_Debug = MmPlayerDebug::Snapshot{};
    return true;
}

void FirstPersonBodyMotion::Stop() {
    m_Shared.reset();
    m_Bind.clear();
    m_BindPose.clear();
    m_Lines.clear();
    m_Matcher = MotionMatcher{};
    if (m_Debug) *m_Debug = MmPlayerDebug::Snapshot{};
}

void FirstPersonBodyMotion::EnterAir(const Input& in, const MotionMatcher::Input& query) {
    const MotionDatabase& db = m_Shared->Db;
    MotionSearchParams p;
    p.Require = MotionLabel::Airborne;
    p.Forbid = MotionLabel::Crouch;
    float traj[12], q[MotionDatabase::kDims];
    MotionMatcher::TrajectoryFeatures(query, db.Stream().RestYaw, traj);
    const auto pose = db.Raw(m_Matcher.Frame());
    MotionSearch::BuildQuery(db, pose.data(), traj, q);
    const MotionSearchResult r = MotionSearch::Search(db, q, p, -1);
    m_AirRate = 1.0f;
    // No jump in the database: keep matching as on the ground. (Not Rising: the landing would then force the matcher to
    // m_AirLast, a frame left over from an earlier jump - or frame 0.)
    if (r.Frame < 0) { m_Air = Air::None; return; }
    m_Air = Air::Rising;
    const auto& clip = db.Clip(db.ClipOf((std::uint32_t)r.Frame));
    const std::uint32_t last = clip.First + clip.Count - 1;
    std::uint32_t first = (std::uint32_t)r.Frame, end = (std::uint32_t)r.Frame;
    while (first > clip.First && (db.Flags(first - 1) & MotionLabel::Airborne)) --first;
    while (end < last && (db.Flags(end + 1) & MotionLabel::Airborne)) ++end;
    const std::uint32_t takeoff = in.Jumped ? (std::uint32_t)std::lround(Config.TakeoffSeconds * db.Fps()) : 0u;
    const std::uint32_t from = first > clip.First + takeoff ? first - takeoff : clip.First;
    // Time-warped to the flight: a jump's (up and down at JumpSpeed) or, falling, half speed until the landing.
    const float flight = in.Jumped && in.Gravity < 0.0f ? 2.0f * in.JumpSpeed / -in.Gravity : 0.0f;
    const float segment = (float)(end - from) / db.Fps();
    m_AirRate = flight > 0.0f ? std::clamp(segment / flight, 0.35f, 1.0f) : 0.5f;
    m_AirLast = end;
    m_Matcher.Locked = true;
    m_Matcher.HoldFrame = end;
    m_Matcher.ForceFrame(from);
}

void FirstPersonBodyMotion::Tick(const Input& in, float& yaw) {
    m_Lines.clear();
    if (!m_Shared || !(in.Dt > 0.0f)) return;
    const MotionDatabase& db = m_Shared->Db;
    const float dt = in.Dt;
    const float prevYaw = yaw;
    glm::vec3 wish(in.Wish.x, 0.0f, in.Wish.z);
    const float wishSpeed = glm::length(wish);
    const glm::vec3 vel(in.Velocity.x, 0.0f, in.Velocity.z);
    m_CapsuleSpeed = glm::length(vel);

    // The facing wanted: the view's while moving; standing still the body holds its facing until the view is Turn
    // Threshold off it, then turns to it (and stops Turn End Angle short).
    m_Still = wishSpeed < 0.1f;
    const float twist = Wrap(in.ViewYaw - yaw);
    if (!m_HaveYaw) {
        m_DesiredYaw = m_SimYaw = yaw;
        m_SimYawRate = 0.0f;
        m_HaveYaw = true;
    }
    if (!m_Still || in.TurnThreshold <= 0.0f) {
        m_DesiredYaw = in.ViewYaw;
        m_Turning = false;
    } else {
        if (!m_Turning && std::fabs(twist) > in.TurnThreshold) m_Turning = true;
        if (m_Turning) {
            m_DesiredYaw = in.ViewYaw;
            if (std::fabs(twist) < std::max(in.TurnEndAngle, 2.0f * kPi / 180.0f)) m_Turning = false;
        }
    }

    // The filters: crouch (the pack's crouch takes), and what is asked for - standing still the idle gait, moving the style
    // (in every direction: the capsule moves at the style's own speed, strafes at its strafe speed - Player::GaitSpeeds).
    const bool crouch = in.Crouched;
    const int style = crouch ? (int)MotionLabel::StyleCrouch : std::clamp(in.Style, (int)MotionLabel::StyleStroll, (int)MotionLabel::StyleSprint);
    const int gait = m_Still ? (int)MotionLabel::Idle : -1;
    MotionSearchParams params;
    for (int g = 0; g < MotionFeature::GroupCount; ++g) params.GroupWeight[g] = m_GroupWeight[g];
    params.DesiredGait = gait;
    params.DesiredStyle = m_Still ? -1 : style;
    params.TransitionCost = Config.TransitionCost;
    // No dead ends: crouched, the jumps' deep landings (a few frames of Crouch before the actor stands) won the search,
    // played into a standing frame at once and forced another search - every frame (the crouch legs, 30-50 jumps a second).
    params.MinRunway = Config.MinRunway;
    // Strafing (the move over 50 degrees off the view): where the root goes outweighs the pose - the pack's strafes are
    // few (crouched, 8 legs), and at the default weights diagonal walks won them.
    if (wishSpeed > 0.1f && std::fabs(Wrap(MotionMath::YawOf(wish) - in.ViewYaw)) > 50.0f * kPi / 180.0f)
        params.GroupWeight[MotionFeature::TrajPos] *= 2.0f;
    else if (wishSpeed > 0.1f && style == (int)MotionLabel::StyleSprint) // (Settings::SprintTrajPosWeight)
        params.GroupWeight[MotionFeature::TrajPos] *= Config.SprintTrajPosWeight;
    if (crouch) params.Require = MotionLabel::Crouch;
    else params.Forbid = MotionLabel::Airborne | MotionLabel::Crouch;

    // The simulation facing: a critically damped spring toward the facing wanted - except standing still and not turning,
    // where it is the character's (data-driven: an idle take turns the body as captured, and the trajectory asks for that).
    // (With no Turn Threshold the body always faces the view, standing too.)
    if (m_Still && !m_Turning && in.TurnThreshold > 0.0f && m_Air == Air::None) {
        m_SimYaw = yaw;
        m_SimYawRate = m_YawRate;
        m_DesiredYaw = yaw;
        m_TurnSide = 0.0f;
    } else {
        // Held on the side it is turning through straight behind (a 180 flick: armed, the body turned 9 deg one way, then
        // - the view crossing behind it - back the other, its turn rate -116 to +98 deg/s in a frame). The wanted facing
        // steered by goes to the trajectory too, so the prediction turns the same way.
        const float delta = HoldTurnSide(Wrap(m_DesiredYaw - m_SimYaw), m_TurnSide, kTurnSideHold);
        m_DesiredYaw = Wrap(m_SimYaw + delta);
        const float goal = m_SimYaw + delta;
        TrajectoryPredictor::SpringScalar(m_SimYaw, m_SimYawRate, goal, Config.SimFacingHalflife, dt);
        m_SimYaw = Wrap(m_SimYaw);
    }
    // The trajectory: the capsule as it is now, with the simulation facing.
    TrajectoryState now;
    now.Position = glm::vec3(in.Feet.x, in.Feet.y, in.Feet.z);
    now.Velocity = vel;
    now.Yaw = m_SimYaw;
    now.YawRate = m_SimYawRate;
    m_Predictor.Params.VelocityHalflife = std::max(in.VelocityHalflife, 0.02f);
    m_Predictor.Params.CapsuleLaw = in.CapsuleLaw;
    m_Predictor.Params.AccelTime = std::max(in.AccelTime, 0.0f);
    m_Predictor.Params.DecelTime = std::max(in.DecelTime, 0.0f);
    m_Predictor.Params.FrameDt = in.Dt;
    m_Predictor.Params.SnapToGoal = in.SnapToGoal;
    m_Predictor.Params.SnapToStop = in.SnapToStop;
    m_Predictor.Params.FacingHalflife = Config.SimFacingHalflife;
    m_Predictor.Sweep = in.Sweep;
    m_Predictor.SetState(now);
    const auto ts = db.TrajSeconds();
    TrajectoryState future[3];
    if (in.Path && in.PathCount > 0 && !m_Still) {
        TrajectoryPredictor::PathGoal path;
        path.Points = in.Path;
        path.Count = in.PathCount;
        path.Speed = wishSpeed;
        path.ArriveTime = in.ArriveTime;
        m_Predictor.PredictAlong(path, m_DesiredYaw, ts.data(), 3, future);
    } else {
        m_Predictor.Predict(wish, m_DesiredYaw, ts.data(), 3, future);
    }
    // The query in the character's frame (where the playing frame's own features are), its trajectory the simulation's.
    MotionMatcher::Input q;
    q.Position = now.Position;
    q.Yaw = yaw;
    for (int k = 0; k < 3; ++k) {
        q.FuturePosition[(size_t)k] = future[k].Position;
        q.FutureYaw[(size_t)k] = future[k].Yaw;
    }
    // What the search schedule watches for an input change: the move as the player asks it, in the view's frame while
    // moving (the heading follows the view, so a mouse sweep alone changes nothing the query is made of - in world space
    // every sweep forced searches: 6-8 jumps a second looking about), the facing wanted standing still (a turn on the spot).
    // (Its speed the gait's own: the run slowing in a turn isn't a new input - looking about at a run, every sweep's slowdown
    // forced searches.)
    const glm::vec3 nominal = wishSpeed > 0.1f ? wish / wishSpeed * m_Shared->Gaits.StyleSpeed(style) : glm::vec3(0.0f);
    // (The trajectory itself keeps the real speed: the predictor's goal is `wish`.)
    q.DesiredVelocity = m_Still ? wish : glm::angleAxis(-in.ViewYaw, glm::vec3(0.0f, 1.0f, 0.0f)) * nominal;
    q.DesiredYaw = m_Still ? m_DesiredYaw : 0.0f;

    // Airborne: the label-locked segment.
    m_AirTime = in.Grounded ? 0.0f : m_AirTime + dt;
    if ((m_Air == Air::None || m_Air == Air::Landing) && (in.Jumped || (!in.Grounded && m_AirTime > Config.AirDelay))) {
        EnterAir(in, q);
    } else if ((m_Air == Air::Rising || m_Air == Air::Holding) && in.Grounded && !in.Jumped) {
        // Landed: the take's own landing plays a moment, locked, then matching takes over (dead-blended). Not on the move:
        // the pack's jumps land in place, and held there under a running capsule the planted feet slid ~25 cm.
        m_Air = Air::Landing;
        m_LandTimer = m_CapsuleSpeed > 1.0f ? 0.0f : Config.LandSeconds;
        const auto& clip = db.Clip(db.ClipOf(m_Matcher.Frame()));
        const std::uint32_t last = clip.First + clip.Count - 1;
        m_Matcher.HoldFrame = std::min<std::int64_t>(last, (std::int64_t)m_AirLast + (std::int64_t)std::lround(Config.LandSeconds * db.Fps()));
        if (m_LandTimer > 0.0f && m_Matcher.Frame() < m_AirLast) m_Matcher.ForceFrame(m_AirLast);
    }
    if (m_Air == Air::Rising && m_Matcher.Frame() >= m_AirLast) m_Air = Air::Holding;
    if (m_Air == Air::Landing) {
        m_LandTimer -= dt;
        if (m_LandTimer <= 0.0f) {
            m_Air = Air::None;
            m_Matcher.Locked = false;
            m_Matcher.HoldFrame = -1;
        }
    }


    // Speed warping: the matched frame plays at the capsule's pace (within MinWarp..MaxWarp).
    m_AnimSpeed = RootSpeed(db, m_Matcher.Frame());
    float rate = 1.0f;
    if (m_AnimSpeed > 0.3f && m_CapsuleSpeed > 0.3f) rate = std::clamp(m_CapsuleSpeed / m_AnimSpeed, Config.MinWarp, Config.MaxWarp);
    m_WarpRate += (rate - m_WarpRate) * Toward(dt, 0.08f);
    m_Matcher.SearchParams = params;
    m_Matcher.ForcedPoseWeight = Config.ForcedPoseWeight;
    m_Matcher.IdleEntryBlendTime = Config.IdleEntryBlendTime;
    m_Matcher.CrouchToggleBlendTime = Config.CrouchBlendTime;
    // Standing idle: the playing take plays on while its continuation fits (MotionMatcher::Idle). Only once the body has
    // truly stopped on an idle frame: as the stick is released the capsule is still slowing, and the stop is searched.
    m_Matcher.Idle = m_Still && !m_Turning && m_Air == Air::None && in.Grounded && m_CapsuleSpeed < Config.IdleSpeed &&
                     db.Gait(m_Matcher.Frame()) == MotionLabel::Idle;
    m_Matcher.IdleTravel = Config.IdleTravel;
    m_Matcher.IdleTurn = Config.IdleTurn;
    // ... and an idle candidate must be able to play on (IdleRunway frames before its labels change): the stillest standing in
    // the database is a jump take's stance before its dip - matched, it played 2.2 s into the dip (refused standing), snapped
    // back and played it again.
    // And only the idle takes (MotionLabel::IdleTake): over the whole database the stillest second is that stance, every time.
    if (m_Matcher.Idle) {
        m_Matcher.SearchParams.MinRunway = std::max(m_Matcher.SearchParams.MinRunway, Config.IdleRunway);
        m_Matcher.SearchParams.MinIdleRunway = Config.IdleRunway; // ... and it must stay Idle gait that long (not into a step)
        m_Matcher.SearchParams.Require |= MotionLabel::IdleTake;
    }
    m_Matcher.PlayRate = m_Air == Air::Rising ? m_AirRate : m_Air == Air::None ? m_WarpRate : 1.0f;
    m_Matcher.Update(dt, q);

    // The character facing: the matched clip's own root turn, then pulled toward the simulation facing and clamped off it.
    // (Before, moving, the heading followed the view on a 0.03 s spring with none of the clip's turn, the query built from
    // it: a flick spun the body over planted feet - a 51 cm skate - and needed a 450 deg/s cap and foot-lock resets.)
    const float clipTurn = RootYawRate(db, m_Matcher.Frame()) * m_Matcher.PlayRate * dt;
    if (db.Style(m_Matcher.Frame()) == MotionLabel::StyleSprint) { // (steered: Settings::SprintCharacterFacingHalflife)
        Settings steered = Config;
        steered.CharacterFacingHalflife = Config.SprintCharacterFacingHalflife;
        yaw = StepCharacterFacing(yaw, clipTurn, m_SimYaw, steered, dt);
    } else {
        yaw = StepCharacterFacing(yaw, clipTurn, m_SimYaw, Config, dt);
    }
    m_YawRate = Wrap(yaw - prevYaw) / dt;

    // Diagnostics (none kept: SetDebug's null).
    if (!m_Debug) return;
    MmPlayerDebug::Snapshot& d = *m_Debug;
    const MotionMatcher& m = m_Matcher;
    d.Valid = true;
    d.Frame = m.Frame();
    d.Clip = db.Clip(db.ClipOf(m.Frame())).Name;
    d.Flags = db.Flags(m.Frame());
    d.Gait = db.Gait(m.Frame());
    d.DesiredGait = gait;
    d.DesiredStyle = params.DesiredStyle;
    d.Style = db.Style(m.Frame());
    d.Crouch = crouch;
    d.Sprint = in.Sprint;
    d.Airborne = m_Air != Air::None;
    d.Locked = m.Locked;
    d.Idle = m.Idle;
    d.IdleRefits = m.IdleRefits;
    d.ForcedByExcluded = m.ForcedByExcluded;
    d.RelaxedAvoid = m.RelaxedAvoid;
    d.RelaxedRunway = m.RelaxedRunway;
    d.RelaxedStyle = m.RelaxedStyle;
    d.SearchFailed = m.SearchFailed;
    d.IdleGestureCuts = m_Shared->IdleGestureCuts;
    d.Still = m_Still;
    d.Turning = m_Turning;
    d.Searched = m.SearchedThisUpdate();
    d.Jumped = m.JumpedThisUpdate();
    d.Searches = m.Searches;
    d.Jumps = m.Jumps;
    d.ForcedByInput = m.Schedule.ForcedByInput;
    d.ForcedByEnd = m.Schedule.ForcedByEnd;
    d.SearchesBy = m.SearchesBy;
    d.JumpsBy = m.JumpsBy;
    d.LastCost = m.LastResult().Cost;
    d.PlayingCost = m.LastResult().CurrentCost;
    d.SearchMsAvg = m.Searches > 0 ? m.SearchMsTotal / m.Searches : 0.0;
    d.SearchMsMax = m.SearchMsMax;
    d.PlayRate = m.PlayRate;
    d.AnimSpeed = m_AnimSpeed;
    d.CapsuleSpeed = m_CapsuleSpeed;
    d.Yaw = yaw;
    d.DesiredYaw = m_DesiredYaw;
    d.YawRate = m_YawRate;
    d.SimYaw = m_SimYaw;
    d.FacingOffset = Wrap(yaw - m_SimYaw);
    d.ClipTurn = clipTurn;
    d.TurnInPlaceRate = m_Shared->TurnInPlaceRate;
    {
        const auto raw = db.Raw(m.Frame()); // where the matched frame's root is 1 s on, in its own root space (+Z ahead)
        const float x = raw[15 + 4], z = raw[15 + 5];
        d.MatchedTravel = std::hypot(x, z);
        d.MatchedTravelDeg = d.MatchedTravel > 1e-4f ? std::fabs(std::atan2(x, z)) * 180.0f / kPi : 0.0f;
        const auto& clip = db.Clip(db.ClipOf(m.Frame()));
        const std::uint32_t half = (std::uint32_t)std::lround(0.5f * db.Fps());
        const std::uint32_t a = std::max(clip.First, m.Frame() >= half ? m.Frame() - half : 0u);
        const std::uint32_t b = std::min(clip.First + clip.Count - 1, m.Frame() + half);
        d.MatchedAccel = b > a ? (RootSpeed(db, b) - RootSpeed(db, a)) * db.Fps() / (float)(b - a) : 0.0f;
    }
    d.PoseStepDeg = 0.0f;
    for (size_t i = 0; i < m.Pose().Rot.size() && i < m_LastOut.Rot.size(); ++i) {
        const float a = MotionMath::QuatAngle(m.Pose().Rot[i], m_LastOut.Rot[i]) * 180.0f / kPi;
        if (a > d.PoseStepDeg) {
            d.PoseStepDeg = a;
            d.PoseStepBone = db.Stream().Bones()[i];
        }
    }
    m_LastOut = m.Pose();
    d.RootOffset = 0.0f; // the body is placed at the capsule's feet (FirstPersonBody): the root is clamped there
    if (m.SearchedThisUpdate()) {
        const auto raw = db.Raw(m.Frame());
        for (int k = 0; k < 27; ++k) {
            d.Query[k] = m.LastQueryRaw()[k];
            d.Matched[k] = raw[(size_t)k];
        }
        const float* a = m.LastQueryRaw() + 15;
        d.TrajErr = std::hypot(a[4] - raw[15 + 4], a[5] - raw[15 + 5]);
    }

    // Debug draw.
    const float rest = db.Stream().RestYaw;
    MotionDebugDraw::Trajectory(m_Lines, now.Position, yaw, rest, m.LastQueryRaw() + 15, glm::vec4(1.0f, 0.85f, 0.1f, 1.0f), 0.04f);
    const auto matched = db.Raw(m.Frame());
    MotionDebugDraw::Trajectory(m_Lines, now.Position, yaw, rest, matched.data() + 15, glm::vec4(1.0f, 0.2f, 0.9f, 1.0f), 0.06f);
    MotionDebugDraw::Character(m_Lines, now.Position, yaw, glm::vec4(0.2f, 1.0f, 0.3f, 1.0f));
    MotionDebugDraw::Feet(m_Lines, now.Position, yaw, rest, matched.data(), d.Flags);
    if (m.JumpedThisUpdate())
        MotionDebugDraw::Line(m_Lines, now.Position + glm::vec3(0, 2.0f, 0), now.Position + glm::vec3(0, 2.3f, 0), glm::vec4(1, 0.2f, 0.9f, 1));
    d.Lines = m_Lines;
}

void FirstPersonBodyMotion::Pose(const std::vector<std::shared_ptr<Model>>& models) {
    if (!m_Shared) return;
    const MotionClipStream& stream = m_Shared->Db.Stream();
    for (size_t k = 0; k < models.size() && k < m_Bind.size(); ++k) {
        if (!models[k] || m_BindPose[k].empty() || (int)m_BindPose[k].size() != models[k]->NodeCount()) continue;
        m_Scratch = m_BindPose[k];
        MotionMatcher::WritePose(m_Matcher.Pose(), stream, m_Bind[k], m_Scratch);
        models[k]->ApplyLocalPose(m_Scratch);
    }
}

namespace MmPlayerDebug {
bool& Wanted() {
    static bool wanted = false;
    return wanted;
}

Snapshot& Info() {
    static Snapshot s;
    return s;
}
} // namespace MmPlayerDebug
