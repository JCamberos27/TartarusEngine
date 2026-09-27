#include "TimeOfDay.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace TimeOfDay {
namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kSynodicMonthDays = 29.530588f;

// Local east/up/south frame -> world, with the compass rotated about +Y.
glm::vec3 LocalToWorld(float east, float up, float north, float northOffsetDeg) {
    const glm::vec3 local(east, up, -north); // north = -Z, east = +X
    const float a = glm::radians(northOffsetDeg);
    const float c = std::cos(a), s = std::sin(a);
    return glm::normalize(glm::vec3(c * local.x + s * local.z, local.y, -s * local.x + c * local.z));
}

// A body at declination `dec` and hour angle `hourAngle` (radians), seen from latitude `phi`.
glm::vec3 CelestialToWorld(float dec, float hourAngle, float phi, float northOffsetDeg) {
    const float east = -std::cos(dec) * std::sin(hourAngle);
    const float north = std::sin(dec) * std::cos(phi) - std::cos(dec) * std::cos(hourAngle) * std::sin(phi);
    const float up = std::sin(dec) * std::sin(phi) + std::cos(dec) * std::cos(hourAngle) * std::cos(phi);
    return LocalToWorld(east, up, north, northOffsetDeg);
}

float Frac(float x) { return x - std::floor(x); }
} // namespace

float SolarDeclination(float dayOfYear) {
    // Cooper's approximation: -23.44 deg * cos(2 pi (N + 10) / 365).
    return glm::radians(-23.44f) * std::cos(2.0f * kPi * (dayOfYear + 10.0f) / 365.0f);
}

glm::vec3 SunDirection(float hours, float dayOfYear, float latitudeDeg, float northOffsetDeg) {
    const float dec = SolarDeclination(dayOfYear);
    const float hourAngle = glm::radians(15.0f * (hours - 12.0f));
    const float phi = glm::radians(std::clamp(latitudeDeg, -90.0f, 90.0f));
    return CelestialToWorld(dec, hourAngle, phi, northOffsetDeg);
}

float MoonPhase(float dayOfYear, float hours, float phaseOffset) {
    return Frac((dayOfYear + hours / 24.0f) / kSynodicMonthDays + phaseOffset);
}

float MoonIllumination(float phase) { return 0.5f * (1.0f - std::cos(2.0f * kPi * phase)); }

glm::vec3 MoonDirection(float hours, float dayOfYear, float latitudeDeg, float northOffsetDeg, float phase) {
    const float sunDec = SolarDeclination(dayOfYear);
    // Near the sun's declination at new moon, opposite it at full moon, plus the moon's own
    // 5.1 deg orbital tilt cycling over the draconic month.
    const float dec = sunDec * std::cos(2.0f * kPi * phase) +
                      glm::radians(5.14f) * std::sin(2.0f * kPi * dayOfYear / 27.21f);
    const float hourAngle = glm::radians(15.0f * (hours - 12.0f)) - 2.0f * kPi * phase;
    const float phi = glm::radians(std::clamp(latitudeDeg, -90.0f, 90.0f));
    return CelestialToWorld(dec, hourAngle, phi, northOffsetDeg);
}

glm::mat3 StarRotation(float hours, float dayOfYear, float latitudeDeg, float northOffsetDeg) {
    const float phi = glm::radians(std::clamp(latitudeDeg, -90.0f, 90.0f));
    // The north celestial pole: due north, as high above the horizon as the latitude.
    const glm::vec3 pole = LocalToWorld(0.0f, std::sin(phi), std::cos(phi), northOffsetDeg);
    // Sidereal angle: one turn per sidereal day (the extra turn per year is the date term).
    const float angle = 2.0f * kPi * (hours / 24.0f + (dayOfYear - 80.0f) / 365.25f);
    return glm::mat3(glm::rotate(glm::mat4(1.0f), angle, pole));
}

float SunsetHour(float dayOfYear, float latitudeDeg) {
    const float dec = SolarDeclination(dayOfYear);
    const float phi = glm::radians(std::clamp(latitudeDeg, -89.9f, 89.9f));
    const float cosH0 = -std::tan(phi) * std::tan(dec);
    if (cosH0 <= -1.0f) return 24.0f; // polar day: the sun never sets
    if (cosH0 >= 1.0f) return 12.0f;  // polar night
    return 12.0f + glm::degrees(std::acos(cosH0)) / 15.0f;
}

} // namespace TimeOfDay
