#pragma once
#include <glm/glm.hpp>

// Scene-authored settings for the physical sky (World::SkySource::Atmosphere): the atmosphere,
// the volumetric clouds, the time of day and the night sky. Stored on World (World::Sky),
// saved with the scene, edited in Lighting > Environment. Rendering lives in
// Renderer/SkyAtmosphere + Renderer/VolumetricClouds; the sun/moon maths in TimeOfDay.h.
struct SkySettings {
    // --- Time of day -------------------------------------------------------------------------
    // On: the sun (and moon) are placed from the clock, date and latitude, and the scene's first
    // directional light follows them - its own rotation is ignored. Off: the directional light's
    // rotation is the sun, as with the other sky modes.
    bool  TimeOfDayEnabled = true;
    float TimeOfDayHours = 10.0f;     // local solar time, 0..24
    float DayOfYear = 172.0f;         // 1..365; 172 = the June solstice
    float LatitudeDegrees = 38.0f;    // -90 (south pole) .. 90 (north pole)
    float NorthOffsetDegrees = 0.0f;  // compass rotation: 0 = north is world -Z, east is +X
    float DayLengthMinutes = 0.0f;    // real minutes per 24 h while the clock runs; 0 = frozen
    bool  AnimateInEditor = false;    // also run the clock outside Play

    // --- Sun and moon ------------------------------------------------------------------------
    // Filter the directional light through the atmosphere: warmer and dimmer toward the horizon,
    // gone after sunset. Off = the light keeps its authored colour and intensity.
    bool  AtmosphereTintsSun = true;
    float SunDiscBrightness = 1.0f;   // visible disc only; the scene lighting is the light's own
    float SunDiscScale = 1.0f;        // x the light's Angular Size
    bool  MoonEnabled = true;
    float MoonBrightness = 0.004f;    // full-moon light as a fraction of the sun's (real: ~2e-6; brightened for play)
    float MoonPhaseOffset = 0.0f;     // 0..1 shifts the lunar cycle (0 = follows the date)
    float MoonDiscScale = 1.0f;
    float StarsBrightness = 1.0f;
    float NightGlow = 0.02f;          // faint airglow so a moonless night isn't pure black
    // Brightens everything the sky lights - sky, haze, clouds, moonlight, ambient - by up to this
    // many stops as the sun goes down, the way eyes adjust to the dark, so dusk and moonlit
    // scenes stay readable. The exposure is untouched, so the scene's own lamps and the editor's
    // overlays keep their look. 0 = physical (and very dark at night).
    float NightBrightness = 5.0f;

    // --- Atmosphere --------------------------------------------------------------------------
    // Multipliers on Earth's measured coefficients (Hillaire 2020 / Bruneton 2017 values), so
    // 1.0 everywhere is a clear day on Earth.
    float SkyIntensity = 1.0f;        // x everything the sky emits (visible sky, ambient, haze)
    float RayleighScale = 1.0f;       // air: blue sky, red sunsets
    glm::vec3 RayleighTint{1.0f};     // tints the air's scattering (alien skies)
    float MieScale = 1.0f;            // aerosols / haze: bright glow around the sun, milky horizon
    float MieAnisotropy = 0.8f;       // how forward the haze scatters (0..0.99)
    float MieAbsorption = 1.0f;       // soot/dust: dims without brightening
    float OzoneScale = 1.0f;          // ozone deepens twilight blue
    glm::vec3 GroundAlbedo{0.3f};     // the planet's surface, lighting the lower sky
    float PlanetRadiusKm = 6360.0f;
    float AtmosphereHeightKm = 100.0f;
    float RayleighHeightKm = 8.0f;
    float MieHeightKm = 1.2f;
    float MultipleScattering = 1.0f;  // strength of the multiple-scattering term
    // Haze on the scene: distances are multiplied by this before the atmosphere is applied, so a
    // small level can read as a large landscape. 1 = physical; 0 = no aerial perspective.
    float AerialPerspectiveScale = 1.0f;
    float SeaLevel = 0.0f;            // world Y of the planet's surface
    float AltitudeOffsetMeters = 0.0f; // lifts the whole scene (a mountain village at 1500 m)

