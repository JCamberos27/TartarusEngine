#pragma once
// Sources, listener, weapon/foley/impact audio, reverb zones/portals/bus, ambience and mix.
// Part of the component set; Components.h includes every part.

#include <string>
#include <vector>
#include <glm/glm.hpp>

// Optional clip triggered from the editor Inspector; formerly PlacedModel-only, now any entity.
struct AudioSourceComponent {
    std::string SoundPath;
    float Volume = 1.0f;
    bool Loop = false;
    // Played automatically on Play-mode entry (see EditorLayer::OnEnterPlayMode) and stopped on
    // exit; false by default so placing a source in a scene doesn't start blaring the moment you
    // press Play unless you opt in.
    bool PlayOnStart = false;
    int Output = 0; // #171 - mixer bus (AudioEngine::Bus): 0 SFX, 1 Music, 2 Ambient, 3 UI, 4 Voice
    // #171 - Unity's 3D Sound Settings. Spatial = heard from this entity's position (panned and
    // attenuated); off = plain 2D, same volume everywhere (music, UI). Rolloff: 0 Logarithmic
    // (Unity's default, inverse-distance), 1 Linear (silent at Max Distance).
    bool  Spatial = true;
    int   Rolloff = 0;
    float MinDistance = 1.0f;
    float MaxDistance = 500.0f;
    float DopplerLevel = 1.0f; // #171 - 0 = no pitch shift from motion, 1 = physical
};

// #171 - Unity's Audio Listener: in Play, 3D sounds are heard from this entity (position and
// facing) instead of from the game camera. The first active one wins.
struct AudioListenerComponent {
    bool Enabled = true;
};

// One gun's audio tuning (Game/Audio/WeaponAudio): add one per gun, Gun naming it ("ak", "870"); a gun with none uses these
// defaults. The per-event sets (reloads, ADS, equip ...) live in the Data File, a json of SoundSets by element; sets with no
// entry there are filled from the audio manifest by key (snd.<Gun>.<element>).
struct WeaponAudioComponent {
    std::string Gun = "ak";
    bool Enabled = true;
    float Volume = 1.0f;
    float PlayerGain = 0.75f;
    float ShotPitchMin = 0.96f;
    float ShotPitchMax = 1.04f;
    float VolumeJitterDb = 0.5f;
    float CloseFullDistance = 18.0f;
    float CloseZeroDistance = 43.0f;
    float FarMinWeight = 0.1f;
    float FarMaxWeight = 1.0f;
    float MaxDistance = 90.0f;
    float FarMaxDistance = 160.0f;
    float ShotMinDistance = 3.0f;
    float BassMinDistance = 6.0f;
    float EventMinDistance = 1.5f;
    float EventMaxDistance = 25.0f;
    int ShotMaxVoices = 6;
    int TailMaxVoices = 3;
    float TailFadeTime = 0.25f;
    float TailMinInterval = 0.0f;
    float TailDuckPerVoice = 0.3f;
    int TailEvery = 2;
    int FarEvery = 3;
    float BurstGap = 0.4f;
    std::string DataFile = "";
    bool EnvEnabled = true;
    int EnvRayCount = 12;
    float EnvMaxDistance = 40.0f;
    float EnvIndoorCover = 0.7f;
    float EnvUrbanWall = 0.35f;
    float EnvUrbanDistance = 25.0f;
    float EnvLargeRoomDistance = 8.0f;
    float EnvBlendFraction = 0.15f;
    float EnvBlendDistance = 0.3f;
    float EnvRefreshInterval = 0.25f;
    float EnvRefreshMoveDistance = 1.0f;
    float EnvMatchRadius = 2.0f;
    float EnvTailGainOutdoorOpen = 1.0f;
    float EnvTailGainOutdoorUrban = 1.0f;
    float EnvTailGainIndoorSmall = 1.0f;
    float EnvTailGainIndoorLarge = 1.0f;
    bool EnvDebugDraw = false;
};

