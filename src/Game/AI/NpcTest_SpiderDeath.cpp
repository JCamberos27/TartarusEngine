// --npc-test spiderdeath: spiders shot in Apartment 301's living room down the player's round path (the hitboxes, the
// damage by zone, the ichor), each moment shot and checked:
//   1. a spider on the floor shot in a leg: it takes a leg's share, flinches, curls that leg and limps, and lives;
//   2. ... then in the abdomen: ichor sprays, stains the floor and splats the body - yellow-green, not red;
//   3. ... then in the head: it dies, its ragdoll folds its legs in, and the corpse is asleep within a few seconds,
//      nothing of it through the floor;
//   4. a spider on the right-hand wall hit hard but not killed: knocked off, it falls, lands and stands again;
//   5. a spider on the ceiling killed: it drops, lands and settles.
// (Creatures/SpiderDamage.h, Creatures/SpiderRagdoll.h; NpcTest_SpiderDeath.cpp.)
#include "NpcTest.h"
#include "Combat/BloodFx.h"
#include "Components.h"
#include "Creatures/SpiderDamage.h"
#include "Creatures/SpiderSystem.h"
#include "GameModuleAPI.h" // RaycastHit, QueryFilter
#include "PhysicsWorld.h"
#include "Renderer/BloodPalette.h"
#include "World.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr float kDt = 1.0f / 60.0f;
const glm::vec3 kEye(2.5f, 1.45f, -4.6f); // the living room's entry end, looking in

bool Cast(const glm::vec3& from, const glm::vec3& dir, glm::vec3& point) {
    RaycastHit h;
    QueryFilter f;
    f.HitTriggers = 0;
    if (!PhysicsWorld::RaycastSolid(&from.x, &dir.x, 6.0f, f, h) || !h.Hit) return false;
    point = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
    return true;
}

void YawPitchAt(const glm::vec3& from, const glm::vec3& to, float& yaw, float& pitch) {
    const glm::vec3 d = glm::normalize(to - from);
    yaw = glm::degrees(std::atan2(d.z, d.x));
    pitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
}

// Yellow-green: green leads, red close behind, blue far below (a person's blood is red over everything).
bool YellowGreen(const glm::vec3& c) { return c.g > c.r && c.r > 2.5f * c.b && c.g > 0.2f; }
} // namespace

struct NpcTest::SpiderDeathRun {
    std::uint32_t Floor = 0xFFFFFFFFu, Wall = 0xFFFFFFFFu, Ceiling = 0xFFFFFFFFu;
    float FloorY = 0.0f, CeilingY = 2.7f;
    float Clock = 0.0f;  // since all three stand ready
    float Wait = 0.0f;   // before that
    int Step = 0;        // the timeline's next event
    float At = 0.0f;     // when the current step began
    bool Ready = false;
    float HealthBefore = 0.0f;
    float FlinchPeak = 0.0f, CurlPeak = 0.0f, SpeedLow = 1.0f;
    glm::vec3 Look{0.0f};
    // the deaths
    float LowestSeen = 1e9f, FoldPeak = 0.0f, FastestAfter = 0.0f;
    bool Blew = false;
    float CeilTop = 0.0f;
    bool KnockedOff = false, WallFell = false;
    int SpraysIchor = 0, SpraysRed = 0;
};

