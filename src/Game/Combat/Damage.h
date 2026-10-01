#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct FirstPersonWeaponGameplay;

// Damage between the player and the enemy squad. The maths is pure (unit tested); the queue is
// where this frame's hits wait for whoever applies them (NpcDirector for NPCs, PlayerVitals for
// the player).

enum class HitZone : std::uint8_t { Head, Torso, Limb };

// 1 up to `start` metres, easing linearly down to `minScale` at `end` and beyond.
float DamageFalloff(float distance, float start, float end, float minScale);

// One round (or pellet) of `weapon` landing on `zone` from `distance` metres away.
float DamageForHit(const FirstPersonWeaponGameplay& weapon, HitZone zone, float distance);

// Where on the body a hit landed, finer than the zone: what the hitboxes report. Limbs split into arms and legs
// (a leg hit makes a soldier limp); both take the Limb multiplier.
enum class HitRegion : std::uint8_t { Head, Torso, Arm, Leg };
HitZone ZoneOfRegion(HitRegion r);
// The region of a standard skeleton bone, by name ("head" -> Head; "neck_01", "spine_03", "pelvis", "clavicle_l" ->
// Torso; "upperarm_l", "hand_r", fingers -> Arm; "thigh_l", "calf_r", "foot_l", "ball_l" -> Leg). Unknown: Torso.
HitRegion RegionFromBone(const char* bone);
// The region of one of a soldier's eleven hitbox / ragdoll parts (Npc/NpcRagdoll.h: pelvis, spine_03, head, the two
// upper and lower arms, the two thighs and calves, in that order). Out of range: Torso.
HitRegion RegionFromPart(int part);

// The zone from where on a standing capsule a hit landed: `hitY` against the capsule's foot and
// height (the top 14% is the head, below half the legs).
HitZone ZoneFromCapsuleHeight(float hitY, float footY, float height);

// Where a hit came from, as an angle on the screen for the damage indicator: radians clockwise
// from straight ahead (0 = in front, +pi/2 = to the right, pi = behind), from the camera's
// position and yaw (degrees, the engine's convention: yaw -90 looks down -Z).
float DamageIndicatorAngle(const glm::vec3& cameraPos, float cameraYawDeg, const glm::vec3& source);

struct DamageEvent {
    unsigned Source = 0xFFFFFFFFu; // who fired (an NPC entity, or kPlayerEntity)
    unsigned Target = 0xFFFFFFFFu; // who was hit
    float Amount = 0.0f;
    HitZone Zone = HitZone::Torso;
    glm::vec3 Point{0.0f}, Direction{0.0f, 0.0f, -1.0f}; // where it struck, and the round's travel
    glm::vec3 SourcePos{0.0f};                           // where it was fired from
};

class DamageQueue {
public:
    void Push(const DamageEvent& e) { if (m_Events.size() < 1024) m_Events.push_back(e); }
    std::vector<DamageEvent> Drain() { std::vector<DamageEvent> out; out.swap(m_Events); return out; }
    void Clear() { m_Events.clear(); }
private:
    std::vector<DamageEvent> m_Events;
};
