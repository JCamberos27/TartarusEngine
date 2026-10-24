// The hunters' minds (SpiderSystem::Hunt). A spider waits on the ceiling, in a corner if it can find one. Once it sees
// the player (or hears them run) it goes after them over the walls and ceiling: half of them (the droppers) get above
// the player and drop on them, the others charge across whatever surface is shortest, pounce from a few metres - a
// crouch first, the tell - and bite. A bite or a pounce that lands shoves the player back and jolts their view. After
// a couple of bites, or a pounce that missed (it stumbles), it backs off up a wall and comes again. At most two attack
// at once; the rest circle overhead. Losing the player, it searches where it last saw them, then goes back to waiting.
#include "SpiderSystem.h"

#include "Components.h"
#include "GameModuleAPI.h" // RaycastHit, QueryFilter
#include "PhysicsWorld.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr float kGravity = 9.81f;
constexpr int kMaxAttackers = 2;

glm::vec3 Flat(const glm::vec3& v) {
    const glm::vec3 f(v.x, 0.0f, v.z);
    const float l = glm::length(f);
    return l > 1e-5f ? f / l : glm::vec3(0.0f);
}

// How far `p` is from the player's body (a capsule from their feet to their eyes), its surface not its middle.
float ToPlayer(const glm::vec3& p, const SpiderSystem::Target& t) {
    const glm::vec3 a = t.Feet + glm::vec3(0.0f, t.Radius, 0.0f), b = t.Eye;
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    const float s = len2 > 1e-8f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * s)) - t.Radius;
}

bool LineClear(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 d = to - from;
    const float dist = glm::length(d);
    if (dist < 1e-3f) return true;
    const glm::vec3 dir = d / dist;
    RaycastHit h;
    QueryFilter f;
    f.HitTriggers = 0;
    return !(PhysicsWorld::RaycastSolid(&from.x, &dir.x, dist, f, h) && h.Hit && h.Distance < dist - 0.25f);
}

} // namespace

int SpiderSystem::Attackers() const {
    int n = 0;
    for (const Body& b : m_Bodies) n += b.Ai.Token ? 1 : 0;
    return n;
}

bool SpiderSystem::SeesPlayer(const Body& b, const SpiderComponent& sc, const Target& t) const {
    const glm::vec3 eye = b.Mover.Position() + b.Mover.Up() * 0.2f;
    const float dist = glm::length(t.Eye - eye);
    if (dist > sc.SightRange) return false;
    return dist < 1.5f || LineClear(eye, t.Eye); // close enough to feel them through anything
}