// Footsteps, jumps, landings and the sprint cloth loop (Game/Audio/FoleyAudio). One per scene (the first counts).
struct FoleyAudioComponent {
    bool Enabled = true;
    float Volume = 1.0f;
    float WalkVolume = 1.0f;
    float RunVolume = 1.0f;
    float CrouchVolume = 0.55f;
    float VolumeJitterDb = 1.0f;
    float PitchMin = 0.94f;
    float PitchMax = 1.06f;
    float StepStrideScale = 1.0f;
    float CrouchStrideScale = 0.8f;
    float MinStepSpeed = 0.6f;
    float RunSpeed = 4.5f;
    bool StepsFromFeet = true;
    float FootLiftHeight = 0.05f;
    float FootLiftMoving = 0.025f;
    float FootContactHeight = 0.02f;
    // Coming to a stop: the body settling into idle plants its feet a few times. Below Min Step Speed after moving, the first
    // touch-down plays at Settle Volume and the rest are silent, until moving again or Settle Quiet seconds with both feet still
    // (then turning on the spot steps again). Two touch-downs closer than Min Step Interval are one sound.
    float SettleVolume = 0.6f;
    float SettleQuiet = 0.6f;
    float MinStepInterval = 0.18f;
    float JumpVolume = 1.0f;
    float LandVolume = 1.0f;
    float LandMinSpeed = 2.5f;
    float LandFullSpeed = 9.0f;
    float LandHeavySpeed = 6.0f;
    float NpcStepVolume = 1.0f;
    float NpcStepMaxDistance = 28.0f;
    float NpcStepMinDistance = 2.5f;
    // Which step takes play: 0 boots (Foley/step_<surface>), 1 barefoot (Foley/barefoot_<surface>, the boots' where a
    // barefoot take is missing). The player's own, and the soldiers'.
    int Footwear = 0;
    int NpcFootwear = 0;
    std::string DefaultSurface = "concrete";
    std::string SurfaceTable = "wood=wood,plank,floor,parquet;metal=metal,steel,iron,grate;glass=glass;carpet=carpet,rug;water=water,puddle;grass=grass;gravel=gravel;concrete=concrete,asphalt,tile,stone,dirt,soil,sand";
};

// What a space asks of the convolution reverb (Audio/ConvolutionReverb): the zone's resolved reverb, or a tail class's default.
struct ReverbPreset {
    std::string Ir;                    // impulse response file (project-relative); empty = the Tail Class's (manifest key ir.<class>)
    float WetDb = 0.0f;                // trim (dB) on the class's calibrated return level (Reverb Bus: Wet <class> dB)
    float PreDelayMs = -1.0f;          // < 0: the class IR's own (manifest predelay_ms)
    float HfDampDb = 0.0f;             // dB of high-shelf attenuation on the reverb input
    float LowCutHz = -1.0f;            // < 0: the Reverb Bus's Low Cut
    int Class = 2;                     // the tail class (0 outdoor open, 1 outdoor urban, 2 indoor small, 3 indoor large)
};

// The default reverb of a tail class (0 outdoor open, 1 outdoor urban, 2 indoor small, 3 indoor large).
inline ReverbPreset ReverbPresetFor(int tailClass) {
    ReverbPreset p;
    p.Class = tailClass < 0 ? 0 : tailClass > 3 ? 3 : tailClass;
    return p;
}

