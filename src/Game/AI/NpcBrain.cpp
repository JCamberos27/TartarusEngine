#include "NpcBrain.h"

#include "FirstPersonPresentation.h"
#include "NpcDirector.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int kBehaviours = kBehaviourCount;
constexpr float kInertia = 0.12f;

float Flat(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); }

glm::vec3 FlatDir(const glm::vec3& from, const glm::vec3& to) {
    glm::vec3 d(to.x - from.x, 0.0f, to.z - from.z);
    const float l = glm::length(d);
    return l > 1e-4f ? d / l : glm::vec3(0.0f, 0.0f, 1.0f);
}

float Rand01(NpcDirector& d);

} // namespace

// The director's RNG and internals are the brain's to use (friend).
namespace {
std::mt19937* g_Rng = nullptr;
float Rand01(NpcDirector&) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(*g_Rng); }
} // namespace

int NpcBrain::FindCover(NpcDirector& d, Npc& n, CoverGoal goal, const glm::vec3& threat) {
    CoverSystem& cover = d.m_Cover;
    if (cover.Points().empty()) return -1;
    d.m_CoverSearchFrame = d.m_Frame;
    const glm::vec3 threatEye = threat + glm::vec3(0.0f, 1.6f, 0.0f);
    const float current = Flat(n.Feet, threat);
    // Where the rest of the squad is (the flank goes wide of it).
    glm::vec3 centroid = n.Feet;
    int count = 1;
    for (const auto& o : d.m_Npcs)
        if (o && !o->Dead && o.get() != &n && o->Squad == n.Squad) { centroid += o->Feet; ++count; }
    centroid /= (float)count;
    const glm::vec3 squadDir = FlatDir(threat, centroid);

    static thread_local std::vector<int> near;
    cover.Query(n.Feet, goal == CoverGoal::Flank ? 34.0f : 28.0f, near);
    struct Cand { int Index; float Score; };
    static thread_local std::vector<Cand> cands;
    cands.clear();
    const bool shotgun = n.Class == WeaponClass::Shotgun;
    for (int i : near) {
        const CoverPoint& c = cover.Points()[(size_t)i];
        if (c.ClaimedBy >= 0 && c.ClaimedBy != n.Index) continue;
        // Not on top of a squadmate (or their cover).
        bool crowded = false;
        for (const auto& o : d.m_Npcs) {
            if (!o || o->Dead || o.get() == &n) continue;
            if (Flat(o->Feet, c.Pos) < 2.2f) { crowded = true; break; }
            if (o->Cover >= 0 && Flat(cover.Points()[(size_t)o->Cover].Pos, c.Pos) < 3.0f) { crowded = true; break; }
        }
        if (crowded) continue;
        const glm::vec3 toThreat = FlatDir(c.Pos, threat);
        const float facing = glm::dot(c.Normal, toThreat);
        if (facing < 0.35f) continue; // the cover isn't between it and the threat
        const float dist = Flat(c.Pos, threat);
        if (dist < 4.0f) continue;
        const float travel = Flat(n.Feet, c.Pos);
        float s[5];
        // Fighting range: a rifle wants 14-30 m, a shotgun 6-12 m.
        const float pref = shotgun ? 9.0f : 21.0f, width = shotgun ? 5.0f : 11.0f;
        const float rd = (dist - pref) / width;
        s[0] = std::exp(-rd * rd) * 0.8f + 0.2f;
        s[1] = std::clamp(1.0f - travel / 32.0f, 0.05f, 1.0f);
        s[2] = 0.5f + 0.5f * facing;
        s[3] = d.m_Now - c.LastUsed < 6.0f ? 0.6f : 1.0f;
        s[4] = c.High && !c.Peek[0] && !c.Peek[1] ? 0.55f : 1.0f;
        float score = CombineScores(s, 5) * DangerScale(c.Pos, d.m_DeathPos, d.m_DeathTime, d.m_DeathCount, d.m_Now);
        switch (goal) {
        case CoverGoal::Fight:
            break;
        case CoverGoal::Flank: {
            // Well round the side from where the squad presses.
            const float angle = glm::degrees(std::acos(std::clamp(glm::dot(FlatDir(threat, c.Pos), squadDir), -1.0f, 1.0f)));
            if (angle < 45.0f || dist > current + 4.0f) continue;
            score *= 0.4f + 0.6f * std::clamp((angle - 45.0f) / 45.0f, 0.0f, 1.0f);
            break;
        }
        case CoverGoal::Push:
            if (dist > current - 3.5f) continue;
            score *= 0.6f + 0.4f * std::clamp((current - dist) / 10.0f, 0.0f, 1.0f);
            break;
        case CoverGoal::Retreat:
            if (dist < current + 3.0f) continue;
            score *= s[1];
            break;
        }
        cands.push_back({i, score});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.Score > b.Score; });
    // The expensive checks on the best few: it really hides from the threat, it can shoot from it, and
    // it can be walked to without a long detour.
    int tested = 0;
    for (const Cand& cd : cands) {
        if (++tested > 6) break;
        const CoverPoint& c = cover.Points()[(size_t)cd.Index];
        const float hideHeight = c.High ? 1.55f : 0.95f;
        if (!CoverSystem::Shielded(c.Pos, hideHeight, threatEye)) continue;
        if (goal != CoverGoal::Retreat) {
            bool canShoot = false;
            if (!c.High) canShoot = !CoverSystem::Shielded(c.Pos, 1.55f, threatEye);
            else
                for (int s = 0; s < 2 && !canShoot; ++s)
                    canShoot = c.Peek[s] && !CoverSystem::Shielded(c.PeekPos[s], 1.55f, threatEye);
            if (!canShoot) continue;
        }
        const float path = d.m_Nav.PathLength(n.Feet, c.Pos);
        if (path < 0.0f || path > Flat(n.Feet, c.Pos) * 1.8f + 5.0f) continue;
        cover.Claim(cd.Index, n.Index, d.m_Now);
        n.Cover = cd.Index;
        n.CoverGood = true;
        n.CoverCheckAt = d.m_Now;
        return cd.Index;
    }
    return -1;
}

