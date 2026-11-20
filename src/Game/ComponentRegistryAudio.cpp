#include "ComponentRegistryInternal.h"

#include "Components.h"
#include "ReflectAttributes.h" // vInspector attributes (Attr)

#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <random>

namespace ComponentRegistry {

namespace {
// 3D distance fields of the audio components that recipes/mix.json "distance" (the manifest's mix.distance) now decides: the engine
// still reads them when no manifest is loaded, the inspector does not offer them.
bool MixOwnsDistanceField(const char* name) {
    for (const char* n : {"Shot Min Distance", "Bass Min Distance", "Event Min Distance", "Event Max Distance", "Max Distance", "Far Max Distance", "NPC Step Min Distance", "NPC Step Max Distance", "Casing Min Distance", "Casing Max Distance", "Impact Min Distance", "Impact Max Distance", "Flyby Min Distance", "Flyby Max Distance"})
        if (std::strcmp(name, n) == 0) return true;
    return false;
}
} // namespace

// Reverb zones, portals and the bus, the audio mix, ambience beds, weapon, foley and impact audio.
void RegisterAudioComponents() {
    using T = ReflectFieldType;

    {
        ReflectComponent m;
        m.Name = "Reverb Zone"; m.Icon = ICON_FA_VOLUME_HIGH; m.Category = "Audio";
        m.Tooltip = "A space for sound. Inside this box / sphere a gunshot's tail is the Tail Class's, the runtime reverb convolves with the zone's\n"
                    "impulse response and the zone's Ambience bed plays (all crossfaded over Fade Distance at the edge); outside every zone the\n"
                    "shooter's raycast probe decides. The higher Priority sits on top where zones overlap. The entity's position and rotation\n"
                    "place it (Scale is ignored).";
        using Z = ReverbZoneComponent;
        m.Fields = {
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(Z, Enabled), 0.0f, "Off: the zone is ignored." },
            { "Shape", T::Enum, TARTARUS_REFLECT_FIELD(Z, Shape), 0.0f, "Box (half extents) or sphere (radius)." },
            { "Extents", T::Vec3, TARTARUS_REFLECT_FIELD(Z, Extents), 0.1f, "Box half extents in metres (the box is 2x this on each axis).", 0.05f, 500.0f },
            { "Radius", T::Float, TARTARUS_REFLECT_FIELD(Z, Radius), 0.1f, "Sphere radius in metres.", 0.05f, 500.0f },
            { "Priority", T::Int, TARTARUS_REFLECT_FIELD(Z, Priority), 1.0f, "Overlapping zones: the higher priority sits on top of the lower.", -100.0f, 100.0f },
            { "Fade Distance", T::Float, TARTARUS_REFLECT_FIELD(Z, FadeDistance), 0.05f, "Metres inside the edge over which the zone fades in (0 = a hard edge). The tails, the reverb and the ambience crossfade over it.", 0.0f, 100.0f },
            { "Tail Class", T::Enum, TARTARUS_REFLECT_FIELD(Z, TailClass), 0.0f, "Which gunshot tail set plays inside, and which class impulse response (manifest ir.<class>) and calibrated wet level the reverb uses. A class with no recorded files plays the generic fire_tail." },
            { "Tail Gain", T::Float, TARTARUS_REFLECT_FIELD(Z, TailGain), 0.01f, "Gain on the shot's tail layer while inside this zone.", 0.0f, 4.0f },
            { "Reverb Mode", T::Enum, TARTARUS_REFLECT_FIELD(Z, ReverbMode), 0.0f, "Class Default: the Tail Class's impulse response, calibrated level and low cut. Custom: the values below." },
            { "IR", T::String, TARTARUS_REFLECT_FIELD(Z, Ir), 0.0f, "Impulse response file (a recorded wav, project-relative, e.g. assets/Audio/IR/indoor_large_1.wav). Empty: the Tail Class's. Normalised to unit energy, so Wet (dB) is the same whatever the file." },
            { "Wet (dB)", T::Float, TARTARUS_REFLECT_FIELD(Z, WetDb), 0.1f, "Return level trim on top of the Tail Class's calibrated wet / dry (Reverb Bus: Wet Indoor Large dB ...). 0 = the calibrated level.", -30.0f, 24.0f },
            { "Pre-Delay (ms)", T::Float, TARTARUS_REFLECT_FIELD(Z, PreDelayMs), 0.5f, "Milliseconds before the reverb starts (a delay on the reverb's input).", 0.0f, 250.0f },
            { "HF Damping (dB)", T::Float, TARTARUS_REFLECT_FIELD(Z, HfDampDb), 0.1f, "Attenuation (dB) of a high shelf at 4 kHz on the reverb's input: a darker return. 0 = the recording as it is.", 0.0f, 24.0f },
            { "Low Cut (Hz)", T::Float, TARTARUS_REFLECT_FIELD(Z, LowCutHz), 1.0f, "High-pass on the reverb's input: keeps low end out of the return (0 = off).", 0.0f, 1000.0f },
            { "Ambience", T::String, TARTARUS_REFLECT_FIELD(Z, Ambience), 0.0f, "Sound key of a looping ambience bed for this space (e.g. snd.amb.indoor_large). Empty: none. 2D, crossfaded by the listener's zone weights (1 s fades), started and stopped with Play." },
            { "Ambience Volume", T::Float, TARTARUS_REFLECT_FIELD(Z, AmbienceVolume), 0.01f, "Linear gain of the ambience bed, on top of its mix_db.", 0.0f, 4.0f },
            // Scenes saved before the convolution reverb: loaded, not drawn (Wet Level and HF Damping are read as trims in Custom mode).
            { "HF Damping", T::Float, TARTARUS_REFLECT_FIELD(Z, Old.HfDamping), 0.01f, "Old 0..1 damping: read as 0 .. 10 dB of HF Damping (dB) in Custom mode. -1 = absent.", -1.0f, 1.0f },
            { "Wet Level", T::Float, TARTARUS_REFLECT_FIELD(Z, Old.WetLevel), 0.01f, "Old wet level: read as a trim in dB relative to the Tail Class's old default (Custom mode). -1 = absent.", -1.0f, 1.0f },
        };
        m.Fields[1].EnumLabels = "Box\0Sphere\0"; m.Fields[1].EnumCount = 2;
        m.Fields[2].VisibleIfField = "Shape"; m.Fields[2].VisibleIfValue = 0;
        m.Fields[3].VisibleIfField = "Shape"; m.Fields[3].VisibleIfValue = 1;
        m.Fields[6].EnumLabels = "Outdoor Open\0Outdoor Urban\0Indoor Small\0Indoor Large\0"; m.Fields[6].EnumCount = 4;
        m.Fields[8].EnumLabels = "Class Default\0Custom\0"; m.Fields[8].EnumCount = 2;
        for (ReflectField& f : m.Fields) {
            const std::string n = f.Name;
            if (n == "Wet (dB)") f.Key = "Wet dB";                    // (the old "Wet Level" key is a different unit: it stays its own, hidden, field)
            if (n == "Pre-Delay (ms)") f.Key = "Pre-Delay ms";        // the old key: same unit, old scenes' values carry over
            if (n == "HF Damping (dB)") f.Key = "HF Damping dB";
            if (n == "IR" || n == "Wet (dB)" || n == "Pre-Delay (ms)" || n == "HF Damping (dB)" || n == "Low Cut (Hz)") {
                f.VisibleIfField = "Reverb Mode";
                f.VisibleIfValue = 1;
                f.Group = "Reverb";
            } else if (n == "Ambience" || n == "Ambience Volume") {
                f.Group = "Ambience";
            } else if (n == "HF Damping" || n == "Wet Level") {
                f.EditorHidden = true;
            }
        }
        Register<ReverbZoneComponent>(std::move(m));
    }
    {
        ReflectComponent m;
        m.Name = "Reverb Portal"; m.Icon = ICON_FA_VOLUME_HIGH; m.Category = "Audio";
        m.Tooltip = "An opening between two Reverb Zones (a door, a window). A sound from the next room is heard from here: its direction, the length of\n"
                    "the path through, softened by Open Amount and by the bend round the opening, with its own room's reverb. Faces local +Z.\n"
                    "With no portal joining two rooms the sound takes the direct path (occlusion applies). Gameplay opens / closes it by writing Open Amount.";
        using P = ReverbPortalComponent;
        m.Fields = {
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(P, Enabled), 0.0f, "Off: the portal is ignored." },
            { "Extents", T::Vec3, TARTARUS_REFLECT_FIELD(P, Extents), 0.05f, "Half width, half height, half thickness of the opening in metres (local X / Y / Z).", 0.01f, 50.0f },
            { "Open Amount", T::Float, TARTARUS_REFLECT_FIELD(P, OpenAmount), 0.01f, "0 = closed .. 1 = fully open. Set from gameplay to swing a door.", 0.0f, 1.0f },
            { "Room A", T::String, TARTARUS_REFLECT_FIELD(P, RoomA), 0.0f, "Name of the Reverb Zone entity on the -Z side. Empty: the zone found one Probe Distance out on that side." },
            { "Room B", T::String, TARTARUS_REFLECT_FIELD(P, RoomB), 0.0f, "Name of the Reverb Zone entity on the +Z side. Empty: the zone found one Probe Distance out on that side." },
            { "Probe Distance", T::Float, TARTARUS_REFLECT_FIELD(P, ProbeDistance), 0.05f, "Metres out from the opening, on each side, where the rooms are looked up.", 0.05f, 10.0f },
            { "Closed Gain dB", T::Float, TARTARUS_REFLECT_FIELD(P, ClosedGainDb), 0.5f, "Gain a fully closed portal lets through.", -80.0f, 0.0f },
            { "Closed Cutoff", T::Float, TARTARUS_REFLECT_FIELD(P, ClosedCutoff), 10.0f, "Hz of the low-pass of a fully closed portal.", 100.0f, 20000.0f },
            { "Diffraction Cutoff", T::Float, TARTARUS_REFLECT_FIELD(P, DiffractionCutoff), 10.0f, "Hz of the low-pass at Diffraction Max Angle (a sound bending hard round the opening is muffled).", 100.0f, 20000.0f },
            { "Diffraction Max Angle", T::Float, TARTARUS_REFLECT_FIELD(P, DiffractionMaxAngle), 1.0f, "Degrees of bend (the angle between the way in and the way out) at which the loss is full.", 5.0f, 180.0f },
            { "Diffraction Gain dB", T::Float, TARTARUS_REFLECT_FIELD(P, DiffractionGainDb), 0.5f, "Gain at that bend.", -60.0f, 0.0f },
            { "Door", T::String, TARTARUS_REFLECT_FIELD(P, Door), 0.0f, "Name of an Openable entity (the door in this opening): in Play how far it is open is the Open Amount. Empty: Open Amount as set." },
        };
        m.Fields[2].Slider = true; m.Fields[2].Format = "%.2f";
        Register<ReverbPortalComponent>(std::move(m));
    }
    {
        ReflectComponent m;
        m.Name = "Reverb Bus"; m.Icon = ICON_FA_VOLUME_HIGH; m.Category = "Audio";
        m.Tooltip = "The runtime reverb (convolution with recorded impulse responses): what each kind of sound sends into it, the calibrated wet level\n"
                    "of each kind of space, how it follows the listener's space (Reverb Zones, else the raycast probe), the occlusion low-pass for\n"
                    "sources behind geometry, and the master limiter. One per scene (the first counts). Gun tails are recorded in their spaces and\n"
                    "send nothing.";
        using B = ReverbBusComponent;
        m.Fields = {
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(B, Enabled), 0.0f, "Off: no reverb at all (the sends are not built)." },
            { "Return Level", T::Float, TARTARUS_REFLECT_FIELD(B, ReturnLevel), 0.01f, "Gain of the wet signal into the mix (times the SFX bus volume).", 0.0f, 4.0f },
            { "Wet Scale", T::Float, TARTARUS_REFLECT_FIELD(B, WetScale), 0.01f, "Linear gain on every space's wet level.", 0.0f, 4.0f },
            { "Glide Time", T::Float, TARTARUS_REFLECT_FIELD(B, GlideTime), 0.01f, "Seconds a change of space takes: the impulse responses crossfade (equal power) over it.", 0.01f, 5.0f },
            { "Wet Outdoor Open dB", T::Float, TARTARUS_REFLECT_FIELD(B, WetOutdoorOpenDb), 0.1f, "Calibrated wet / dry (RMS of the reverb return against the dry sound at the listener) in open ground, for a send of 1.", -60.0f, 12.0f },
            { "Wet Outdoor Urban dB", T::Float, TARTARUS_REFLECT_FIELD(B, WetOutdoorUrbanDb), 0.1f, "The same among buildings.", -60.0f, 12.0f },
            { "Wet Indoor Small dB", T::Float, TARTARUS_REFLECT_FIELD(B, WetIndoorSmallDb), 0.1f, "The same in a small room.", -60.0f, 12.0f },
            { "Wet Indoor Large dB", T::Float, TARTARUS_REFLECT_FIELD(B, WetIndoorLargeDb), 0.1f, "The same in a hall / warehouse.", -60.0f, 12.0f },
            { "Wet Trim dB", T::Float, TARTARUS_REFLECT_FIELD(B, WetTrimDb), 0.1f, "On every Wet level: they are calibrated on white noise, and real material (impacts, shots) comes back about 4 dB hotter from the recorded rooms. -4 makes the Wet levels what is heard.", -12.0f, 6.0f },
            { "Low Cut Hz", T::Float, TARTARUS_REFLECT_FIELD(B, LowCutHz), 1.0f, "High-pass on the reverb's input for a space with no Low Cut of its own (0 = off).", 0.0f, 1000.0f },
            { "Send Foley", T::Float, TARTARUS_REFLECT_FIELD(B, SendFoley), 0.01f, "Cloth, jumps, landings, ADS / equip / fire-mode gear. 1 = the space's calibrated wet level.", 0.0f, 2.0f },
            { "Send Footsteps", T::Float, TARTARUS_REFLECT_FIELD(B, SendFootsteps), 0.01f, "Player and soldier footfalls.", 0.0f, 2.0f },
            { "Send Actions", T::Float, TARTARUS_REFLECT_FIELD(B, SendActions), 0.01f, "Each gun's own mags, bolts, pumps, shells.", 0.0f, 2.0f },
            { "Send Impacts", T::Float, TARTARUS_REFLECT_FIELD(B, SendImpacts), 0.01f, "Bullet impacts, flesh hits and flybys.", 0.0f, 2.0f },
            { "Send Casings", T::Float, TARTARUS_REFLECT_FIELD(B, SendCasings), 0.01f, "Shell casings on the ground.", 0.0f, 2.0f },
            { "Send Shot", T::Float, TARTARUS_REFLECT_FIELD(B, SendShot), 0.01f, "The close / mech / sub layers of a gunshot: the crack feeds the room, which is what makes it audible indoors.", 0.0f, 2.0f },
            { "Send Tail", T::Float, TARTARUS_REFLECT_FIELD(B, SendTail), 0.01f, "The recorded gun tails (and the far layer). 0: they already hold their room.", 0.0f, 2.0f },
            { "Occlusion Enabled", T::Bool, TARTARUS_REFLECT_FIELD(B, OcclusionEnabled), 0.0f, "A 3D voice whose line of sight from the listener is blocked by solid geometry is low-passed." },
            { "Occlusion Cutoff", T::Float, TARTARUS_REFLECT_FIELD(B, OcclusionCutoff), 10.0f, "Hz of a fully occluded voice's low-pass.", 100.0f, 10000.0f },
            { "Occlusion Interval", T::Float, TARTARUS_REFLECT_FIELD(B, OcclusionInterval), 0.01f, "Seconds between line-of-sight checks of one voice.", 0.02f, 2.0f },
            { "Occlusion Rays Per Frame", T::Int, TARTARUS_REFLECT_FIELD(B, OcclusionRaysPerFrame), 1.0f, "Line-of-sight casts a frame, over every voice.", 1.0f, 64.0f },
            { "Occlusion Min Distance", T::Float, TARTARUS_REFLECT_FIELD(B, OcclusionMinDistance), 0.1f, "Closer sources are never occluded.", 0.0f, 50.0f },
            { "Occlusion Glide", T::Float, TARTARUS_REFLECT_FIELD(B, OcclusionGlide), 0.1f, "Per second: how fast the low-pass follows a change (a source stepping behind a corner closes up, no click), and a portal voice follows a door swinging.", 0.5f, 100.0f },
            { "Portals Enabled", T::Bool, TARTARUS_REFLECT_FIELD(B, PortalsEnabled), 0.0f, "A voice in another room is heard from the Reverb Portal that joins the rooms (direction, path length, open amount, bend), with its own room's reverb." },
            { "Remote Reverb Hold", T::Float, TARTARUS_REFLECT_FIELD(B, RemoteReverbHold), 0.1f, "Seconds the remote room's reverb keeps running after the last voice heard through a portal (its tail rings out).", 0.0f, 30.0f },
            { "Master Limiter", T::Bool, TARTARUS_REFLECT_FIELD(B, MasterLimiterEnabled), 0.0f, "The master peak limiter: the last node before the output, after every bus and the reverb returns." },
            { "Master Ceiling dB", T::Float, TARTARUS_REFLECT_FIELD(B, MasterCeilingDb), 0.1f, "dBFS the output never exceeds.", -12.0f, 0.0f },
            { "Master Lookahead ms", T::Float, TARTARUS_REFLECT_FIELD(B, MasterLookaheadMs), 0.1f, "The limiter looks this far ahead so the gain is down before the peak arrives (adds this much latency).", 0.1f, 10.0f },
            { "Master Release ms", T::Float, TARTARUS_REFLECT_FIELD(B, MasterReleaseMs), 1.0f, "Time constant of the gain recovering after a peak.", 1.0f, 1000.0f },
        };
        const std::pair<const char*, const char*> groups[] = {
            {"Wet Outdoor Open dB", "Wet Level"}, {"Wet Outdoor Urban dB", "Wet Level"}, {"Wet Indoor Small dB", "Wet Level"}, {"Wet Indoor Large dB", "Wet Level"},
            {"Wet Trim dB", "Wet Level"}, {"Low Cut Hz", "Wet Level"},
            {"Send Foley", "Sends"}, {"Send Footsteps", "Sends"}, {"Send Actions", "Sends"}, {"Send Impacts", "Sends"}, {"Send Casings", "Sends"},
            {"Send Voice", "Sends"}, {"Send Shot", "Sends"}, {"Send Tail", "Sends"},
            {"Occlusion Enabled", "Occlusion"}, {"Occlusion Cutoff", "Occlusion"}, {"Occlusion Interval", "Occlusion"}, {"Occlusion Rays Per Frame", "Occlusion"},
            {"Occlusion Min Distance", "Occlusion"}, {"Occlusion Glide", "Occlusion"}, {"Portals Enabled", "Portals"}, {"Remote Reverb Hold", "Portals"},
            {"Master Limiter", "Master Limiter"}, {"Master Ceiling dB", "Master Limiter"}, {"Master Lookahead ms", "Master Limiter"}, {"Master Release ms", "Master Limiter"}};
        for (ReflectField& f : m.Fields) {
            for (const auto& [n, g] : groups)
                if (std::strcmp(f.Name, n) == 0) f.Group = g;
            if (std::strcmp(f.Name, "Send Voice") == 0) f.EditorHidden = true;
        }
        Register<ReverbBusComponent>(std::move(m));
    }
    {
        ReflectComponent m;
        m.Name = "Audio Mix"; m.Icon = ICON_FA_VOLUME_HIGH; m.Category = "Audio";
        m.Tooltip = "The dynamic mix: what ducks under gunfire and rounds cracking past (the beds, your own foley, casings; soldiers' shots and steps,\n"
                    "flybys and hit markers never duck), the aim-down-sights focus, air absorption with distance, and the master bus's trim and glue\n"
                    "compressor (before the limiter). One per scene (the first counts); without one the defaults play. Window > Audio edits it live.";
        using M = AudioMixComponent;
        m.Fields = {
            { "Duck Enabled", T::Bool, TARTARUS_REFLECT_FIELD(M, DuckEnabled), 0.0f, "A loud event at the listener ducks the groups under it." },
            { "Duck Threshold dB", T::Float, TARTARUS_REFLECT_FIELD(M, DuckThresholdDb), 0.5f, "An event this loud at the listener (dB re your own shot) starts to duck.", -40.0f, 0.0f },
            { "Duck Full dB", T::Float, TARTARUS_REFLECT_FIELD(M, DuckFullDb), 0.5f, "An event this loud ducks by the full depths.", -30.0f, 6.0f },
            { "Duck Attack ms", T::Float, TARTARUS_REFLECT_FIELD(M, DuckAttackMs), 1.0f, "How fast the groups go down.", 1.0f, 200.0f },
            { "Duck Hold", T::Float, TARTARUS_REFLECT_FIELD(M, DuckHold), 0.01f, "Seconds at depth after the last loud event.", 0.0f, 2.0f },
            { "Duck Release", T::Float, TARTARUS_REFLECT_FIELD(M, DuckRelease), 0.01f, "Seconds back up from full depth.", 0.05f, 5.0f },
            { "Duck Bed dB", T::Float, TARTARUS_REFLECT_FIELD(M, DuckBedDb), 0.5f, "Ambience beds.", 0.0f, 24.0f },
            { "Duck Own Foley dB", T::Float, TARTARUS_REFLECT_FIELD(M, DuckOwnFoleyDb), 0.5f, "Your own steps, cloth, gear, reloads.", 0.0f, 24.0f },
            { "Duck Debris dB", T::Float, TARTARUS_REFLECT_FIELD(M, DuckDebrisDb), 0.5f, "Shell casings.", 0.0f, 24.0f },
            { "Duck World dB", T::Float, TARTARUS_REFLECT_FIELD(M, DuckWorldDb), 0.5f, "Bullet impacts, bodies falling.", 0.0f, 24.0f },
            { "Focus Enabled", T::Bool, TARTARUS_REFLECT_FIELD(M, FocusEnabled), 0.0f, "Aiming down sights quietens the beds and your own foley so the world comes forward." },
            { "Focus Bed dB", T::Float, TARTARUS_REFLECT_FIELD(M, FocusBedDb), 0.5f, "", 0.0f, 24.0f },
            { "Focus Own Foley dB", T::Float, TARTARUS_REFLECT_FIELD(M, FocusOwnFoleyDb), 0.5f, "", 0.0f, 24.0f },
            { "Focus Debris dB", T::Float, TARTARUS_REFLECT_FIELD(M, FocusDebrisDb), 0.5f, "", 0.0f, 24.0f },
            { "Focus Time", T::Float, TARTARUS_REFLECT_FIELD(M, FocusTime), 0.01f, "Seconds in / out.", 0.01f, 2.0f },
            { "Air Enabled", T::Bool, TARTARUS_REFLECT_FIELD(M, AirEnabled), 0.0f, "3D sounds lose their top end with distance (on top of occlusion)." },
            { "Air Start Distance", T::Float, TARTARUS_REFLECT_FIELD(M, AirStartDistance), 0.5f, "Metres: open up to here.", 1.0f, 200.0f },
            { "Air Exponent", T::Float, TARTARUS_REFLECT_FIELD(M, AirExponent), 0.01f, "Past the start the cutoff is 20 kHz x (start / distance)^exponent.", 0.1f, 2.0f },
            { "Air Min Hz", T::Float, TARTARUS_REFLECT_FIELD(M, AirMinHz), 50.0f, "The cutoff never goes below this.", 500.0f, 20000.0f },
            { "Glue Enabled", T::Bool, TARTARUS_REFLECT_FIELD(M, GlueEnabled), 0.0f, "A gentle bus compressor before the master limiter: a firefight holds together and the limiter barely works." },
            { "Glue Threshold dB", T::Float, TARTARUS_REFLECT_FIELD(M, GlueThresholdDb), 0.5f, "dBFS (linked peak).", -40.0f, 0.0f },
            { "Glue Ratio", T::Float, TARTARUS_REFLECT_FIELD(M, GlueRatio), 0.1f, "", 1.0f, 10.0f },
            { "Glue Knee dB", T::Float, TARTARUS_REFLECT_FIELD(M, GlueKneeDb), 0.5f, "", 0.0f, 24.0f },
            { "Glue Attack ms", T::Float, TARTARUS_REFLECT_FIELD(M, GlueAttackMs), 0.5f, "Slow enough to let a shot's transient through.", 0.1f, 200.0f },
            { "Glue Release ms", T::Float, TARTARUS_REFLECT_FIELD(M, GlueReleaseMs), 5.0f, "", 10.0f, 2000.0f },
            { "Master Trim dB", T::Float, TARTARUS_REFLECT_FIELD(M, MasterTrimDb), 0.1f, "The whole mix, before the glue and the limiter.", -24.0f, 12.0f },
        };
        for (ReflectField& f : m.Fields) {
            const std::string n = f.Name;
            f.Group = n.rfind("Duck", 0) == 0 ? "Ducking" : n.rfind("Focus", 0) == 0 ? "Focus" : n.rfind("Air", 0) == 0 ? "Air" : "Master";
        }
        Register<AudioMixComponent>(std::move(m));
    }
    {
        ReflectComponent m;
        m.Name = "Ambience Bed"; m.Icon = ICON_FA_TREE; m.Category = "Audio";
        m.Tooltip = "A quad ambience bed: the Sound's variants loop on four speakers round the listener at fixed compass points (they follow\n"
                    "your position, not your turning, so the bed stays put while your head turns through it). Indoors (the listener's indoor\n"
                    "Reverb Zones) it drops and darkens. One per scene (the first counts).";
        using A = AmbienceBedComponent;
        m.Fields = {
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(A, Enabled), 0.0f, "" },
            { "Sound", T::String, TARTARUS_REFLECT_FIELD(A, Sound), 0.0f, "A sound key (e.g. snd.amb.night_field): its variants are the north, east, south and west loops." },
            { "Volume", T::Float, TARTARUS_REFLECT_FIELD(A, Volume), 0.01f, "Linear, on top of the sound's mix level.", 0.0f, 4.0f },
            { "Radius", T::Float, TARTARUS_REFLECT_FIELD(A, Radius), 0.1f, "Metres from the listener to each speaker.", 1.0f, 100.0f },
            { "Height", T::Float, TARTARUS_REFLECT_FIELD(A, Height), 0.1f, "Metres above the listener.", -10.0f, 30.0f },
            { "Fade In", T::Float, TARTARUS_REFLECT_FIELD(A, FadeIn), 0.05f, "Seconds from silence when Play starts.", 0.0f, 30.0f },
            { "Indoor Gain dB", T::Float, TARTARUS_REFLECT_FIELD(A, IndoorGainDb), 0.5f, "Fully inside indoor Reverb Zones.", -60.0f, 0.0f },
            { "Indoor Cutoff Hz", T::Float, TARTARUS_REFLECT_FIELD(A, IndoorCutoffHz), 25.0f, "The low-pass fully inside indoor Reverb Zones.", 100.0f, 20000.0f },
            { "Glide", T::Float, TARTARUS_REFLECT_FIELD(A, Glide), 0.05f, "Seconds for the indoor muffling to go all the way in or out (no snap at a doorway).", 0.0f, 10.0f },
            { "Door Reach", T::Float, TARTARUS_REFLECT_FIELD(A, DoorReach), 0.1f, "Metres from an open portal to the outside at which the bed starts to open back up (0 = off).", 0.0f, 20.0f },
        };
        Register<AmbienceBedComponent>(std::move(m));
    }
    {
        ReflectComponent m;
        m.Name = "Weapon Audio"; m.Icon = ICON_FA_VOLUME_HIGH; m.Category = "Audio";
        m.Tooltip = "One gun's report and gear sounds (add one per gun; Gun is its key prefix, e.g. ak or 870).\n"
                    "A gun with none uses the built-in defaults. Animator events named snd.<gun>.<element> play the set of that key.";
        using W = WeaponAudioComponent;
        m.Fields = {
            { "Gun", T::String, TARTARUS_REFLECT_FIELD(W, Gun), 0.0f, "Key prefix: events snd.<Gun>.<element>, files Audio/Weapons/<folder>/. ak = AKS74U, 870 = Remington870." },
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(W, Enabled), 0.0f, "Off: this gun is silent." },
            { "Volume", T::Float, TARTARUS_REFLECT_FIELD(W, Volume), 0.01f, "Master gain for everything this gun plays.", 0.0f, 2.0f },
            { "Player Gain", T::Float, TARTARUS_REFLECT_FIELD(W, PlayerGain), 0.01f, "Gain of the report when it is the first-person player's (2D).", 0.0f, 2.0f },
            { "Shot Pitch Min", T::Float, TARTARUS_REFLECT_FIELD(W, ShotPitchMin), 0.005f, "Random playback rate per shot (shared by the layers so they stay coherent).", 0.5f, 2.0f },
            { "Shot Pitch Max", T::Float, TARTARUS_REFLECT_FIELD(W, ShotPitchMax), 0.005f, "Upper end of the per-shot playback rate.", 0.5f, 2.0f },
            { "Volume Jitter dB", T::Float, TARTARUS_REFLECT_FIELD(W, VolumeJitterDb), 0.1f, "+- dB of random gain per layer per shot.", 0.0f, 12.0f },
            { "Close Full Distance", T::Float, TARTARUS_REFLECT_FIELD(W, CloseFullDistance), 0.5f, "Metres inside which the close crack plays at full gain (3D shots).", 0.0f, 300.0f },
            { "Close Zero Distance", T::Float, TARTARUS_REFLECT_FIELD(W, CloseZeroDistance), 0.5f, "Metres from which the close crack is gone and the distant report is full.", 0.0f, 600.0f },
            { "Far Min Weight", T::Float, TARTARUS_REFLECT_FIELD(W, FarMinWeight), 0.01f, "Gain of the distant report up close.", 0.0f, 1.0f },
            { "Far Max Weight", T::Float, TARTARUS_REFLECT_FIELD(W, FarMaxWeight), 0.01f, "Gain of the distant report past Close Zero Distance.", 0.0f, 1.0f },
            { "Max Distance", T::Float, TARTARUS_REFLECT_FIELD(W, MaxDistance), 1.0f, "Metres the close, mech and sub layers carry.", 1.0f, 1000.0f },
            { "Far Max Distance", T::Float, TARTARUS_REFLECT_FIELD(W, FarMaxDistance), 1.0f, "Metres the distant report carries.", 1.0f, 2000.0f },
            { "Shot Min Distance", T::Float, TARTARUS_REFLECT_FIELD(W, ShotMinDistance), 0.1f, "3D rolloff (logarithmic, -6 dB per doubling): metres inside which the close and mech layers are full volume.", 0.1f, 100.0f },
            { "Bass Min Distance", T::Float, TARTARUS_REFLECT_FIELD(W, BassMinDistance), 0.1f, "The same for the sub, tail and distant layers (low end carries).", 0.1f, 100.0f },
            { "Event Min Distance", T::Float, TARTARUS_REFLECT_FIELD(W, EventMinDistance), 0.1f, "The same for the gun's gear and foley sounds in the world (reloads, ADS, equip).", 0.1f, 100.0f },
            { "Event Max Distance", T::Float, TARTARUS_REFLECT_FIELD(W, EventMaxDistance), 0.5f, "Metres past which a gear sound stops getting quieter.", 1.0f, 500.0f },
            { "Shot Max Voices", T::Int, TARTARUS_REFLECT_FIELD(W, ShotMaxVoices), 1.0f, "Close / mech / sub voices at once, per layer; past it the oldest is stolen.", 1.0f, 32.0f },
            { "Tail Max Voices", T::Int, TARTARUS_REFLECT_FIELD(W, TailMaxVoices), 1.0f, "Full auto: tails ringing at once; past it the oldest fades out.", 1.0f, 16.0f },
            { "Tail Fade Time", T::Float, TARTARUS_REFLECT_FIELD(W, TailFadeTime), 0.01f, "Seconds a stolen tail fades out over.", 0.0f, 2.0f },
            { "Tail Min Interval", T::Float, TARTARUS_REFLECT_FIELD(W, TailMinInterval), 0.005f, "Seconds between tails (0 = one per shot, capped by Tail Max Voices).", 0.0f, 1.0f },
            { "Tail Duck Per Voice", T::Float, TARTARUS_REFLECT_FIELD(W, TailDuckPerVoice), 0.01f, "Each tail still ringing quietens a new one by this share, so a long burst doesn't clip.", 0.0f, 1.0f },
            { "Tail Every", T::Int, TARTARUS_REFLECT_FIELD(W, TailEvery), 1.0f, "Full auto: the tail layer plays on every Nth shot of a burst (1 = every shot; S1's mix policy is 2).", 1.0f, 16.0f },
            { "Far Every", T::Int, TARTARUS_REFLECT_FIELD(W, FarEvery), 1.0f, "Full auto: the distant report plays on every Nth shot of a burst (S1's mix policy is 3).", 1.0f, 16.0f },
            { "Burst Gap", T::Float, TARTARUS_REFLECT_FIELD(W, BurstGap), 0.01f, "Seconds without a shot that end a burst; the next shot plays every layer.", 0.05f, 5.0f },
            { "Data File", T::String, TARTARUS_REFLECT_FIELD(W, DataFile), 0.0f, "Optional json of SoundSet overrides by element (files, volume, jitter, pitch, bus, range, voices). Relative to the project." },
            { "Env Enabled", T::Bool, TARTARUS_REFLECT_FIELD(W, EnvEnabled), 0.0f, "The shot's tail follows the space the shooter is in: a Reverb Zone volume first, else a raycast probe around the shooter. Off: always the generic tail (fire_tail). A space with no recorded tail files also plays the generic one." },
            { "Env Ray Count", T::Int, TARTARUS_REFLECT_FIELD(W, EnvRayCount), 1.0f, "Rays per probe (1 up, a diagonal ring and a horizontal ring). More = a finer read of the space, a little more cost per refresh.", 6.0f, 64.0f },
            { "Env Max Distance", T::Float, TARTARUS_REFLECT_FIELD(W, EnvMaxDistance), 0.5f, "Metres each ray looks; a miss counts as this far.", 5.0f, 200.0f },
            { "Env Indoor Cover", T::Float, TARTARUS_REFLECT_FIELD(W, EnvIndoorCover), 0.01f, "Overhead cover (half the up ray, half the share of upward diagonals that hit) at which a space turns indoor. The centre of the crossfade.", 0.0f, 1.0f },
            { "Env Urban Wall", T::Float, TARTARUS_REFLECT_FIELD(W, EnvUrbanWall), 0.01f, "Share of the horizon with a wall within Urban Distance at which open air turns urban (building slaps).", 0.0f, 1.0f },
            { "Env Urban Distance", T::Float, TARTARUS_REFLECT_FIELD(W, EnvUrbanDistance), 0.5f, "Metres: a wall farther than this is not a building slap.", 1.0f, 200.0f },
            { "Env Large Room Distance", T::Float, TARTARUS_REFLECT_FIELD(W, EnvLargeRoomDistance), 0.1f, "Mean wall distance (m) at which an indoor space turns from small to large.", 1.0f, 100.0f },
            { "Env Blend Fraction", T::Float, TARTARUS_REFLECT_FIELD(W, EnvBlendFraction), 0.005f, "Half-width of the crossfade around the cover and wall thresholds (0 = a hard switch).", 0.0f, 0.5f },
            { "Env Blend Distance", T::Float, TARTARUS_REFLECT_FIELD(W, EnvBlendDistance), 0.005f, "Half-width of the small/large crossfade, as a share of Env Large Room Distance.", 0.0f, 0.9f },
            { "Env Refresh Interval", T::Float, TARTARUS_REFLECT_FIELD(W, EnvRefreshInterval), 0.01f, "Seconds between probes of one shooter (cached in between, never per shot).", 0.0f, 10.0f },
            { "Env Refresh Move Distance", T::Float, TARTARUS_REFLECT_FIELD(W, EnvRefreshMoveDistance), 0.05f, "Metres a shooter moves that probe it again at once.", 0.0f, 50.0f },
            { "Env Match Radius", T::Float, TARTARUS_REFLECT_FIELD(W, EnvMatchRadius), 0.1f, "Shots from a shooter the game does not name are told apart by position: shots this close are one shooter.", 0.1f, 20.0f },
            { "Env Gain Outdoor Open", T::Float, TARTARUS_REFLECT_FIELD(W, EnvTailGainOutdoorOpen), 0.01f, "Gain of the tail in open ground, on top of the tail layer's.", 0.0f, 4.0f },
            { "Env Gain Outdoor Urban", T::Float, TARTARUS_REFLECT_FIELD(W, EnvTailGainOutdoorUrban), 0.01f, "Gain of the tail among buildings.", 0.0f, 4.0f },
            { "Env Gain Indoor Small", T::Float, TARTARUS_REFLECT_FIELD(W, EnvTailGainIndoorSmall), 0.01f, "Gain of the tail in a small room.", 0.0f, 4.0f },
            { "Env Gain Indoor Large", T::Float, TARTARUS_REFLECT_FIELD(W, EnvTailGainIndoorLarge), 0.01f, "Gain of the tail in a hall / warehouse.", 0.0f, 4.0f },
            { "Env Debug Draw", T::Bool, TARTARUS_REFLECT_FIELD(W, EnvDebugDraw), 0.0f, "Keep each shooter's probe rays for the overlay (WeaponAudio::EnvironmentDebugLines)." },
        };
        const std::pair<const char*, const char*> groups[] = {
            {"Shot Pitch Min", "Shot"}, {"Shot Pitch Max", "Shot"}, {"Volume Jitter dB", "Shot"},
            {"Close Full Distance", "Distance Blend"}, {"Close Zero Distance", "Distance Blend"}, {"Far Min Weight", "Distance Blend"},
            {"Far Max Weight", "Distance Blend"}, {"Max Distance", "Distance Blend"}, {"Far Max Distance", "Distance Blend"},
            {"Shot Min Distance", "Distance Blend"}, {"Bass Min Distance", "Distance Blend"}, {"Event Min Distance", "Distance Blend"}, {"Event Max Distance", "Distance Blend"},
            {"Tail Max Voices", "Full Auto"}, {"Tail Every", "Full Auto"}, {"Far Every", "Full Auto"}, {"Burst Gap", "Full Auto"},
            {"Tail Fade Time", "Full Auto"}, {"Tail Min Interval", "Full Auto"}, {"Tail Duck Per Voice", "Full Auto"},
            {"Env Enabled", "Environment"}, {"Env Ray Count", "Environment"}, {"Env Max Distance", "Environment"}, {"Env Indoor Cover", "Environment"},
            {"Env Urban Wall", "Environment"}, {"Env Urban Distance", "Environment"}, {"Env Large Room Distance", "Environment"},
            {"Env Blend Fraction", "Environment"}, {"Env Blend Distance", "Environment"}, {"Env Refresh Interval", "Environment"},
            {"Env Refresh Move Distance", "Environment"}, {"Env Match Radius", "Environment"}, {"Env Gain Outdoor Open", "Environment"},
            {"Env Gain Outdoor Urban", "Environment"}, {"Env Gain Indoor Small", "Environment"}, {"Env Gain Indoor Large", "Environment"},
            {"Env Debug Draw", "Environment"}};
        for (ReflectField& f : m.Fields) {
            for (const auto& [n, g] : groups)
                if (std::strcmp(f.Name, n) == 0) f.Group = g;
            if (MixOwnsDistanceField(f.Name)) f.EditorHidden = true; // the mix spec's distance models own it (manifest mix.distance)
        }
        Register<WeaponAudioComponent>(std::move(m));
    }
    {
        ReflectComponent m;
        m.Name = "Foley Audio"; m.Icon = ICON_FA_SHOE_PRINTS; m.Category = "Audio";
        m.Tooltip = "Footsteps (on the view bob's stride), jumps, landings and the sprint cloth loop; soldiers' footsteps. The first one in the scene counts.";
        using Fo = FoleyAudioComponent;
        m.Fields = {
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(Fo, Enabled), 0.0f, "Off: no foley." },
            { "Volume", T::Float, TARTARUS_REFLECT_FIELD(Fo, Volume), 0.01f, "Master gain for all foley.", 0.0f, 2.0f },
            { "Walk Volume", T::Float, TARTARUS_REFLECT_FIELD(Fo, WalkVolume), 0.01f, "Footstep gain walking.", 0.0f, 2.0f },
            { "Run Volume", T::Float, TARTARUS_REFLECT_FIELD(Fo, RunVolume), 0.01f, "Footstep gain running.", 0.0f, 2.0f },
            { "Crouch Volume", T::Float, TARTARUS_REFLECT_FIELD(Fo, CrouchVolume), 0.01f, "Footstep gain crouched.", 0.0f, 2.0f },
            { "Volume Jitter dB", T::Float, TARTARUS_REFLECT_FIELD(Fo, VolumeJitterDb), 0.1f, "+- dB of random gain per step.", 0.0f, 12.0f },
            { "Pitch Min", T::Float, TARTARUS_REFLECT_FIELD(Fo, PitchMin), 0.005f, "Random playback rate per step.", 0.5f, 2.0f },
            { "Pitch Max", T::Float, TARTARUS_REFLECT_FIELD(Fo, PitchMax), 0.005f, "Upper end of the per-step playback rate.", 0.5f, 2.0f },
            { "Step Stride Scale", T::Float, TARTARUS_REFLECT_FIELD(Fo, StepStrideScale), 0.01f, "Footfall spacing against the view bob's stride (two footfalls per stride): above 1 = slower steps.", 0.25f, 4.0f },
            { "Crouch Stride Scale", T::Float, TARTARUS_REFLECT_FIELD(Fo, CrouchStrideScale), 0.01f, "Footfall spacing when crouched, on top of Step Stride Scale.", 0.25f, 4.0f },
            { "Min Step Speed", T::Float, TARTARUS_REFLECT_FIELD(Fo, MinStepSpeed), 0.05f, "m/s of planar speed below which there are no footsteps.", 0.0f, 10.0f },
            { "Run Speed", T::Float, TARTARUS_REFLECT_FIELD(Fo, RunSpeed), 0.05f, "m/s from which the run set plays.", 0.5f, 20.0f },
            { "Steps From Feet", T::Bool, TARTARUS_REFLECT_FIELD(Fo, StepsFromFeet), 0.0f, "A footstep when an animated foot (the player's body, a soldier's) touches down. Off, or with no body: one every half stride of travel." },
            { "Foot Lift Height", T::Float, TARTARUS_REFLECT_FIELD(Fo, FootLiftHeight), 0.005f, "Standing (below Min Step Speed): m a foot must rise above its planted height before its next touch-down counts. Weight shifts below this are silent.", 0.01f, 0.3f },
            { "Foot Lift Moving", T::Float, TARTARUS_REFLECT_FIELD(Fo, FootLiftMoving), 0.005f, "The same while moving: lower, so a walk's first, shuffling stride still steps.", 0.005f, 0.3f },
            { "Foot Contact Height", T::Float, TARTARUS_REFLECT_FIELD(Fo, FootContactHeight), 0.005f, "m above its planted height at which a lowering foot counts as down: higher = the step a little earlier.", 0.0f, 0.2f },
            { "Settle Volume", T::Float, TARTARUS_REFLECT_FIELD(Fo, SettleVolume), 0.01f, "Coming to a stop: the one settling step's volume (the feet planting after it are silent).", 0.0f, 1.5f },
            { "Settle Quiet", T::Float, TARTARUS_REFLECT_FIELD(Fo, SettleQuiet), 0.05f, "Seconds stood with both feet still after a stop before steps on the spot (turning) sound again.", 0.0f, 5.0f },
            { "Min Step Interval", T::Float, TARTARUS_REFLECT_FIELD(Fo, MinStepInterval), 0.01f, "Two touch-downs closer than this (s) are one footstep.", 0.0f, 0.5f },
            { "Jump Volume", T::Float, TARTARUS_REFLECT_FIELD(Fo, JumpVolume), 0.01f, "Gain of the jump.", 0.0f, 2.0f },
            { "Land Volume", T::Float, TARTARUS_REFLECT_FIELD(Fo, LandVolume), 0.01f, "Gain of a landing at Land Full Speed.", 0.0f, 2.0f },
            { "Land Min Speed", T::Float, TARTARUS_REFLECT_FIELD(Fo, LandMinSpeed), 0.05f, "m/s of fall below which a landing is silent.", 0.0f, 30.0f },
            { "Land Full Speed", T::Float, TARTARUS_REFLECT_FIELD(Fo, LandFullSpeed), 0.05f, "m/s of fall at which a landing is at Land Volume.", 0.1f, 40.0f },
            { "Land Heavy Speed", T::Float, TARTARUS_REFLECT_FIELD(Fo, LandHeavySpeed), 0.05f, "m/s of fall from which the heavy landing plays (the light one below).", 0.0f, 40.0f },
            { "NPC Step Volume", T::Float, TARTARUS_REFLECT_FIELD(Fo, NpcStepVolume), 0.01f, "Gain of soldiers' footsteps.", 0.0f, 2.0f },
            { "NPC Step Min Distance", T::Float, TARTARUS_REFLECT_FIELD(Fo, NpcStepMinDistance), 0.1f, "Metres inside which a soldier's step is full volume.", 0.1f, 50.0f },
            { "NPC Step Max Distance", T::Float, TARTARUS_REFLECT_FIELD(Fo, NpcStepMaxDistance), 0.5f, "Metres a soldier's step carries.", 1.0f, 200.0f },
            { "Footwear", T::Enum, TARTARUS_REFLECT_FIELD(Fo, Footwear), 0.0f, "The player's footsteps: Boots, or Barefoot (Foley/barefoot_<surface>; the boots' take where there is no barefoot one)." },
            { "NPC Footwear", T::Enum, TARTARUS_REFLECT_FIELD(Fo, NpcFootwear), 0.0f, "The soldiers' footsteps: Boots or Barefoot." },
            { "Default Surface", T::String, TARTARUS_REFLECT_FIELD(Fo, DefaultSurface), 0.0f, "Surface when the ground matches nothing in the table." },
            { "Surface Table", T::String, TARTARUS_REFLECT_FIELD(Fo, SurfaceTable), 0.0f, "surface=word,word;surface=word. The first surface with a word in the ground's physics material, tag or name. Surfaces are foley categories (Audio/Foley/<surface>/)." },
        };
        for (ReflectField& f : m.Fields)
            if (std::strcmp(f.Name, "Footwear") == 0 || std::strcmp(f.Name, "NPC Footwear") == 0) { f.EnumLabels = "Boots\0Barefoot\0"; f.EnumCount = 2; }
        const std::pair<const char*, const char*> groups[] = {
            {"Footwear", "Footsteps"}, {"Walk Volume", "Footsteps"}, {"Run Volume", "Footsteps"}, {"Crouch Volume", "Footsteps"}, {"Volume Jitter dB", "Footsteps"},
            {"Pitch Min", "Footsteps"}, {"Pitch Max", "Footsteps"}, {"Step Stride Scale", "Footsteps"}, {"Crouch Stride Scale", "Footsteps"},
            {"Min Step Speed", "Footsteps"}, {"Run Speed", "Footsteps"}, {"Steps From Feet", "Footsteps"}, {"Foot Lift Height", "Footsteps"}, {"Foot Lift Moving", "Footsteps"},
            {"Foot Contact Height", "Footsteps"}, {"Settle Volume", "Footsteps"}, {"Settle Quiet", "Footsteps"}, {"Min Step Interval", "Footsteps"},
            {"Jump Volume", "Jump / Land"}, {"Land Volume", "Jump / Land"}, {"Land Min Speed", "Jump / Land"}, {"Land Heavy Speed", "Jump / Land"}, {"Land Full Speed", "Jump / Land"},
            
            {"NPC Footwear", "NPC Footsteps"}, {"NPC Step Volume", "NPC Footsteps"}, {"NPC Step Min Distance", "NPC Footsteps"}, {"NPC Step Max Distance", "NPC Footsteps"}};
        for (ReflectField& f : m.Fields) {
            for (const auto& [n, g] : groups)
                if (std::strcmp(f.Name, n) == 0) f.Group = g;
            if (MixOwnsDistanceField(f.Name)) f.EditorHidden = true; // the mix spec's distance models own it (manifest mix.distance)
        }
        Register<FoleyAudioComponent>(std::move(m));
    }
    {
        ReflectComponent m;
        m.Name = "Impact Audio"; m.Icon = ICON_FA_VOLUME_HIGH; m.Category = "Audio";
        m.Tooltip = "Bullet impacts at the hit point, shell casings on their first ground contacts and rounds whipping past the listener.\n"
                    "The surface is the struck collider's physics material / tag / name against the Surface Table. One per scene (the first counts).";
        using I = ImpactAudioComponent;
        m.Fields = {
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(I, Enabled), 0.0f, "Off: no impact, casing or flyby sounds from here." },
            { "Surface Table", T::String, TARTARUS_REFLECT_FIELD(I, SurfaceTable), 0.0f, "surface=word,word;surface=word. The first surface with a word in the collider's material, tag or name." },
            { "Default Surface", T::String, TARTARUS_REFLECT_FIELD(I, DefaultSurface), 0.0f, "Surface when nothing matches, and for a surface with no recorded takes." },
            { "Casings Enabled", T::Bool, TARTARUS_REFLECT_FIELD(I, CasingsEnabled), 0.0f, "Spent cases sound when they land." },
            { "Casing Max Contacts", T::Int, TARTARUS_REFLECT_FIELD(I, CasingMaxContacts), 1.0f, "Ground contacts of one case that sound (the first N).", 0.0f, 8.0f },
            { "Casing Min Speed", T::Float, TARTARUS_REFLECT_FIELD(I, CasingMinSpeed), 0.05f, "m/s into the surface below which a contact is silent.", 0.0f, 20.0f },
            { "Casing Full Speed", T::Float, TARTARUS_REFLECT_FIELD(I, CasingFullSpeed), 0.05f, "m/s at which a contact plays at full gain.", 0.1f, 30.0f },
            { "Casing Gain Min", T::Float, TARTARUS_REFLECT_FIELD(I, CasingGainMin), 0.01f, "Gain just above the minimum speed.", 0.0f, 1.0f },
            { "Casing Volume", T::Float, TARTARUS_REFLECT_FIELD(I, CasingVolume), 0.01f, "Master gain of the casing sounds.", 0.0f, 2.0f },
            { "Casing Min Distance", T::Float, TARTARUS_REFLECT_FIELD(I, CasingMinDistance), 0.1f, "Metres inside which a casing is full volume.", 0.1f, 50.0f },
            { "Casing Max Distance", T::Float, TARTARUS_REFLECT_FIELD(I, CasingMaxDistance), 0.5f, "Metres a casing carries.", 1.0f, 200.0f },
            { "Casing Max Voices", T::Int, TARTARUS_REFLECT_FIELD(I, CasingMaxVoices), 1.0f, "Casing voices at once; the oldest is stolen.", 1.0f, 32.0f },
            { "Shell Radius", T::Float, TARTARUS_REFLECT_FIELD(I, ShellRadius), 0.0005f, "Metres: a case wider than this is a shotgun shell (snd.casing.shell), else a rifle case.", 0.001f, 0.05f },
            { "Impacts Enabled", T::Bool, TARTARUS_REFLECT_FIELD(I, ImpactsEnabled), 0.0f, "Bullet impacts at the hit point." },
            { "Impact Volume", T::Float, TARTARUS_REFLECT_FIELD(I, ImpactVolume), 0.01f, "Master gain of the impact sounds.", 0.0f, 2.0f },
            { "Impact Min Distance", T::Float, TARTARUS_REFLECT_FIELD(I, ImpactMinDistance), 0.1f, "Metres inside which an impact is full volume.", 0.1f, 50.0f },
            { "Impact Max Distance", T::Float, TARTARUS_REFLECT_FIELD(I, ImpactMaxDistance), 0.5f, "Metres an impact carries.", 1.0f, 500.0f },
            { "Impact Max Voices", T::Int, TARTARUS_REFLECT_FIELD(I, ImpactMaxVoices), 1.0f, "Impact voices at once per surface; the oldest is stolen.", 1.0f, 32.0f },
            { "Impact Min Interval", T::Float, TARTARUS_REFLECT_FIELD(I, ImpactMinInterval), 0.005f, "Seconds between impacts of one surface (a shotgun's pellets are not nine voices).", 0.0f, 1.0f },
            { "Flesh Uses Recordings", T::Bool, TARTARUS_REFLECT_FIELD(I, FleshUsesRecordings), 0.0f, "The flesh-hit cue plays the recorded snd.impact.flesh instead of the placeholder wav (never both)." },
            { "Flesh Volume", T::Float, TARTARUS_REFLECT_FIELD(I, FleshVolume), 0.01f, "Gain of the flesh impact.", 0.0f, 2.0f },
            { "Flyby Enabled", T::Bool, TARTARUS_REFLECT_FIELD(I, FlybyEnabled), 0.0f, "Soldiers' rounds passing the listener whip by (the recorded flyby replaces the placeholder whizz)." },
            { "Flyby Radius", T::Float, TARTARUS_REFLECT_FIELD(I, FlybyRadius), 0.1f, "Metres: a round that passes closer than this to the listener is heard.", 0.2f, 30.0f },
            { "Flyby Volume", T::Float, TARTARUS_REFLECT_FIELD(I, FlybyVolume), 0.01f, "Master gain of the flyby.", 0.0f, 2.0f },
            { "Flyby Min Interval", T::Float, TARTARUS_REFLECT_FIELD(I, FlybyMinInterval), 0.005f, "Seconds between flybys (a burst reads as a few cracks).", 0.0f, 2.0f },
            { "Flyby Min Distance", T::Float, TARTARUS_REFLECT_FIELD(I, FlybyMinDistance), 0.1f, "Metres inside which a flyby is full volume.", 0.1f, 20.0f },
            { "Flyby Max Distance", T::Float, TARTARUS_REFLECT_FIELD(I, FlybyMaxDistance), 0.5f, "Metres a flyby carries.", 1.0f, 100.0f },
            { "Flyby Far Gain", T::Float, TARTARUS_REFLECT_FIELD(I, FlybyFarGain), 0.01f, "Gain of a round at the edge of the radius (1 at zero miss).", 0.0f, 1.0f },
        };
        const std::pair<const char*, const char*> groups[] = {
            {"Casings Enabled", "Casings"}, {"Casing Max Contacts", "Casings"}, {"Casing Min Speed", "Casings"}, {"Casing Full Speed", "Casings"},
            {"Casing Gain Min", "Casings"}, {"Casing Volume", "Casings"}, {"Casing Min Distance", "Casings"}, {"Casing Max Distance", "Casings"},
            {"Casing Max Voices", "Casings"}, {"Shell Radius", "Casings"},
            {"Impacts Enabled", "Impacts"}, {"Impact Volume", "Impacts"}, {"Impact Min Distance", "Impacts"}, {"Impact Max Distance", "Impacts"},
            {"Impact Max Voices", "Impacts"}, {"Impact Min Interval", "Impacts"}, {"Flesh Uses Recordings", "Impacts"}, {"Flesh Volume", "Impacts"},
            {"Flyby Enabled", "Flyby"}, {"Flyby Radius", "Flyby"}, {"Flyby Volume", "Flyby"}, {"Flyby Min Interval", "Flyby"},
            {"Flyby Min Distance", "Flyby"}, {"Flyby Max Distance", "Flyby"}, {"Flyby Far Gain", "Flyby"}};
        for (ReflectField& f : m.Fields) {
            for (const auto& [n, g] : groups)
                if (std::strcmp(f.Name, n) == 0) f.Group = g;
            if (MixOwnsDistanceField(f.Name)) f.EditorHidden = true; // the mix spec's distance models own it (manifest mix.distance)
        }
        Register<ImpactAudioComponent>(std::move(m));
    }
}

} // namespace ComponentRegistry
