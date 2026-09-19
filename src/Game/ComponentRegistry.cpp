#include "ComponentRegistry.h"

#include "Components.h"

#include <IconsFontAwesome6.h>

#include <cstring>

// ---------------------------------------------------------------------------------------------
// House rule: ship components and controls fully wired, or not visible (#215)
//
//  1. A component or import setting is not shown in the editor until its runtime behaviour is
//     implemented. If the field must exist for serialization, keep it in the struct and the
//     serializer but leave it out of the Inspector.
//  2. A component that ships with runtime behaviour ships with an Inspector section and an Add
//     Component entry in the same change, not a follow-up.
//  3. If something must be visible before it works, show it disabled, labelled
//     "(not implemented)", with a tooltip naming the tracking issue.
//
// Registering a component here generates its serialization, Inspector section and Add Component
// entry together, so both failure modes are impossible by construction. Prefer it. Every struct
// in Components.h must be registered here or listed in tools/component_registration_allowlist.txt
// with a reason; tools/check_component_registration.py enforces that in CI.
//
// Opt-outs: ReflectComponent::GenericSerialize / GenericInspector (both default true) let a
// registered component keep a hand-written JSON block and/or Inspector section for state that
// isn't plain reflected fields. Use sparingly. Current users: Mesh Renderer (both — see its
// registration below), Collider and Joint (GenericSerialize only; their bespoke fields are
// EditorHidden and drawn by DrawReflectedComponentExtra).
// ---------------------------------------------------------------------------------------------

namespace ComponentRegistry {

namespace {
std::vector<RegisteredComponent>& Storage() {
    static std::vector<RegisteredComponent> registry;
    return registry;
}
} // namespace

const std::vector<RegisteredComponent>& All() { return Storage(); }

void Add(RegisteredComponent entry) { Storage().push_back(std::move(entry)); }

void RegisterEngineComponents() {
    using T = ReflectFieldType;

    {
        ReflectComponent m;
        m.Name = "Goal Trigger"; m.Icon = ICON_FA_BULLSEYE; m.Category = "Gameplay";
        m.Tooltip = "Scores when an object with the Tag drops into this trigger collider (Is Trigger on).\n"
                    "Adds points to the scene's Scoreboard, bursts child Particle Systems, flashes child\n"
                    "Lights and plays the Score Sound.";
        m.Fields = {
            { "Tag", T::String, TARTARUS_REFLECT_FIELD(GoalTriggerComponent, Tag), 0.0f,
              "Only objects with this Tag score." },
            { "Team", T::Enum, TARTARUS_REFLECT_FIELD(GoalTriggerComponent, Team), 0.0f,
              "Whose score a goal here adds to." },
            { "Points", T::Int, TARTARUS_REFLECT_FIELD(GoalTriggerComponent, Points), 1.0f,
              "Points per goal.", 0.0f, 100.0f },
            { "Three Points", T::Int, TARTARUS_REFLECT_FIELD(GoalTriggerComponent, ThreePoints), 1.0f,
              "Points for a long shot (see Three Point Distance).", 0.0f, 100.0f },
            { "Three Point Distance", T::Float, TARTARUS_REFLECT_FIELD(GoalTriggerComponent, ThreePointDistance), 0.05f,
              "A goal thrown from at least this far away (flat distance from where the gravity gun\n"
              "released it) scores Three Points. 0 = off.", 0.0f, 1000.0f },
            { "Require Downward", T::Bool, TARTARUS_REFLECT_FIELD(GoalTriggerComponent, RequireDownward), 0.0f,
              "Only count objects moving down (through a hoop from above)." },
            { "Score Sound", T::AssetRef, TARTARUS_REFLECT_FIELD(GoalTriggerComponent, ScoreSound), 0.0f,
              "Played at the goal when it scores." },
            { "Flash Intensity", T::Float, TARTARUS_REFLECT_FIELD(GoalTriggerComponent, FlashIntensity), 0.5f,
              "Child Lights jump to this intensity on a goal and fade back.", 0.0f, 1000.0f },
        };
        m.Fields[1].EnumLabels = "Home\0Away\0"; m.Fields[1].EnumCount = 2;
        m.Fields[6].AssetKind = ReflectAssetKind::Sound;
        Register<GoalTriggerComponent>(std::move(m));
    }
    Register<ScoreboardComponent>({
        "Scoreboard", ICON_FA_TABLE_LIST,
        "Holds the Home and Away score that Goal Triggers add to (the first Scoreboard in the scene).\n"
        "Score Digit objects show it. Play -> Stop puts it back to the values here.",
        "Gameplay",
        {
            { "Home", T::Int, TARTARUS_REFLECT_FIELD(ScoreboardComponent, Home), 1.0f, "Home score.", 0.0f, 999.0f },
            { "Away", T::Int, TARTARUS_REFLECT_FIELD(ScoreboardComponent, Away), 1.0f, "Away score.", 0.0f, 999.0f },
        },
    });
    {
        ReflectComponent m;
        m.Name = "Score Digit"; m.Icon = ICON_FA_HASHTAG; m.Category = "Gameplay";
        m.Tooltip = "A seven-segment digit of a team's score. Lights its children named \"Seg A\" .. \"Seg G\"\n"
                    "(A top, B top right, C bottom right, D bottom, E bottom left, F top left, G middle)\n"
                    "by raising their material's emission.";
        m.Fields = {
            { "Team", T::Enum, TARTARUS_REFLECT_FIELD(ScoreDigitComponent, Team), 0.0f, "Whose score it shows." },
            { "Place", T::Enum, TARTARUS_REFLECT_FIELD(ScoreDigitComponent, Place), 0.0f,
              "Which digit: ones, or tens (blank below 10)." },
            { "On Strength", T::Float, TARTARUS_REFLECT_FIELD(ScoreDigitComponent, OnStrength), 0.05f,
              "Emissive strength of a lit segment.", 0.0f, 100.0f },
            { "Off Strength", T::Float, TARTARUS_REFLECT_FIELD(ScoreDigitComponent, OffStrength), 0.005f,
              "Emissive strength of an unlit segment.", 0.0f, 100.0f },
        };
        m.Fields[0].EnumLabels = "Home\0Away\0"; m.Fields[0].EnumCount = 2;
        m.Fields[1].EnumLabels = "Ones\0Tens\0"; m.Fields[1].EnumCount = 2;
        Register<ScoreDigitComponent>(std::move(m));
    }
    {
        ReflectComponent m;
        m.Name = "Impact Sound"; m.Icon = ICON_FA_VOLUME_HIGH; m.Category = "Audio";
        m.Tooltip = "Plays a sound where this object hits something, louder the harder the hit.\n"
                    "Needs a Collider. Both objects in a hit play their own Impact Sound.";
        m.Fields = {
            { "Clip", T::AssetRef, TARTARUS_REFLECT_FIELD(ImpactSoundComponent, Clip), 0.0f, "The sound." },
            { "Volume", T::Float, TARTARUS_REFLECT_FIELD(ImpactSoundComponent, Volume), 0.01f,
              "Volume of the hardest hit.", 0.0f, 1.0f },
            { "Min Speed", T::Float, TARTARUS_REFLECT_FIELD(ImpactSoundComponent, MinSpeed), 0.05f,
              "Hits slower than this (m/s) are silent.", 0.0f, 100.0f },
            { "Max Speed", T::Float, TARTARUS_REFLECT_FIELD(ImpactSoundComponent, MaxSpeed), 0.1f,
              "Hits at this speed (m/s) or faster play at full Volume.", 0.01f, 200.0f },
            { "Pitch Variation", T::Float, TARTARUS_REFLECT_FIELD(ImpactSoundComponent, PitchVariation), 0.005f,
              "Random pitch change per hit, +/- this fraction.", 0.0f, 0.5f },
        };
        m.Fields[0].AssetKind = ReflectAssetKind::Sound;
        m.Fields[1].Slider = true; m.Fields[1].Format = "%.2f";
        Register<ImpactSoundComponent>(std::move(m));
    }

    Register<SpinComponent>({
        "Spin", ICON_FA_ARROWS_SPIN,
        "Spins the object around a local axis while playing (SpinSystem, in TartarusGame.dll).",
        "Scripts",
        {
            { "Axis",  T::Vec3,  TARTARUS_REFLECT_FIELD(SpinComponent, Axis),  0.01f, "Local axis to rotate around." },
            { "Speed", T::Float, TARTARUS_REFLECT_FIELD(SpinComponent, Speed), 1.0f,  "Degrees per second." },
        },
    });

    // Migrated from hand-coded serialization/Inspector code onto reflection (#184). Previously
    // this component had runtime behaviour (TransformControllerSystem, in TartarusGame.dll) but
    // no Inspector section or Add Component entry at all — a violation of rule 2 at the top of
    // this file, invisible until now because nothing else in the editor referenced it.
    // Runtime-only scratch fields (Initialized, Base*, Elapsed) are deliberately not reflected:
    // Play -> Stop reloads the authored scene snapshot, same as before.
    Register<TransformControllerComponent>({
        "Transform Controller", ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT,
        "Drag-and-drop motion script: rotates, translates and scale-pulses the object while playing.",
        "Scripts",
        {
            { "Script Path", T::String, TARTARUS_REFLECT_FIELD(TransformControllerComponent, ScriptPath), 0.0f,
              "Informational: which .tescript asset this behaviour was dragged from." },
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(TransformControllerComponent, Enabled), 0.0f,
              "Runs the motion while playing when checked." },
            { "Rotation Deg/Sec", T::Vec3, TARTARUS_REFLECT_FIELD(TransformControllerComponent, RotationDegPerSec), 1.0f,
              "Continuous local rotation, degrees per second per axis." },
            { "Translation Units/Sec", T::Vec3, TARTARUS_REFLECT_FIELD(TransformControllerComponent, TranslationUnitsPerSec), 0.1f,
              "Continuous local translation, units per second per axis." },
            { "Scale Pulse Amplitude", T::Float, TARTARUS_REFLECT_FIELD(TransformControllerComponent, ScalePulseAmplitude), 0.01f,
              "Fraction of the authored scale (0.2 = +/-20%).", -2.0f, 2.0f },
            { "Scale Pulse Frequency Hz", T::Float, TARTARUS_REFLECT_FIELD(TransformControllerComponent, ScalePulseFrequencyHz), 0.05f,
              "Scale pulse cycles per second.", 0.0f, 50.0f },
        },
    });