void NpcBrain::Think(NpcDirector& d, World& world, Npc& n, const PlayerSnapshot& p, float dt) {
    g_Rng = &d.m_Rng;
    if (d.m_Now >= n.NextThink) {
        n.NextThink = d.m_Now + 0.25f + 0.08f * Rand01(d);
        Choose(d, n, p);
    }
    Run(d, world, n, p, dt);
}

void NpcBrain::Choose(NpcDirector& d, Npc& n, const PlayerSnapshot& p) {
    (void)p;
    const float now = d.m_Now;
    // Down and bleeding: nothing to decide, it crawls for cover and shoots from it till it dies.
    if (n.Wounded) {
        if (n.Doing != Behaviour::Wounded || n.Phase < 0) Enter(d, n, (int)Behaviour::Wounded, p);
        return;
    }
    const TargetMemory& m = n.Mem;
    const float since = now - m.LastSeen;
    const float health = n.Health / std::max(n.MaxHealth, 1.0f);
    const bool underFire = now - n.LastHurt < 2.5f || n.Suppression > 0.45f;
    const glm::vec3 threat = m.Known ? m.Predicted(now) : m.LastKnown;
    const float dist = Flat(n.Feet, threat);
    const bool shotgun = n.Class == WeaponClass::Shotgun;
    bool inCover = false;
    if (n.Cover >= 0) {
        const CoverPoint& c = d.m_Cover.Points()[(size_t)n.Cover];
        inCover = Flat(n.Feet, c.Pos) < 0.9f || (n.Doing == Behaviour::CoverFight && n.Phase == 1);
        if (now - n.CoverCheckAt > 0.5f && m.Known) {
            n.CoverCheckAt = now;
            n.CoverGood = CoverSystem::Shielded(c.Pos, c.High ? 1.55f : 0.95f, threat + glm::vec3(0.0f, 1.6f, 0.0f));
        }
    }

    float s[kBehaviours] = {};
    auto at = [&](Behaviour b) -> float& { return s[(int)b]; };
    if (!m.Known) {
        at(Behaviour::Idle) = m.Awareness < 0.3f && now - m.LastHeard > 6.0f ? 0.35f : 0.05f;
        if (m.Awareness >= 0.3f || now - m.LastHeard < 8.0f) at(Behaviour::Investigate) = 0.5f + 0.4f * m.Awareness;
    } else {
        // Searching once the trail is cold; sooner for the bold.
        const float searchAfter = 7.0f + 6.0f * (1.0f - n.Morale) + (n.Role == NpcRole::Anchor ? 4.0f : 0.0f);
        if (!m.Visible && since > searchAfter) at(Behaviour::Search) = 0.45f + 0.4f * std::clamp((since - searchAfter) / 15.0f, 0.0f, 1.0f);
        if (!inCover && now >= n.NoCoverUntil) {
            float c = 0.45f + (m.Visible || underFire ? 0.35f : 0.0f);
            if (shotgun && dist < 9.0f && m.Visible) c *= 0.45f; // close in with a shotgun: fight
            at(Behaviour::TakeCover) = c;
        }
        if (inCover) at(Behaviour::CoverFight) = n.CoverGood ? 0.72f : 0.12f;
        const float ownSince = now - n.LastOwnSight;
        if (m.Visible || ownSince < 1.0f) {
            float e = 0.35f;
            if (shotgun && dist < 11.0f) e += 0.4f;
            if (now < n.NoCoverUntil) e += 0.25f;
            if (!m.Visible) e *= 0.6f;
            at(Behaviour::Engage) = e;
        }
        // Known but out of its own sight, with no cover to fight from: go and find a line on them.
        if (!m.Visible && ownSince > 3.0f && !inCover && now < n.NoCoverUntil)
            at(Behaviour::Search) = std::max(at(Behaviour::Search), 0.55f);
        if (n.HasFlankToken && since < 20.0f) at(Behaviour::Flank) = 0.66f + (d.m_PlayerUnseen > 3.0f ? 0.12f : 0.0f);
        if (n.HasPushToken) at(Behaviour::Push) = 0.82f;
        else if (shotgun && dist > 13.0f && health > 0.5f && since < 12.0f) at(Behaviour::Push) = 0.6f;
        if (health < 0.35f && underFire && !n.Retreated) at(Behaviour::Retreat) = 0.88f;
    }
    // A bad leg: no running round the side, no pushing up.
    if (now < n.LimpUntil) { at(Behaviour::Flank) = 0.0f; at(Behaviour::Push) = 0.0f; }
    // Commitment: what it's doing scores a little higher, unless that is finished (Phase -1).
    if (n.Phase >= 0 && n.Doing != Behaviour::Dead) {
        const float minCommit = n.Doing == Behaviour::TakeCover || n.Doing == Behaviour::Flank || n.Doing == Behaviour::Push ||
                                        n.Doing == Behaviour::Retreat ? 1.2f : 0.6f;
        float& cur = s[(int)n.Doing];
        if (cur > 0.0f) cur += kInertia;
        if (now - n.DoingSince < minCommit && cur > 0.0f && now - n.LastHurt > 0.2f) cur += 0.5f;
    } else if (n.Phase < 0) {
        s[(int)n.Doing] *= 0.2f;
    }
    int best = 0;
    for (int i = 1; i < kBehaviours; ++i) if (s[i] > s[best]) best = i;
    for (int i = 0; i < kBehaviours; ++i) n.Scores[i] = s[i];
    if ((Behaviour)best != n.Doing || n.Phase < 0) {
        const Behaviour b = (Behaviour)best;
        const bool needsCover = b == Behaviour::TakeCover || b == Behaviour::Flank || b == Behaviour::Push || b == Behaviour::Retreat;
        if (needsCover && d.m_CoverSearchFrame == d.m_Frame) {
            n.NextThink = now; // someone searched this frame already: decide again next frame
            return;
        }
        Enter(d, n, best, p);
    }
}