void NpcTest::SpiderDeath(World& world) {
    if (!m_SD) m_SD = std::make_shared<SpiderDeathRun>();
    SpiderDeathRun& r = *m_SD;
    auto& reg = world.Registry;
    const auto entity = [](std::uint32_t id) { return (entt::entity)id; };

    if (r.Step == 0) { // the room, the light, three spiders
        r.Step = 1;
        const entt::entity lamp = world.CreateEmptyEntity(glm::vec3(2.6f, 2.1f, -5.6f), glm::vec3(0.0f), glm::vec3(1.0f), "Spider Test Light");
        auto& light = reg.emplace<LightComponent>(lamp);
        light.Intensity = 7.0f;
        light.Range = 9.0f;
        glm::vec3 floor(0.0f), wall(0.0f), ceiling(0.0f);
        const bool ok = Cast(glm::vec3(2.5f, 1.0f, -6.3f), glm::vec3(0.0f, -1.0f, 0.0f), floor) &&
                        Cast(glm::vec3(2.5f, 1.3f, -7.3f), glm::vec3(1.0f, 0.0f, 0.0f), wall) &&
                        Cast(glm::vec3(3.0f, 1.3f, -5.6f), glm::vec3(0.0f, 1.0f, 0.0f), ceiling);
        Check(ok, "301's living room has its floor, right-hand wall and ceiling where the scenario expects");
        Check(SpawnSpider && ShootSpider && SpiderHurt && BloodFxView, "the host wired the spider hooks");
        if (!ok || !SpawnSpider || !ShootSpider || !SpiderHurt || !BloodFxView) {
            m_Done = true;
            return;
        }
        r.FloorY = floor.y;
        r.CeilingY = ceiling.y;
        // On the floor facing the room's far end (its abdomen toward the camera); on the wall, its back to the wall; on
        // the ceiling, upside down.
        r.Floor = SpawnSpider(floor, 0.0f);
        r.Wall = SpawnSpider(wall, 0.0f);
        r.Ceiling = SpawnSpider(ceiling, 0.0f);
        if (auto* t = reg.try_get<TransformComponent>(entity(r.Wall))) t->Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
        if (auto* t = reg.try_get<TransformComponent>(entity(r.Ceiling))) t->Rotation = glm::angleAxis(glm::radians(180.0f), glm::vec3(1, 0, 0));
        Check(r.Floor != 0xFFFFFFFFu && r.Wall != 0xFFFFFFFFu && r.Ceiling != 0xFFFFFFFFu, "three spiders are built from Spider.json");
        std::printf("[NpcTest] spiderdeath: floor %.2f, wall x %.2f, ceiling %.2f\n", floor.y, wall.x, ceiling.y);
        return;
    }

    SpiderDamage::View fv, wv, cv;
    const bool haveF = SpiderHurt->Inspect(entity(r.Floor), fv) && fv.Ready && fv.Parts > 0 && (fv.Hitboxes >= 0 || fv.Dead);
    const bool haveW = SpiderHurt->Inspect(entity(r.Wall), wv) && wv.Ready && (wv.Hitboxes >= 0 || wv.Dead);
    const bool haveC = SpiderHurt->Inspect(entity(r.Ceiling), cv) && cv.Ready && (cv.Hitboxes >= 0 || cv.Dead);
    if (!r.Ready) {
        r.Wait += kDt;
        if (haveF && haveW && haveC && r.Wait > 1.5f) {
            r.Ready = true;
            Check(fv.Parts >= 26, "spiderdeath: the rig's part table has the thorax, abdomen and 24 leg segments (" + std::to_string(fv.Parts) + ")");
        } else if (r.Wait > 8.0f) {
            Check(false, "spiderdeath: the spiders come alive with their hitboxes");
            m_Done = true;
        }
        return;
    }
    r.Clock += kDt;
    const float since = r.Clock - r.At;
    auto next = [&] { ++r.Step; r.At = r.Clock; };
    auto health = [&](std::uint32_t id) {
        const auto* h = reg.try_get<HealthComponent>(entity(id));
        return h ? (h->Current < 0.0f ? h->Max : h->Current) : -1.0f;
    };
    auto part = [&](std::uint32_t id, const char* n, glm::vec3& at) { return SpiderHurt->PartCentre(entity(id), SpiderHurt->PartIndex(n), at); };
    auto look = [&](const glm::vec3& at) {
        r.Look = at;
        m_CamPos = kEye;
        YawPitchAt(kEye, at, m_CamYaw, m_CamPitch);
        m_HaveCam = true;
    };
    // A round from `from` at `target`: true when a spider took it.
    auto shoot = [&](const glm::vec3& from, const glm::vec3& target, float damage, int pellets, std::uint32_t want) {
        bool spider = false;
        const std::uint32_t hit = ShootSpider(from, glm::normalize(target - from), damage, pellets, spider);
        return spider && hit == want;
    };
    SpiderSystem::Stats fs;
    if (Spiders) Spiders->Inspect(entity(r.Floor), fs);
    glm::vec3 thorax(0.0f), abdomen(0.0f), leg(0.0f);
    part(r.Floor, "thorax", thorax);
    part(r.Floor, "abdomen", abdomen);
    part(r.Floor, "L1.2", leg);
    if (r.Step <= 9) look(fs.Pos + glm::vec3(0.0f, 0.15f, 0.0f));

    switch (r.Step) {
    case 1: // the leg
        if (since < 1.0f) break;
        r.HealthBefore = health(r.Floor);
        {
            // From outside the leg, along its own line out from the body: the round meets the leg before anything else.
            glm::vec3 out = leg - thorax;
            out.y = 0.0f;
            out = glm::length(out) > 1e-3f ? glm::normalize(out) : glm::vec3(-1, 0, 0);
            Check(shoot(leg + out * 1.2f + glm::vec3(0.0f, 0.1f, 0.0f), leg, 34.0f, 1, r.Floor), "spiderdeath: a round into a leg stops in the spider");
        }
        next();
        break;
    case 2: // its flinch, curl and limp
        r.FlinchPeak = std::max(r.FlinchPeak, fv.Flinch);
        r.CurlPeak = std::max(r.CurlPeak, fv.LegCurlMax);
        r.SpeedLow = std::min(r.SpeedLow, fv.SpeedScale);
        if (std::fabs(since - 4.0f * kDt) < 0.5f * kDt) m_Shot = "spiderdeath_1_leg_flinch";
        if (since < 0.6f) break;
        {
            const float lost = r.HealthBefore - health(r.Floor);
            const bool legZone = fv.LastPart >= 0 && SpiderHurt->Rig().Parts[(size_t)fv.LastPart].Zone == Spider::PartZone::Leg;
            std::printf("[NpcTest] spiderdeath leg: part %d (%s), %.1f damage, flinch peak %.1f deg, curl %.2f, speed x %.2f\n", fv.LastPart,
                        fv.LastPart >= 0 ? SpiderHurt->Rig().Parts[(size_t)fv.LastPart].Name.c_str() : "-", lost, glm::degrees(r.FlinchPeak),
                        r.CurlPeak, r.SpeedLow);
            Check(legZone, "spiderdeath: the leg round struck a leg part");
            Check(lost > 12.0f && lost < 22.0f, "spiderdeath: a leg takes half a round (" + std::to_string(lost) + " of 34)");
            Check(r.FlinchPeak > glm::radians(5.0f), "spiderdeath: the struck leg flinches");
            Check(r.CurlPeak > 0.5f && r.SpeedLow < 0.6f, "spiderdeath: the leg curls up and the spider limps");
            Check(!fv.Dead, "spiderdeath: a leg round doesn't kill it");
        }
        next();
        break;
    case 3: // the abdomen
        if (since < 0.8f) break;
        r.HealthBefore = health(r.Floor);
        Check(shoot(kEye + glm::vec3(0.0f, 0.2f, 0.0f), abdomen, 34.0f, 1, r.Floor), "spiderdeath: a round into the abdomen stops in the spider");
        next();
        break;
    case 4: { // the ichor
        if (std::fabs(since - 0.2f) < 0.5f * kDt) {
            m_Shot = "spiderdeath_2_abdomen_spray";
            for (const auto& s : BloodFxView->Sprays()) r.SpraysIchor += s.Palette == BloodPalette::kIchor, r.SpraysRed += s.Palette != BloodPalette::kIchor;
        }
        if (since < 1.6f) break;
        int sprays = r.SpraysIchor, red = r.SpraysRed, decals = 0, splats = 0;
        for (const auto& d : BloodFxView->Decals()) d.Palette == BloodPalette::kIchor ? ++decals : ++red;
        for (const auto& s : BloodFxView->Splats()) s.Palette == BloodPalette::kIchor ? ++splats : ++red;
        const float lost = r.HealthBefore - health(r.Floor);
        const BloodPalette::Look& ichor = BloodPalette::Of(BloodPalette::kIchor);
        std::printf("[NpcTest] spiderdeath abdomen: %.1f damage (part %d), ichor sprays %d, stains %d, splats %d; red %d; fresh %.2f %.2f %.2f "
                    "dried %.3f %.3f %.3f\n", lost, fv.LastPart, sprays, decals, splats, red, ichor.Fresh.r, ichor.Fresh.g, ichor.Fresh.b,
                    ichor.Dried.r, ichor.Dried.g, ichor.Dried.b);
        Check(fv.LastPart >= 0 && SpiderHurt->Rig().Parts[(size_t)fv.LastPart].Zone == Spider::PartZone::Abdomen, "spiderdeath: the round struck the abdomen");
        Check(lost > 36.0f && lost < 46.0f, "spiderdeath: the abdomen takes 1.2x (" + std::to_string(lost) + " of 34)");
        Check(!fv.Dead, "spiderdeath: alive after a leg and the abdomen");
        Check(sprays > 0 && decals > 0 && splats > 0, "spiderdeath: the abdomen sprays ichor, stains the floor and the body");
        Check(red == 0, "spiderdeath: none of it is a person's red");
        Check(YellowGreen(ichor.Fresh) && YellowGreen(ichor.Thin) && ichor.FilmOpacity < 1.0f, "spiderdeath: fresh ichor is a translucent yellow-green");
        Check(ichor.Dried.g < 0.15f && ichor.Dried.g > ichor.Dried.b * 3.0f, "spiderdeath: dried ichor is olive");
        m_Shot = "spiderdeath_3_ichor_stains";
        next();
        break;
    }
    case 5: // the head
        if (since < 1.0f) break;
        {
            // From above and a little ahead: over the legs' knees, straight into the cephalothorax.
            Check(shoot(thorax + fs.Up * 1.3f + fs.Forward * 0.3f, thorax, 34.0f, 1, r.Floor), "spiderdeath: a head round stops in the spider");
        }
        next();
        break;
    case 6: // the death
        if (fv.Dead) {
            r.LowestSeen = std::min(r.LowestSeen, fv.LowestPart);
            r.FoldPeak = std::max(r.FoldPeak, fv.LegFold);
            r.Blew = r.Blew || !fv.Finite || fv.MaxSpeed > 12.0f;
            if (since > 0.5f) r.FastestAfter = std::max(r.FastestAfter, fv.MaxSpeed);
        }
        if (std::fabs(since - 0.25f) < 0.5f * kDt) m_Shot = "spiderdeath_4_head_dying";
        if (std::fabs(since - 1.2f) < 0.5f * kDt) m_Shot = "spiderdeath_5_death_curl";
        if (since < 6.0f && !(fv.Asleep && since > 2.0f)) break;
        std::printf("[NpcTest] spiderdeath head: dead %d, ragdoll %d, asleep %d at %.2f s, legs folded %.0f deg (peak %.0f), lowest part %.3f "
                    "(floor %.3f), fastest after 0.5 s %.2f m/s\n", fv.Dead, fv.Ragdoll, fv.Asleep, fv.AsleepAt, fv.LegFold, r.FoldPeak, r.LowestSeen,
                    r.FloorY, r.FastestAfter);
        Check(fv.Dead && fv.Ragdoll >= 0, "spiderdeath: a head round kills it, into a ragdoll");
        Check(fv.LastPart >= 0 && SpiderHurt->Rig().Parts[(size_t)fv.LastPart].Zone == Spider::PartZone::Head, "spiderdeath: the kill was the head");
        Check(r.FoldPeak > 20.0f, "spiderdeath: the legs curl in as it dies");
        Check(fv.Asleep && fv.AsleepAt >= 0.0f && fv.AsleepAt < 5.0f, "spiderdeath: the corpse is asleep within 5 s");
        Check(r.LowestSeen > r.FloorY - 0.03f, "spiderdeath: nothing of the corpse goes through the floor");
        Check(!r.Blew, "spiderdeath: the ragdoll holds together (finite, no part flung)");
        m_Shot = "spiderdeath_6_corpse";
        next();
        break;
    case 7: // the wall: a heavy round that doesn't kill
        {
            glm::vec3 a(0.0f);
            if (part(r.Wall, "thorax", a)) look(a);
            if (since < 0.5f) break;
            if (since < 0.5f + kDt) { m_Shot = "spiderdeath_7_wall_before"; break; }
            glm::vec3 ab(0.0f);
            part(r.Wall, "abdomen", ab);
            Check(shoot(kEye + glm::vec3(-0.6f, 0.0f, -0.5f), ab, 26.0f, 1, r.Wall), "spiderdeath: a heavy round into the spider on the wall stops in it");
            SpiderSystem::Stats ws;
            r.KnockedOff = Spiders && Spiders->Inspect(entity(r.Wall), ws) && !ws.Attached;
            Check(r.KnockedOff, "spiderdeath: a heavy round knocks it off the wall");
        }
        next();
        break;
    case 8: { // it falls, lands, rights itself
        SpiderSystem::Stats ws;
        if (Spiders) Spiders->Inspect(entity(r.Wall), ws);
        look(ws.Pos + glm::vec3(0.0f, 0.1f, 0.0f));
        if (std::fabs(since - 0.2f) < 0.5f * kDt) m_Shot = "spiderdeath_8_wall_knocked_off";
        if (since < 2.5f) break;
        std::printf("[NpcTest] spiderdeath wall: attached %d, up %.2f %.2f %.2f, at %.2f %.2f %.2f, dead %d\n", ws.Attached, ws.Up.x, ws.Up.y, ws.Up.z,
                    ws.Pos.x, ws.Pos.y, ws.Pos.z, wv.Dead);
        Check(ws.Attached && ws.Up.y > 0.8f && ws.Pos.y < r.FloorY + 0.1f && !wv.Dead, "spiderdeath: knocked off, it lands on the floor on its feet");
        m_Shot = "spiderdeath_9_wall_landed";
        next();
        break;
    }
    case 9: { // the ceiling
        glm::vec3 c(0.0f);
        if (part(r.Ceiling, "thorax", c)) look(c);
        if (since < 0.5f) break;
        if (since < 0.5f + kDt) { m_Shot = "spiderdeath_10_ceiling_before"; break; }
        Check(shoot(c + glm::vec3(0.3f, -1.4f, 0.6f), c, 150.0f, 1, r.Ceiling), "spiderdeath: a round into the spider on the ceiling stops in it");
        r.CeilTop = c.y;
        r.LowestSeen = 1e9f;
        r.Blew = false;
        next();
        break;
    }
    case 10: { // it drops, lands and settles
        if (cv.Dead) {
            look(cv.Centre);
            r.LowestSeen = std::min(r.LowestSeen, cv.LowestPart);
            r.Blew = r.Blew || !cv.Finite || cv.MaxSpeed > 14.0f;
        }
        if (std::fabs(since - 0.3f) < 0.5f * kDt) m_Shot = "spiderdeath_11_ceiling_falling";
        if (since < 7.0f && !(cv.Asleep && since > 2.5f)) break;
        std::printf("[NpcTest] spiderdeath ceiling: dead %d, centre y %.2f (from %.2f), lowest %.3f (floor %.3f), asleep %d at %.2f s, legs %.0f deg\n",
                    cv.Dead, cv.Centre.y, r.CeilTop, r.LowestSeen, r.FloorY, cv.Asleep, cv.AsleepAt, cv.LegFold);
        Check(cv.Dead && cv.Ragdoll >= 0, "spiderdeath: the spider on the ceiling dies into a ragdoll");
        Check(cv.Centre.y < r.FloorY + 0.4f, "spiderdeath: killed on the ceiling, it drops to the floor");
        Check(r.LowestSeen > r.FloorY - 0.03f, "spiderdeath: the dropped corpse doesn't go through the floor");
        Check(cv.Asleep && cv.AsleepAt < 6.5f, "spiderdeath: the dropped corpse settles and sleeps");
        Check(!r.Blew, "spiderdeath: the dropped ragdoll holds together");
        // The first corpse is still there, still asleep.
        Check(fv.Dead && fv.Asleep, "spiderdeath: the first corpse persists, asleep");
        m_Shot = "spiderdeath_12_ceiling_landed";
        next();
        break;
    }
    default:
        m_Done = true;
        break;
    }
    if (r.Clock > 55.0f && !m_Done) {
        Check(false, "spiderdeath: the timeline finishes (stuck at step " + std::to_string(r.Step) + ")");
        m_Done = true;
    }
}
