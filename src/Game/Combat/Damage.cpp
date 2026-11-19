#include "Damage.h"

#include "FirstPersonAnimation.h"

#include <algorithm>
#include <cmath>

float DamageFalloff(float distance, float start, float end, float minScale) {
    minScale = std::clamp(minScale, 0.0f, 1.0f);
    if (!(distance > start)) return 1.0f;
    if (!(end > start) || distance >= end) return minScale;
    const float t = (distance - start) / (end - start);
    return 1.0f + (minScale - 1.0f) * t;
}

float DamageForHit(const FirstPersonWeaponGameplay& w, HitZone zone, float distance) {
    const float zoneScale = zone == HitZone::Head ? w.HeadMultiplier : zone == HitZone::Limb ? w.LimbMultiplier : 1.0f;
    return std::max(0.0f, w.Damage * zoneScale * DamageFalloff(distance, w.FalloffStart, w.FalloffEnd, w.FalloffMin));
}

HitZone ZoneFromCapsuleHeight(float hitY, float footY, float height) {
    if (height <= 0.0f) return HitZone::Torso;
    const float t = (hitY - footY) / height;
    if (t > 0.86f) return HitZone::Head;
    if (t < 0.5f) return HitZone::Limb;
    return HitZone::Torso;
}

HitZone ZoneOfRegion(HitRegion r) {
    switch (r) {
    case HitRegion::Head: return HitZone::Head;
    case HitRegion::Torso: return HitZone::Torso;
    case HitRegion::Arm:
    case HitRegion::Leg: return HitZone::Limb;
    }
    return HitZone::Torso;
}

HitRegion RegionFromPart(int part) {
    if (part == 2) return HitRegion::Head;
    if (part >= 3 && part <= 6) return HitRegion::Arm;
    if (part >= 7 && part <= 10) return HitRegion::Leg;
    return HitRegion::Torso;
}