void NpcBrain::Enter(NpcDirector& d, Npc& n, int behaviour, const PlayerSnapshot& p) {
    (void)p;
    const Behaviour b = (Behaviour)behaviour;
    const float now = d.m_Now;
    const glm::vec3 threat = n.Mem.Known ? n.Mem.Predicted(now) : n.Mem.LastKnown;
    // Leaving a flank or a push hands the token back.
    if (n.Doing == Behaviour::Flank && b != Behaviour::Flank && n.Squad < (int)d.m_Squads.size()) {
        auto& sq = d.m_Squads[(size_t)n.Squad];
        if (sq.FlankHolder == n.Index) { sq.FlankHolder = -1; sq.FlankDoneAt = now; }
        n.HasFlankToken = false;
    }
    if (n.Doing == Behaviour::Push && b != Behaviour::Push && n.Squad < (int)d.m_Squads.size()) {
        auto& sq = d.m_Squads[(size_t)n.Squad];
        if (sq.PushHolder == n.Index) { sq.PushHolder = -1; sq.PushUntil = 0.0f; }
        n.HasPushToken = false;
    }
    const Behaviour prev = n.Doing;
    n.Doing = b;
    n.DoingSince = now;
    n.BoundWaitFrom = -1.0f;
    n.Phase = 0;
    n.PhaseStart = now;
    n.PhaseUntil = now;
    n.ShotsAtPhase = n.ShotsFired;
    n.SearchStep = 0;
    char why[96];
    std::snprintf(why, sizeof why, "%s (%.2f)", BehaviourName(b), n.Scores[behaviour]);
    n.Why = why;
    switch (b) {
    case Behaviour::TakeCover:
        if (FindCover(d, n, CoverGoal::Fight, threat) < 0) { n.Phase = -1; n.NoCoverUntil = now + 3.0f; }
        break;
    case Behaviour::Flank:
        if (FindCover(d, n, CoverGoal::Flank, threat) < 0) {
            n.Phase = -1;
            if (n.Squad < (int)d.m_Squads.size()) { d.m_Squads[(size_t)n.Squad].FlankHolder = -1; d.m_Squads[(size_t)n.Squad].FlankDoneAt = now; }
            n.HasFlankToken = false;
        } else {
            const CoverPoint& c = d.m_Cover.Points()[(size_t)n.Cover];
            const glm::vec3 right = glm::normalize(glm::cross(FlatDir(n.Feet, threat), glm::vec3(0, 1, 0)));
            d.Callout(n, glm::dot(c.Pos - n.Feet, right) > 0.0f ? Bark::FlankRight : Bark::FlankLeft);
            n.Body.Signal(c.Pos - n.Feet);
            n.BoundWaitFrom = now; // the order given, it goes once someone covers it
        }
        break;
    case Behaviour::Push:
        if (FindCover(d, n, CoverGoal::Push, threat) < 0) {
            // Nothing closer to hide behind: close in in the open if it's a shotgun, else hold.
            n.Cover = -1;
            d.m_Cover.Release(n.Index, now);
            n.Phase = n.Class == WeaponClass::Shotgun ? 2 : -1;
        }
        if (n.Phase >= 0) {
            d.Callout(n, Bark::MovingUp);
            n.Body.Signal((n.Cover >= 0 ? d.m_Cover.Points()[(size_t)n.Cover].Pos : threat) - n.Feet);
            if (n.Cover >= 0) n.BoundWaitFrom = now;
        }
        break;
    case Behaviour::Retreat:
        n.Retreated = true;
        if (FindCover(d, n, CoverGoal::Retreat, threat) < 0) n.Phase = -1;
        else d.Callout(n, Bark::FallingBack);
        break;
    case Behaviour::Investigate:
        d.Callout(n, Bark::Investigating);
        break;
    case Behaviour::CoverFight:
        n.PhaseUntil = now + 0.6f + 0.8f * Rand01(d);
        break;
    case Behaviour::Search:
        d.Callout(n, Bark::LostTarget);
        n.Goal = threat;
        break;
    case Behaviour::Idle:
        if (prev == Behaviour::Investigate || prev == Behaviour::Search) d.Callout(n, Bark::AllClear);
        d.m_Cover.Release(n.Index, now);
        n.Cover = -1;
        n.IdleUntil = now + 1.0f + 3.0f * Rand01(d);
        break;
    case Behaviour::Wounded: {
        // The nearest cover that stands between it and the threat, near enough to crawl to.
        d.m_Cover.Release(n.Index, now);
        n.Cover = -1;
        static thread_local std::vector<int> near;
        d.m_Cover.Query(n.Feet, 14.0f, near);
        float best = 1e9f;
        int pick = -1;
        // Cover that faces the threat first; failing that, any, the nearest.
        for (int pass = 0; pass < 2 && pick < 0; ++pass)
            for (int i : near) {
                const CoverPoint& c = d.m_Cover.Points()[(size_t)i];
                if (c.ClaimedBy >= 0 && c.ClaimedBy != n.Index) continue;
                if (pass == 0 && glm::dot(c.Normal, FlatDir(c.Pos, threat)) < 0.2f) continue;
                const float dist = Flat(n.Feet, c.Pos);
                if (dist > 1.0f && dist < best) { best = dist; pick = i; }
            }
        if (pick >= 0) {
            d.m_Cover.Claim(pick, n.Index, now);
            n.Cover = pick;
            n.CoverGood = true;
        }
        break;
    }
    default:
        break;
    }
}