    // Migrated from hand-coded serialization/Inspector code onto reflection (#184) — the third
    // such migration. Unlike Camera/Audio/Collider (all ruled out: they carry multi-select
    // and/or custom widgets the generic path doesn't support), Animator was plain fields only,
    // so this is a pure move with no functional loss besides the sub-heading grouping the old
    // hand-written section had (see the field-naming note on AnimatorComponent in Components.h).
    // Runtime-only scratch fields (Initialized, Base*, BaseColor, Elapsed) are not reflected.
    // #175 / #113 — skeletal clip playback. Clip is drawn by a custom combo (the model's clip
    // names) in DrawReflectedComponentExtra, so it's EditorHidden here but still serialized.
    {
        static const char* kWrapLabels = "Once\0Loop\0Ping Pong\0Clamp Forever\0";
        ReflectComponent m;
        m.Name = "Animation"; m.Icon = ICON_FA_FILM; m.Category = "Rendering";
        m.Tooltip = "Plays one of this object's model animation clips in Play mode. Game code can switch the clip "
                    "(it crossfades) or stop / start it.";
        m.Key = "animation";
        m.Fields = {
            { "Clip", T::String, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, Clip), 0.0f,
              "Which clip to play (empty = the model's first clip)." },
            { "Play Automatically", T::Bool, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, PlayAutomatically), 0.0f,
              "Start playing as soon as Play mode begins." },
            { "Wrap Mode", T::Enum, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, WrapMode), 0.0f,
              "Once: play to the end, then stop. Loop: repeat. Ping Pong: forwards then backwards.\n"
              "Clamp Forever: play to the end and hold the last frame." },
            { "Speed", T::Float, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, Speed), 0.01f,
              "Playback rate (1 = authored speed; negative plays backwards).", -10.0f, 10.0f },
            { "Cross Fade", T::Float, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, CrossFade), 0.01f,
              "Seconds to blend when the clip changes while playing.", 0.0f, 5.0f },
        };
        m.Fields[0].EditorHidden = true;
        m.Fields[2].EnumLabels = kWrapLabels;
        m.Fields[2].EnumCount = 4;
        Register<SkeletalAnimationComponent>(std::move(m));
    }

    // #175 Part B - the Animator Controller state machine. Controller is an EditorHidden string:
    // the Inspector draws a picker of the project's .controller files plus the controller's
    // states / parameters / transitions editor (EditorLayer_Inspector.cpp, keyed "Animator Controller").
    {
        ReflectComponent m;
        m.Name = "Animator Controller"; m.Icon = ICON_FA_DIAGRAM_PROJECT; m.Category = "Rendering";
        m.Tooltip = "Plays this object's model clips from a state machine (a .controller file): states, and "
                    "crossfaded transitions on parameters that game code sets.";
        m.Fields = {
            { "Controller", T::String, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, Controller), 0.0f,
              "The .controller file this object runs." },
            { "Speed", T::Float, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, Speed), 0.01f,
              "Multiplies every state's playback speed.", 0.0f, 10.0f },
        };
        m.Fields[0].EditorHidden = true;
        Register<AnimatorControllerComponent>(std::move(m));
    }

    Register<AnimatorComponent>({
        "Animator", ICON_FA_PERSON_RUNNING,
        "Procedural motion driven every frame in Play mode - continuous spin, orbit around an "
        "axis, vertical bob, and light hue-cycling. All fields are additive and reversible "
        "(turning a rate back to 0 undoes its contribution).",
        "Scripts",
        {
            { "Spin Deg/Sec", T::Vec3, TARTARUS_REFLECT_FIELD(AnimatorComponent, SpinDegPerSec), 1.0f,
              "Continuous local rotation, in degrees/second per axis." },
            { "Orbit Axis", T::Vec3, TARTARUS_REFLECT_FIELD(AnimatorComponent, OrbitAxis), 0.01f,
              "Axis this object revolves around, relative to its base position." },
            { "Orbit Speed", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, OrbitDegPerSec), 0.5f,
              "Revolution rate around the orbit axis, in degrees/second." },
            { "Orbit Radius", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, OrbitRadius), 0.05f,
              "Distance from the base position while orbiting, in world units." },
            { "Bob Amplitude", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, BobAmplitude), 0.01f,
              "Vertical sine offset from the base position, in world units." },
            { "Bob Frequency Hz", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, BobFreqHz), 0.02f,
              "Bob rate in Hz (cycles/second)." },
            { "Color Cycle Hz", T::Float, TARTARUS_REFLECT_FIELD(AnimatorComponent, ColorCycleHzPerSec), 0.01f,
              "Hue revolutions/second for this object's Light color. 0 leaves the color alone. "
              "Has no effect without a Light component." },
        },
    });

    // Migrated from hand-coded serialization/Inspector code onto reflection (#302 Wave 1a). The
    // three clip-plane / FOV fields are plain floats and multi-select already works generically
    // (#235); the one Camera-specific control, the "Align to View" button (it reads the editor
    // camera and writes this entity's TransformComponent), stays editor-side as a registered
    // inspector-extra keyed "Camera" (EditorLayer_Inspector.cpp), since reflection describes data
    // only and the editor camera isn't visible from TartarusGame.dll.
    Register<CameraComponent>({
        "Camera", ICON_FA_VIDEO,
        "The Game view renders through this camera while editing, so you can frame a shot "
        "without walking there. In Play it's the game camera too, unless the scene has a "
        "First Person Controller.",
        "Rendering",
        {
            { "Field of View", T::Float, TARTARUS_REFLECT_FIELD(CameraComponent, FovDegrees), 0.25f,
              "Vertical field of view, in degrees.", 1.0f, 179.0f },
            { "Near", T::Float, TARTARUS_REFLECT_FIELD(CameraComponent, NearPlane), 0.01f,
              "Closest distance the camera renders.", 0.001f, 100.0f },
            { "Far", T::Float, TARTARUS_REFLECT_FIELD(CameraComponent, FarPlane), 1.0f,
              "Farthest distance the camera renders.", 0.1f, 100000.0f },
        },
    });

    // #165 - the Play-mode player, configurable per scene instead of hard-wired in main.cpp.
    Register<FirstPersonControllerComponent>({
        "First Person Controller", ICON_FA_PERSON_WALKING,
        "Play spawns the first-person player here (feet at this position, facing this object's "
        "forward) with the settings below. WASD to move, Shift to sprint, Space to jump.",
        "Gameplay",
        {
            { "Move Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, MoveSpeed), 0.05f,
              "Walking speed, metres per second.", 0.0f, 100.0f },
            { "Sprint Multiplier", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, SprintMultiplier), 0.01f,
              "Speed multiplier while Shift is held.", 1.0f, 10.0f },
            { "Jump Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, JumpSpeed), 0.05f,
              "Upward launch speed. Jump height is about Jump Speed^2 / (2 x Gravity).", 0.0f, 50.0f },
            { "Eye Height", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, EyeHeight), 0.01f,
              "Camera height above the feet.", 0.1f, 10.0f },
            { "Capsule Radius", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, CapsuleRadius), 0.01f,
              "Collision capsule radius.", 0.05f, 5.0f },
            { "Capsule Height", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, CapsuleHeight), 0.01f,
              "Total collision capsule height, feet to head.", 0.2f, 10.0f },
            { "Mouse Sensitivity", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, MouseSensitivity), 0.005f,
              "Degrees of turn per pixel of mouse movement.", 0.01f, 1.0f },
            { "Invert Y", T::Bool, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, InvertY), 0.0f,
              "Moving the mouse up looks down." },
            { "Field of View", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, FieldOfView), 0.25f,
              "Vertical field of view, in degrees.", 20.0f, 150.0f },
            { "Kill Height", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, KillY), 0.5f,
              "Falling below this world height respawns the player at this object.", -100000.0f, 100000.0f },
            { "Gravity", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, Gravity), 0.1f,
              "How hard the player falls, m/s\xC2\xB2. Separate from the physics world's gravity (Project\n"
              "Settings > Physics, Earth's 9.81 by default): most games give the player a heavier,\n"
              "snappier fall than real life. Jump height is about Jump Speed\xC2\xB2 / (2 x Gravity).", 0.0f, 200.0f },
            { "Gravity Gun", T::Bool, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, GravityGun), 0.0f,
              "The built-in tool: right mouse picks up a rigidbody, left mouse throws it." },
            { "Min Throw Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, MinThrowSpeed), 0.1f,
              "Gravity gun: launch speed (m/s) of a quick click. Hold left mouse to charge up.", 0.0f, 200.0f },
            { "Max Throw Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, MaxThrowSpeed), 0.1f,
              "Gravity gun: launch speed (m/s) when fully charged.", 0.0f, 200.0f },
            { "Throw Charge Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ThrowChargeTime), 0.01f,
              "Gravity gun: seconds of holding left mouse to reach Max Throw Speed.", 0.05f, 10.0f },
            { "Throw Backspin", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ThrowBackspin), 0.05f,
              "Gravity gun: backspin (revolutions per second) put on a thrown ball, like a real shot.\n"
              "Only round (sphere collider) bodies get it.", 0.0f, 20.0f },
        },
    });

    // #177 - a basic CPU particle emitter (fire, sparks, smoke, dust). Simulated every frame,
    // in the editor as well as Play, so it can be tuned live; the live particles aren't saved.
    {
        static const char* kBlendLabels = "Alpha Blended\0Additive\0";
        ReflectComponent m;
        m.Name = "Particle System"; m.Icon = ICON_FA_FIRE; m.Category = "Effects";
        m.Tooltip = "Emits soft, camera-facing particles from this object along its local +Y axis.";
        m.Fields = {
            { "Emitting", T::Bool, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Emitting), 0.0f,
              "Spawn new particles. Turning it off lets the live ones finish." },
            { "Rate", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Rate), 0.5f,
              "Particles spawned per second.", 0.0f, 10000.0f },
            { "Max Particles", T::Int, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, MaxParticles), 1.0f,
              "Upper limit on live particles; emission pauses while at the limit.", 1.0f, 100000.0f },
            { "Lifetime", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Lifetime), 0.01f,
              "How long each particle lives, in seconds (varies by about 15%).", 0.01f, 60.0f },
            { "Start Speed", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, StartSpeed), 0.05f,
              "Launch speed, metres per second.", 0.0f, 200.0f },
            { "Spread", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Spread), 0.5f,
              "Cone half-angle around the object's +Y axis, in degrees. 0 = a straight jet, 180 = every direction.",
              0.0f, 180.0f },
            { "Start Size", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, StartSize), 0.005f,
              "Particle diameter when spawned, in metres.", 0.0f, 50.0f },
            { "End Size", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, EndSize), 0.005f,
              "Particle diameter at the end of its life.", 0.0f, 50.0f },
            { "Start Color", T::Color, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, StartColor), 0.0f,
              "Colour when spawned." },
            { "End Color", T::Color, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, EndColor), 0.0f,
              "Colour at the end of its life." },
            { "Start Alpha", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, StartAlpha), 0.01f,
              "Opacity when spawned.", 0.0f, 1.0f },
            { "End Alpha", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, EndAlpha), 0.01f,
              "Opacity at the end of its life (0 = fades out).", 0.0f, 1.0f },
            { "Intensity", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, Intensity), 0.05f,
              "Brightness multiplier. Above 1 glows through Bloom.", 0.0f, 100.0f },
            { "Gravity Modifier", T::Float, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, GravityModifier), 0.01f,
              "How much project gravity pulls the particles (1 = falls like a rigidbody, negative rises).",
              -10.0f, 10.0f },
            { "Blend Mode", T::Enum, TARTARUS_REFLECT_FIELD(ParticleSystemComponent, BlendMode), 0.0f,
              "Additive: brightens what's behind (fire, sparks, magic). Alpha Blended: covers it (smoke, dust)." },
        };
        m.Fields.back().EnumLabels = kBlendLabels;
        m.Fields.back().EnumCount = 2;
        Register<ParticleSystemComponent>(std::move(m));
    }

    // Migrated from hand-coded serialization/Inspector code onto reflection (#302 Wave 2b) — the
    // first component to use every widget hint the reflection layer has (Enum, Color, sliders,
    // log scale, format strings, per-Kind VisibleIf, the "Shadows" TreeNode group). Kind values
    // must stay Point=0 / Spot=1 / Directional=2 to match LightComponent::Type and the VisibleIf
    // gates below. ColorTempK is reflected (so it serializes) but EditorHidden — the Kelvin bar,
    // eyedropper and Look-through / Drop-to-surface buttons are editor-only and live in
    // EditorLayer::DrawReflectedComponentExtra / ...ExtraMulti keyed "Light".
    {
        ReflectComponent m;
        m.Name = "Light"; m.Icon = ICON_FA_LIGHTBULB; m.Category = "Rendering";
        m.Tooltip = "Casts light into the scene from this object's position.";
        const char* kMoonShadows = ICON_FA_MOON "  Shadows";
        m.Fields = {
            { "Type", T::Enum, TARTARUS_REFLECT_FIELD(LightComponent, Kind), 0.0f,
              "Point: all directions. Spot: a cone. Directional: a sun (parallel rays, no position or range)." },
            { "Color", T::Color, TARTARUS_REFLECT_FIELD(LightComponent, Color), 0.0f,
              "The light's color. The button below drives it from a color temperature instead." }, // #19
            { "ColorTempK", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, ColorTempK) },
            { "Intensity", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Intensity), 0.05f,
              "Brightness multiplier - higher is brighter.", 0.0f, 100.0f },
            { "Range", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Range), 0.1f,
              "Distance (in world units) at which the light's effect fades to zero.", 0.1f, 200.0f },
            { "Spot Angle", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, SpotAngleDegrees), 0.5f,
              "Half-angle of the light cone, in degrees.\nThe cone points along the entity's -Z axis - use Rotation to aim it.",
              1.0f, 89.0f },
            { "Angular Size", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, AngularSizeDegrees), 0.05f,
              "Apparent diameter of the sun disc, in degrees (~0.53 = Earth's sun).\nWider = softer shadows.\n"
              "Aim the sun with the entity's Rotation (it shines along -Z).", 0.1f, 20.0f },
            { "Cast Shadows", T::Bool, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.Enabled), 0.0f,
              "Render a shadow map for this light. Spot: one perspective pass (up to 4). Point: a\n"
              "6-face cube (up to 2). Directional: cascaded maps for the sun." },
            { "Shadow Bias", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.Bias), 0.02f,
              "Multiplier on the shader's depth bias. >1 pushes the shadow off contact\n(fixes acne); <1 pulls it back toward the caster.",
              0.0f, 4.0f },
            { "Shadow Normal Bias", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.NormalBias), 0.02f,
              "Multiplier on the normal-offset term. Widen if edges show light bleed.", 0.0f, 4.0f },
            { "Shadow Softness", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.Softness), 0.02f,
              "Multiplier on the PCF filter radius. Higher = wider, softer penumbra.", 0.0f, 4.0f },
            { "Shadow Near Plane", T::Float, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.NearPlane), 0.01f,
              "Perspective near distance for this light's depth pass.\nRaise it to reclaim depth precision when the light sits far from what it lights.",
              0.001f, 10.0f },
            { "Shadow Resolution", T::Enum, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.Resolution), 0.0f,
              "Per-light shadow-map size (applies after per-light shadow maps)." },
            { "Shadow Update Mode", T::Enum, TARTARUS_REFLECT_FIELD(LightComponent, Shadow.UpdateMode), 0.0f,
              "Dynamic re-renders every frame; Static bakes once (applies after per-light shadow maps)." },
            // #203 - drawn as a layer checklist by DrawReflectedComponentExtra("Light").
            { "Culling Mask", T::Int, TARTARUS_REFLECT_FIELD(LightComponent, CullingMask), 0.0f,
              "Which layers this light illuminates." },
        };
        auto F = [&](const char* name) -> ReflectField& {
            for (auto& f : m.Fields) if (std::strcmp(f.Name, name) == 0) return f;
            return m.Fields[0];
        };
        F("Type").EnumLabels = "Point\0Spot\0Directional\0"; F("Type").EnumCount = 3;
        // Colour + temperature share one control (RGB swatch OR a Kelvin bar, mutually
        // exclusive), plus an eyedropper — all editor-only, so both fields serialize but the
        // widget lives in DrawReflectedComponentExtra / ...ExtraMulti ("Light").
        F("Color").EditorHidden = true;
        F("ColorTempK").EditorHidden = true;
        F("Intensity").Slider = true; F("Intensity").Logarithmic = true; F("Intensity").Format = "%.2f";
        F("Range").Slider = true; F("Range").Logarithmic = true; F("Range").Format = "%.1f";
        F("Range").VisibleIfField = "Type"; F("Range").VisibleIfValue = 2; F("Range").VisibleIfNot = true;
        F("Spot Angle").Slider = true; F("Spot Angle").Format = "%.0f deg";
        F("Spot Angle").VisibleIfField = "Type"; F("Spot Angle").VisibleIfValue = 1;
        F("Angular Size").Slider = true; F("Angular Size").Format = "%.2f deg";
        F("Angular Size").VisibleIfField = "Type"; F("Angular Size").VisibleIfValue = 2;
        for (const char* g : { "Cast Shadows", "Shadow Bias", "Shadow Normal Bias", "Shadow Softness",
                               "Shadow Near Plane", "Shadow Resolution", "Shadow Update Mode" })
            F(g).Group = kMoonShadows;
        for (const char* s : { "Shadow Bias", "Shadow Normal Bias", "Shadow Softness" }) {
            F(s).Slider = true; F(s).Format = "x%.2f";
        }
        F("Shadow Near Plane").Slider = true; F("Shadow Near Plane").Logarithmic = true; F("Shadow Near Plane").Format = "%.3f";
        for (const char* s : { "Shadow Near Plane", "Shadow Resolution", "Shadow Update Mode" }) {
            F(s).VisibleIfField = "Type"; F(s).VisibleIfValue = 2; F(s).VisibleIfNot = true;
        }
        F("Shadow Resolution").EnumLabels = "Follow global\0" "512\0" "1024\0" "2048\0" "4096\0";
        F("Shadow Resolution").EnumCount = 5;
        F("Shadow Update Mode").EnumLabels = "Dynamic\0" "Static (bake once)\0" "Off\0";
        F("Shadow Update Mode").EnumCount = 3;
        F("Culling Mask").EditorHidden = true;
        Register<LightComponent>(std::move(m));
    }

    // Migrated from hand-coded serialization/Inspector code onto reflection (#302 Wave 3) — first
    // user of ReflectFieldType::AssetRef (the Clip field picks from AssetLibrary::Sounds()). The
    // "Preview" button is editor-only and lives in DrawReflectedComponentExtra keyed "Audio Source".
    {
        ReflectComponent m;
        m.Name = "Audio Source"; m.Icon = ICON_FA_VOLUME_HIGH; m.Category = "Audio";
        m.Tooltip = "A sound clip that can be played from this object.";
        m.Fields = {
            { "Clip", T::AssetRef, TARTARUS_REFLECT_FIELD(AudioSourceComponent, SoundPath), 0.0f,
              "Which imported sound this object plays.\nImport sounds via File > Import, or the Asset Browser." },
            { "Volume", T::Float, TARTARUS_REFLECT_FIELD(AudioSourceComponent, Volume), 0.01f,
              "Playback volume - 1 is unattenuated.", 0.0f, 1.0f },
            { "Loop", T::Bool, TARTARUS_REFLECT_FIELD(AudioSourceComponent, Loop), 0.0f,
              "Restart the clip automatically when it finishes." },
            { "Play On Start", T::Bool, TARTARUS_REFLECT_FIELD(AudioSourceComponent, PlayOnStart), 0.0f,
              "Plays this clip automatically the instant Play mode is entered." },
            { "Output", T::Enum, TARTARUS_REFLECT_FIELD(AudioSourceComponent, Output), 0.0f,
              "Mixer bus this source plays through. Each bus has its own volume in\n"
              "Project Settings > Audio (on top of the Master volume)." },
            { "3D Sound", T::Bool, TARTARUS_REFLECT_FIELD(AudioSourceComponent, Spatial), 0.0f,
              "On: heard from this object's position - panned left/right and quieter with distance.\n"
              "Off: plain 2D, the same everywhere (music, UI sounds)." },
            { "Volume Rolloff", T::Enum, TARTARUS_REFLECT_FIELD(AudioSourceComponent, Rolloff), 0.0f,
              "How volume falls off with distance.\nLogarithmic: realistic, loud up close, long quiet tail.\n"
              "Linear: fades evenly and is silent at Max Distance." },
            { "Min Distance", T::Float, TARTARUS_REFLECT_FIELD(AudioSourceComponent, MinDistance), 0.05f,
              "Within this many metres the sound plays at full volume.", 0.01f, 10000.0f },
            { "Max Distance", T::Float, TARTARUS_REFLECT_FIELD(AudioSourceComponent, MaxDistance), 0.5f,
              "Beyond this the volume stops dropping (Logarithmic) or is silent (Linear).", 0.02f, 10000.0f },
        };
        m.Fields[0].AssetKind = ReflectAssetKind::Sound;
        m.Fields[1].Slider = true; m.Fields[1].Format = "%.2f";
        m.Fields[4].EnumLabels = "SFX\0Music\0Ambient\0UI\0Voice\0"; // #171 - AudioEngine::Bus order
        m.Fields[4].EnumCount = 5;
        m.Fields[6].EnumLabels = "Logarithmic\0Linear\0";
        m.Fields[6].EnumCount = 2;
        Register<AudioSourceComponent>(std::move(m));
    }

    // #171 - Unity's Audio Listener.
    Register<AudioListenerComponent>({
        "Audio Listener", ICON_FA_HEADPHONES,
        "In Play, 3D sounds are heard from this object (its position and facing) instead of from "
        "the game camera - e.g. put it on the player's head in a third-person game.",
        "Audio",
        {
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(AudioListenerComponent, Enabled), 0.0f,
              "When off, the game camera is the listener again." },
        },
    });

    // #185 PR 4 — Rigidbody. Plain reflected fields; the shape comes from the sibling
    // ColliderComponent (hand-written, PR 2) and PhysicsWorld reads both on Play-enter.
    {
        ReflectComponent m;
        m.Name = "Rigidbody"; m.Icon = ICON_FA_WEIGHT_HANGING; m.Category = "Physics";
        m.Tooltip =
            "Simulates this collider as a dynamic PhysX body while playing - it falls, tumbles and "
            "gets pushed around, and its pose is written back every frame. Needs a Collider for its "
            "shape. Play -> Stop restores the authored pose.";
        m.Fields = {
            { "Mass", T::Float, TARTARUS_REFLECT_FIELD(RigidbodyComponent, Mass), 0.05f,
              "Body mass in kg. Ignored while Kinematic.", 0.001f, 1000.0f },
            { "Use Gravity", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, UseGravity), 0.0f,
              "When off the body still collides but doesn't fall." },
            { "Is Kinematic", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, IsKinematic), 0.0f,
              "Driven from this object's Transform each frame (e.g. an Animator-moved platform) "
              "instead of by forces - pushes other bodies, isn't pushed back." },
            { "Initial Velocity", T::Vec3, TARTARUS_REFLECT_FIELD(RigidbodyComponent, InitialVelocity), 0.1f,
              "Linear velocity (units/sec) applied once, the moment Play starts." },
            { "Linear Damping", T::Float, TARTARUS_REFLECT_FIELD(RigidbodyComponent, LinearDamping), 0.01f,
              "Per-second bleed of linear velocity. 0 = drifts forever.", 0.0f, 10.0f },
            { "Angular Damping", T::Float, TARTARUS_REFLECT_FIELD(RigidbodyComponent, AngularDamping), 0.01f,
              "Per-second bleed of spin.", 0.0f, 10.0f },
            { "Continuous Collision", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, ContinuousCollision), 0.0f,
              "Swept collision so a fast small body can't tunnel a thin wall (#185). Costs a little." },
            { "Freeze Position X", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezePositionX), 0.0f,
              "Lock linear motion along world X." },
            { "Freeze Position Y", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezePositionY), 0.0f,
              "Lock linear motion along world Y." },
            { "Freeze Position Z", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezePositionZ), 0.0f,
              "Lock linear motion along world Z." },
            { "Freeze Rotation X", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezeRotationX), 0.0f,
              "Lock rotation about world X." },
            { "Freeze Rotation Y", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezeRotationY), 0.0f,
              "Lock rotation about world Y." },
            { "Freeze Rotation Z", T::Bool, TARTARUS_REFLECT_FIELD(RigidbodyComponent, FreezeRotationZ), 0.0f,
              "Lock rotation about world Z." },
            { "Interpolate", T::Enum, TARTARUS_REFLECT_FIELD(RigidbodyComponent, Interpolation), 0.0f,
              "Physics steps at a fixed rate (60 Hz by default); this smooths motion between steps on\n"
              "faster displays. Interpolate: smooth, one step behind. Extrapolate: predicted from\n"
              "velocity, no lag but can overshoot. None: snaps to each step (judders above 60 FPS)." },
        };
        auto F = [&](const char* name) -> ReflectField& {
            for (auto& f : m.Fields) if (std::strcmp(f.Name, name) == 0) return f;
            return m.Fields[0];
        };
        for (const char* g : { "Freeze Position X", "Freeze Position Y", "Freeze Position Z",
                               "Freeze Rotation X", "Freeze Rotation Y", "Freeze Rotation Z" })
            F(g).Group = "Constraints";
        F("Interpolate").EnumLabels = "None\0Interpolate\0Extrapolate\0"; // #168
        F("Interpolate").EnumCount = 3;
        Register<RigidbodyComponent>(std::move(m));
    }

    // PR14 (#333) — Reflection Probe. Registered with the generic reflection path so the
    // Add Component menu, Inspector, and SceneSerializer all work for free.
    Register<ReflectionProbeComponent>({
        "Reflection Probe", ICON_FA_CIRCLE_HALF_STROKE,
        "Defines a box volume for parallax-corrected environment reflections. "
        "Place inside rooms or near reflective surfaces; the 2 nearest probes "
        "blend per draw call. No probes → global sky IBL fallback.",
        "Rendering",
        {
            { "Size", T::Vec3, TARTARUS_REFLECT_FIELD(ReflectionProbeComponent, Size), 0.1f,
              "Full extents of the box capture volume in world units.\n"
              "The parallax correction clips reflection rays to this box boundary.", 0.0f, 0.0f },
            { "Importance", T::Float, TARTARUS_REFLECT_FIELD(ReflectionProbeComponent, Importance), 0.05f,
              "Blend weight priority. Higher wins 2-probe selection when many probes overlap.", 0.0f, 10.0f },
        },
    });

    // Phase 4 (#6) item 1 — Collider. Registered for the generic Inspector/Add-Component path
    // only: GenericSerialize stays false because a dedicated hand-written pass in
    // SceneSerializer.cpp already reads/writes it under a "collider" JSON key predating this
    // reflection layer (issue #185 PR 2) — flipping GenericSerialize on would have the generic
    // per-component pass write it a second time under a different key, corrupting saved scenes
    // with no benefit (Inspector genericization works directly against the live struct, entirely
    // independent of how it's persisted). HalfExtents is EditorHidden: it means something
    // different per Shape (three half-extents for Box, just .x as a radius for Sphere, .x/.y as
    // radius/half-height for Capsule, unused for Convex Hull/Mesh) — a single ReflectField can't
    // relabel/reshape itself like that, so the per-shape size row stays hand-coded in
    // DrawReflectedComponentExtra("Collider", Bottom), same escape hatch Light's Kelvin bar uses.
    {
        ReflectComponent m;
        m.Name = "Collider"; m.Icon = ICON_FA_CUBE; m.Category = "Physics";
        static const char* kCombineLabels = "Average\0Minimum\0Multiply\0Maximum\0";
        m.Tooltip = "Blocks movement and is hit by raycasts. While playing this drives a PhysX actor:\n"
                    "static on its own, dynamic/kinematic with a sibling Rigidbody. The shape below\n"
                    "picks its geometry.";
        m.GenericSerialize = false;
        m.Key = "collider"; // matches the hand-written SceneSerializer JSON key (#185 PR 2) — see ReflectComponent::Key
        m.Fields = {
            { "Shape", T::Enum, TARTARUS_REFLECT_FIELD(ColliderComponent, Kind), 0.0f,
              "Box / Sphere / Capsule are analytic primitives sized by the fields below.\n"
              "Convex Hull / Mesh cook a shape from this object's mesh + scale:\n"
              "Convex Hull works on any body; Mesh (triangle mesh) is static / kinematic only\n"
              "(a dynamic Mesh collider falls back to a convex hull)." },
            { "Is Trigger", T::Bool, TARTARUS_REFLECT_FIELD(ColliderComponent, IsTrigger), 0.0f,
              "A trigger reports enter / stay / exit overlaps to gameplay\nbut doesn't block movement." },
            { "Center", T::Vec3, TARTARUS_REFLECT_FIELD(ColliderComponent, Center), 0.05f,
              "Shape offset from the object's origin, local space." },
            { "Bounciness", T::Float, TARTARUS_REFLECT_FIELD(ColliderComponent, Bounciness), 0.0f,
              "Restitution: 0 stops dead, 1 loses no energy on a bounce.", 0.0f, 1.0f },
            { "Friction", T::Float, TARTARUS_REFLECT_FIELD(ColliderComponent, Friction), 0.0f,
              "Dynamic friction: how much a sliding contact is slowed. 0 = ice.", 0.0f, 2.0f },
            { "Static Friction", T::Float, TARTARUS_REFLECT_FIELD(ColliderComponent, StaticFriction), 0.0f,
              "How hard it is to start sliding. Usually equal to or a little above Friction.", 0.0f, 2.0f },
            { "Friction Combine", T::Enum, TARTARUS_REFLECT_FIELD(ColliderComponent, FrictionCombine), 0.0f,
              "How this collider's friction combines with the one it touches.\n"
              "If the two differ, the later mode in the list wins (Maximum beats everything)." },
            { "Bounce Combine", T::Enum, TARTARUS_REFLECT_FIELD(ColliderComponent, BounceCombine), 0.0f,
              "How this collider's bounciness combines with the one it touches (same rule)." },
            { "HalfExtents", T::Vec3, TARTARUS_REFLECT_FIELD(ColliderComponent, HalfExtents), 0.05f },
        };
        auto F = [&](const char* name) -> ReflectField& {
            for (auto& f : m.Fields) if (std::strcmp(f.Name, name) == 0) return f;
            return m.Fields[0];
        };
        F("Shape").EnumLabels = "Box\0Sphere\0Capsule\0Convex Hull\0Mesh\0"; F("Shape").EnumCount = 5;
        F("Bounciness").Slider = true; F("Bounciness").Format = "%.2f";
        F("Friction").Slider = true; F("Friction").Format = "%.2f";
        F("Static Friction").Slider = true; F("Static Friction").Format = "%.2f";
        F("Friction Combine").EnumLabels = kCombineLabels; F("Friction Combine").EnumCount = 4;
        F("Bounce Combine").EnumLabels = kCombineLabels; F("Bounce Combine").EnumCount = 4;
        F("HalfExtents").EditorHidden = true;
        Register<ColliderComponent>(std::move(m));
    }

    // Phase 4 (#6) item 1 — Joint. Same GenericSerialize = false reasoning as Collider (a
    // dedicated hand-written "joint" JSON pass predates this, #185 PR 11). ConnectedOrder needs a
    // custom entity picker (enumerates every OrderComponent in the scene, not a fixed asset/enum
    // list); Axis is only meaningful for Hinge/Slider and UseLimit/LimitLower/LimitUpper only for
    // Hinge/Slider/Distance — VisibleIfField only expresses "equals" or "not-equals" ONE sibling
    // value, not membership in a set of two, so these stay EditorHidden and hand-coded together in
    // DrawReflectedComponentExtra("Joint", Bottom) rather than stretching the reflection schema
    // for one component (see the schema note if a second component ever needs this).
    {
        ReflectComponent m;
        m.Name = "Joint"; m.Icon = ICON_FA_LINK; m.Category = "Physics";
        m.Tooltip = "Constrains this body to another (or the world) while playing. Needs a Rigidbody\n"
                    "on this object and, unless connected to the world, on the target too.";
        m.GenericSerialize = false;
        m.Key = "joint"; // matches the hand-written SceneSerializer JSON key (#185 PR 11) — see ReflectComponent::Key
        m.Fields = {
            { "Type", T::Enum, TARTARUS_REFLECT_FIELD(JointComponent, Kind), 0.0f,
              "Fixed weld; Hinge/Slider use the Axis; Ball is a point pivot; Distance is a leash." },
            { "Anchor", T::Vec3, TARTARUS_REFLECT_FIELD(JointComponent, Anchor), 0.05f,
              "Joint point in this object's local space." },
            { "Break Force", T::Float, TARTARUS_REFLECT_FIELD(JointComponent, BreakForce), 1.0f,
              "Force (N) that snaps the joint. 0 = unbreakable.", 0.0f, 1.0e6f, false, false, "%.0f" },
            { "Break Torque", T::Float, TARTARUS_REFLECT_FIELD(JointComponent, BreakTorque), 1.0f,
              "Torque (N\xC2\xB7m) that snaps the joint. 0 = unbreakable.", 0.0f, 1.0e6f, false, false, "%.0f" },
            { "ConnectedOrder", T::Int, TARTARUS_REFLECT_FIELD(JointComponent, ConnectedOrder) },
            { "Axis", T::Vec3, TARTARUS_REFLECT_FIELD(JointComponent, Axis), 0.02f },
            { "UseLimit", T::Bool, TARTARUS_REFLECT_FIELD(JointComponent, UseLimit) },
            { "LimitLower", T::Float, TARTARUS_REFLECT_FIELD(JointComponent, LimitLower), 0.5f },
            { "LimitUpper", T::Float, TARTARUS_REFLECT_FIELD(JointComponent, LimitUpper), 0.5f },
        };
        auto F = [&](const char* name) -> ReflectField& {
            for (auto& f : m.Fields) if (std::strcmp(f.Name, name) == 0) return f;
            return m.Fields[0];
        };
        F("Type").EnumLabels = "Fixed\0Hinge\0Ball\0Slider\0Distance\0"; F("Type").EnumCount = 5;
        for (const char* h : { "ConnectedOrder", "Axis", "UseLimit", "LimitLower", "LimitUpper" })
            F(h).EditorHidden = true;
        Register<JointComponent>(std::move(m));
    }

    // #315 — Mesh Renderer. Registered so the component-registration guard rail counts it and
    // its Icon/Category live here, but BOTH generic paths are opted out: RenderableComponent
    // holds a shared_ptr<Model> (no reflectable fields), its scene form is the box "color/size"
    // or model "path" + "material" written by dedicated code in SceneSerializer, and its add
    // stashes/restores the mesh via DetachedMeshComponent using the editor-only AssetLibrary.
    // The Inspector section (mesh picker, primitive popup, drag-drop, ping, animation controls,
    // the level-geometry Color row) stays hand-coded in EditorLayer_Inspector.cpp, permanently —
    // settled #6 item 3, not a deferred follow-up: ModelRef is a live shared_ptr<Model>, not an
    // AssetRef-shaped path, so genericising it would mean rewriting RenderableComponent's storage,
    // not just its Inspector widget. (ComponentRegistry can't depend on src/Editor's AssetLibrary
    // either, so a new reflected field type for it would carry no drawable metadata anyway.)
    {
        ReflectComponent m;
        m.Name = "Mesh Renderer"; m.Icon = ICON_FA_DRAW_POLYGON; m.Category = "Rendering";
        m.Tooltip = "The mesh this object draws, and its material color/texture options.";
        m.GenericSerialize = false;
        m.GenericInspector = false;
        Register<RenderableComponent>(std::move(m));
    }

    // #132 - String fields that hold asset paths: tracked by GUID so renaming or moving the file
    // outside the editor keeps the reference (see ReflectField::AssetPath).
    const std::pair<const char*, const char*> kAssetPathFields[] = {
        {"Animation", "Clip"},
        {"Animator Controller", "Controller"},
        {"Transform Controller", "Script Path"},
    };
    for (const auto& [component, field] : kAssetPathFields)
        for (RegisteredComponent& rc : Storage())
            if (std::strcmp(rc.Meta.Name, component) == 0)
                for (ReflectField& f : rc.Meta.Fields)
                    if (std::strcmp(f.Name, field) == 0) f.AssetPath = true;
}

// Runs once, before main(). It only appends to All()'s function-local static, so there is no
// static-init-order dependency.
namespace {
struct AutoRegister {
    AutoRegister() { RegisterEngineComponents(); }
} g_autoRegister;
} // namespace

} // namespace ComponentRegistry