// A designer-placed space for sound: inside the volume the shots' tail is the Tail Class's, the runtime reverb convolves with the
// zone's impulse response and the zone's ambience bed fades in; all crossfaded over Fade Distance at the edge. A shot in no zone falls
// back to the raycast probe. Overlapping zones: the higher Priority sits on top (fades in over the lower ones). The entity's position and
// rotation place it (Scale is ignored: extents and radius are metres).
struct ReverbZoneComponent {
    bool Enabled = true;
    int Shape = 0;                     // 0 box, 1 sphere
    glm::vec3 Extents{6.0f, 3.0f, 6.0f}; // box half extents, metres
    float Radius = 6.0f;               // sphere radius, metres
    int Priority = 0;                  // higher sits on top of lower
    float FadeDistance = 2.0f;         // metres inside the edge over which the zone fades in (0 = hard edge)
    int TailClass = 2;                 // 0 outdoor open, 1 outdoor urban, 2 indoor small, 3 indoor large
    float TailGain = 1.0f;             // on the shot's tail layer while inside
    int ReverbMode = 0;                // 0 = the Tail Class's default reverb, 1 = the values below
    // Convolution reverb (Custom mode).
    std::string Ir;                    // impulse response file; empty = the Tail Class's
    float WetDb = 0.0f;                // trim on the class's calibrated level
    float PreDelayMs = 10.0f;
    float HfDampDb = 0.0f;             // high-shelf attenuation of the reverb input
    float LowCutHz = 80.0f;
    // Ambience bed: a looping 2D sound (sound key, e.g. snd.amb.indoor_large) crossfaded by the listener's zone weights.
    std::string Ambience;              // empty = none
    float AmbienceVolume = 1.0f;       // linear, on top of the sound's mix_db
    // Scenes saved before the convolution reverb carry the old feedback-network values under these keys: the wet level and the
    // damping are read as trims (see Resolved()); the network's other keys (room size, decay, early/late mix) are ignored. -1 = absent.
    struct Legacy {
        float HfDamping = -1.0f;
        float WetLevel = -1.0f;
    } Old;
    static float LegacyDefaultWet(int tailClass) { return tailClass == 0 ? 0.12f : tailClass == 1 ? 0.2f : tailClass == 3 ? 0.35f : 0.3f; }
    ReverbPreset Resolved() const {
        if (ReverbMode != 1) return ReverbPresetFor(TailClass);
        ReverbPreset p;
        p.Class = TailClass < 0 ? 0 : TailClass > 3 ? 3 : TailClass;
        p.Ir = Ir;
        p.PreDelayMs = PreDelayMs;
        p.LowCutHz = LowCutHz;
        p.WetDb = WetDb;
        p.HfDampDb = HfDampDb;
        if (Old.WetLevel >= 0.0f) { // an old Wet Level: its ratio to that class's old default, in dB (+- 12)
            const float r = Old.WetLevel / LegacyDefaultWet(p.Class);
            p.WetDb += r <= 0.2512f ? -12.0f : r >= 3.981f ? 12.0f : 20.0f * std::log10(r);
        }
        if (Old.HfDamping >= 0.0f) p.HfDampDb += 10.0f * Old.HfDamping; // 0 = bright .. 1 = dark -> 0 .. 10 dB
        return p;
    }
};

// An opening between two Reverb Zones (Wwise "rooms and portals"): a door, a window, a breach. A 3D voice whose source and listener are in
// different rooms joined by portals is heard from the portal (its direction and the path's length), softened by the portal's open amount
// and by how sharply the sound has to bend round it, and carries its own room's reverb with it. The portal is a rectangle in the
// entity's local X (width) / Y (height) plane, facing local +Z; the rooms are the zones on its two sides (sampled one Probe Distance out
// from the opening), or the zones named in Room A / Room B. Gameplay opens and closes it by writing Open Amount.
struct ReverbPortalComponent {
    bool Enabled = true;
    glm::vec3 Extents{0.5f, 1.0f, 0.1f}; // half width, half height, half thickness (metres)
    float OpenAmount = 1.0f;           // 0 closed .. 1 fully open (a door's angle)
    std::string RoomA, RoomB;          // names of Reverb Zone entities on the -Z / +Z side (empty = the zone found on that side)
    float ProbeDistance = 0.5f;        // metres out from the opening where the rooms are looked up
    float ClosedGainDb = -30.0f;       // what a shut portal lets through
    float ClosedCutoff = 500.0f;       // Hz of its low-pass
    float DiffractionCutoff = 2500.0f; // Hz of the low-pass at Diffraction Max Angle
    float DiffractionMaxAngle = 120.0f; // degrees of bend at which the diffraction loss is full
    float DiffractionGainDb = -6.0f;   // gain at that bend
    std::string Door;                  // name of an Openable entity: in Play its opening (0 shut .. 1 open) is the Open Amount
};