void NpcBrain::Run(NpcDirector& d, World& world, Npc& n, const PlayerSnapshot& p, float dt) {
    (void)world;
    (void)dt;
    const float now = d.m_Now;
    NpcIntent& it = n.Intent;
    const TargetMemory& m = n.Mem;
    const glm::vec3 threat = m.Known ? m.Predicted(now) : m.LastKnown;
    const glm::vec3 threatChest = threat + glm::vec3(0.0f, 1.3f, 0.0f);
    // What to aim at: the player as seen, else where they were (pre-aimed at chest height).
    const glm::vec3 aimAt = m.Visible ? (n.VisiblePoints > 0 ? n.SeenPoint : p.Feet + glm::vec3(0.0f, p.Height * 0.7f, 0.0f)) : threatChest;
    const bool shotgun = n.Class == WeaponClass::Shotgun;
    const float ammo01 = n.Weapon && n.Weapon->IsActive() ? (float)n.Weapon->Ammo() / (float)std::max(1, n.Weapon->MagazineSize()) : 1.0f;
    const bool reloading = n.Reloading;
    const float arriveDist = 0.55f;

    const NpcIntent prev = it;
    it = NpcIntent{};
    it.LookPoint = m.Known || m.Awareness > 0.3f ? threatChest : prev.LookPoint;
    it.AimPoint = aimAt;

    auto moveTo = [&](const glm::vec3& goal, Gait pace) {
        it.Move = true;
        it.MoveTarget = goal;
        it.Pace = pace;
    };
    auto arrived = [&](const glm::vec3& goal) { return Flat(n.Feet, goal) < arriveDist; };
    // Fire and maneuver: a bound across ground the player can see waits - down, gun up, shooting if there's a shot - for a
    // squadmate's covering fire, though not for long if none comes (a push, called while the player reloads, hardly at all).
    // False while it waits.
    auto bound = [&](float maxWait) {
        if (n.BoundWaitFrom < 0.0f || n.Squad >= (int)d.m_Squads.size()) return true;
        auto& sq = d.m_Squads[(size_t)n.Squad];
        const bool exposed = m.Visible || now - n.LastOwnSight < 0.5f;
        const bool covered = now < sq.CoverFireUntil;
        if (MayBound(exposed, covered, now - n.BoundWaitFrom, maxWait)) {
            if (exposed) {
                ++d.m_Tactics.Bounds;
                if (covered) ++d.m_Tactics.CoveredBounds;
            }
            n.BoundWaitFrom = -1.0f;
            if (sq.CoverRequest == n.Index) sq.CoverRequest = -1;
            return true;
        }
        sq.CoverRequest = n.Index;
        sq.CoverRequestAt = now;
        it.Crouch = true;
        it.Aim = true;
        it.FaceAim = true;
        it.Fire = m.Visible;
        return false;
    };
    // Stuck: give the behaviour up and let the next decision pick again.
    if (n.BlockedTime > 1.6f) {
        n.BlockedTime = 0.0f;
        n.Phase = -1;
        d.m_Cover.Release(n.Index, now);
        n.Cover = -1;
        n.NoCoverUntil = now + 1.5f;
    }

    switch (n.Doing) {
    case Behaviour::Idle: {
        // A slow patrol round its post, pausing to look about.
        it.LookPoint = prev.LookPoint;
        if (n.Phase == 0) {
            if (now >= n.IdleUntil) {
                glm::vec3 pt;
                if (d.m_Nav.RandomPointNear(n.PostPos, 7.0f, Rand01(d), Rand01(d), pt)) {
                    n.Goal = pt;
                    n.Phase = 1;
                }
                n.IdleUntil = now + 3.0f;
            }
            // Now and then a glance somewhere new; otherwise the look holds.
            if (Rand01(d) < 0.008f || glm::length(prev.LookPoint) < 1e-3f) {
                const float a = Rand01(d) * 6.283f;
                it.LookPoint = n.Eye + glm::vec3(std::cos(a), -0.1f, std::sin(a)) * 8.0f;
            }
        } else {
            moveTo(n.Goal, Gait::Walk);
            it.LookPoint = n.Goal + glm::vec3(0.0f, 1.5f, 0.0f) + FlatDir(n.Feet, n.Goal) * 6.0f;
            if (arrived(n.Goal)) { n.Phase = 0; n.IdleUntil = now + 2.0f + 4.0f * Rand01(d); }
        }
        break;
    }
    case Behaviour::Investigate: {
        // Weapon up, walk over to the noise, look round, then a couple of places nearby.
        it.Aim = true;
        it.AimPoint = threatChest;
        it.FaceAim = n.Phase == 1;
        if (n.Phase == 0) {
            moveTo(threat, m.Awareness > 0.6f ? Gait::Jog : Gait::Walk);
            it.AimPoint = threatChest;
            if (arrived(threat) || Flat(n.Feet, threat) < 2.0f) { n.Phase = 1; n.PhaseUntil = now + 2.5f; }
        } else if (n.Phase == 1) {
            const float sweep = std::sin((now - n.PhaseStart) * 1.3f) * 1.1f;
            const glm::vec3 base = FlatDir(n.Feet, threat);
            const glm::vec3 dir(base.x * std::cos(sweep) - base.z * std::sin(sweep), 0.0f, base.x * std::sin(sweep) + base.z * std::cos(sweep));
            it.AimPoint = n.Eye + dir * 10.0f;
            if (now > n.PhaseUntil) {
                glm::vec3 pt;
                if (n.SearchStep < 2 && d.m_Nav.RandomPointNear(threat, 6.0f, Rand01(d), Rand01(d), pt)) {
                    n.Goal = pt;
                    n.Phase = 2;
                    ++n.SearchStep;
                } else {
                    n.Phase = -1; // nothing here
                    n.Mem.Awareness = std::min(n.Mem.Awareness, 0.25f);
                }
            }
        } else {
            moveTo(n.Goal, Gait::Walk);
            it.AimPoint = n.Goal + glm::vec3(0.0f, 1.4f, 0.0f) + FlatDir(n.Feet, n.Goal) * 5.0f;
            if (arrived(n.Goal)) { n.Phase = 1; n.PhaseStart = now; n.PhaseUntil = now + 2.0f; }
        }
        break;
    }
    case Behaviour::Engage: {
        // In the open: shoot, and keep moving across the line of fire between bursts. A shotgun closes in.
        it.Aim = true;
        it.FaceAim = true;
        it.Fire = true;
        it.Reload = ammo01 <= 0.0f;
        const float dist = Flat(n.Feet, threat);
        glm::vec3 snapped;
        if (shotgun && dist > 6.5f) {
            moveTo(threat + FlatDir(threat, n.Feet) * 5.0f, dist > 14.0f ? Gait::Jog : Gait::Walk);
        } else if (!shotgun && dist < 5.0f && (n.Phase != 2 || now > n.PhaseUntil) &&
                   d.m_Nav.Closest(n.Feet + FlatDir(threat, n.Feet) * 3.0f, snapped) && d.m_Nav.Walkable(n.Feet, snapped)) {
            // Too close for a rifle: back off while shooting.
            if (n.Phase != 2) ++d.m_Tactics.Backpedals;
            n.Goal = snapped;
            n.Phase = 2;
            n.PhaseUntil = now + 0.8f;
        } else if (n.Phase == 0 || now > n.PhaseUntil) {
            // A strafe step: 2-4 m across, alternating sides, or a crouch on the spot at range.
            const glm::vec3 across = glm::normalize(glm::cross(FlatDir(n.Feet, threat), glm::vec3(0, 1, 0)));
            const float side = (n.SearchStep++ % 2 == 0) ? 1.0f : -1.0f;
            glm::vec3 pt = n.Feet + across * side * (2.0f + 2.0f * Rand01(d));
            n.Goal = d.m_Nav.Closest(pt, snapped) && d.m_Nav.Walkable(n.Feet, snapped) ? snapped : n.Feet;
            n.Phase = 1;
            n.PhaseUntil = now + 1.4f + 1.2f * Rand01(d);
        }
        if ((n.Phase == 1 || n.Phase == 2) && !(shotgun && dist > 6.5f)) {
            if (!arrived(n.Goal)) moveTo(n.Goal, Gait::Walk);
            else it.Crouch = !shotgun && dist > 14.0f && n.SearchStep % 3 == 0;
        }
        break;
    }
    case Behaviour::TakeCover:
    case Behaviour::Flank:
    case Behaviour::Retreat: {
        if (n.Cover < 0) { n.Phase = -1; break; }
        if (!bound(1.6f)) break;
        const CoverPoint& c = d.m_Cover.Points()[(size_t)n.Cover];
        const bool exposed = m.Visible || now - n.LastHurt < 2.0f;
        const float left = Flat(n.Feet, c.Pos);
        // Sprint across open ground; jog when hidden; turn and shoot on the way if it's close and in view.
        Gait pace = exposed || left > 12.0f || n.Doing == Behaviour::Retreat ? Gait::Run : Gait::Jog;
        if (n.Doing == Behaviour::Flank && !exposed) pace = left > 8.0f ? Gait::Run : Gait::Jog;
        moveTo(c.Pos, pace);
        if (pace == Gait::Jog && m.Visible && Flat(n.Feet, threat) < 25.0f) {
            it.Aim = true;
            it.FaceAim = true;
            it.Fire = true;
            it.Move = true;
        }
        if (left < arriveDist) {
            // In: fight from here.
            if (n.Doing == Behaviour::Flank && n.Squad < (int)d.m_Squads.size()) {
                auto& sq = d.m_Squads[(size_t)n.Squad];
                if (sq.FlankHolder == n.Index) { sq.FlankHolder = -1; sq.FlankDoneAt = now; }
                n.HasFlankToken = false;
            }
            n.Doing = Behaviour::CoverFight;
            n.DoingSince = now;
            n.Phase = 0;
            n.PhaseStart = now;
            n.PhaseUntil = now + 0.5f + 0.7f * Rand01(d);
        }
        break;
    }
    case Behaviour::Push: {
        if (n.Phase == 2 || n.Cover < 0) {
            // A shotgun with nothing to hide behind: straight at them.
            it.Aim = true;
            it.FaceAim = true;
            it.Fire = true;
            moveTo(threat + FlatDir(threat, n.Feet) * 4.0f, Flat(n.Feet, threat) > 10.0f ? Gait::Run : Gait::Jog);
            if (Flat(n.Feet, threat) < 6.0f) n.Phase = -1;
            break;
        }
        if (!bound(0.8f)) break;
        const CoverPoint& c = d.m_Cover.Points()[(size_t)n.Cover];
        moveTo(c.Pos, Gait::Run);
        if (Flat(n.Feet, c.Pos) < arriveDist) {
            if (n.Squad < (int)d.m_Squads.size()) {
                auto& sq = d.m_Squads[(size_t)n.Squad];
                if (sq.PushHolder == n.Index) sq.PushHolder = -1;
            }
            n.HasPushToken = false;
            n.Doing = Behaviour::CoverFight;
            n.DoingSince = now;
            n.Phase = 1; // come up shooting
            n.PhaseStart = now;
            n.PhaseUntil = now + 2.0f;
            n.ShotsAtPhase = n.ShotsFired;
        }
        break;
    }
    case Behaviour::CoverFight: {
        if (n.Cover < 0) { n.Phase = -1; break; }
        CoverPoint& c = d.m_Cover.Point(n.Cover);
        const float sinceSeen = now - m.LastSeen;
        if (n.Phase == 0) {
            // Down behind it. Reload here if the magazine's getting light; stay down longer under fire.
            moveTo(c.Pos, Gait::Walk);
            if (Flat(n.Feet, c.Pos) < 0.3f) it.Move = false;
            it.Crouch = !c.High;
            it.Aim = false;
            it.LookPoint = threatChest;
            it.Reload = ammo01 < 0.5f;
            if (n.Suppression > 0.6f) n.PhaseUntil = std::max(n.PhaseUntil, now + 0.2f);
            // Told to cover a squadmate's bound: up now.
            const bool ordered = now < n.CoverFireOrder;
            if (ordered) n.PhaseUntil = std::min(n.PhaseUntil, now);
            // Seen while hiding: the cover's no good any more (flanked) - move.
            if (m.Visible && !n.CoverGood) { n.Phase = -1; d.m_Cover.Release(n.Index, now); n.Cover = -1; break; }
            // Pinned: the gun up over low cover, or out past the edge of high cover, and a burst without looking.
            if ((!c.High || c.Peek[0] || c.Peek[1]) && !reloading && ammo01 > 0.2f && now - n.LastBlindFire > 5.0f && n.PinnedSince >= 0.0f &&
                WantsBlindFire(n.Suppression, now - n.PinnedSince, m.Known, sinceSeen)) {
                n.Phase = 3;
                n.PhaseStart = now;
                n.PhaseUntil = now + 1.1f + 0.8f * Rand01(d);
                n.LastBlindFire = now;
                n.ShotsAtPhase = n.ShotsFired;
                n.PeekSide = -1;
                if (c.High) {
                    float bestD = 1e9f;
                    for (int s = 0; s < 2; ++s)
                        if (c.Peek[s] && Flat(c.PeekPos[s], threat) < bestD) { bestD = Flat(c.PeekPos[s], threat); n.PeekSide = s; }
                }
                ++d.m_Tactics.BlindFires;
                break;
            }
            if (now > n.PhaseUntil && !reloading && ammo01 >= 0.3f) {
                // Out to shoot: over low cover, else past whichever end sees the threat.
                n.PeekSide = -1;
                if (c.High) {
                    float bestD = 1e9f;
                    for (int s = 0; s < 2; ++s)
                        if (c.Peek[s] && !CoverSystem::Shielded(c.PeekPos[s], 1.55f, threat + glm::vec3(0.0f, 1.6f, 0.0f))) {
                            const float dd = Flat(c.PeekPos[s], threat);
                            if (dd < bestD) { bestD = dd; n.PeekSide = s; }
                        }
                    if (n.PeekSide < 0) {
                        // Can't shoot from here: somewhere better.
                        n.Phase = -1;
                        d.m_Cover.Release(n.Index, now);
                        n.Cover = -1;
                        break;
                    }
                }
                // Peek after peek with nothing to shoot at: this spot is no use - find another.
                if (n.EmptyPeeks >= 2 && !ordered) {
                    n.EmptyPeeks = 0;
                    n.Phase = -1;
                    d.m_Cover.Release(n.Index, now);
                    n.Cover = -1;
                    break;
                }
                n.Phase = 1;
                n.PhaseStart = now;
                n.ShotsAtPhase = n.ShotsFired;
                n.PhaseUntil = now + 1.6f + 1.6f * Rand01(d) + (n.Role == NpcRole::Suppressor ? 0.8f : 0.0f);
            }
        } else if (n.Phase == 1) {
            // Up and shooting: at them when seen, at where they were when suppressing.
            it.Aim = true;
            it.FaceAim = true;
            it.Fire = true;
            it.Crouch = false;
            if (c.High && n.PeekSide >= 0) {
                moveTo(c.PeekPos[n.PeekSide], Gait::Walk);
                if (Flat(n.Feet, c.PeekPos[n.PeekSide]) < 0.25f) it.Move = false;
                it.Lean = n.PeekSide == 0 ? -0.6f : 0.6f;
            }
            if (!m.Visible) {
                const bool suppress = (n.Role == NpcRole::Suppressor || d.m_Squads[(size_t)n.Squad].PushHolder >= 0 ||
                                       d.m_Squads[(size_t)n.Squad].FlankHolder >= 0 || now < n.CoverFireOrder) &&
                                      sinceSeen < 6.0f && !shotgun;
                it.Suppress = suppress;
                it.Fire = suppress;
                if (!suppress && now - n.PhaseStart > 1.2f) n.PhaseUntil = std::min(n.PhaseUntil, now);
            }
            if (now < n.CoverFireOrder) n.PhaseUntil = std::max(n.PhaseUntil, n.CoverFireOrder);
            const int bursts = n.ShotsFired - n.ShotsAtPhase;
            const bool hurtNow = n.LastHurt > n.PhaseStart;
            if (now > n.PhaseUntil || hurtNow || bursts >= (shotgun ? 2 : 12) || ammo01 <= 0.0f) {
                n.EmptyPeeks = bursts == 0 && n.LastOwnSight < n.PhaseStart ? n.EmptyPeeks + 1 : 0;
                n.Phase = 0;
                n.PhaseStart = now;
                const float base = c.High ? 1.0f : 1.3f;
                n.PhaseUntil = now + base * (0.8f + 1.4f * Rand01(d)) * (1.0f + n.Suppression) * (1.2f - 0.4f * n.Skill) *
                                         (n.Retreated ? 1.6f : 1.0f);
            }
        } else if (n.Phase == 3) {
            // Blind fire: down behind the cover, head tucked, the gun held out over or round it toward where the player was.
            moveTo(c.Pos, Gait::Walk);
            if (Flat(n.Feet, c.Pos) < 0.3f) it.Move = false;
            it.Crouch = !c.High; // behind high cover it stands, the gun out round the edge
            it.Aim = true;
            it.AimPoint = threatChest;
            it.FaceAim = true;
            it.Fire = true;
            it.Suppress = true;
            it.BlindFire = true;
            it.Cower = 0.8f;
            // Done, empty, hit, or someone at arm's length to strike instead.
            if (now > n.PhaseUntil || ammo01 <= 0.0f || n.LastHurt > n.PhaseStart || n.MeleeAt > n.PhaseStart) {
                it.BlindFire = false;
                n.Phase = 0;
                n.PhaseStart = now;
                n.PhaseUntil = now + 0.8f + 0.8f * Rand01(d);
            }
        }
        break;
    }
    case Behaviour::Search: {
        // Hunt: to where they were headed, look round, then spread out over where they could be.
        it.Aim = true;
        it.FaceAim = true;
        if (n.Phase == 0) {
            moveTo(n.Goal, Gait::Jog);
            it.AimPoint = n.Goal + glm::vec3(0.0f, 1.3f, 0.0f);
            if (arrived(n.Goal) || n.BlockedTime > 1.0f) { n.Phase = 1; n.PhaseStart = now; n.PhaseUntil = now + 2.2f; }
        } else if (n.Phase == 1) {
            const float sweep = std::sin((now - n.PhaseStart) * 1.6f) * 1.3f;
            const glm::vec3 base = FlatDir(n.Feet, threat + glm::vec3(0.001f, 0, 0));
            const glm::vec3 dir(base.x * std::cos(sweep) - base.z * std::sin(sweep), 0.0f, base.x * std::sin(sweep) + base.z * std::cos(sweep));
            it.AimPoint = n.Eye + dir * 10.0f;
            if (now > n.PhaseUntil) {
                glm::vec3 pt;
                const float r = std::clamp(m.Uncertainty, 4.0f, 14.0f);
                if (n.SearchStep < 4 && d.m_Nav.RandomPointNear(threat, r, Rand01(d), Rand01(d), pt)) {
                    n.Goal = pt;
                    n.Phase = 0;
                    ++n.SearchStep;
                } else {
                    // Lost them: back to being watchful.
                    n.Mem.Known = false;
                    n.Mem.Awareness = 0.4f;
                    n.Phase = -1;
                }
            }
        }
        break;
    }
    case Behaviour::Wounded: {
        // On a knee, crawling for cover (the director holds its speed to a crawl), shooting when it has the player.
        it.Crouch = true;
        it.Aim = m.Known;
        it.FaceAim = true;
        it.Fire = m.Visible || now - n.LastOwnSight < 1.5f;
        it.Reload = ammo01 <= 0.0f;
        if (n.Cover >= 0) {
            const CoverPoint& c = d.m_Cover.Points()[(size_t)n.Cover];
            if (Flat(n.Feet, c.Pos) > 0.5f) moveTo(c.Pos, Gait::Walk);
        }
        break;
    }
    case Behaviour::Dead:
        break;
    }
    // Reactions over whatever the behaviour wants: a duck from a near miss holds the trigger for its beat; a
    // squadmate calling out draws a look while the gun is down.
    if (now < n.CowerUntil) {
        it.Cower = 1.0f;
        it.Fire = false;
    }
    if (now < n.GlanceUntil && !it.Aim) it.LookPoint = n.GlanceAt;
    if (it.Move && !d.m_Nav.Valid()) it.Move = false;
}
