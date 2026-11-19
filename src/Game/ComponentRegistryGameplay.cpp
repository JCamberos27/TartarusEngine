#include "ComponentRegistryInternal.h"

#include "Components.h"
#include "ReflectAttributes.h" // vInspector attributes (Attr)

#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <random>

namespace ComponentRegistry {

// Weapons and their attachments, health, NPC spawns and squads, transform controllers, light flicker / circuits / switches, openables.
void RegisterGameplayComponents() {
    using T = ReflectFieldType;

    Register<WeaponDefinitionComponent>({
        "Weapon Definition", ICON_FA_CROSSHAIRS, "What this weapon prefab is: its animation set (rigs, animation, recoil and camera profiles), its gameplay\n"
        "numbers and damage, and its muzzle light and smoke. Save prefab changes before starting Play.", "Gameplay",
        {
            {"Description", T::String, TARTARUS_REFLECT_FIELD(WeaponDefinitionComponent, Description)},
            {"Animation Set", T::String, TARTARUS_REFLECT_FIELD(WeaponDefinitionComponent, AnimationSet), 0.0f, "Animation, gameplay, recoil and camera profiles for this weapon (.fpsanim)."},
        }
    });
        { ReflectField f{"Muzzle Enabled",T::Bool,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.Enabled; },0.0f,"Enable this weapon's visual muzzle effects.",0.0f,0.0f}; f.Group="Overview"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flash Time",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlashTime; },0.001f,"Light duration in seconds.",0.001f,1.0f}; f.Group="Muzzle Light"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Light Intensity",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.LightIntensity; },0.1f,"Peak intensity before the player's scale.",0.0f,1000.0f}; f.Group="Muzzle Light"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Player Flash Scale",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.PlayerFlashScale; },0.01f,"Light multiplier for the player's own weapon.",0.0f,2.0f}; f.Group="Muzzle Light"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Light Range",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.LightRange; },0.1f,"Metres illuminated by the flash.",0.0f,100.0f}; f.Group="Muzzle Light"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Light Color",T::Color,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.LightColor; },0.0f,"Flash light tint.",0.0f,0.0f}; f.Group="Muzzle Light"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flame Texture",T::AssetRef,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlameTexture; },0.0f,"Packed muzzle-flame mask texture.",0.0f,0.0f}; f.Group="Flame"; f.AssetKind=ReflectAssetKind::Texture; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flame Glow",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlameGlow; },1.0f,"HDR flame emission.",0.0f,1000.0f}; f.Group="Flame"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flame Scale",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlameScale; },0.01f,"Flame length and width multiplier.",0.0f,10.0f}; f.Group="Flame"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flame Color",T::Color,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlameColor; },0.0f,"Flame emission tint.",0.0f,0.0f}; f.Group="Flame"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flame Lifetime Min",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlameLifetimeMin; },0.001f,"Seconds, sampled per shot.",0.0f,5.0f}; f.Group="Flame"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flame Lifetime Max",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlameLifetimeMax; },0.001f,"Seconds, sampled per shot.",0.0f,5.0f}; f.Group="Flame"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flame Length Min",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlameLengthMin; },0.001f,"Metres before Flame Scale.",0.0f,10.0f}; f.Group="Flame"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flame Length Max",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlameLengthMax; },0.001f,"Metres before Flame Scale.",0.0f,10.0f}; f.Group="Flame"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flame Width Min",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlameWidthMin; },0.001f,"Metres before Flame Scale.",0.0f,10.0f}; f.Group="Flame"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flame Width Max",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlameWidthMax; },0.001f,"Metres before Flame Scale.",0.0f,10.0f}; f.Group="Flame"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Muzzle Style",T::Int,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.MuzzleStyle; },1.0f,"0: flame only. 1: layered flash, glow and smoke.",0.0f,1.0f}; f.Group="Flash"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flash Sprite",T::String,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlashSprite; },0.0f,"Imported FX entry name, e.g. muzzle_star or muzzle_burst.",0.0f,0.0f}; f.Group="Flash"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flash Lifetime",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlashLifetime; },0.001f,"Flash sprite lifetime in seconds.",0.001f,5.0f}; f.Group="Flash"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flash Size Min",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlashSizeMin; },0.01f,"Minimum flash diameter in metres.",0.0f,10.0f}; f.Group="Flash"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flash Size Max",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlashSizeMax; },0.01f,"Maximum flash diameter in metres.",0.0f,10.0f}; f.Group="Flash"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Flash Intensity",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.FlashIntensity; },0.1f,"HDR flash sprite brightness.",0.0f,1000.0f}; f.Group="Flash"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Side Jets",T::Bool,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.SideJets; },0.0f,"Emit the barrel's side jets.",0.0f,0.0f}; f.Group="Flash"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Core Glow",T::Bool,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.CoreGlow; },0.0f,"Emit the muzzle core glow.",0.0f,0.0f}; f.Group="Flash"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Spark Count",T::Int,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.SparkCount; },1.0f,"World-view sparks per shot.",0.0f,128.0f}; f.Group="Flash"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Smoke",T::Bool,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.Smoke; },0.0f,"Emit smoke and gas puffs.",0.0f,0.0f}; f.Group="Smoke"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Afterfire Smoke",T::Bool,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.AfterfireSmoke; },0.0f,"Emit the lingering barrel wisp.",0.0f,0.0f}; f.Group="Smoke"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Smoke Scale",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.SmokeScale; },0.01f,"Smoke size multiplier.",0.0f,10.0f}; f.Group="Smoke"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Smoke Lifetime Min",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.SmokeLifetimeMin; },0.01f,"Minimum smoke lifetime in seconds.",0.01f,30.0f}; f.Group="Smoke"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Smoke Lifetime Max",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.SmokeLifetimeMax; },0.01f,"Maximum smoke lifetime in seconds.",0.01f,30.0f}; f.Group="Smoke"; Storage().back().Meta.Fields.push_back(f); }
        { ReflectField f{"Smoke Alpha",T::Float,[](void* p)->void* { return &static_cast<WeaponDefinitionComponent*>(p)->Muzzle.SmokeAlpha; },0.01f,"Gas puff opacity.",0.0f,1.0f}; f.Group="Smoke"; Storage().back().Meta.Fields.push_back(f); }

    {
        // Gameplay and Damage: FirstPersonWeaponGameplay, read by the first-person weapon and the soldiers' guns.
#define WEAPON_GAMEPLAY(Member) [](void* p) -> void* { return &static_cast<WeaponDefinitionComponent*>(p)->Gameplay.Member; }
        auto add = [](ReflectField f, const char* group) { f.Group = group; Storage().back().Meta.Fields.push_back(f); };
        add({"Magazine", T::Int, WEAPON_GAMEPLAY(Magazine), 1.0f, "Rounds in a full magazine.", 1.0f, 10000.0f}, "Gameplay");
        add({"Rounds Per Minute", T::Float, WEAPON_GAMEPLAY(RoundsPerMinute), 1.0f, "Full-auto cadence.", 1.0f, 100000.0f}, "Gameplay");
        add({"Allow Full Auto", T::Bool, WEAPON_GAMEPLAY(AllowFullAuto), 0.0f, "Off: a semi-only weapon (B does nothing)."}, "Gameplay");
        add({"Burst Rounds", T::Int, WEAPON_GAMEPLAY(BurstRounds), 1.0f, "Above 1: a semi trigger fires this many rounds; letting go interrupts it.", 0.0f, 32.0f}, "Gameplay");
        add({"Reload Hold Seconds", T::Float, WEAPON_GAMEPLAY(ReloadHoldSeconds), 0.01f, "R held this long checks the magazine instead of reloading.", 0.01f, 10.0f}, "Gameplay");
        add({"Regrip Min", T::Float, WEAPON_GAMEPLAY(RegripMin), 0.1f, "Seconds of settled Idle before a Fidget (shortest).", 0.0f, 600.0f}, "Gameplay");
        add({"Regrip Max", T::Float, WEAPON_GAMEPLAY(RegripMax), 0.1f, "Seconds of settled Idle before a Fidget (longest).", 0.0f, 600.0f}, "Gameplay");
        add({"Impact Impulse", T::Float, WEAPON_GAMEPLAY(ImpactImpulse), 0.1f, "N*s each round gives the dynamic body it hits. 0 = rounds push nothing.", 0.0f, 1000.0f}, "Gameplay");
        add({"Impact Max Speed", T::Float, WEAPON_GAMEPLAY(ImpactMaxSpeed), 0.1f, "Most velocity change (m/s) one round gives a body.", 0.0f, 100.0f}, "Gameplay");
        add({"Bullet Hole Radius", T::Float, WEAPON_GAMEPLAY(BulletHoleRadius), 0.0005f, "Radius of the holes rounds leave, metres.", 0.0f, 0.1f}, "Gameplay");
        add({"Zero Distance", T::Float, WEAPON_GAMEPLAY(ZeroDistance), 0.5f, "Metres out at which rounds cross the sight line. 0 = straight down the bore.", 0.0f, 10000.0f}, "Gameplay");
        add({"Has Sight Line", T::Bool, WEAPON_GAMEPLAY(HasSightLine), 0.0f, "Use the Sight Origin and Direction below. Off: measured in Play the first time the sights settle."}, "Gameplay");
        add({"Sight Origin", T::Vec3, WEAPON_GAMEPLAY(SightOrigin), 0.001f, "The eye's position with the sights up, weapon-root space."}, "Gameplay");
        add({"Sight Direction", T::Vec3, WEAPON_GAMEPLAY(SightDirection), 0.001f, "The look direction with the sights up, weapon-root space."}, "Gameplay");
        add({"Pellets", T::Int, WEAPON_GAMEPLAY(Pellets), 1.0f, "Rays per round (a shotgun).", 1.0f, 64.0f}, "Gameplay");
        add({"Spread Hip", T::Float, WEAPON_GAMEPLAY(SpreadHip), 0.1f, "Half angle (degrees) of the pellets' cone from the hip.", 0.0f, 45.0f}, "Gameplay");
        add({"Spread Ads", T::Float, WEAPON_GAMEPLAY(SpreadAds), 0.1f, "Half angle (degrees) of the pellets' cone with the sights up.", 0.0f, 45.0f}, "Gameplay");
        {
            ReflectField f{"Reload", T::Enum, WEAPON_GAMEPLAY(Reload), 0.0f, "Magazine: the Refill event fills it at once. Per Round: each LoadRound event adds one."};
            f.EnumLabels = "Magazine\0Per Round\0"; f.EnumCount = 2;
            add(f, "Gameplay");
        }
        add({"Cycle After Shot", T::Bool, WEAPON_GAMEPLAY(CycleAfterShot), 0.0f, "A manual action (pump, bolt, lever): the Cycle trigger is set after every round."}, "Gameplay");
        add({"Cycle Delay", T::Float, WEAPON_GAMEPLAY(CycleDelay), 0.01f, "Seconds after a round before the Cycle trigger is set.", 0.0f, 2.0f}, "Gameplay");
        add({"Damage", T::Float, WEAPON_GAMEPLAY(Damage), 0.5f, "Health a round (a pellet) takes from a character.", 0.0f, 10000.0f}, "Damage");
        add({"Head Multiplier", T::Float, WEAPON_GAMEPLAY(HeadMultiplier), 0.05f, "Damage multiplier on the head.", 0.0f, 100.0f}, "Damage");
        add({"Limb Multiplier", T::Float, WEAPON_GAMEPLAY(LimbMultiplier), 0.05f, "Damage multiplier on arms and legs.", 0.0f, 100.0f}, "Damage");
        add({"Falloff Start", T::Float, WEAPON_GAMEPLAY(FalloffStart), 0.5f, "Metres where damage starts falling off.", 0.0f, 10000.0f}, "Damage");
        add({"Falloff End", T::Float, WEAPON_GAMEPLAY(FalloffEnd), 0.5f, "Metres from which damage is at Falloff Min.", 0.0f, 10000.0f}, "Damage");
        add({"Falloff Min", T::Float, WEAPON_GAMEPLAY(FalloffMin), 0.01f, "Share of the damage left past Falloff End.", 0.0f, 1.0f}, "Damage");
#undef WEAPON_GAMEPLAY
    }
    for (ReflectField& f : Storage().back().Meta.Fields) {
        if (std::strcmp(f.Name, "Description") == 0 || std::strcmp(f.Name, "Animation Set") == 0) f.Group = "Overview";
        if (std::strcmp(f.Name, "Animation Set") == 0) f.AssetPath = true;
        // Older prefab effects; child Particle Systems author the flash on current weapons.
        if (f.Group && (std::strcmp(f.Group, "Flame") == 0 || std::strcmp(f.Group, "Flash") == 0)) f.EditorHidden = true;
    }
    Register<MuzzleAttachmentComponent>({
        "Muzzle Attachment", ICON_FA_CROSSHAIRS,
        "A weapon prefab child: one muzzle device (suppressor, brake...). One muzzle attachment is active at a time.\n"
        "References are hierarchy paths relative to this object.", "Gameplay",
        {
            {"Enabled", T::Bool, TARTARUS_REFLECT_FIELD(MuzzleAttachmentComponent, Enabled), 0.0f, "Off: the weapon ignores this attachment."},
            {"Muzzle Transform", T::String, TARTARUS_REFLECT_FIELD(MuzzleAttachmentComponent, MuzzleTransform), 0.0f, "Path to the muzzle transform below this object. Its local -Z axis is the bore direction."},
            {"Muzzle Flash", T::String, TARTARUS_REFLECT_FIELD(MuzzleAttachmentComponent, MuzzleFlash), 0.0f, "Path to the muzzle flash Particle System below this object. Its Texture supplies the flash asset."},
            {"Firing Sounds", T::String, TARTARUS_REFLECT_FIELD(MuzzleAttachmentComponent, FiringSounds), 0.0f, "Audio assets for the firing sound; variants alternate per shot. Empty uses the gun's layered audio."},
            {"Firing Sound Profile", T::String, TARTARUS_REFLECT_FIELD(MuzzleAttachmentComponent, FiringSoundProfile), 0.0f, "Layered firing sound profile (empty, ak, 870 or revolver38), used when Firing Sounds is empty."},
        }
    });
    Register<GripAttachmentComponent>({
        "Grip Attachment", ICON_FA_HAND, "A weapon prefab child: one grip. One grip attachment is active at a time.", "Gameplay",
        {
            {"Enabled", T::Bool, TARTARUS_REFLECT_FIELD(GripAttachmentComponent, Enabled), 0.0f, "Off: the weapon ignores this attachment."},
        }
    });
    Register<OpticAttachmentComponent>({
        "Optic Attachment", ICON_FA_EYE, "A weapon prefab child: one sight. One optic attachment is active at a time.\n"
        "References are hierarchy paths relative to this object.", "Gameplay",
        {
            {"Enabled", T::Bool, TARTARUS_REFLECT_FIELD(OpticAttachmentComponent, Enabled), 0.0f, "Off: the weapon ignores this attachment."},
            {"Aim Point", T::String, TARTARUS_REFLECT_FIELD(OpticAttachmentComponent, AimPoint), 0.0f, "Path to the aim point below this object. Its local -Z axis points along the sight line."},
            {"Default Optic", T::Bool, TARTARUS_REFLECT_FIELD(OpticAttachmentComponent, DefaultOptic), 0.0f, "Marks the iron-sight aim point as the reference for the weapon's existing ADS animation."},
        }
    });
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
        m.Name = "Dummy Spawn"; m.Icon = ICON_FA_PERSON; m.Category = "AI";
        m.LegacyKeys = {"NPC Spawn"};
        m.Tooltip = "A training dummy appears here in Play, facing this object's forward: unarmed, it wanders the\n"
                    "navigation mesh near here (Roam), flinches when shot, bleeds, and dies into a ragdoll.";
        m.Fields = {
            { "Roam", T::Bool, TARTARUS_REFLECT_FIELD(NpcSpawnComponent, Roam), 0.0f,
              "Wander the navigation mesh near the spawn. Off: stand where placed." },
        };
        Register<NpcSpawnComponent>(std::move(m));
    }
    {
        ReflectComponent m;
        m.Name = "Dummy Settings"; m.Icon = ICON_FA_USERS; m.Category = "AI";
        m.LegacyKeys = {"Squad Settings"};
        m.Tooltip = "The training dummies' rules for this scene (the first Dummy Settings counts).";
        m.Fields = {
            { "Respawn", T::Bool, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, Respawn), 0.0f,
              "A dummy that dies comes back at its spawn." },
            { "Respawn Delay", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, RespawnDelay), 0.1f,
              "Seconds before a dead dummy comes back.", 0.0f, 120.0f },
            { "Heavy Hit Damage", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, HeavyHitDamage), 0.5f,
              "A single hit of at least this much damage staggers the dummy.", 0.0f, 500.0f },
            { "Stagger Time", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, StaggerTime), 0.01f,
              "Seconds a heavy hit slows the dummy.", 0.0f, 5.0f },
            { "Corpse Time", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, CorpseTime), 0.5f,
              "Seconds a dead dummy's body stays before it is removed.", 0.0f, 600.0f },
            { "Max Corpses", T::Int, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, MaxCorpses), 1.0f,
              "Most bodies kept at once; past it the oldest is removed.", 1.0f, 64.0f },
            { "Unseen Time", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, UnseenTime), 0.1f,
              "Seconds a body must be out of the player's sight before it can be removed.", 0.0f, 600.0f },
            { "Fall Gravity", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, FallGravity), 0.1f,
              "Gravity (m/s^2) on a dummy that steps off a ledge. Ragdolls use the physics world's gravity.", 0.0f, 60.0f },
            { "Hitbox Range", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, HitboxRange), 1.0f,
              "Dummies further than this from the player (m) keep only their movement capsule, no per-bone hitboxes.", 5.0f, 500.0f },
            { "Foot IK Range", T::Float, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, FootIKRange), 0.5f,
              "Dummies this close (m) and in view put their feet on uneven ground.", 0.0f, 200.0f },
            { "Body Prefab", T::String, TARTARUS_REFLECT_FIELD(SquadSettingsComponent, BodyPrefab), 0.0f,
              "The body every dummy is built from (.json)." },
        };
        for (ReflectField& f : m.Fields) {
            if (std::strcmp(f.Name, "Body Prefab") == 0) { f.Group = "Content"; f.AssetPath = true; }
            else if (std::strstr(f.Name, "Hit") == f.Name || std::strcmp(f.Name, "Stagger Time") == 0) f.Group = std::strcmp(f.Name, "Hitbox Range") == 0 ? "Distances / LOD" : "Damage / Stagger";
            else if (std::strcmp(f.Name, "Foot IK Range") == 0) f.Group = "Distances / LOD";
            else if (std::strstr(f.Name, "Corpse") || std::strcmp(f.Name, "Unseen Time") == 0 || std::strcmp(f.Name, "Fall Gravity") == 0) f.Group = "Corpses";
        }
        Register<SquadSettingsComponent>(std::move(m));
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

    {
        ReflectComponent m;
        m.Name = "Light Flicker"; m.Icon = ICON_FA_LIGHTBULB; m.Category = "Rendering";
        m.Tooltip = "Flickers the Light on this object while playing: a fluorescent tube's stutters, a TV's shifting glow, or a candle's shimmer.";
        m.Fields = {
            { "Mode", T::Enum, TARTARUS_REFLECT_FIELD(LightFlickerComponent, Mode), 0.0f,
              "Fluorescent: steady with bursts of off/on stutters. Tv: shifting brightness and tint. Candle: fast small noise." },
            { "Min Interval", T::Float, TARTARUS_REFLECT_FIELD(LightFlickerComponent, MinInterval), 0.05f,
              "Shortest wait between events, seconds (Fluorescent: between stutter bursts; Tv: between picture changes).", 0.05f, 60.0f },
            { "Max Interval", T::Float, TARTARUS_REFLECT_FIELD(LightFlickerComponent, MaxInterval), 0.05f,
              "Longest wait between events, seconds.", 0.05f, 60.0f },
            { "Depth", T::Float, TARTARUS_REFLECT_FIELD(LightFlickerComponent, Depth), 0.01f,
              "How far the light dips: 1 = all the way off.", 0.0f, 1.0f },
            { "Seed", T::Int, TARTARUS_REFLECT_FIELD(LightFlickerComponent, Seed), 1.0f,
              "Random seed, so lights with the same settings don't flicker in step." },
        };
        m.Fields[0].EnumLabels = "Fluorescent\0Tv\0Candle\0"; m.Fields[0].EnumCount = 3;
        Register<LightFlickerComponent>(std::move(m));
    }

    {
        ReflectComponent m;
        m.Name = "Light Circuit"; m.Icon = ICON_FA_LIGHTBULB; m.Category = "Rendering";
        m.Tooltip = "Puts this object on a light circuit that a Light Switch works while playing: a Light goes dark with "
                    "it, a fitting or lamp loses its glow (its materials' emission).";
        m.Fields = {
            { "Circuit", T::String, TARTARUS_REFLECT_FIELD(LightCircuitComponent, Circuit), 0.0f,
              "The circuit's name. A Light Switch with the same name turns it on and off." },
            { "Glow", T::Float, TARTARUS_REFLECT_FIELD(LightCircuitComponent, Glow), 0.05f,
              "Models: their materials' emission when on, as a multiple of what the materials have (0 = leave the "
              "materials alone).", 0.0f, 20.0f },
            { "Glow When Off", T::Bool, TARTARUS_REFLECT_FIELD(LightCircuitComponent, GlowWhenOff), 0.0f,
              "Models: glow while the circuit is off and go dark when it's on - a lit switch's toggle, easy to find "
              "in a dark room." },
        };
        Register<LightCircuitComponent>(std::move(m));
    }

    {
        ReflectComponent m;
        m.Name = "Light Switch"; m.Icon = ICON_FA_LIGHTBULB; m.Category = "Gameplay";
        m.Tooltip = "Interact (E) on this object turns its light circuit on and off while playing: a wall switch, or a "
                    "lamp's own body. Every switch on the circuit flips together.";
        m.Fields = {
            { "Circuit", T::String, TARTARUS_REFLECT_FIELD(LightSwitchComponent, Circuit), 0.0f,
              "The circuit it works (Light Circuit components with the same name)." },
            { "Start On", T::Bool, TARTARUS_REFLECT_FIELD(LightSwitchComponent, StartOn), 0.0f,
              "The circuit is on when Play starts." },
            { "Rocker", T::String, TARTARUS_REFLECT_FIELD(LightSwitchComponent, Rocker), 0.0f,
              "A child's name that tips over when the switch flips (empty = none)." },
            { "Rocker Angle", T::Float, TARTARUS_REFLECT_FIELD(LightSwitchComponent, RockerAngle), 0.5f,
              "Degrees the rocker tips either way, about its local Z (across the plate).", 0.0f, 45.0f },
            { "On Sound", T::String, TARTARUS_REFLECT_FIELD(LightSwitchComponent, OnSound), 0.0f,
              "Audio assets played when it turns on; ';'-separated variants." },
            { "Off Sound", T::String, TARTARUS_REFLECT_FIELD(LightSwitchComponent, OffSound), 0.0f,
              "Audio assets played when it turns off; ';'-separated variants." },
        };
        Register<LightSwitchComponent>(std::move(m));
    }

    {
        ReflectComponent m;
        m.Name = "Openable"; m.Icon = ICON_FA_DOOR_OPEN; m.Category = "Gameplay";
        m.Tooltip = "A door, drawer or lid the player opens and shuts with Interact (E) while playing. The hinge or slide "
                    "comes from the model's bounds, front facing local +Z; put it on the part, as a child of its body.";
        m.Fields = {
            { "Motion", T::Enum, TARTARUS_REFLECT_FIELD(OpenableComponent, Motion), 0.0f,
              "Hinge Left / Right: a door hinged on that edge (seen from the front), swinging out. Lid: hinged along its "
              "back edge, lifting. Flap Down: hinged along its bottom edge, dropping forward (an oven door). Flap Up: "
              "hinged along its top edge. Slide Out: a drawer. Slide Left / Right: a sliding door." },
            { "Amount", T::Float, TARTARUS_REFLECT_FIELD(OpenableComponent, Amount), 1.0f,
              "How far it opens: degrees for a hinge, metres for a slide. 0 = the usual amount (100 degrees for a door, "
              "95 for a lid; a drawer most of its body's depth).", 0.0f, 360.0f },
            { "Open Time", T::Float, TARTARUS_REFLECT_FIELD(OpenableComponent, OpenTime), 0.05f,
              "Seconds from shut to open.", 0.05f, 10.0f },
            { "Start Open", T::Bool, TARTARUS_REFLECT_FIELD(OpenableComponent, StartOpen), 0.0f,
              "Open when Play starts." },
            { "Start Ajar", T::Float, TARTARUS_REFLECT_FIELD(OpenableComponent, StartAjar), 0.01f,
              "With Start Open: how far open it starts, as a fraction of Amount (0 = all the way). A door left ajar still "
              "swings its full Amount: one Interact opens it the rest of the way.", 0.0f, 1.0f },
            { "Swing Back", T::Bool, TARTARUS_REFLECT_FIELD(OpenableComponent, SwingBack), 0.0f,
              "A hinge or flap opens toward local -Z (the back face) instead of the front, its pin on the back face: an "
              "apartment door modelled with its chain and thumb-turn on the back, swinging into the room." },
            { "Open Sound", T::String, TARTARUS_REFLECT_FIELD(OpenableComponent, OpenSound), 0.0f,
              "Audio assets played as it starts to open; ';'-separated variants, one picked per play. Empty = silent." },
            { "Close Sound", T::String, TARTARUS_REFLECT_FIELD(OpenableComponent, CloseSound), 0.0f,
              "Audio assets played when it lands shut; ';'-separated variants." },
            { "Latch Sound", T::String, TARTARUS_REFLECT_FIELD(OpenableComponent, LatchSound), 0.0f,
              "Audio assets layered on the Close Sound when it lands shut (the latch snapping home); ';'-separated variants." },
        };
        m.Fields[0].EnumLabels = "Hinge Left\0Hinge Right\0Lid\0Flap Down\0Flap Up\0Slide Out\0Slide Left\0Slide Right\0";
        m.Fields[0].EnumCount = 8;
        Register<OpenableComponent>(std::move(m));
    }
}

} // namespace ComponentRegistry
