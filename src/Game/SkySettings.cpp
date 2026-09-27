#include "SkySettings.h"
#include "TimeOfDay.h"

#include <cmath>

bool SkySettings::operator==(const SkySettings& o) const {
#define SKY_EQ(f) && f == o.f
    return true SKY_SETTINGS_FIELDS(SKY_EQ);
#undef SKY_EQ
}

const char* SkyPresetName(SkyPreset preset) {
    switch (preset) {
    case SkyPreset::Custom:       return "Custom";
    case SkyPreset::ClearDay:     return "Clear Day";
    case SkyPreset::FairWeather:  return "Fair Weather";
    case SkyPreset::PartlyCloudy: return "Partly Cloudy";
    case SkyPreset::Overcast:     return "Overcast";
    case SkyPreset::Storm:        return "Storm";
    case SkyPreset::GoldenHour:   return "Golden Hour";
    case SkyPreset::Sunset:       return "Sunset";
    case SkyPreset::Twilight:     return "Twilight";
    case SkyPreset::Night:        return "Moonlit Night";
    case SkyPreset::Hazy:         return "Hazy";
    case SkyPreset::Alien:        return "Alien World";
    default:                      return "?";
    }
}

void ApplySkyPreset(SkySettings& s, SkyPreset preset) {
    if (preset == SkyPreset::Custom || preset == SkyPreset::Count) return;

    // Start from Earth defaults, keeping where and how the scene looks at the sky: location,
    // compass, clock behaviour, quality and the scene-scale settings aren't part of a "look".
    const SkySettings keep = s;
    s = SkySettings{};
    s.TimeOfDayEnabled = keep.TimeOfDayEnabled;
    s.DayOfYear = keep.DayOfYear;
    s.LatitudeDegrees = keep.LatitudeDegrees;
    s.NorthOffsetDegrees = keep.NorthOffsetDegrees;
    s.DayLengthMinutes = keep.DayLengthMinutes;
    s.AnimateInEditor = keep.AnimateInEditor;
    s.AtmosphereTintsSun = keep.AtmosphereTintsSun;
    s.AerialPerspectiveScale = keep.AerialPerspectiveScale;
    s.SeaLevel = keep.SeaLevel;
    s.AltitudeOffsetMeters = keep.AltitudeOffsetMeters;
    s.CloudQuality = keep.CloudQuality;
    s.CloudShadows = keep.CloudShadows;

    const float sunset = TimeOfDay::SunsetHour(s.DayOfYear, s.LatitudeDegrees);
    const bool hasSunset = sunset > 12.01f && sunset < 23.99f;

    switch (preset) {
    case SkyPreset::ClearDay:
        s.TimeOfDayHours = 11.5f;
        s.CloudCoverage = 0.12f;
        s.CloudType = 0.4f;
        s.CirrusCoverage = 0.2f;
        s.MieScale = 0.7f;
        break;
    case SkyPreset::FairWeather:
        s.TimeOfDayHours = 10.0f;
        break;
    case SkyPreset::PartlyCloudy:
        s.TimeOfDayHours = 14.0f;
        s.CloudCoverage = 0.62f;
        s.CloudType = 0.55f;
        s.CloudDensity = 1.15f;
        s.CirrusCoverage = 0.4f;
        s.CloudWindSpeed = 16.0f;
        break;
    case SkyPreset::Overcast:
        s.TimeOfDayHours = 12.5f;
        s.CloudCoverage = 0.97f;
        s.CloudType = 0.12f;
        s.CloudDensity = 1.4f;
        s.CloudDetail = 0.2f;
        s.CloudBaseMeters = 1100.0f;
        s.CloudThicknessMeters = 2200.0f;
        s.CloudAmbient = 1.3f;
        s.CirrusCoverage = 0.0f;
        s.MieScale = 2.0f;
        s.CloudShadowStrength = 0.7f;
        break;
    case SkyPreset::Storm:
        s.TimeOfDayHours = 15.5f;
        s.CloudCoverage = 1.0f;
        s.CloudType = 0.9f;
        s.CloudDensity = 2.4f;
        s.CloudDetail = 0.45f;
        s.CloudBaseMeters = 800.0f;
        s.CloudThicknessMeters = 7000.0f;
        s.CloudWindSpeed = 28.0f;
        s.CloudWindShear = 1.5f;
        s.CloudAmbient = 0.8f;
        s.CirrusCoverage = 0.0f;
        s.MieScale = 3.5f;
        s.CloudShadowStrength = 0.95f;
        break;
    case SkyPreset::GoldenHour:
        s.TimeOfDayHours = hasSunset ? sunset - 0.9f : 18.0f;
        s.CloudCoverage = 0.38f;
        s.CloudType = 0.5f;
        s.CirrusCoverage = 0.45f;
        s.MieScale = 1.4f;
        break;
    case SkyPreset::Sunset:
        s.TimeOfDayHours = hasSunset ? sunset - 0.12f : 19.0f;
        s.CloudCoverage = 0.42f;
        s.CloudType = 0.35f;
        s.CirrusCoverage = 0.6f;
        s.MieScale = 1.8f;
        s.CloudForwardScattering = 0.85f;
        break;
    case SkyPreset::Twilight:
        s.TimeOfDayHours = hasSunset ? sunset + 0.55f : 20.0f;
        s.CloudCoverage = 0.3f;
        s.CirrusCoverage = 0.5f;
        s.MieScale = 1.2f;
        break;
    case SkyPreset::Night: {
        s.TimeOfDayHours = 0.5f;
        s.CloudCoverage = 0.25f;
        s.CirrusCoverage = 0.2f;
        // Full moon, high at midnight.
        const float base = TimeOfDay::MoonPhase(s.DayOfYear, s.TimeOfDayHours, 0.0f);
        s.MoonPhaseOffset = 0.5f - base - std::floor(0.5f - base);
        s.MoonBrightness = 0.005f;
        s.StarsBrightness = 1.2f;
        break;
    }
    case SkyPreset::Hazy:
        s.TimeOfDayHours = 15.0f;
        s.MieScale = 5.0f;
        s.MieAbsorption = 2.5f;
        s.MieHeightKm = 2.0f;
        s.CloudCoverage = 0.25f;
        s.CloudType = 0.3f;
        s.CirrusCoverage = 0.0f;
        s.GroundAlbedo = glm::vec3(0.35f, 0.32f, 0.28f);
        break;
    case SkyPreset::Alien:
        s.TimeOfDayHours = 16.5f;
        s.RayleighTint = glm::vec3(0.45f, 1.0f, 0.62f);
        s.RayleighScale = 1.3f;
        s.MieScale = 2.0f;
        s.OzoneScale = 0.2f;
        s.GroundAlbedo = glm::vec3(0.35f, 0.2f, 0.3f);
        s.CloudCoverage = 0.5f;
        s.CloudType = 0.75f;
        s.CloudScale = 1.6f;
        s.CirrusCoverage = 0.5f;
        s.MoonDiscScale = 4.0f;
        s.MoonBrightness = 0.01f;
        break;
    default:
        break;
    }
}