int SpiderSystem::PickPerch(const glm::vec3& near) const {
    int best = -1;
    float bestScore = -1e9f;
    for (int i = 0; i < m_Nav.NodeCount(); ++i) {
        if (m_Nav.Normal(i).y > -0.9f) continue; // the ceiling
        const float d = glm::length(m_Nav.Pos(i) - near);
        if (d > 7.0f) continue;
        // A corner (a node linked to a wall) hides the body against two surfaces.
        bool corner = false;
        for (int k = m_Nav.FirstLink(i); k < m_Nav.FirstLink(i + 1) && !corner; ++k)
            corner = std::fabs(m_Nav.Normal(m_Nav.Neighbour(k)).y) < 0.7f;
        const float score = -0.5f * d + (corner ? 2.0f : 0.0f);
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

void SpiderSystem::Hunt(World& world, Body& b, SpiderComponent& sc, const Target& t, float dt) {
    auto& ai = b.Ai;
    auto* anim = world.Registry.try_get<AnimatorControllerComponent>(b.ModelEntity);
    const glm::vec3 pos = b.Mover.Position(), up = b.Mover.Up();
    auto enter = [&](Mode m) {
#pragma warning(suppress : 4996)
        static const bool trace = std::getenv("TARTARUS_SPIDER_TRACE") != nullptr; // the hunters' decisions, for tuning
        if (trace)
            std::printf("[Spider] %u: %d -> %d  pos %.2f %.2f %.2f up %.2f  player %.2f %.2f %.2f (%.2f m)%s\n",
                        (unsigned)entt::to_integral(b.Root), (int)ai.State, (int)m, pos.x, pos.y, pos.z, up.y, t.Feet.x, t.Feet.y,
                        t.Feet.z, glm::length(t.Feet - pos), b.Mover.Attached() ? "" : " (in the air)");
        ai.State = m;
        ai.Clock = 0.0f;
        b.RouteAge = 1e9f; // a new state weighs routes its own way: plan afresh
        ai.Struck = false;
        ai.Hold = false;
    };
    auto strike = [&](float amount, const glm::vec3& push, bool pounce) {
        PlayerHit h;
        h.Amount = amount;
        h.Source = pos;
        h.Push = push;
        h.Pounce = pounce;
        m_PlayerHits.push_back(h);
        ai.Struck = true;
    };
    ai.Clock += dt;
    ai.PounceIn -= dt;
    ai.SeenAgo += dt;
    ai.LookIn -= dt;

    // Senses, a few times a second.
    const bool target = t.Valid && !t.Dead;
    const bool alarmed = ai.Alarmed;
    ai.Alarmed = false;
    if (target && (ai.LookIn <= 0.0f || alarmed)) {
        ai.LookIn = 0.15f;
        if (alarmed || SeesPlayer(b, sc, t) || (t.Loud && glm::length(t.Feet - pos) < 20.0f)) {
            ai.SeenAgo = 0.0f;
            ai.LastKnown = t.Feet;
            if (!ai.Aware && ai.State == Mode::Perch) {
                enter(Mode::Stalk);
                Sound("hiss", pos); // it has seen them
            }
            ai.Aware = true;
        }
    }
    if (!target) {
        ai.Aware = false;
        if (ai.State != Mode::Perch) enter(Mode::Perch);
    } else if (ai.Aware && ai.SeenAgo > 8.0f) {
        ai.Aware = false;
        enter(Mode::Search);
    }
    const float dist = target ? glm::length(t.Feet - pos) : 1e9f;

    switch (ai.State) {
    case Mode::Perch: {
        b.Costs = Spider::PathCosts{3.0f, 1.0f, 1.0f};
        sc.Run = false;
        if (ai.Perch < 0) ai.Perch = PickPerch(pos);
        if (ai.Perch >= 0) {
            sc.Goal = m_Nav.Pos(ai.Perch);
            sc.HasGoal = glm::length(pos - sc.Goal) > 0.15f;
        } else {
            sc.HasGoal = false;
        }
        break;
    }
    case Mode::Stalk: {
        // Over walls and ceiling to the ceiling above the player (droppers), or near them until it may attack.
        b.Costs = Spider::PathCosts{4.0f, 1.0f, 1.0f};
        sc.Run = dist < 8.0f;
        ai.AimIn -= dt;
        if (ai.AimIn <= 0.0f || !sc.HasGoal) { // re-aimed a few times a second
            ai.AimIn = 0.4f;
            int node = -1;
            if (ai.Dropper && !m_Nav.Empty()) {
                // The ceiling right over the player.
                glm::vec3 above = t.Feet + glm::vec3(0.0f, 3.0f, 0.0f);
                RaycastHit h;
                QueryFilter f;
                f.HitTriggers = 0;
                const glm::vec3 from = t.Eye, dir(0.0f, 1.0f, 0.0f);
                if (PhysicsWorld::RaycastSolid(&from.x, &dir.x, 5.0f, f, h) && h.Hit) above = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
                const glm::vec3 down(0.0f, -1.0f, 0.0f);
                node = m_Nav.Nearest(above, 1.0f, &down);
            } else if (!m_Nav.Empty()) {
                // Circling: high on a wall a few metres off, waiting for a place in the attack.
                const glm::vec3 away = Flat(pos - t.Feet);
                const glm::vec3 spot = t.Feet + (glm::length(away) > 0.0f ? away : glm::vec3(1.0f, 0.0f, 0.0f)) * 3.0f +
                                       glm::vec3(0.0f, 1.6f, 0.0f);
                node = m_Nav.Nearest(spot, 2.5f);
            }
            sc.Goal = node >= 0 ? m_Nav.Pos(node) : t.Feet;
            sc.HasGoal = true;
        }
        // Overhead: drop - aimed so the fall meets the player's chest.
        const glm::vec3 off = t.Feet - pos;
        if (ai.Dropper && b.Mover.Attached() && up.y < -0.8f && glm::length(glm::vec2(off.x, off.z)) < 1.4f && -off.y > 0.8f &&
            Attackers() < kMaxAttackers) {
            const float fall = std::max(0.2f, pos.y - (t.Feet.y + 1.1f));
            const float time = std::sqrt(2.0f * fall / kGravity);
            b.Mover.Detach(glm::vec3(off.x, 0.0f, off.z) / std::max(time, 0.15f) + t.Velocity * 0.5f);
            ai.FromDrop = true;
            ai.Token = true;
            Sound("screech", pos);
            Sound("pounce", pos, 0.7f);
            ++b.Last.Leaps;
            enter(Mode::Leap);
            break;
        }
        // A charger, or a dropper that has waited long enough, attacks when a place is free.
        if ((!ai.Dropper || ai.Clock > 6.0f) && Attackers() < kMaxAttackers) {
            ai.Token = true;
            enter(Mode::Engage);
        }
        break;
    }
    case Mode::Engage: {
        ai.Token = true;
        b.Costs = Spider::PathCosts{1.0f, 1.4f, 3.0f};
        sc.Run = true;
        sc.Goal = t.Feet;
        sc.HasGoal = true;
        if (dist < 1.2f) {
            enter(Mode::Bite);
            if (anim) anim->SetTrigger("Attack");
        } else if (ai.PounceIn <= 0.0f && dist > 2.2f && dist < 5.0f && b.Mover.Attached() && (dist < 3.5f || SeesPlayer(b, sc, t))) {
            enter(Mode::Telegraph);
            if (anim) anim->SetTrigger("Jump");
            Sound("hiss", pos, 0.8f); // the tell
        }
        break;
    }
    case Mode::Telegraph: {
        // The tell: it stops, faces the player and crouches - then springs at where they will be.
        sc.Goal = t.Feet;
        sc.HasGoal = true;
        ai.Hold = true;
        if (ai.Clock >= 0.45f) {
            const float flight = std::clamp(dist / 7.0f, 0.35f, 0.6f);
            const glm::vec3 aim = t.Feet + t.Velocity * (flight * 0.6f) + glm::vec3(0.0f, 0.8f, 0.0f); // at the chest
            glm::vec3 v = (aim - pos) / flight;
            v.y += 0.5f * kGravity * flight;
            b.Mover.Detach(v);
            ai.FromDrop = false;
            ai.PounceIn = 3.0f;
            Sound("screech", pos);
            Sound("pounce", pos);
            ++b.Last.Leaps;
            enter(Mode::Leap);
        }
        break;
    }
    case Mode::Leap: {
        sc.HasGoal = false;
        // Swept along this frame's flight (it covers a hand's width a frame), against the reach of a body with legs
        // spread wide: a leap that passes within half a metre of the player has them.
        const glm::vec3 flightStep = b.Mover.Velocity() * dt;
        const float near = std::min({ToPlayer(pos + up * 0.15f, t), ToPlayer(pos + up * 0.15f + flightStep * 0.5f, t),
                                     ToPlayer(pos + up * 0.15f + flightStep, t)});
        if (!ai.Struck && target && near < 0.5f) {
            strike(sc.PounceDamage, Flat(t.Feet - pos) * (sc.Knockback * 1.5f), true);
            ++(ai.FromDrop ? b.Last.Drops : b.Last.Pounces);
            ai.Landed = true;
        }
        if (b.Mover.Attached() && ai.Clock > 0.1f) {
            const bool hit = ai.Landed;
            ai.Landed = false;
            if (target && (hit || dist < 1.3f)) { // on them, or landed beside them: straight in with the fangs
                enter(Mode::Bite);
                if (anim) anim->SetTrigger("Attack");
            } else {
                enter(Mode::Recover); // missed: it stumbles
            }
        } else if (ai.Clock > 3.0f) {
            enter(Mode::Recover);
        }
        break;
    }
    case Mode::Bite: {
        ai.Token = true;
        sc.Goal = t.Feet;
        sc.HasGoal = true;
        ai.Hold = true;
        if (ai.Clock >= 0.3f && ai.Clock - dt < 0.3f) Sound("bite", pos + b.Mover.Forward() * 0.45f); // the fangs snap, hit or miss
        if (!ai.Struck && ai.Clock >= 0.3f && target) {
            const glm::vec3 fangs = pos + b.Mover.Forward() * 0.45f + up * 0.15f;
            if (ToPlayer(fangs, t) < 0.45f) {
                strike(sc.BiteDamage, Flat(t.Feet - pos) * sc.Knockback, false);
                ++b.Last.Bites;
            }
        }
        if (ai.Clock >= 0.8f) {
            ++ai.BitesInRow;
            if (ai.BitesInRow >= 2) enter(Mode::Reposition);
            else enter(Mode::Engage);
        }
        break;
    }
    case Mode::Recover: {
        sc.HasGoal = false;
        if (ai.Clock >= 0.9f) enter(Mode::Reposition);
        break;
    }
    case Mode::Reposition: {
        // Back off up a wall a few metres away, then come again.
        ai.Token = false;
        ai.BitesInRow = 0;
        b.Costs = Spider::PathCosts{3.0f, 1.0f, 1.0f};
        sc.Run = true;
        if (ai.Clock <= dt * 1.5f && target) {
            const glm::vec3 away = Flat(pos - t.Feet);
            const glm::vec3 spot = t.Feet + (glm::length(away) > 0.0f ? away : glm::vec3(1.0f, 0.0f, 0.0f)) * 3.5f +
                                   glm::vec3(0.0f, 1.3f, 0.0f);
            const int node = m_Nav.Empty() ? -1 : m_Nav.Nearest(spot, 2.5f);
            sc.Goal = node >= 0 ? m_Nav.Pos(node) : spot;
        }
        sc.HasGoal = true;
        if ((b.Mover.Arrived() && ai.Clock > 0.5f) || ai.Clock > 3.5f) enter(Mode::Stalk);
        break;
    }
    case Mode::Search: {
        b.Costs = Spider::PathCosts{1.0f, 1.2f, 1.5f};
        sc.Run = false;
        sc.Goal = ai.LastKnown;
        sc.HasGoal = !b.Mover.Arrived();
        if (ai.Aware) enter(Mode::Stalk);
        else if (ai.Clock > 10.0f) {
            ai.Perch = -1;
            enter(Mode::Perch);
        }
        break;
    }
    case Mode::Scripted:
        break;
    }
    const bool attacking = ai.State == Mode::Engage || ai.State == Mode::Telegraph || ai.State == Mode::Leap ||
                           ai.State == Mode::Bite || ai.State == Mode::Recover;
    if (!attacking) ai.Token = false;
}
