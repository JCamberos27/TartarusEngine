#include "Damage.h"

#include "FirstPersonAnimation.h"

#include <algorithm>
#include <cmath>
#include <string>

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

HitRegion RegionFromBone(const char* bone) {
    if (!bone) return HitRegion::Torso;
    const std::string b(bone);
    auto has = [&](const char* s) { return b.find(s) != std::string::npos; };
    if (b == "head" || has("jaw") || has("eye")) return HitRegion::Head;
    if (has("upperarm") || has("lowerarm") || has("hand") || has("index") || has("middle") || has("ring") || has("pinky") ||
        has("thumb") || has("twist"))
        return has("thigh") || has("calf") ? HitRegion::Leg : HitRegion::Arm;
    if (has("thigh") || has("calf") || has("foot") || has("ball") || has("toe")) return HitRegion::Leg;
    return HitRegion::Torso; // pelvis, spine, neck, clavicles
}

HitRegion RegionFromPart(int part) {
    if (part == 2) return HitRegion::Head;
    if (part >= 3 && part <= 6) return HitRegion::Arm;
    if (part >= 7 && part <= 10) return HitRegion::Leg;
    return HitRegion::Torso;
}

float DamageIndicatorAngle(const glm::vec3& cameraPos, float cameraYawDeg, const glm::vec3& source) {
    const float yaw = glm::radians(cameraYawDeg);
    const glm::vec2 fwd(std::cos(yaw), std::sin(yaw));     // (x, z) - Camera::Front without pitch
    const glm::vec2 right(-std::sin(yaw), std::cos(yaw));  // Camera::Right
    const glm::vec2 d(source.x - cameraPos.x, source.z - cameraPos.z);
    if (glm::dot(d, d) < 1e-8f) return 0.0f;
    return std::atan2(glm::dot(d, right), glm::dot(d, fwd));
}
