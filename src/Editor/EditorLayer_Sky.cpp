// Lighting > Environment > Physical: the physical sky's settings (World::Sky, SkySettings.h).
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "SkySettings.h"
#include "TimeOfDay.h"
#include "World.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
const char* kMonthNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const int kMonthStart[12] = {1, 32, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};

// "14 Jun" for a day of the year.
void FormatDate(float dayOfYear, char* out, size_t n) {
    const int d = std::clamp((int)std::lround(dayOfYear), 1, 365);
    int m = 11;
    while (m > 0 && d < kMonthStart[m]) --m;
    std::snprintf(out, n, "%d %s", d - kMonthStart[m] + 1, kMonthNames[m]);
}

void FormatClock(float hours, char* out, size_t n) {
    hours = hours - 24.0f * std::floor(hours / 24.0f);
    const int total = (int)std::floor(hours * 60.0f + 0.5f) % (24 * 60);
    std::snprintf(out, n, "%02d:%02d", total / 60, total % 60);
}
} // namespace

void EditorLayer::DrawPhysicalSkySettings(World& world, float w) {
    SkySettings& s = world.Sky;

    // One slider row with the undo step taken when the drag starts (EditorUI::SliderFloat's
    // out-param, not IsItemActivated - see DrawEnvironmentSettings).
    auto slider = [&](const char* label, float* v, float lo, float hi, const char* fmt, const char* tip,
                      ImGuiSliderFlags flags = 0) {
        ImGui::SetNextItemWidth(w);
        bool activated = false;
        EditorUI::SliderFloat(label, v, lo, hi, fmt, flags, &activated);
        if (activated) PushUndo(world, "Edit Sky");
        if (ImGui::IsItemHovered() && tip) EditorUI::SetTooltip("%s", tip);
    };
    auto checkbox = [&](const char* label, bool* v, const char* tip) {
        if (EditorUIPrimitives::Checkbox(label, v)) PushUndo(world, "Edit Sky");
        if (ImGui::IsItemHovered() && tip) EditorUI::SetTooltip("%s", tip);
    };
    auto color = [&](const char* label, glm::vec3* v, const char* tip) {
        EditorUI::ColorEditLinear(label, &v->x, ImGuiColorEditFlags_DisplayHex);
        if (ImGui::IsItemActivated()) PushUndo(world, "Edit Sky");
        if (ImGui::IsItemHovered() && tip) EditorUI::SetTooltip("%s", tip);
    };
    // Every section open by default the first time, remembered by ImGui after that.
    auto section = [](const char* label) {
        ImGui::Spacing();
        return ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
    };

    // --- Presets ---
    {
        static int preset = 0;
        const char* preview = SkyPresetName((SkyPreset)preset);
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("Preset", preview)) {
            for (int i = 1; i < (int)SkyPreset::Count; ++i) {
                if (ImGui::Selectable(SkyPresetName((SkyPreset)i), preset == i)) {
                    PushUndo(world, std::string("Sky Preset: ") + SkyPresetName((SkyPreset)i));
                    ApplySkyPreset(s, (SkyPreset)i);
                    preset = i;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Sets the atmosphere, clouds and time of day to a named look.\n"
                                 "Location, compass, clock speed and quality are kept.");
    }

    // --- Time of day ---
    if (section("Time of Day")) {
        checkbox("Drive the sun from the clock", &s.TimeOfDayEnabled,
                 "On: the sun and moon are placed from the time, date and latitude, and the\n"
                 "scene's directional light follows them (its own rotation is ignored).\n"
                 "Off: the directional light's rotation is the sun, as with the other skies.");
        if (s.TimeOfDayEnabled) {
            char clock[16];
            FormatClock(s.TimeOfDayHours, clock, sizeof(clock));
            char fmt[48];
            std::snprintf(fmt, sizeof(fmt), "%s", clock);
            slider("Time", &s.TimeOfDayHours, 0.0f, 24.0f, fmt, "Local solar time: 12:00 is when the sun is highest.");
            char date[16];
            FormatDate(s.DayOfYear, date, sizeof(date));
            slider("Date", &s.DayOfYear, 1.0f, 365.0f, date,
                   "Day of the year: sets the sun's height through the seasons and the moon's phase.");
            slider("Latitude", &s.LatitudeDegrees, -90.0f, 90.0f, "%.1f deg",
                   "Where on Earth: 0 = the equator, 90 = the north pole. Changes how high the sun\n"
                   "climbs, how long the days are, and where the stars turn.");
            slider("North", &s.NorthOffsetDegrees, -180.0f, 180.0f, "%.0f deg",
                   "Rotates the compass around the scene. 0 = north is world -Z and east is +X.");
            const float sunset = TimeOfDay::SunsetHour(s.DayOfYear, s.LatitudeDegrees);
            if (sunset >= 23.99f) ImGui::TextDisabled("Polar day: the sun doesn't set.");
            else if (sunset <= 12.01f) ImGui::TextDisabled("Polar night: the sun doesn't rise.");
            else {
                char rise[16], set[16];
                FormatClock(24.0f - sunset, rise, sizeof(rise));
                FormatClock(sunset, set, sizeof(set));
                ImGui::TextDisabled("Sunrise %s, sunset %s", rise, set);
            }
            slider("Day length", &s.DayLengthMinutes, 0.0f, 120.0f, s.DayLengthMinutes > 0.0f ? "%.1f min" : "Frozen",
                   "Real minutes for a full 24 hours while the clock runs (in Play, or with\n"
                   "Animate in Editor). 0 keeps the time fixed.");
            if (s.DayLengthMinutes > 0.0f)
                checkbox("Animate in editor", &s.AnimateInEditor,
                         "Run the clock in the editor too, not just in Play. The scene's saved time\n"
                         "doesn't change; the clock resets to it when you turn this off or stop Play.");
            if (m_SkyClockRunning) {
                char now[16];
                FormatClock(m_SkyClockHours, now, sizeof(now));
                ImGui::TextDisabled("Clock is running: %s", now);
            }
        }
    }

    // --- Sun and moon ---
    if (section("Sun, Moon and Stars")) {
        checkbox("Atmosphere colours the sun", &s.AtmosphereTintsSun,
                 "The directional light is filtered through the air: warmer and dimmer toward the\n"
                 "horizon, gone after sunset. Off: it keeps its authored colour and intensity.");
        slider("Sun disc brightness", &s.SunDiscBrightness, 0.0f, 4.0f, "%.2f x",
               "The visible disc only. The scene's light is the directional light's own intensity.");
        slider("Sun disc size", &s.SunDiscScale, 0.2f, 20.0f, "%.2f x",
               "x the directional light's Angular Size (the real sun is 0.53 deg).", ImGuiSliderFlags_Logarithmic);
        checkbox("Moon", &s.MoonEnabled, "Draw the moon, and let it light the scene once the sun has set.");
        if (s.MoonEnabled) {
            slider("Moon brightness", &s.MoonBrightness, 0.0005f, 0.05f, "%.4f",
                   "Full-moon light as a fraction of the sun's. The real ratio (~1:400,000) is far\n"
                   "too dark to play in; this is brightened for readable nights.",
                   ImGuiSliderFlags_Logarithmic);
            slider("Moon phase", &s.MoonPhaseOffset, 0.0f, 1.0f, "%.2f",
                   "Shifts the lunar cycle. 0 follows the date; add 0.5 to swap new and full.");
            slider("Moon size", &s.MoonDiscScale, 0.5f, 10.0f, "%.2f x", "x the real moon's apparent size.",
                   ImGuiSliderFlags_Logarithmic);
        }
        slider("Stars", &s.StarsBrightness, 0.0f, 4.0f, "%.2f x", "Brightness of the stars and the Milky Way.");
        slider("Night glow", &s.NightGlow, 0.0f, 0.2f, "%.3f",
               "Faint airglow near the horizon, so a moonless night isn't pure black.");
        slider("Night brightness", &s.NightBrightness, 0.0f, 8.0f, "%.1f stops",
               "Brightens the sky's light - sky, haze, clouds, moonlight, ambient - by up to this\n"
               "many stops after sunset, like eyes adjusting to the dark. The exposure is left\n"
               "alone, so your own lamps keep their look. 0 = physically dark nights.");
    }

    // --- Atmosphere ---
    if (section("Atmosphere")) {
        slider("Sky intensity", &s.SkyIntensity, 0.0f, 4.0f, "%.2f x",
               "Everything the sky emits: the visible sky, the haze, and the ambient light it gives.");
        slider("Air (Rayleigh)", &s.RayleighScale, 0.0f, 4.0f, "%.2f x",
               "Air molecules: the blue of the sky and the red of sunsets. 1 = Earth.");
        color("Air tint", &s.RayleighTint, "Tints the air's scattering. White = Earth; try other colours for alien skies.");
        slider("Haze (Mie)", &s.MieScale, 0.0f, 20.0f, "%.2f x",
               "Dust, pollution and water droplets: the white glow around the sun and a milky\n"
               "horizon. 1 = a clear day.", ImGuiSliderFlags_Logarithmic);
        slider("Haze direction", &s.MieAnisotropy, 0.0f, 0.99f, "%.2f",
               "How tightly the haze glows around the sun (0 = evenly in every direction).");
        slider("Haze absorption", &s.MieAbsorption, 0.0f, 5.0f, "%.2f x",
               "Smoke and soot: dims the sky without brightening it.");
        slider("Ozone", &s.OzoneScale, 0.0f, 4.0f, "%.2f x", "Deepens the blue of twilight.");
        color("Ground colour", &s.GroundAlbedo, "The planet's surface, which lights the lower sky and shows below the horizon.");
        slider("Distance haze", &s.AerialPerspectiveScale, 0.0f, 200.0f, "%.1f x",
               "Aerial perspective on the scene: distances are multiplied by this before the\n"
               "atmosphere fades them, so a small level can read as a vast landscape.\n"
               "1 = physical, 0 = off.", ImGuiSliderFlags_Logarithmic);
        slider("Sea level", &s.SeaLevel, -1000.0f, 1000.0f, "%.1f m", "World height of the planet's surface.");
        slider("Altitude", &s.AltitudeOffsetMeters, 0.0f, 9000.0f, "%.0f m",
               "Lifts the whole scene: a mountain village at 1500 m sees a darker, clearer sky.");
        if (ImGui::TreeNode("Planet")) {
            slider("Planet radius", &s.PlanetRadiusKm, 100.0f, 20000.0f, "%.0f km", "Earth = 6360 km.",
                   ImGuiSliderFlags_Logarithmic);
            slider("Atmosphere height", &s.AtmosphereHeightKm, 10.0f, 400.0f, "%.0f km", "Earth = 100 km.");
            slider("Air falloff", &s.RayleighHeightKm, 1.0f, 30.0f, "%.1f km", "Height over which the air thins by e. Earth = 8 km.");
            slider("Haze falloff", &s.MieHeightKm, 0.1f, 10.0f, "%.2f km", "Height over which the haze thins by e. Earth = 1.2 km.");
            slider("Multiple scattering", &s.MultipleScattering, 0.0f, 2.0f, "%.2f x",
                   "Light scattered more than once: the softness of the sky's gradient.");
            ImGui::TreePop();
        }
    }

    // --- Clouds ---
    if (section("Volumetric Clouds")) {
        checkbox("Clouds", &s.CloudsEnabled, "Raymarched volumetric clouds with their own lighting and shadows.");
        if (s.CloudsEnabled) {
            slider("Coverage", &s.CloudCoverage, 0.0f, 1.0f, "%.2f", "0 = clear sky, 1 = overcast.");
            slider("Density", &s.CloudDensity, 0.0f, 4.0f, "%.2f x", "Thin and wispy .. dark and heavy.");
            slider("Type", &s.CloudType, 0.0f, 1.0f, "%.2f",
                   "0 = flat stratus sheets, 0.5 = puffy cumulus, 1 = towering cumulonimbus.");
            slider("Base altitude", &s.CloudBaseMeters, 100.0f, 8000.0f, "%.0f m", "Height of the bottom of the cloud layer.");
            slider("Thickness", &s.CloudThicknessMeters, 200.0f, 12000.0f, "%.0f m", "Depth of the cloud layer.");
            slider("Size", &s.CloudScale, 0.25f, 4.0f, "%.2f x", "Size of the cloud formations.", ImGuiSliderFlags_Logarithmic);
            slider("Detail", &s.CloudDetail, 0.0f, 1.0f, "%.2f", "Edge erosion: soft blobs .. ragged, wispy edges.");
            slider("Wind speed", &s.CloudWindSpeed, 0.0f, 80.0f, "%.1f m/s", "How fast the clouds drift. They move in the editor too.");
            slider("Wind direction", &s.CloudWindDirectionDegrees, 0.0f, 360.0f, "%.0f deg",
                   "Compass bearing the wind blows toward (0 = north).");
            slider("Wind shear", &s.CloudWindShear, 0.0f, 4.0f, "%.2f km", "How far cloud tops lean downwind.");
            if (ImGui::TreeNode("Lighting##clouds")) {
                slider("Silver lining", &s.CloudForwardScattering, 0.0f, 0.95f, "%.2f",
                       "How brightly cloud edges glow when you look toward the sun.");
                slider("Sky light", &s.CloudAmbient, 0.0f, 3.0f, "%.2f x", "Sky light on the clouds: how bright their shaded sides are.");
                slider("Dark edges", &s.CloudPowder, 0.0f, 1.0f, "%.2f",
                       "The \"powder\" effect: darker rims on clouds lit from behind you.");
                slider("Inner glow", &s.CloudMultiScattering, 0.0f, 1.5f, "%.2f",
                       "Light scattered many times inside the cloud: bright, soft interiors.");
                slider("Horizon fade", &s.CloudHorizonHaze, 0.0f, 0.2f, "%.3f",
                       "How quickly distant clouds fade into the sky's colour.");
                ImGui::TreePop();
            }
            ImGui::SeparatorText("Cirrus");
            slider("Cirrus coverage", &s.CirrusCoverage, 0.0f, 1.0f, "%.2f", "High, thin ice cloud streaked by the wind.");
            if (s.CirrusCoverage > 0.0f) {
                slider("Cirrus altitude", &s.CirrusAltitudeMeters, 4000.0f, 15000.0f, "%.0f m", "Height of the cirrus sheet.");
                slider("Cirrus size", &s.CirrusScale, 0.25f, 4.0f, "%.2f x", "Size of the cirrus streaks.",
                       ImGuiSliderFlags_Logarithmic);
            }
            ImGui::SeparatorText("Shadows and quality");
            checkbox("Cloud shadows", &s.CloudShadows, "Clouds shade the sun (or moon) on the scene below them.");
            if (s.CloudShadows)
                slider("Shadow strength", &s.CloudShadowStrength, 0.0f, 1.0f, "%.2f", "0 = no shadows, 1 = fully dark under thick cloud.");
            {
                static const char* kQualities[] = {"Low", "Medium", "High", "Ultra"};
                ImGui::SetNextItemWidth(w);
                int q = std::clamp(s.CloudQuality, 0, 3);
                if (ImGui::Combo("Quality", &q, kQualities, IM_ARRAYSIZE(kQualities))) {
                    PushUndo(world, "Edit Sky");
                    s.CloudQuality = q;
                }
                if (ImGui::IsItemHovered())
                    EditorUI::SetTooltip("Low: quarter resolution. Medium: half resolution. High: half resolution,\n"
                                         "more steps. Ultra: full resolution. Higher costs more GPU time.");
            }
            slider("Weather seed", &s.CloudSeed, 0.0f, 100.0f, "%.1f", "Shifts the pattern of where clouds form.");
        }
    }
}