    // --- Volumetric clouds -------------------------------------------------------------------
    bool  CloudsEnabled = true;
    float CloudCoverage = 0.45f;      // 0 clear .. 1 overcast
    float CloudDensity = 1.0f;        // x extinction: thin and wispy .. dark and heavy
    float CloudType = 0.45f;          // 0 stratus sheets, 0.5 cumulus, 1 towering cumulonimbus
    float CloudBaseMeters = 1500.0f;  // altitude of the layer's base above sea level
    float CloudThicknessMeters = 3000.0f;
    float CloudScale = 1.0f;          // x the size of cloud formations
    float CloudDetail = 0.45f;        // edge erosion: 0 soft blobs .. 1 ragged wisps
    float CloudWindSpeed = 12.0f;     // m/s
    float CloudWindDirectionDegrees = 45.0f; // the wind blows TOWARD this compass bearing
    float CloudWindShear = 0.6f;      // how far tops lean downwind (km over the layer)
    float CloudForwardScattering = 0.8f; // silver lining when looking toward the sun (0..0.95)
    float CloudAmbient = 1.0f;        // x sky light on the clouds
    float CloudPowder = 0.35f;        // dark-edge "powder" effect
    float CloudMultiScattering = 1.0f; // bright, soft interiors
    float CloudHorizonHaze = 0.02f;   // 1/km: how fast distant clouds fade into the sky
    float CloudSeed = 0.0f;           // shifts the weather pattern
    bool  CloudShadows = true;
    float CloudShadowStrength = 0.8f;
    int   CloudQuality = 1;           // 0 Low, 1 Medium, 2 High, 3 Ultra

    // High, thin ice cloud above the main layer.
    float CirrusCoverage = 0.3f;
    float CirrusAltitudeMeters = 9000.0f;
    float CirrusScale = 1.0f;

    bool operator==(const SkySettings& o) const;
    bool operator!=(const SkySettings& o) const { return !(*this == o); }
};

// Every SkySettings field, for the comparison in SkySettings.cpp. Add new fields here too.
#define SKY_SETTINGS_FIELDS(X)     X(TimeOfDayEnabled) \
    X(TimeOfDayHours) \
    X(DayOfYear) \
    X(LatitudeDegrees) \
    X(NorthOffsetDegrees) \
    X(DayLengthMinutes) \
    X(AnimateInEditor) \
    X(AtmosphereTintsSun) \
    X(SunDiscBrightness) \
    X(SunDiscScale) \
    X(MoonEnabled) \
    X(MoonBrightness) \
    X(MoonPhaseOffset) \
    X(MoonDiscScale) \
    X(StarsBrightness) \
    X(NightGlow) \
    X(NightBrightness) \
    X(SkyIntensity) \
    X(RayleighScale) \
    X(RayleighTint) \
    X(MieScale) \
    X(MieAnisotropy) \
    X(MieAbsorption) \
    X(OzoneScale) \
    X(GroundAlbedo) \
    X(PlanetRadiusKm) \
    X(AtmosphereHeightKm) \
    X(RayleighHeightKm) \
    X(MieHeightKm) \
    X(MultipleScattering) \
    X(AerialPerspectiveScale) \
    X(SeaLevel) \
    X(AltitudeOffsetMeters) \
    X(CloudsEnabled) \
    X(CloudCoverage) \
    X(CloudDensity) \
    X(CloudType) \
    X(CloudBaseMeters) \
    X(CloudThicknessMeters) \
    X(CloudScale) \
    X(CloudDetail) \
    X(CloudWindSpeed) \
    X(CloudWindDirectionDegrees) \
    X(CloudWindShear) \
    X(CloudForwardScattering) \
    X(CloudAmbient) \
    X(CloudPowder) \
    X(CloudMultiScattering) \
    X(CloudHorizonHaze) \
    X(CloudSeed) \
    X(CloudShadows) \
    X(CloudShadowStrength) \
    X(CloudQuality) \
    X(CirrusCoverage) \
    X(CirrusAltitudeMeters) \
    X(CirrusScale)


// Named looks for the physical sky: each sets the atmosphere, clouds and time of day together.
// Custom is the "no preset" entry; applying it does nothing.
enum class SkyPreset {
    Custom = 0,
    ClearDay,
    FairWeather,
    PartlyCloudy,
    Overcast,
    Storm,
    GoldenHour,
    Sunset,
    Twilight,
    Night,
    Hazy,
    Alien,
    Count
};
const char* SkyPresetName(SkyPreset preset);
// Overwrites `s` with the preset's values (keeps anything the preset doesn't mention, such as
// latitude and the compass, at the value it had).
void ApplySkyPreset(SkySettings& s, SkyPreset preset);
