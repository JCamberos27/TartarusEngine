#pragma once

#include <glm/glm.hpp>

#include <cstdint>

struct FirstPersonWeaponGameplay;

// Damage between the player and the dummies: pure maths (unit tested).

enum class HitZone : std::uint8_t { Head, Torso, Limb };

// 1 up to `start` metres, easing linearly down to `minScale` at `end` and beyond.
float DamageFalloff(float distance, float start, float end, float minScale);

// One round (or pellet) of `weapon` landing on `zone` from `distance` metres away.
float DamageForHit(const FirstPersonWeaponGameplay& weapon, HitZone zone, float distance);

// Where on the body a hit landed, finer than the zone: what the hitboxes report. Limbs split into arms and legs
// (a leg hit makes a soldier limp); both take the Limb multiplier.
enum class HitRegion : std::uint8_t { Head, Torso, Arm, Leg };
HitZone ZoneOfRegion(HitRegion r);
// The region of one of a soldier's eleven hitbox / ragdoll parts (Npc/NpcRagdoll.h: pelvis, spine_03, head, the two
// upper and lower arms, the two thighs and calves, in that order). Out of range: Torso.
HitRegion RegionFromPart(int part);

// The zone from where on a standing capsule a hit landed: `hitY` against the capsule's foot and
// height (the top 14% is the head, below half the legs).
HitZone ZoneFromCapsuleHeight(float hitY, float footY, float height);
