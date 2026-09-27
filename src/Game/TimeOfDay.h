#pragma once
#include <cmath>
#include <glm/glm.hpp>

// Sun, moon and star-field placement for the physical sky's clock (SkySettings time of day).
// World convention: +Y up, north = -Z and east = +X before NorthOffsetDegrees rotates the
// compass around +Y. All directions point FROM the viewer TOWARD the body.
namespace TimeOfDay {

// Solar declination (radians) for a day of the year (1..365): +23.44 deg at the June solstice.
float SolarDeclination(float dayOfYear);

// Direction toward the sun for local solar time `hours` (0..24; 12 = noon), the date and the
// observer's latitude, rotated by the compass offset.
glm::vec3 SunDirection(float hours, float dayOfYear, float latitudeDeg, float northOffsetDeg);

// Lunar phase in [0, 1): 0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter. Follows the
// synodic month from the date and time, shifted by `phaseOffset`.
float MoonPhase(float dayOfYear, float hours, float phaseOffset);

// Fraction of the moon's disc that is lit for a phase (0 new .. 1 full).
float MoonIllumination(float phase);

// Direction toward the moon. Approximation good enough for a sky: the moon trails the sun by
// its phase angle along the same diurnal path, with its declination opposite at full moon.
glm::vec3 MoonDirection(float hours, float dayOfYear, float latitudeDeg, float northOffsetDeg,
                        float phase);

// Rotation taking a world direction into the fixed star frame: the sky turns about the
// celestial pole once per sidereal day.
glm::mat3 StarRotation(float hours, float dayOfYear, float latitudeDeg, float northOffsetDeg);

// Local solar time (hours) of sunset for the date and latitude: 12 + the sunset hour angle.
// Clamped to 24 in polar day and 12 in polar night. Sunrise is 24 minus this.
float SunsetHour(float dayOfYear, float latitudeDeg);

// Elevation (radians) of a direction above the horizon.
inline float Elevation(const glm::vec3& dir) { return std::asin(glm::clamp(dir.y, -1.0f, 1.0f)); }

} // namespace TimeOfDay