// The runtime reverb bus (Audio/ConvolutionReverb, wired in AudioEngine): what each category of sound sends into it, the calibrated wet
// level of each kind of space, how the reverb follows the listener's space (Reverb Zones, else the raycast probe), the listener-side
// occlusion low-pass and the master limiter. One per scene (the first counts). The gun tails are recorded in their spaces and send
// nothing (Send Tail stays 0).
//
// Level: the reverb return of a space is wet / dry = (the sound's send) x (the space's Wet dB below + the zone's Wet (dB) trim); the
// impulse responses are energy-normalised, so a send of 1 puts the return at that many dB under the dry sound.
struct ReverbBusComponent {
    bool Enabled = true;
    float ReturnLevel = 1.0f;          // linear gain of the wet signal into the mix (times the SFX bus volume)
    float WetScale = 1.0f;             // linear, on every space's wet level
    float GlideTime = 0.35f;           // seconds a change of space takes (the crossfade between impulse responses)
    // Calibrated wet / dry at the listener (dB) per kind of space, for a send of 1.
    float WetOutdoorOpenDb = -20.0f;
    float WetOutdoorUrbanDb = -12.0f;
    float WetIndoorSmallDb = -6.0f;
    float WetIndoorLargeDb = -4.0f;
    // The Wet levels are calibrated on white noise (the impulse responses are energy-normalised); real material (K-weighted, the
    // impacts and shots a room is heard with) comes back about 4 dB hotter, since the recorded rooms ring longest where that
    // material has its energy. This trim makes the Wet levels what is heard.
    float WetTrimDb = -4.0f;
    float LowCutHz = 80.0f;            // the reverb input's high-pass for a space with no Low Cut of its own
    // Sends by category (linear, into the reverb after the voice's own fader and spatialisation; 1 = the space's calibrated level).
    float SendFoley = 1.0f;            // cloth, jumps, landings, ADS / equip / fire-mode gear
    float SendFootsteps = 1.0f;
    float SendActions = 1.0f;          // each gun's own mags, bolts, pumps, shells
    float SendImpacts = 1.0f;          // bullet impacts, flesh hits, flybys
    float SendCasings = 1.0f;
    float SendShot = 1.0f;             // the close / mech / sub layers of a gunshot: the crack feeds the room
    float SendTail = 0.0f;             // the recorded tails already hold their room
    // Occlusion: a 3D voice whose line of sight from the listener is blocked by solid geometry is low-passed.
    bool OcclusionEnabled = true;
    float OcclusionCutoff = 900.0f;    // Hz of a fully occluded voice
    float OcclusionInterval = 0.15f;   // seconds between checks of one voice
    int OcclusionRaysPerFrame = 8;
    float OcclusionMinDistance = 3.0f; // closer sources are never occluded
    float OcclusionGlide = 10.0f;      // per second (also how fast a sound heard through a portal follows a door swinging)
    // Portals (Reverb Portal components): a voice in another room is heard from the portal that joins the rooms.
    bool PortalsEnabled = true;
    float RemoteReverbHold = 5.0f;     // seconds the remote room's reverb keeps running after the last voice heard through a portal
    // The master peak limiter (the last node before the output).
    bool MasterLimiterEnabled = true;
    float MasterCeilingDb = -1.0f;     // dBFS
    float MasterLookaheadMs = 1.5f;
    float MasterReleaseMs = 80.0f;
};

// A quad ambience bed (Game/Audio/AmbienceBed): the Sound's variants as looping 3D voices on four speakers Radius from the listener at
// fixed compass points (north +Z, east +X, south, west, one variant each). They follow the listener's position but not its turning,
// so the bed stays put in the world while your head turns through it. Indoors (the share of the mix the listener's indoor Reverb
// Zones claim) it drops by Indoor Gain dB and darkens to Indoor Cutoff Hz; an open portal to the outside within Door Reach opens it
// back up by how open it is and how near; the indoor amount moves at most 1 per Glide seconds, so no doorway snaps it. One per
// scene (the first counts).
struct AmbienceBedComponent {
    bool Enabled = true;
    std::string Sound = "snd.amb.night_field"; // a sound key whose variants are the speakers' loops
    float Volume = 1.0f;               // linear, on top of the sound's mix_db
    float Radius = 15.0f;              // metres from the listener
    float Height = 2.0f;               // metres above the listener
    float FadeIn = 3.0f;               // seconds from silence when Play starts
    float IndoorGainDb = -10.0f;
    float IndoorCutoffHz = 4000.0f;    // the night bed is crickets: nearly all of it sits above 2.5 kHz
    float Glide = 1.5f;                // seconds for the indoor amount to go all the way in or out
    float DoorReach = 4.0f;            // metres from an open outside portal at which it starts to open the bed up
};

