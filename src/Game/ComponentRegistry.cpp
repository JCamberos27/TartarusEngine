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

namespace {
// The four root-motion fields (RootMotionOptions) starting at `first`: stable keys, the mode's
// labels, one collapsible group, and the options hidden while it's Off. The bone is drawn by a
// custom picker (the model's bones), so it's EditorHidden.
void SetupRootMotionFields(ReflectComponent& m, size_t first) {
    static const char* kModeLabels = "Off\0Apply\0In Place\0";
    static const char* kKeys[] = {"rootMotion", "rootMotionBone", "rootMotionRotation", "rootMotionVertical"};
    for (size_t i = 0; i < 4; ++i) {
        ReflectField& f = m.Fields[first + i];
        f.Key = kKeys[i];
        f.Group = "Root Motion";
        if (i > 0) { f.VisibleIfField = "Root Motion"; f.VisibleIfValue = 0; f.VisibleIfNot = true; }
    }
    m.Fields[first].EnumLabels = kModeLabels;
    m.Fields[first].EnumCount = 3;
    m.Fields[first + 1].EditorHidden = true;
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
    Register<HealthComponent>({
        "Health", ICON_FA_HEART,
        "Hit points: rounds that hit this object take them away, and at 0 it is dead.\n"
        "Starts full every Play.",
        "Gameplay",
        {
            { "Max", T::Float, TARTARUS_REFLECT_FIELD(HealthComponent, Max), 1.0f, "Full health.", 1.0f, 100000.0f },
            { "Invulnerable", T::Bool, TARTARUS_REFLECT_FIELD(HealthComponent, Invulnerable), 0.0f,
              "Hits register but take nothing." },
        },
    });
    {
        ReflectComponent m;
        m.Name = "NPC Spawn"; m.Icon = ICON_FA_PERSON_RIFLE; m.Category = "AI";
        m.Tooltip = "An enemy soldier appears here in Play, facing this object's forward. The squad is built\n"
                    "from these spawns (see Squad Settings); replacements come back at the one farthest\n"
                    "out of the player's sight.";
        m.Fields = {
            { "Weapon", T::Enum, TARTARUS_REFLECT_FIELD(NpcSpawnComponent, Weapon), 0.0f, "What the soldier carries." },
            { "Squad", T::Int, TARTARUS_REFLECT_FIELD(NpcSpawnComponent, Squad), 1.0f,
              "Soldiers with the same Squad share what they know and fight together.", 0.0f, 16.0f },
            { "Skill", T::Float, TARTARUS_REFLECT_FIELD(NpcSpawnComponent, Skill), 0.01f,
              "0 = green (slow, inaccurate, timid) .. 1 = veteran.", 0.0f, 1.0f },
            { "Outfit Seed", T::Int, TARTARUS_REFLECT_FIELD(NpcSpawnComponent, OutfitSeed), 1.0f,
              "Which random outfit. 0 = a different one every Play.", 0.0f, 100000.0f },
            { "Brain", T::Enum, TARTARUS_REFLECT_FIELD(NpcSpawnComponent, Brain), 0.0f,
              "Squad AI, or a Training Dummy that just stands there and takes hits." },
        };
        m.Fields[0].EnumLabels = "AKS-74U\0Remington 870\0Random\0"; m.Fields[0].EnumCount = 3;
        m.Fields[2].Slider = true; m.Fields[2].Format = "%.2f";
        m.Fields[4].EnumLabels = "Squad AI\0Training Dummy\0"; m.Fields[4].EnumCount = 2;
        Register<NpcSpawnComponent>(std::move(m));
    }
    Register<SquadSettingsComponent>({
        "Squad Settings", ICON_FA_USERS,
        "The enemy squad's rules for this scene (the first Squad Settings counts).",
        "AI",
        {
            { "Squad Size", T::Int, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, SquadSize), 1.0f,
              "Enemies alive at once.", 0.0f, 8.0f },
            { "Respawn", T::Bool, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, Respawn), 0.0f,
              "Replace enemies that die." },
            { "Respawn Delay", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, RespawnDelay), 0.1f,
              "Seconds before a dead enemy's replacement appears.", 0.0f, 120.0f },
            { "Difficulty", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, Difficulty), 0.01f,
              "Scales the enemies' accuracy and reaction speed.", 0.25f, 2.0f },
            { "NPC Damage Scale", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, NpcDamageScale), 0.01f,
              "Enemy rounds do this much of their weapon's damage to the player.", 0.0f, 4.0f },
            { "Heavy Hit Damage", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, HeavyHitDamage), 0.5f,
              "A single hit of at least this much damage (from the player) staggers the enemy.", 0.0f, 500.0f },
            { "Stagger Time", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, StaggerTime), 0.01f,
              "Seconds an enemy's aim is paused after a heavy hit.", 0.0f, 5.0f },
            { "Bleed-Out Time", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, BleedOutTime), 0.5f,
              "A wounded (downed) enemy dies this many seconds after going down.", 0.0f, 300.0f },
            { "Crawl Speed", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, CrawlSpeed), 0.01f,
              "A wounded enemy crawls at this speed (m/s).", 0.0f, 5.0f },
            { "Limp Speed Scale", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, LimpSpeedScale), 0.01f,
              "A leg-shot enemy moves at this fraction of its normal speed.", 0.05f, 1.0f },
            { "Limp Time", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, LimpTime), 0.1f,
              "Seconds a leg wound slows an enemy.", 0.0f, 120.0f },
            { "Corpse Time", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, CorpseTime), 0.5f,
              "Seconds a dead enemy's body stays before it is removed.", 0.0f, 600.0f },
            { "Fall Gravity", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, FallGravity), 0.1f,
              "Gravity (m/s^2) on an enemy that steps off a ledge. Ragdolls use the physics world's gravity.", 0.0f, 60.0f },
            { "Melee Damage", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, MeleeDamage), 0.5f,
              "Damage of a rifle-butt strike at the player (scaled by Difficulty, 0.5 to 1.5).", 0.0f, 500.0f },
            { "Melee Time", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, MeleeTime), 0.01f,
              "A strike's length, wind-up to recovery (seconds).", 0.1f, 3.0f },
            { "Melee Hit Time", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, MeleeHitTime), 0.01f,
              "The blow lands this many seconds into the strike.", 0.0f, 3.0f },
            { "Hitbox Range", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, HitboxRange), 1.0f,
              "Enemies further than this from the player (m) keep only their movement capsule, no per-bone hitboxes.", 5.0f, 500.0f },
            { "Foot IK Range", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, FootIKRange), 0.5f,
              "Enemies this close (m) and in view put their feet on uneven ground.", 0.0f, 200.0f },
            { "Mesh Check Range", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, MeshCheckRange), 0.5f,
              "Within this distance (m) the weapon hold checks the gun and elbows against the drawn body.", 0.0f, 100.0f },
            { "Cover Sample Spacing", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, CoverSpacing), 0.05f,
              "Metres between cover samples along a navigation-mesh edge.", 0.3f, 4.0f },
            { "Cover Reach", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, CoverReach), 0.05f,
              "How far beyond the edge something must stand to count as cover (m).", 0.2f, 3.0f },
            { "Low Cover Height", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, CoverKneeHeight), 0.05f,
              "Probe height for low cover (m): something solid here means crouch-behind cover.", 0.2f, 1.5f },
            { "High Cover Height", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, CoverHeadHeight), 0.05f,
              "Probe height for high cover (m): solid here too means stand-behind cover.", 0.8f, 2.5f },
            { "Cover Peek Step", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, CoverStep), 0.05f,
              "High cover: how far along the wall an enemy steps to peek round the end (m).", 0.2f, 3.0f },
        },
    });
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
              "Continuous local rotation: the object turns about this vector's own direction at its length in degrees per second, so (0,90,0) is a yaw at 90 deg/s and (1,1,0) a spin about that diagonal." },
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
            { "Root Motion", T::Enum, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, RootMotion.Mode), 0.0f,
              "What happens to the travel the clips author on the root bone.\n"
              "Off: the clips play as authored (a walk drags the mesh away and snaps back each loop).\n"
              "Apply: the travel moves this object instead, and the pose stays in place.\n"
              "In Place: the pose stays in place and the object doesn't move - game code reads the\n"
              "motion and moves it (e.g. through a character controller)." },
            { "Root Motion Bone", T::String, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, RootMotion.Bone), 0.0f,
              "The bone whose travel is the character's (empty = auto: \"root\", else the hips / pelvis)." },
            { "Root Motion Rotation", T::Bool, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, RootMotion.Rotation), 0.0f,
              "Turns in the clips turn the object. Off: the turn stays in the pose." },
            { "Root Motion Vertical", T::Bool, TARTARUS_REFLECT_FIELD(SkeletalAnimationComponent, RootMotion.Vertical), 0.0f,
              "Height changes in the clips move the object up and down. Off (usual): jumps and crouches\n"
              "stay in the pose, and the object keeps to the ground." },
        };
        m.Fields[0].EditorHidden = true;
        m.Fields[2].EnumLabels = kWrapLabels;
        m.Fields[2].EnumCount = 4;
        SetupRootMotionFields(m, 5);
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
            { "Track", T::String, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, Track), 0.0f,
              "Which of the controller's clip tracks this object plays (empty = the first).\n"
              "A controller can carry several clip sets per state, e.g. \"arms\" and \"weapon\"." },
            { "Root Motion", T::Enum, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, RootMotion.Mode), 0.0f,
              "What happens to the travel the clips author on the root bone.\n"
              "Off: the clips play as authored (a walk drags the mesh away and snaps back each loop).\n"
              "Apply: the travel moves this object instead, and the pose stays in place.\n"
              "In Place: the pose stays in place and the object doesn't move - game code reads the\n"
              "motion and moves it (e.g. through a character controller)." },
            { "Root Motion Bone", T::String, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, RootMotion.Bone), 0.0f,
              "The bone whose travel is the character's (empty = auto: \"root\", else the hips / pelvis)." },
            { "Root Motion Rotation", T::Bool, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, RootMotion.Rotation), 0.0f,
              "Turns in the clips turn the object. Off: the turn stays in the pose." },
            { "Root Motion Vertical", T::Bool, TARTARUS_REFLECT_FIELD(AnimatorControllerComponent, RootMotion.Vertical), 0.0f,
              "Height changes in the clips move the object up and down. Off (usual): jumps and crouches\n"
              "stay in the pose, and the object keeps to the ground." },
        };
        m.Fields[0].EditorHidden = true;
        SetupRootMotionFields(m, 3);
        Register<AnimatorControllerComponent>(std::move(m));
    }

    // Procedural IK on top of the Animator Controller's pose (src/Game/IK.h). Runs in Play,
    // after the controller blends its layers and before the pose reaches the renderer.
    {
        ReflectComponent m;
        m.Name = "IK Rig"; m.Icon = ICON_FA_HAND; m.Category = "Rendering";
        m.Tooltip = "Inverse kinematics on this object's animated pose: up to two two-bone limbs (arms, legs)\n"
                    "and a look-at. Needs an Animator Controller on the same object.";
        m.Fields = {
            { "Enabled", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, Enabled), 0.0f,
              "Run the rig." },
            { "Weight", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, Weight), 0.01f,
              "Blend from the animated pose (0) to the solved one (1).", 0.0f, 1.0f },

            { "Limb A Enabled", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.Enabled), 0.0f, "Solve this limb." },
            { "Limb A Upper", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.Upper), 0.0f, "Upper bone, e.g. upperarm_r." },
            { "Limb A Lower", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.Lower), 0.0f, "Middle bone, e.g. lowerarm_r." },
            { "Limb A End", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.End), 0.0f, "End bone, e.g. hand_r." },
            { "Limb A Target", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.Target), 0.0f, "Bone the end reaches for." },
            { "Limb A Keep Animated Offset", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.KeepAnimatedOffset), 0.0f,
              "Reach for where the end sat relative to Target in the animated pose (hands keep\n"
              "their grip on a procedurally moved gun), instead of for Target itself." },
            { "Limb A Match Rotation", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.MatchRotation), 0.0f,
              "Also turn the end bone to the goal's rotation." },
            { "Limb A Weight", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbA.Weight), 0.01f, "Limb blend.", 0.0f, 1.0f },

            { "Limb B Enabled", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.Enabled), 0.0f, "Solve this limb." },
            { "Limb B Upper", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.Upper), 0.0f, "Upper bone, e.g. upperarm_l." },
            { "Limb B Lower", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.Lower), 0.0f, "Middle bone, e.g. lowerarm_l." },
            { "Limb B End", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.End), 0.0f, "End bone, e.g. hand_l." },
            { "Limb B Target", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.Target), 0.0f, "Bone the end reaches for." },
            { "Limb B Keep Animated Offset", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.KeepAnimatedOffset), 0.0f,
              "Reach for where the end sat relative to Target in the animated pose, instead of for Target itself." },
            { "Limb B Match Rotation", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.MatchRotation), 0.0f,
              "Also turn the end bone to the goal's rotation." },
            { "Limb B Weight", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LimbB.Weight), 0.01f, "Limb blend.", 0.0f, 1.0f },

            { "Look At Enabled", T::Bool, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtEnabled), 0.0f, "Aim a bone at another." },
            { "Look At Bone", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtBone), 0.0f, "The bone that turns, e.g. head." },
            { "Look At Target", T::String, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtTarget), 0.0f, "The bone it faces." },
            { "Look At Aim Axis", T::Vec3, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtAxis), 0.01f,
              "The bone's local axis that should point at the target." },
            { "Look At Max Angle", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtMaxAngle), 0.5f,
              "Largest turn, in degrees.", 0.0f, 180.0f },
            { "Look At Weight", T::Float, TARTARUS_REFLECT_FIELD(IKRigComponent, LookAtWeight), 0.01f, "Look-at blend.", 0.0f, 1.0f },
        };
        // Collapsible groups in the Inspector.
        for (size_t i = 0; i < m.Fields.size(); ++i) {
            m.Fields[i].Group = i < 2 ? nullptr : i < 10 ? "Limb A" : i < 18 ? "Limb B" : "Look At";
        }
        Register<IKRigComponent>(std::move(m));
    }

    Register<AnimatorComponent>({
        "Animator", ICON_FA_PERSON_RUNNING,
        "Procedural motion driven every frame in Play mode - continuous spin, orbit around an "
        "axis, vertical bob, and light hue-cycling. All fields are additive and reversible "
        "(turning a rate back to 0 undoes its contribution).",
        "Scripts",
        {
            { "Spin Deg/Sec", T::Vec3, TARTARUS_REFLECT_FIELD(AnimatorComponent, SpinDegPerSec), 1.0f,
              "Continuous local rotation: the object turns about this vector's own direction at its length in degrees per second, so (0,90,0) is a yaw at 90 deg/s and (1,1,0) a spin about that diagonal." },
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
            { "Jump Buffer Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, JumpBufferTime), 0.005f,
              "A jump pressed this many seconds before landing still happens the moment you land.", 0.0f, 0.5f },
            { "Coyote Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, CoyoteTime), 0.005f,
              "A jump pressed this many seconds after stepping off an edge still counts.", 0.0f, 0.5f },
            { "Ground Accel Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, GroundAccelTime), 0.005f,
              "How quickly the player gets up to speed on the ground: the time constant, in seconds (about 2.3x\n"
              "this reaches 90% of full speed). 0 = instantly.", 0.0f, 1.0f },
            { "Ground Decel Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, GroundDecelTime), 0.005f,
              "... and slows down on letting go. 0 = stops dead.", 0.0f, 1.0f },
            { "Air Accel Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, AirAccelTime), 0.005f,
              "How quickly the input steers the player in the air. Higher keeps more of the jump's momentum.\n"
              "0 = full air control.", 0.0f, 5.0f },
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
            { "Stick Look Deg/Sec", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, StickLookDegPerSec), 5.0f,
              "Gamepad right stick: turn rate in degrees per second (not a delta).", 1.0f, 720.0f },
            { "Eye Radius", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, EyeRadius), 0.001f,
              "Camera lean collision: sphere radius used for wall detection, metres (keeps the near plane off the wall).", 0.01f, 1.0f },
            { "Field of View", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, FieldOfView), 0.25f,
              "Horizontal field of view in degrees, measured on a 16:9 screen (90 = the usual shooter FOV). "
              "Wider screens see more at the sides.", 30.0f, 150.0f },
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
            { "Grab Range", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, GrabRange), 1.0f,
              "Gravity gun: aiming distance for the primary pick-up ray, metres.", 10.0f, 1000.0f },
            { "Assist Range", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, AssistRange), 1.0f,
              "Gravity gun: search radius when no object is under the exact crosshair, metres.", 1.0f, 500.0f },
            { "Assist Cone Deg", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, AssistConeDeg), 0.5f,
              "Gravity gun: within this many degrees of the crosshair during aim assist, degrees.", 1.0f, 45.0f },
            { "Scroll Turn Deg", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ScrollTurnDeg), 0.5f,
              "Gravity gun: rotation applied per scroll notch while holding an object, degrees.", 1.0f, 90.0f },
            { "Animation Set", T::String, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, AnimationSet), 0.0f,
              "Optional .fpsanim asset for a camera-bound first-person arms and weapon presentation." },
            { "Secondary Animation Set", T::String, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, SecondaryAnimationSet), 0.0f,
              "Optional second weapon (.fpsanim). In Play, 1 draws the Animation Set, 3 this one and 2 goes\n"
              "unarmed; switching holsters the weapon in hand first, and each weapon keeps its own ammo." },
            { "View Model Offset", T::Vec3, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ViewModelOffset), 0.01f,
              "Residual nudge, in the play camera's frame, applied on top of the Camera Bone\n"
              "anchor. Leave at zero unless you are deliberately nudging the view model." },
            { "Camera Bone", T::String, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, CameraBone), 0.0f,
              "Rig bone the play camera sits on (\"head\" by default). The view model is placed so\n"
              "this bone lands exactly on the camera. Empty puts the model's root there instead." },
            { "View Model Rotation", T::Vec3, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ViewModelRotation), 0.5f,
              "Euler rotation offset, in degrees, applied after the play camera orientation." },
            { "View Model Scale", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ViewModelScale), 0.01f,
              "Scale of the first-person arms and weapon presentation.", 0.01f, 100.0f },
            { "View Model FOV", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, ViewModelFov), 0.25f,
              "Vertical FOV, in degrees, the first-person arms and weapon are projected with in the\n"
              "renderer's view-model pass (drawn after a depth clear, so the world can't clip them).\n"
              "Independent of the world camera's FOV: this changes how the held weapon is framed,\n"
              "never the scene behind it. Narrower than the world FOV by default, which is what\n"
              "keeps the weapon reading as held instead of stretched.", 20.0f, 150.0f },
            { "Max Health", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, MaxHealth), 1.0f,
              "The player's health. Enemy rounds take it; at 0 the player dies and respawns here.", 1.0f, 100000.0f },
            { "Regen Delay", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, RegenDelay), 0.05f,
              "Seconds without being hit before health starts coming back.", 0.0f, 120.0f },
            { "Regen Rate", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, RegenRate), 0.5f,
              "Health per second coming back after Regen Delay. 0 = no regeneration.", 0.0f, 10000.0f },
            { "Respawn Delay", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, RespawnDelay), 0.05f,
              "Seconds from death to respawning here.", 0.0f, 60.0f },
            { "Spawn Protection", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonControllerComponent, SpawnProtection), 0.05f,
              "Seconds after a respawn when nothing can hurt the player.", 0.0f, 60.0f },
        },
    });

    // True FPS (#405) - the player's own body under the play camera, walked by root motion.
    {
        ReflectComponent m;
        m.Name = "First Person Body"; m.Icon = ICON_FA_PERSON; m.Category = "Gameplay";
        m.Tooltip = "The player's own body in Play: it stands at the First Person Controller's feet facing the\n"
                    "view, its Animator Controller gets the movement as parameters (MoveX, MoveY, Speed, Sprint,\n"
                    "Grounded, Airborne, Jump), and the clips' root motion walks the player. The camera sits\n"
                    "in its head. Put it on the body's root; its children are the pieces (head, torso, legs,\n"
                    "feet or clothing), and the first one with an Animator Controller drives the rest.";
        m.Fields = {
            { "Responsiveness", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, Responsiveness), 0.01f,
              "How the player moves: 0 = exactly as the clips travel (root motion - weighty, feet planted),\n"
              "1 = as the input asks (snappy; the clips only animate). In between blends the two.", 0.0f, 1.0f },
            { "Parameter Smoothing", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ParamSmoothing), 0.005f,
              "Seconds MoveX / MoveY take to follow the input - how quickly the body changes gait.", 0.0f, 1.0f },
            { "Run Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, RunSpeed), 0.01f,
              "Metres per second asked of the blend tree when moving. Match a clip's speed (the jog, 3.26).", 0.0f, 20.0f },
            { "Sprint Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, SprintSpeed), 0.01f,
              "... and while sprinting (the run clip, 4.72).", 0.0f, 20.0f },
            { "Player Run Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, PlayerRunSpeed), 0.01f,
              "The player's own run speed (m/s) when it should be faster than the clips travel. 0 = Run Speed.\n"
              "With Responsiveness up the gait clips play faster to keep pace (up to Max Play Rate).", 0.0f, 20.0f },
            { "Player Sprint Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, PlayerSprintSpeed), 0.01f,
              "... and sprint speed. 0 = Sprint Speed.", 0.0f, 20.0f },
            { "Max Play Rate", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, MaxPlayRate), 0.01f,
              "The fastest the gait clips play (x authored) to keep the feet with a faster player. Needs the\n"
              "controller's Locomotion state to take its speed from a PlayRate parameter.", 1.0f, 3.0f },
            { "Head Bone", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, HeadBone), 0.0f,
              "The bone the camera sits on." },
            { "Camera Offset", T::Vec3, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, CameraOffset), 0.005f,
              "The eyes from the head bone, in the body's frame (X right, Y up, Z forward), metres." },
            { "Armed Eye Offset", T::Vec3, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmedEyeOffset), 0.005f,
              "With the gun out, the eye goes where the weapon's arms rig has its camera against its shoulders, which is\n"
              "low (a first-person rig's camera sits just over its shoulders). This lifts it back toward the eyes (body\n"
              "frame: X right, Y up, Z forward, metres) so the chest and collar stay out of view. The gun doesn't move on\n"
              "screen; the body sits lower under it, and the hands reach further. Too much and a hand comes off the gun.\n"
              "Applies near level; it eases out looking up or down (gone by 60 degrees), where it isn't needed." },
            { "Head Bob", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, HeadBob), 0.01f,
              "How much of the head's own motion the camera follows: 0 = steady at the head's standing\n"
              "height, 1 = locked to the head bone.", 0.0f, 1.0f },
            { "Camera Smoothing", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, CameraSmoothing), 0.005f,
              "Seconds of smoothing on the head's motion (in the body's frame, so it never lags behind).", 0.0f, 0.5f },
            { "Hidden Parts", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, HiddenParts), 0.0f,
              "Body pieces that cast a shadow but aren't drawn in first person, comma separated: a child\n"
              "is hidden when its name contains one of these. The camera sits in the head." },
            { "Hidden Bones", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, HiddenBones), 0.0f,
              "Bones collapsed in Play, comma separated (empty = none) - e.g. the arms of a one-piece\n"
              "body while a first-person arms model draws them." },
        };
        m.Fields.push_back({ "Weapon Arms", T::Bool, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, WeaponArms), 0.0f,
              "The body's arms hold the weapon: they take the first-person arms' pose and reach their\n"
              "hands onto theirs, and the separate arms model stops being drawn (its gun still is)." });
        m.Fields.push_back({ "Spine Aim", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, SpineAim), 0.01f,
              "How much of the camera's pitch the spine takes: 0 = upright, 1 = the chest tilts as far\n"
              "as the view. The shoulders follow the view, so the hands stay in reach.", 0.0f, 1.0f });
        m.Fields.push_back({ "Spine Aim Down", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, SpineAimDown), 0.01f,
              "Spine Aim for looking down. Armed, the camera hangs off the shoulders as the arms rig's does, so it clears the\n"
              "chest looking down only as far as the chest pitches with the view: lower it and the torso may be seen from inside.", 0.0f, 1.0f });
        m.Fields.push_back({ "Shoulder Line Match", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ShoulderLineMatch), 0.01f,
              "Armed, how far the chest takes the arms rig's stance - its shoulder line (bladed, the support shoulder forward) -\n"
              "instead of squaring to the view. 1 = the rig's: both hands then reach the gun without the shoulders moving.", 0.0f, 1.0f });
        m.Fields.push_back({ "Bone Map", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, BoneMap), 0.0f,
              "For a rig whose bones are not named like the UE5 mannequin: 'standard = theirs', comma or line\n"
              "separated, e.g. pelvis = Hips, foot_l = LeftFoot, foot_r = RightFoot. Unlisted bones keep the\n"
              "standard name. Weapon Arms still needs the body and the arms rig to share names." });
        m.Fields.push_back({ "Arms Piece", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmsPiece), 0.0f,
              "The child piece that is the body's arms (its name contains this). With Weapon Arms it is\n"
              "drawn with the gun, in the view-model pass, so its hands sit exactly where the rig's do." });
        m.Fields.push_back({ "Turn Threshold", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, TurnThreshold), 1.0f,
              "Standing still, the body keeps its heading until the view is this many degrees off it,\n"
              "then turns on the spot with the turn clips. 0 = the body always faces the view.", 0.0f, 170.0f });
        m.Fields.push_back({ "Spine Twist", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, SpineTwist), 0.01f,
              "How much of the view's twist off the body's heading the spine takes: the chest faces the\n"
              "view while the feet have not turned yet.", 0.0f, 1.0f });
        m.Fields.push_back({ "Max Turn Rate", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, MaxTurnRate), 5.0f,
              "Standing still, the view turns no faster than this (degrees a second) past the Turn Threshold, where the feet must step round: what\n"
              "the turn clips can keep up with, so the feet don't slide. 0 = no limit.", 0.0f, 720.0f });
        m.Fields.push_back({ "Crouch Height", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, CrouchHeight), 0.01f,
              "The capsule's height while crouching (hold Crouch, Left Ctrl by default). 0 = no crouching.", 0.0f, 3.0f });
        m.Fields.push_back({ "Crouch Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, CrouchSpeed), 0.01f,
              "The move speed while crouched, as a fraction of Run Speed. The crouch-walk clips travel\n"
              "about 1.35 m/s, so Run Speed x this should be near that or the feet slide.", 0.05f, 1.0f });
        m.Fields.push_back({ "Foot IK", T::Bool, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootIK), 0.0f,
              "Each foot is put on the ground under it, the pelvis drops to the lower foot and the legs are re-solved, so the feet meet stairs and slopes." });
        m.Fields.push_back({ "Foot IK Max Drop", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootIKMaxDrop), 0.01f,
              "The most the pelvis may drop (metres) to let the lower foot reach the ground.", 0.0f, 1.0f });
        m.Fields.push_back({ "Start Stop Clips", T::Bool, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StartStopClips), 0.0f,
              "Starting and stopping play their own clips (a push-off, a braking step) instead of blending\n"
              "straight between idle and the gait; their root motion eases the capsule up to speed and down." });
        m.Fields.push_back({ "Eye Slack", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, EyeSlack), 0.001f,
              "How far (metres) the camera may trail or lead the body's shoulders. A clip that throws the shoulders about (a stop pulling the body back)\nwould otherwise carry them out of the arms' reach and a hand would come off the gun. Bigger motions move the camera with them.", 0.0f, 0.2f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Reach Slack", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ReachSlack), 0.001f,
              "The body's shoulders sit this much (metres) further toward the gun than the arms rig's, so the support arm is never at full stretch\n(a straight arm jumps at every small motion).", 0.0f, 0.2f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Shrug Start", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ShrugStart), 0.01f,
              "A shoulder shrugs toward the gun once its hand is this fraction of the arm's length away, instead of the arm stretching.", 0.5f, 1.2f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Shrug Max", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ShrugMax), 0.005f,
              "The most (metres) a shoulder may shrug toward the gun.", 0.0f, 0.4f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Shoulder Max Angle", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ShoulderMaxAngle), 0.5f,
              "The most (degrees) a collarbone turns to move its shoulder, shrug and steadying together. The shoulder turns about the\n"
              "collarbone's inner end, like a real one; past this the arm straightens instead.", 0.0f, 60.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Clavicle Follow", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ClavicleFollow), 0.01f,
              "How much of the arms rig's collarbone motion the body takes (its arms and its chest's stance). The rig has no torso or\n"
              "head in the way, so a reload swings its collarbones freely: taken whole, the body's shoulder hunched up at the camera.\n"
              "1 = all of it, 0 = the body's own pose. The hands stay on the gun either way.", 0.0f, 1.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Reach Lean Max", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ReachLeanMax), 0.5f,
              "The most (degrees) the chest leans toward a hand still out of reach once its collarbone has turned all it may\n"
              "(looking far up or down, the gun moves further than the chest). 0 = off.", 0.0f, 45.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Arm Steadiness", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmSteadiness), 0.01f,
              "How much the arms ignore the body's walk and run sway (Weapon Arms): 1 = the shoulders are held steady against the view and the\n"
              "elbows bend the way the rig's do, so the gait never reaches the elbows; 0 = the arms follow the chest.", 0.0f, 1.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Arm Steady Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmSteadyTime), 0.01f,
              "Seconds the steadied shoulders take to follow a lasting change (a crouch, the view pitching). Longer than a step so the sway is filtered out.", 0.05f, 2.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Arm Steady Max", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmSteadyMax), 0.005f,
              "The most (metres) a shoulder is held off where the body's pose puts it; past this it is carried along.", 0.0f, 0.3f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Elbow Clearance", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ElbowClearance), 0.005f,
              "Split poses: how far (metres) each elbow, in every view but the player's own, keeps from the drawn torso. It swings out\n"
              "about the shoulder-to-hand line, so the hand stays on the gun. The rig tucks its elbows to a narrower body. 0 = off.", 0.0f, 0.2f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Look Down Push", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, LookDownPush), 0.005f,
              "Looking down, the eye comes this far (metres) forward over the chest. It moves the gun with it, away from the shoulders,\n"
              "so it costs the arms their reach - prefer Spine Aim Down. Straight down gets all of it. 0 = off.", 0.0f, 0.4f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Look Down Start", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, LookDownStart), 1.0f,
              "Degrees below level where the look-down push begins; it eases in from here to straight down.", 0.0f, 85.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Camera Hidden Bones", T::String, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, CameraHiddenBones), 0.0f,
              "Bones whose skin isn't drawn in the camera's own view on the pieces other than the arms, comma\n"
              "separated (at most 8): the torso's shoulders, which move with the arms' so the arms stay on them\n"
              "in every other view - but would bulge into the camera, where the arms are drawn over them." });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Near Hide", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NearHide), 0.005f,
              "Whatever of the body comes within this many metres of the eye is not drawn in the camera's view (its shadow stays). The camera\n"
              "sits in the body, and the near plane would otherwise slice the neck and shoulders into slivers. 0 = off.", 0.0f, 0.3f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Clothing Near Hide", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ClothingNearHide), 0.005f,
              "Near Hide for clothing (outfit pieces that aren't body parts), above, below and ahead of the eye - never less than Near Hide.\n"
              "Clothing is bulkier than skin and swings with the walk: a hood's rim or a jacket's shoulder would sweep across the view.", 0.0f, 0.4f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Clothing Near Hide Width", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ClothingNearHideWidth), 0.005f,
              "How far to either side of the eye clothing is hidden (metres): the shoulders' cloth sits off to the sides, at the\n"
              "view's edges, while the chest seen looking down is below. 0 = as far as Clothing Near Hide.", 0.0f, 0.5f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Collar Hide Drop", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, CollarHideDrop), 0.005f,
              "Clothing that sits around the neck in its bind pose - between the shoulder joints and no lower than this many metres\n"
              "below them - isn't drawn in the camera's view, whatever bones the pack skinned it to (a hood or a scarf on the chest).\n"
              "Up hides more of the collar; below 0 = off.", -0.01f, 0.2f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Arms Ease Out", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ArmsEaseOut), 0.005f,
              "Seconds the body's arms take to ease back to the locomotion pose once the gun is holstered.", 0.01f, 1.0f });
        m.Fields.back().Group = "Camera & Arms (advanced)";
        m.Fields.push_back({ "Turn Lag Floor", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, TurnLagFloor), 1.0f,
              "The body never lags the view by more than the larger of this (degrees) and Turn Threshold + Turn Lag Margin: a bounded slide of the feet\nbeats a chest twisted right round. Keep Mouse Sensitivity low enough that the turn clips keep up.", 0.0f, 180.0f });
        m.Fields.back().Group = "Turning (advanced)";
        m.Fields.push_back({ "Turn Lag Margin", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, TurnLagMargin), 0.5f,
              "Added to Turn Threshold when working out the most the body may lag the view (see Turn Lag Floor).", 0.0f, 90.0f });
        m.Fields.back().Group = "Turning (advanced)";
        m.Fields.push_back({ "Turn End Angle", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, TurnEndAngle), 0.5f,
              "A turn on the spot ends once the body is within this many degrees of the view.", 0.0f, 45.0f });
        m.Fields.back().Group = "Turning (advanced)";
        m.Fields.push_back({ "Turn Min Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, TurnMinTime), 0.01f,
              "A turn lasts at least this many seconds before it can end because its clip has finished.", 0.0f, 2.0f });
        m.Fields.back().Group = "Turning (advanced)";
        m.Fields.push_back({ "Turn Timeout", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, TurnTimeout), 0.1f,
              "A guard: a turn ends after this many seconds whatever the clip does.", 0.5f, 20.0f });
        m.Fields.back().Group = "Turning (advanced)";
        m.Fields.push_back({ "Turn Move Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, TurnMoveEase), 0.005f,
              "Moving, the body eases to face the view in this many seconds (a quick ease, no pop).", 0.005f, 1.0f });
        m.Fields.back().Group = "Turning (advanced)";
        m.Fields.push_back({ "Start Idle Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StartIdleTime), 0.01f,
              "A start clip plays only after the input has been idle this many seconds: a tap between keys is not a start.", 0.0f, 2.0f });
        m.Fields.back().Group = "Start & Stop (advanced)";
        m.Fields.push_back({ "Start Max Move", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StartMaxMove), 0.05f,
              "... and only while the body's current speed (m/s) is below this.", 0.0f, 5.0f });
        m.Fields.back().Group = "Start & Stop (advanced)";
        m.Fields.push_back({ "Stop Min Run Time", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StopMinRunTime), 0.01f,
              "A stop clip needs a run of at least this many seconds to stop from: a tap of the keys (or a step or two) just eases to a halt.", 0.0f, 3.0f });
        m.Fields.back().Group = "Start & Stop (advanced)";
        m.Fields.push_back({ "Stop Min Run Time (Crouched)", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StopMinRunTimeCrouched), 0.01f,
              "The same while crouched.", 0.0f, 3.0f });
        m.Fields.back().Group = "Start & Stop (advanced)";
        m.Fields.push_back({ "Stop Min Speed", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StopMinSpeed), 0.05f,
              "... and at least this speed (m/s) to shed.", 0.0f, 10.0f });
        m.Fields.back().Group = "Start & Stop (advanced)";
        m.Fields.push_back({ "Stop Min Speed (Crouched)", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StopMinSpeedCrouched), 0.05f,
              "The same while crouched.", 0.0f, 10.0f });
        m.Fields.back().Group = "Start & Stop (advanced)";
        m.Fields.push_back({ "Stop Debounce", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StopDebounce), 0.005f,
              "Seconds of no input before a stop is decided (tapping between two keys isn't one).", 0.0f, 0.5f });
        m.Fields.back().Group = "Start & Stop (advanced)";
        m.Fields.push_back({ "Stop Run Forward", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StopRunForward), 0.05f,
              "A stop after a sprint uses the Stop Run clip when the last direction of travel was at least this far forward (0 = sideways, 1 = straight ahead).", 0.0f, 1.0f });
        m.Fields.back().Group = "Start & Stop (advanced)";
        m.Fields.push_back({ "Airborne Delay", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, AirborneDelay), 0.01f,
              "Seconds off the ground before the controller's Airborne parameter is set (a step down a stair isn't a fall).", 0.0f, 1.0f });
        m.Fields.back().Group = "Locomotion (advanced)";
        m.Fields.push_back({ "Foot Lock Drift", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootLockDrift), 0.005f,
              "A planted foot is pinned where it landed; if the animation and the capsule's travel drift this far apart (metres) it plants again where the animation is.", 0.01f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Planted Height", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootPlantedHeight), 0.005f,
              "The animated foot counts as planted (pinned to the ground) while it is lower than this (metres).", 0.0f, 0.3f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Ray Up", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootRayUp), 0.05f,
              "The ground ray under each foot starts this far (metres) above the animated foot.", 0.0f, 2.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Ray Length", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootRayLength), 0.05f,
              "... and reaches this far (metres) down.", 0.1f, 3.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Max Raise", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootMaxRaise), 0.01f,
              "The most (metres) a foot is lifted to meet ground higher than the capsule's.", 0.0f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Pelvis Max Raise", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, PelvisMaxRaise), 0.01f,
              "The most (metres) the pelvis may rise when both feet are on higher ground. 0 = never: the legs reach for the step instead.", 0.0f, 0.5f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Offset Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootOffsetEase), 0.005f,
              "Seconds the ground height under each foot takes to follow the ray.", 0.005f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Normal Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootNormalEase), 0.005f,
              "Seconds the ground's slope under each foot takes to follow the ray.", 0.005f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot IK Fade", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootIKFade), 0.005f,
              "Seconds foot IK takes to fade in and out (it lets go in the air).", 0.005f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Tilt Max", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootTiltMax), 1.0f,
              "The most (degrees) a planted foot tilts to lie on a slope.", 0.0f, 60.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Lock Ease In", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootLockEaseIn), 0.005f,
              "Seconds a foot takes to be pinned once it is planted.", 0.005f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Foot Lock Ease Out", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, FootLockEaseOut), 0.005f,
              "Seconds a foot takes to be released once it lifts.", 0.005f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Stair Pop Rise", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StairPopRise), 0.005f,
              "A sudden change of the capsule's height at least this big (metres) counts as a stair: the body eases to the new height instead of popping.", 0.005f, 0.3f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Stair Pop Rate", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StairPopRate), 0.1f,
              "... and only when it is at least this fast (m/s), so a slope doesn't trigger it.", 0.5f, 20.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Stair Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, StairEase), 0.005f,
              "Seconds the body takes to ease up or down a stair (0.03 s when Foot IK is off).", 0.005f, 1.0f });
        m.Fields.back().Group = "Foot IK (advanced)";
        m.Fields.push_back({ "Elbow Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ElbowEase), 0.005f,
              "Seconds the player body's elbow takes to follow its target direction (smooths elbow jitter).", 0.005f, 1.0f });
        m.Fields.back().Group = "Arms";
        m.Fields.push_back({ "Elbow Max Rate", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, ElbowMaxRate), 10.0f,
              "The fastest (degrees per second) a player-body elbow may swing toward its target.", 30.0f, 3600.0f });
        m.Fields.back().Group = "Arms";
        m.Fields.push_back({ "NPC Turn Threshold", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcTurnThreshold), 1.0f,
              "NPCs: a still body this many degrees off its aim turns on the spot. NPCs copy this from the scene's player body when Play starts.", 0.0f, 180.0f });
        m.Fields.back().Group = "NPC Body";
        m.Fields.push_back({ "NPC Move Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcMoveEase), 0.005f,
              "NPCs: seconds the locomotion blend tree's speed and direction take to follow the movement.", 0.005f, 1.0f });
        m.Fields.back().Group = "NPC Body";
        m.Fields.push_back({ "NPC Face Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFaceEase), 0.005f,
              "NPCs: seconds the body heading takes to ease toward the movement direction while walking.", 0.005f, 1.0f });
        m.Fields.back().Group = "NPC Body";
        m.Fields.push_back({ "NPC Max Twist", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcMaxTwist), 1.0f,
              "NPCs: the most (degrees) the spine twists to aim past the legs' heading.", 0.0f, 120.0f });
        m.Fields.back().Group = "NPC Body";
        m.Fields.push_back({ "NPC Aim Lean", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcAimLean), 0.5f,
              "NPCs: degrees the torso leans forward when aiming standing.", 0.0f, 45.0f });
        m.Fields.back().Group = "NPC Body";
        m.Fields.push_back({ "NPC Aim Lean Crouched", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcAimLeanCrouched), 0.5f,
              "NPCs: degrees the torso leans forward when aiming crouched.", 0.0f, 60.0f });
        m.Fields.back().Group = "NPC Body";
        m.Fields.push_back({ "NPC Ready Lean Crouched", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcReadyLeanCrouched), 0.5f,
              "NPCs: degrees the torso leans forward at low ready, crouched.", 0.0f, 60.0f });
        m.Fields.back().Group = "NPC Body";
        m.Fields.push_back({ "NPC Cower Hunch", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcCowerHunch), 0.5f,
              "NPCs: degrees the spine curls forward when ducking for cover.", 0.0f, 60.0f });
        m.Fields.back().Group = "NPC Body";
        m.Fields.push_back({ "NPC Head Max Yaw", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcHeadMaxYaw), 1.0f,
              "NPCs: the most (degrees) the head turns left or right past the chest to look at a target.", 0.0f, 120.0f });
        m.Fields.back().Group = "NPC Head";
        m.Fields.push_back({ "NPC Head Max Pitch", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcHeadMaxPitch), 1.0f,
              "NPCs: the most (degrees) the head nods up or down to look at a target.", 0.0f, 90.0f });
        m.Fields.back().Group = "NPC Head";
        m.Fields.push_back({ "NPC Foot IK Max Drop", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootIKMaxDrop), 0.01f,
              "NPCs: the most (metres) the pelvis drops to let the lower foot reach the ground.", 0.0f, 1.0f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot IK Max Raise", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootIKMaxRaise), 0.01f,
              "NPCs: the most (metres) a foot is lifted to meet higher ground.", 0.0f, 1.0f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot IK Pelvis Raise", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootIKPelvisRaise), 0.005f,
              "NPCs: the most (metres) the pelvis may rise when both feet are on higher ground.", 0.0f, 0.5f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot IK Tilt Max", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootIKTiltMax), 1.0f,
              "NPCs: the most (degrees) a planted foot tilts to lie on a slope.", 0.0f, 60.0f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot Offset Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootOffsetEase), 0.005f,
              "NPCs: seconds the ground height under each foot takes to follow the ray.", 0.005f, 1.0f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot Normal Ease", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootNormalEase), 0.005f,
              "NPCs: seconds the ground's slope under each foot takes to follow the ray.", 0.005f, 1.0f });
        m.Fields.back().Group = "NPC Foot IK";
        m.Fields.push_back({ "NPC Foot IK Fade", T::Float, TARTARUS_REFLECT_FIELD(FirstPersonBodyComponent, NpcFootIKFade), 0.005f,
              "NPCs: seconds foot IK takes to fade in and out.", 0.005f, 1.0f });
        m.Fields.back().Group = "NPC Foot IK";
        // Groups, by name (not by position: reordering fields must not move them).
        {
            static const std::pair<const char*, const char*> kGroups[] = {
                {"Head Bone", "Camera"}, {"Camera Offset", "Camera"}, {"Armed Eye Offset", "Camera"}, {"Head Bob", "Camera"}, {"Camera Smoothing", "Camera"},
                {"Weapon Arms", "Arms"}, {"Spine Aim", "Arms"}, {"Spine Aim Down", "Arms"}, {"Shoulder Line Match", "Arms"}, {"Arms Piece", "Arms"},
                {"Turn Threshold", "Turning"}, {"Spine Twist", "Turning"}, {"Max Turn Rate", "Turning"},
                {"Crouch Height", "Crouch"}, {"Crouch Speed", "Crouch"},
                {"Foot IK", "Foot IK"}, {"Foot IK Max Drop", "Foot IK"}, {"Bone Map", "Camera"}, {"Start Stop Clips", "Locomotion"},
            };
            for (auto& f : m.Fields)
                for (const auto& [name, group] : kGroups)
                    if (std::strcmp(f.Name, name) == 0) f.Group = group;
        }
        Register<FirstPersonBodyComponent>(std::move(m));
    }

    // Character outfits (docs/CHARACTER_OUTFITS.md): the Inspector's outfit editor draws the choices.
    {
        ReflectComponent m;
        m.Name = "Character Outfit"; m.Icon = ICON_FA_PERSON_DRESS; m.Category = "Gameplay";
        m.Tooltip = "Dresses a modular character from a wardrobe: body, gender, race, hair and clothes.\n"
                    "Put it on the body's root. The outfit is its children with an Outfit Piece; the\n"
                    "editor below builds and swaps them. Works with First Person Body on the same object.";
        m.Fields = {
            { "Wardrobe", T::String, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, Wardrobe), 0.0f,
              "The pack's .wardrobe file: its bodies, races, slots and pairing rules." },
            { "Gender", T::Enum, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, Gender), 0.0f, "The body's gender." },
            { "Race", T::String, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, Race), 0.0f, "The head and skin." },
            { "Locks", T::String, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, Locks), 0.0f,
              "Slots Randomize leaves alone, comma separated." },
            { "Auto Hide Skin", T::Bool, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, AutoHide), 0.0f,
              "Skin covered by clothing (and a shirt under a jacket) isn't drawn, so it can't poke\n"
              "through as the body moves. Worked out from the models once per outfit change." },
            { "Randomize On Play", T::Bool, TARTARUS_REFLECT_FIELD(CharacterOutfitComponent, RandomizeOnPlay), 0.0f,
              "Every Play starts in a new random outfit (Locks kept). Stop puts the edited one back." },
        };
        m.Fields[0].AssetPath = true;
        m.Fields[1].EnumLabels = "Male\0Female\0"; m.Fields[1].EnumCount = 2;
        for (int i = 1; i < 4; ++i) m.Fields[i].EditorHidden = true;
        Register<CharacterOutfitComponent>(std::move(m));
    }
    {
        ReflectComponent m;
        m.Name = "Outfit Piece"; m.Icon = ICON_FA_SHIRT; m.Category = "Gameplay";
        m.Tooltip = "One piece of the parent's Character Outfit (a body part or an item). Managed by the\n"
                    "parent's outfit editor; remove it to take the object out of the outfit.";
        m.Fields = {
            { "Slot", T::String, TARTARUS_REFLECT_FIELD(OutfitPieceComponent, Slot), 0.0f, "Where it's worn." },
            { "Item", T::String, TARTARUS_REFLECT_FIELD(OutfitPieceComponent, Item), 0.0f, "The model it was built from." },
            { "Flags", T::Int, TARTARUS_REFLECT_FIELD(OutfitPieceComponent, Flags), 0.0f, "Body part / head attached." },
        };
        m.Fields[1].AssetPath = true;
        m.Fields[2].EditorHidden = true;
        Register<OutfitPieceComponent>(std::move(m));
    }

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
            { "Doppler Level", T::Float, TARTARUS_REFLECT_FIELD(AudioSourceComponent, DopplerLevel), 0.01f,
              "How much the pitch rises as this source and the listener approach each other and\n"
              "falls as they separate. 0 = off, 1 = physically correct (3D sounds only).", 0.0f, 5.0f },
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
    Register<LODGroupComponent>({
        "LOD Group", ICON_FA_LAYER_GROUP,
        "Swaps between simpler versions of an object as it gets smaller on screen.\n"
        "Its children are the levels in order: the first child is LOD 0 (full detail), the second\n"
        "LOD 1, and so on (up to 4). Below the last level's threshold the object isn't drawn.",
        "Rendering",
        {
            { "LOD 0", T::Float, TARTARUS_REFLECT_FIELD(LODGroupComponent, Lod0), 0.005f,
              "The first child is drawn while the object is at least this tall on screen\n"
              "(fraction of the view height, e.g. 0.6 = 60%).", 0.0f, 1.0f },
            { "LOD 1", T::Float, TARTARUS_REFLECT_FIELD(LODGroupComponent, Lod1), 0.005f,
              "The second child is drawn from here down to the LOD 2 threshold.", 0.0f, 1.0f },
            { "LOD 2", T::Float, TARTARUS_REFLECT_FIELD(LODGroupComponent, Lod2), 0.005f,
              "The third child is drawn from here down to the LOD 3 threshold.", 0.0f, 1.0f },
            { "LOD 3", T::Float, TARTARUS_REFLECT_FIELD(LODGroupComponent, Lod3), 0.001f,
              "The fourth child is drawn from here down; smaller than this, nothing is drawn.", 0.0f, 1.0f },
            { "Size", T::Float, TARTARUS_REFLECT_FIELD(LODGroupComponent, Size), 0.05f,
              "World-space size used to work out the height on screen, in metres.\n"
              "0 = automatic (the size of the first child's meshes).", 0.0f, 10000.0f },
        },
    });
    // #162 / #203 - Unity's post-processing Volume.
    {
        ReflectComponent m{
            "Post-process Volume", ICON_FA_WAND_MAGIC_SPARKLES,
            "Changes the look (exposure, colour, vignette, bloom, lens effects) while the camera is\n"
            "inside this box, fading in over Blend Distance - or everywhere, when Global. Tick an\n"
            "override to take that setting over from Lighting > Post-processing.",
            "Rendering",
            {
                { "Global", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Global), 0.0f,
                  "Applies everywhere, not just inside the box." },
                { "Size", T::Vec3, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Size), 0.1f,
                  "Box size in this object's local units (scaled by its Scale)." },
                { "Blend Distance", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, BlendDistance), 0.05f,
                  "Metres outside the box over which the effect fades in. 0 = hard edge.", 0.0f, 1000.0f },
                { "Weight", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Weight), 0.01f,
                  "How strongly the overrides apply at full effect.", 0.0f, 1.0f },
                { "Priority", T::Int, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Priority), 0.1f,
                  "Where volumes overlap, higher priority is applied last and wins." },
                { "Override Exposure", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideExposure), 0.0f, "" },
                { "Exposure", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, ExposureEV), 0.05f,
                  "Exposure compensation in stops.", -10.0f, 10.0f },
                { "Override Temperature", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideTemperature), 0.0f, "" },
                { "Temperature", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Temperature), 0.5f,
                  "White balance: negative = cooler / bluer, positive = warmer.", -100.0f, 100.0f },
                { "Override Tint", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideTint), 0.0f, "" },
                { "Tint", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Tint), 0.5f,
                  "White balance: negative = green, positive = magenta.", -100.0f, 100.0f },
                { "Override Contrast", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideContrast), 0.0f, "" },
                { "Contrast", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Contrast), 0.5f, "", -100.0f, 100.0f },
                { "Override Saturation", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideSaturation), 0.0f, "" },
                { "Saturation", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Saturation), 0.5f,
                  "-100 = greyscale.", -100.0f, 100.0f },
                { "Override Vignette", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideVignette), 0.0f, "" },
                { "Vignette", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, Vignette), 0.01f, "", 0.0f, 1.0f },
                { "Override Bloom", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideBloom), 0.0f,
                  "Needs bloom enabled in Lighting > Post-processing." },
                { "Bloom Intensity", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, BloomIntensity), 0.005f, "", 0.0f, 2.0f },
                { "Override Chromatic Aberration", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideChromatic), 0.0f, "" },
                { "Chromatic Aberration", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, ChromaticAberration), 0.01f, "", 0.0f, 1.0f },
                { "Override Film Grain", T::Bool, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, OverrideGrain), 0.0f, "" },
                { "Film Grain", T::Float, TARTARUS_REFLECT_FIELD(PostProcessVolumeComponent, FilmGrain), 0.01f, "", 0.0f, 1.0f },
            },
        };
        for (auto& f : m.Fields)
            if (f.Type == T::Float && std::strcmp(f.Name, "Blend Distance") != 0) { f.Slider = true; f.Format = "%.2f"; }
        Register<PostProcessVolumeComponent>(std::move(m));
    }
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

    // Lane P: scene-level visual effects and HUD settings
    Register<FxHudSettingsComponent>({
        "FX & HUD Settings", ICON_FA_SLIDERS,
        "Scene-level visual effects and HUD display parameters (add one per scene to tune muzzle flash,\n"
        "laser beam and HUD display).",
        "Gameplay",
        {
            { "Flash Time", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, FlashTime), 0.001f,
              "Seconds the muzzle flash light stays on.", 0.001f, 1.0f },
            { "Player Flash Scale", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, PlayerFlashScale), 0.01f,
              "Player's flash light scale relative to soldier's.", 0.01f, 2.0f },
            { "Flame Glow", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, FlameGlow), 5.0f,
              "Flame peak emission intensity (red channel).", 0.0f, 1000.0f },
            { "Flame Scale", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, FlameScale), 0.05f,
              "Flame tongue length/width scale vs. tactical shooter pack.", 0.1f, 5.0f },
            { "Beam Range", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, BeamRange), 1.0f,
              "Laser beam metres drawn before fading in the haze.", 1.0f, 500.0f },
            { "Beam Half Width", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, BeamHalfWidth), 0.0001f,
              "Laser beam width in metres.", 0.0001f, 0.1f },
            { "Beam Falloff", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, BeamFalloff), 0.1f,
              "Laser beam glow falloff distance in metres near the emitter.", 0.1f, 50.0f },
            { "Beam Bend", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, BeamBend), 0.1f,
              "Laser beam easing distance over which view-model emitter eases onto the true path (metres).", 0.1f, 20.0f },
            { "Feed Life", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, FeedLife), 0.1f,
              "Seconds a kill feed line stays on screen.", 0.1f, 60.0f },
            { "Streak Window", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, StreakWindow), 0.1f,
              "Seconds to count consecutive kills for streak display.", 0.1f, 60.0f },
            { "Sub Linger", T::Float, TARTARUS_REFLECT_FIELD(FxHudSettingsComponent, SubLinger), 0.01f,
              "Seconds a subtitle lingers after its clip ends.", 0.01f, 10.0f },
        },
    });
    // ---- lane R ----
    {
        ReflectComponent m;
        m.Name = "Ragdoll Settings"; m.Icon = ICON_FA_PERSON_FALLING; m.Category = "AI";
        m.Tooltip = "How dead soldiers' ragdolls are built and behave (the first Ragdoll Settings in the scene counts).\nJoint limits are degrees from the neutral pose.";
        m.Fields = {
            { "Anatomical Limits", T::Bool, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, AnatomicalLimits), 0.0f,
              "Joints use human ranges of motion (knees and elbows are one-way hinges). Off: the old generous symmetric cones, which let knees and elbows bend backwards." },
            { "Drive Stiffness", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, DriveStiffness), 5.0f,
              "Strength of the joint drives that hold the death pose (acceleration units, so independent of mass). 0 = limp at once.", 0.0f, 5000.0f },
            { "Drive Damping", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, DriveDamping), 1.0f,
              "Damping of those drives.", 0.0f, 500.0f },
            { "Drive Fade", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, DriveFade), 0.01f,
              "Seconds from death until the drives are off and the body is limp (squared fade).", 0.01f, 5.0f },
            { "Linear Damping", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, LinearDamping), 0.005f,
              "Air drag on every part.", 0.0f, 5.0f },
            { "Angular Damping", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, AngularDamping), 0.005f,
              "Rotational drag on every part.", 0.0f, 10.0f },
            { "Solver Position Iterations", T::Int, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SolverPosIters), 1.0f,
              "Joint accuracy: more is stiffer and costs more per corpse.", 1.0f, 64.0f },
            { "Solver Velocity Iterations", T::Int, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SolverVelIters), 1.0f,
              "Velocity-solver iterations per part.", 1.0f, 64.0f },
            { "Depenetration Speed", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, Depenetration), 0.1f,
              "Max speed (m/s) at which overlapping parts are pushed apart.", 0.0f, 50.0f },
            { "Sleep Threshold", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SleepThreshold), 0.005f,
              "Kinetic energy per mass below which a part sleeps; a body is at rest when all parts sleep.", 0.0f, 2.0f },
            { "Static Friction", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, StaticFriction), 0.01f,
              "Part against world.", 0.0f, 4.0f },
            { "Dynamic Friction", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, DynamicFriction), 0.01f,
              "Part against world, sliding.", 0.0f, 4.0f },
            { "Restitution", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, Restitution), 0.01f,
              "Bounciness of a part.", 0.0f, 1.0f },
            { "Part Impulse Speed", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, PartImpulseSpeed), 0.1f,
              "A shot's shove on the struck part is capped at mass x this (m/s), so a light forearm is not torn off.", 0.1f, 50.0f },
            { "Chest Impulse Speed", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ChestImpulseSpeed), 0.1f,
              "The remainder of the shove goes into the chest, capped at chest mass x this (m/s).", 0.0f, 50.0f },
            { "Corpse Shot Base", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CorpseShotBase), 0.05f,
              "A shot into a corpse shoves the part with mass x (this + Per Damage x damage) N s.", 0.0f, 20.0f },
            { "Corpse Shot Per Damage", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CorpseShotPerDamage), 0.005f,
              "See Corpse Shot Base.", 0.0f, 1.0f },
            { "Pelvis Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, PelvisMass), 0.1f,
              "kg. The whole body is about 75 kg.", 0.1f, 200.0f },
            { "Spine Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Spine Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Spine Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Spine Lateral In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineLatIn), 0.5f,
              "Degrees toward the body's midline (adduction); a spine or head leans this far either way.", 0.0f, 175.0f },
            { "Spine Lateral Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineLatOut), 0.5f,
              "Degrees away from the midline (abduction).", 0.0f, 175.0f },
            { "Spine Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Spine Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Head Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Head Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Head Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Head Lateral In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadLatIn), 0.5f,
              "Degrees toward the body's midline (adduction); a spine or head leans this far either way.", 0.0f, 175.0f },
            { "Head Lateral Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadLatOut), 0.5f,
              "Degrees away from the midline (abduction).", 0.0f, 175.0f },
            { "Head Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Head Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Upper Arm Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Upper Arm Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Upper Arm Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Upper Arm Lateral In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmLatIn), 0.5f,
              "Degrees toward the body's midline (adduction); a spine or head leans this far either way.", 0.0f, 175.0f },
            { "Upper Arm Lateral Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmLatOut), 0.5f,
              "Degrees away from the midline (abduction).", 0.0f, 175.0f },
            { "Upper Arm Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Upper Arm Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Forearm Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Forearm Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Forearm Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Forearm Sideways In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmLatIn), 0.5f,
              "Degrees of sideways wobble the hinge allows (slack so the joint doesn't bind).", 0.0f, 175.0f },
            { "Forearm Sideways Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmLatOut), 0.5f,
              "Degrees of sideways wobble the hinge allows (slack so the joint doesn't bind).", 0.0f, 175.0f },
            { "Forearm Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Forearm Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Thigh Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Thigh Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Thigh Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Thigh Lateral In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighLatIn), 0.5f,
              "Degrees toward the body's midline (adduction); a spine or head leans this far either way.", 0.0f, 175.0f },
            { "Thigh Lateral Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighLatOut), 0.5f,
              "Degrees away from the midline (abduction).", 0.0f, 175.0f },
            { "Thigh Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Thigh Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Calf Mass", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfMass), 0.1f,
              "kg, each side. Heavier parts drag the body harder.", 0.1f, 200.0f },
            { "Calf Flexion Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfFlexMax), 0.5f,
              "Degrees the part can swing in its flexion direction (forward for spine, head, shoulder and hip; the way the joint folds for elbow and knee), from its neutral pose.", 0.0f, 175.0f },
            { "Calf Extension Max", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfExtMax), 0.5f,
              "Degrees it can swing the other way (back). 0 on an elbow or knee: it cannot hyperextend.", 0.0f, 175.0f },
            { "Calf Sideways In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfLatIn), 0.5f,
              "Degrees of sideways wobble the hinge allows (slack so the joint doesn't bind).", 0.0f, 175.0f },
            { "Calf Sideways Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfLatOut), 0.5f,
              "Degrees of sideways wobble the hinge allows (slack so the joint doesn't bind).", 0.0f, 175.0f },
            { "Calf Twist In", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfTwistIn), 0.5f,
              "Degrees of inward rotation along the bone (toes or thumb toward the midline).", 0.0f, 175.0f },
            { "Calf Twist Out", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfTwistOut), 0.5f,
              "Degrees of outward rotation along the bone.", 0.0f, 175.0f },
            { "Limb Velocity Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, LimbVelocityScale), 0.05f,
              "How much of each bone's own motion (from the last two animated poses) the parts keep at death: a soldier shot mid-stride keeps his swinging limbs. 0 = only the body's velocity.", 0.0f, 2.0f },
            { "Max Limb Speed", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, MaxLimbSpeed), 0.1f,
              "Cap on a part's speed relative to the body from that (m/s); stops a teleport or a bad first frame throwing a limb.", 0.0f, 30.0f },
            { "Max Limb Spin", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, MaxLimbSpin), 0.5f,
              "Cap on a part's spin from that (rad/s).", 0.0f, 100.0f },
            { "Shaped Torso Inertia", T::Bool, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ShapedTorsoInertia), 0.0f,
              "Pelvis and chest turn like a box wider than deep (a human trunk) instead of a round capsule. Off: the capsule's own inertia." },
            { "Torso Half Width", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, TorsoHalfWidth), 0.005f,
              "Half the trunk's width (shoulder to shoulder), for its inertia only; hitboxes are unchanged.", 0.05f, 0.4f },
            { "Torso Half Depth", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, TorsoHalfDepth), 0.005f,
              "Half the trunk's depth (chest to back), for its inertia only.", 0.05f, 0.3f },
            { "Inertia Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, InertiaScale), 0.05f,
              "Multiplies every part's rotational inertia. Above 1 the parts turn more slowly (stabler), below 1 they whip about.", 0.2f, 5.0f },
            { "Pelvis Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, PelvisFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Spine Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, SpineFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Head Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, HeadFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Upper Arm Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, UpperArmFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Forearm Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ForearmFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Thigh Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, ThighFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
            { "Calf Fade Scale", T::Float, TARTARUS_REFLECT_FIELD(RagdollSettingsComponent, CalfFadeScale), 0.05f,
              "Multiplies Drive Fade for this region's joint drive: below 1 it goes limp sooner (legs give out first), above 1 it holds longer (spine and neck). 1 = with the rest.", 0.1f, 4.0f },
        };
        m.Fields[0].Group = "Joints";
        m.Fields[1].Group = "Death Drive";
        m.Fields[2].Group = "Death Drive";
        m.Fields[3].Group = "Death Drive";
        m.Fields[4].Group = "Body Physics";
        m.Fields[5].Group = "Body Physics";
        m.Fields[6].Group = "Body Physics";
        m.Fields[7].Group = "Body Physics";
        m.Fields[8].Group = "Body Physics";
        m.Fields[9].Group = "Body Physics";
        m.Fields[10].Group = "Contact";
        m.Fields[11].Group = "Contact";
        m.Fields[12].Group = "Contact";
        m.Fields[13].Group = "Impulse Caps";
        m.Fields[14].Group = "Impulse Caps";
        m.Fields[15].Group = "Impulse Caps";
        m.Fields[16].Group = "Impulse Caps";
        m.Fields[17].Group = "Pelvis";
        m.Fields[18].Group = "Spine";
        m.Fields[19].Group = "Spine";
        m.Fields[20].Group = "Spine";
        m.Fields[21].Group = "Spine";
        m.Fields[22].Group = "Spine";
        m.Fields[23].Group = "Spine";
        m.Fields[24].Group = "Spine";
        m.Fields[25].Group = "Head";
        m.Fields[26].Group = "Head";
        m.Fields[27].Group = "Head";
        m.Fields[28].Group = "Head";
        m.Fields[29].Group = "Head";
        m.Fields[30].Group = "Head";
        m.Fields[31].Group = "Head";
        m.Fields[32].Group = "Upper Arm";
        m.Fields[33].Group = "Upper Arm";
        m.Fields[34].Group = "Upper Arm";
        m.Fields[35].Group = "Upper Arm";
        m.Fields[36].Group = "Upper Arm";
        m.Fields[37].Group = "Upper Arm";
        m.Fields[38].Group = "Upper Arm";
        m.Fields[39].Group = "Forearm";
        m.Fields[40].Group = "Forearm";
        m.Fields[41].Group = "Forearm";
        m.Fields[42].Group = "Forearm";
        m.Fields[43].Group = "Forearm";
        m.Fields[44].Group = "Forearm";
        m.Fields[45].Group = "Forearm";
        m.Fields[46].Group = "Thigh";
        m.Fields[47].Group = "Thigh";
        m.Fields[48].Group = "Thigh";
        m.Fields[49].Group = "Thigh";
        m.Fields[50].Group = "Thigh";
        m.Fields[51].Group = "Thigh";
        m.Fields[52].Group = "Thigh";
        m.Fields[53].Group = "Calf";
        m.Fields[54].Group = "Calf";
        m.Fields[55].Group = "Calf";
        m.Fields[56].Group = "Calf";
        m.Fields[57].Group = "Calf";
        m.Fields[58].Group = "Calf";
        m.Fields[59].Group = "Calf";
        m.Fields[60].Group = "Death Momentum";
        m.Fields[61].Group = "Death Momentum";
        m.Fields[62].Group = "Death Momentum";
        m.Fields[63].Group = "Inertia";
        m.Fields[64].Group = "Inertia";
        m.Fields[65].Group = "Inertia";
        m.Fields[66].Group = "Inertia";
        m.Fields[67].Group = "Death Drive";
        m.Fields[68].Group = "Death Drive";
        m.Fields[69].Group = "Death Drive";
        m.Fields[70].Group = "Death Drive";
        m.Fields[71].Group = "Death Drive";
        m.Fields[72].Group = "Death Drive";
        m.Fields[73].Group = "Death Drive";
        Register<RagdollSettingsComponent>(std::move(m));
    }
    for (RegisteredComponent& rc : Storage())
        if (std::strcmp(rc.Meta.Name, "Squad Settings") == 0)
            for (ReflectField& f : rc.Meta.Fields) {
                if (std::strcmp(f.Name, "Heavy Hit Damage") == 0) f.Group = "Damage / Stagger";
                if (std::strcmp(f.Name, "Stagger Time") == 0) f.Group = "Damage / Stagger";
                if (std::strcmp(f.Name, "Bleed-Out Time") == 0) f.Group = "Wounded";
                if (std::strcmp(f.Name, "Crawl Speed") == 0) f.Group = "Wounded";
                if (std::strcmp(f.Name, "Limp Speed Scale") == 0) f.Group = "Wounded";
                if (std::strcmp(f.Name, "Limp Time") == 0) f.Group = "Wounded";
                if (std::strcmp(f.Name, "Corpse Time") == 0) f.Group = "Corpses";
                if (std::strcmp(f.Name, "Fall Gravity") == 0) f.Group = "Corpses";
                if (std::strcmp(f.Name, "Melee Damage") == 0) f.Group = "Melee";
                if (std::strcmp(f.Name, "Melee Time") == 0) f.Group = "Melee";
                if (std::strcmp(f.Name, "Melee Hit Time") == 0) f.Group = "Melee";
                if (std::strcmp(f.Name, "Hitbox Range") == 0) f.Group = "Distances / LOD";
                if (std::strcmp(f.Name, "Foot IK Range") == 0) f.Group = "Distances / LOD";
                if (std::strcmp(f.Name, "Mesh Check Range") == 0) f.Group = "Distances / LOD";
                if (std::strcmp(f.Name, "Cover Sample Spacing") == 0) f.Group = "Cover";
                if (std::strcmp(f.Name, "Cover Reach") == 0) f.Group = "Cover";
                if (std::strcmp(f.Name, "Low Cover Height") == 0) f.Group = "Cover";
                if (std::strcmp(f.Name, "High Cover Height") == 0) f.Group = "Cover";
                if (std::strcmp(f.Name, "Cover Peek Step") == 0) f.Group = "Cover";
            }
    // ---- end lane R ----

    // #132 - String fields that hold asset paths: tracked by GUID so renaming or moving the file
    // outside the editor keeps the reference (see ReflectField::AssetPath).
    const std::pair<const char*, const char*> kAssetPathFields[] = {
        {"Animation", "Clip"},
        {"Animator Controller", "Controller"},
        {"Transform Controller", "Script Path"},
        {"First Person Controller", "Animation Set"},
        {"First Person Controller", "Secondary Animation Set"},
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