// The dynamic mix (Game/Audio/AudioMix): ducking under loud events, the aim-down-sights focus, air absorption with distance, the
// master bus's glue compressor and trim. One per scene (the first counts); a scene without one plays these defaults. The Audio
// panel (Window > Audio) edits the live values and can write them back here.
struct AudioMixComponent {
    bool DuckEnabled = true;
    float DuckThresholdDb = -20.0f;    // an event this loud at the listener (dB re the player's shot) starts to duck ...
    float DuckFullDb = -6.0f;          // ... and this loud ducks by the full depths
    float DuckAttackMs = 15.0f;
    float DuckHold = 0.15f;            // seconds at depth after the last loud event
    float DuckRelease = 0.8f;          // seconds back up from full depth
    float DuckBedDb = 6.0f;            // depths by group (dB): the ambience beds ...
    float DuckOwnFoleyDb = 3.0f;       // ... the player's own steps, cloth, gear ...
    float DuckDebrisDb = 3.0f;         // ... shell casings ...
    float DuckWorldDb = 0.0f;          // ... impacts, bodies (soldiers' shots / steps, flybys and hit markers are never ducked)
    bool FocusEnabled = true;          // aiming down sights brings the world forward
    float FocusBedDb = 4.0f;
    float FocusOwnFoleyDb = 3.0f;
    float FocusDebrisDb = 2.0f;
    float FocusTime = 0.25f;           // seconds in / out
    bool AirEnabled = true;            // 3D voices lose their top end with distance
    float AirStartDistance = 15.0f;    // metres: open up to here ...
    float AirExponent = 0.6f;          // ... then cutoff = 20 kHz x (start / d)^exponent
    float AirMinHz = 4000.0f;
    bool GlueEnabled = true;           // a gentle bus compressor before the limiter: holds a firefight together, the limiter barely works
    float GlueThresholdDb = -15.0f;    // dBFS (peak detector; after the trim)
    float GlueRatio = 2.0f;
    float GlueKneeDb = 6.0f;
    float GlueAttackMs = 15.0f;        // lets a shot's transient through, then holds the body
    float GlueReleaseMs = 200.0f;
    float MasterTrimDb = -3.0f;        // the whole mix, before the glue and the limiter: a sustained firefight integrates near -19 LUFS
                                       // (AAA practice: fights -18..-20, a whole session about -24), the limiter all but idle
};

// Bullet impacts, shell casings and rounds passing the listener (Game/Audio/ImpactAudio). One per scene (the first counts).
struct ImpactAudioComponent {
    bool Enabled = true;
    std::string SurfaceTable = "metal=metal,steel,iron,grate;wood=wood,plank,floor,parquet;tile=tile;carpet=carpet,rug;glass=glass;ice=ice,snow;dirt=dirt,soil,sand,grass,gravel,mud;concrete=concrete,asphalt,stone,brick";
    std::string DefaultSurface = "concrete";
    bool CasingsEnabled = true;
    int CasingMaxContacts = 2;
    float CasingMinSpeed = 0.9f;
    float CasingFullSpeed = 4.0f;
    float CasingGainMin = 0.25f;
    float CasingVolume = 1.0f;
    float CasingMinDistance = 1.5f;
    float CasingMaxDistance = 25.0f;
    int CasingMaxVoices = 6;
    float ShellRadius = 0.0075f;
    bool ImpactsEnabled = true;
    float ImpactVolume = 1.0f;
    float ImpactMinDistance = 2.0f;
    float ImpactMaxDistance = 60.0f;
    int ImpactMaxVoices = 8;
    float ImpactMinInterval = 0.03f;
    bool FleshUsesRecordings = true;
    float FleshVolume = 1.0f;
    bool FlybyEnabled = true;
    float FlybyRadius = 5.0f;
    float FlybyVolume = 1.0f;
    float FlybyMinInterval = 0.07f;
    float FlybyMinDistance = 0.5f;
    float FlybyMaxDistance = 14.0f;
    float FlybyFarGain = 0.35f;
};
