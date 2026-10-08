#include "NpcDefinitions.h"
#include "ScriptComponent.h"
#include "ScriptRuntime.h"
#include "Components.h"
#include "World.h"
#include "AssetDatabase.h"
#include <json.hpp>
#include <stdexcept>
namespace Scripting {
namespace {
using Json=nlohmann::json;
void Read(NpcSpawnComponent& view,const Json& fields) {
    view.Weapon=fields.at("Weapon").get<int>();
    view.Squad=fields.at("Squad").get<int>();
    view.Skill=fields.at("Skill").get<float>();
    view.OutfitSeed=fields.at("OutfitSeed").get<int>();
    view.Brain=fields.at("Brain").get<int>();
}
void Read(SquadSettingsComponent& view,const Json& fields) {
    view.BodyPrefab=AssetDatabase::FollowRef(fields.at("BodyPrefab").value("path",std::string{}),fields.at("BodyPrefab").value("pathGuid",std::string{}));
    view.RiflePrefab=AssetDatabase::FollowRef(fields.at("RiflePrefab").value("path",std::string{}),fields.at("RiflePrefab").value("pathGuid",std::string{}));
    view.ShotgunPrefab=AssetDatabase::FollowRef(fields.at("ShotgunPrefab").value("path",std::string{}),fields.at("ShotgunPrefab").value("pathGuid",std::string{}));
    view.RifleAnimationSet=AssetDatabase::FollowRef(fields.at("RifleAnimationSet").value("path",std::string{}),fields.at("RifleAnimationSet").value("pathGuid",std::string{}));
    view.ShotgunAnimationSet=AssetDatabase::FollowRef(fields.at("ShotgunAnimationSet").value("path",std::string{}),fields.at("ShotgunAnimationSet").value("pathGuid",std::string{}));

    view.SquadSize=fields.at("SquadSize").get<int>();
    view.RespawnDelay=fields.at("RespawnDelay").get<float>();
    view.Difficulty=fields.at("Difficulty").get<float>();
    view.NpcDamageScale=fields.at("NpcDamageScale").get<float>();
    view.Respawn=fields.at("Respawn").get<bool>();
    view.HeavyHitDamage=fields.at("HeavyHitDamage").get<float>();
    view.StaggerTime=fields.at("StaggerTime").get<float>();
    view.BleedOutTime=fields.at("BleedOutTime").get<float>();
    view.CrawlSpeed=fields.at("CrawlSpeed").get<float>();
    view.LimpSpeedScale=fields.at("LimpSpeedScale").get<float>();
    view.LimpTime=fields.at("LimpTime").get<float>();
    view.CorpseTime=fields.at("CorpseTime").get<float>();
    view.FallGravity=fields.at("FallGravity").get<float>();
    view.MeleeDamage=fields.at("MeleeDamage").get<float>();
    view.MeleeTime=fields.at("MeleeTime").get<float>();
    view.MeleeHitTime=fields.at("MeleeHitTime").get<float>();
    view.HitboxRange=fields.at("HitboxRange").get<float>();
    view.FootIKRange=fields.at("FootIKRange").get<float>();
    view.MeshCheckRange=fields.at("MeshCheckRange").get<float>();
    view.CoverSpacing=fields.at("CoverSpacing").get<float>();
    view.CoverReach=fields.at("CoverReach").get<float>();
    view.CoverKneeHeight=fields.at("CoverKneeHeight").get<float>();
    view.CoverHeadHeight=fields.at("CoverHeadHeight").get<float>();
    view.CoverStep=fields.at("CoverStep").get<float>();
}
void Read(WeaponAudioComponent& view,const Json& fields) {
    view.Gun=fields.at("Gun").get<std::string>();
    view.Enabled=fields.at("Enabled").get<bool>();
    view.Volume=fields.at("Volume").get<float>();
    view.PlayerGain=fields.at("PlayerGain").get<float>();
    view.ShotPitchMin=fields.at("ShotPitchMin").get<float>();
    view.ShotPitchMax=fields.at("ShotPitchMax").get<float>();
    view.VolumeJitterDb=fields.at("VolumeJitterDb").get<float>();
    view.CloseFullDistance=fields.at("CloseFullDistance").get<float>();
    view.CloseZeroDistance=fields.at("CloseZeroDistance").get<float>();
    view.FarMinWeight=fields.at("FarMinWeight").get<float>();
    view.FarMaxWeight=fields.at("FarMaxWeight").get<float>();
    view.MaxDistance=fields.at("MaxDistance").get<float>();
    view.FarMaxDistance=fields.at("FarMaxDistance").get<float>();
    view.ShotMinDistance=fields.at("ShotMinDistance").get<float>();
    view.BassMinDistance=fields.at("BassMinDistance").get<float>();
    view.EventMinDistance=fields.at("EventMinDistance").get<float>();
    view.EventMaxDistance=fields.at("EventMaxDistance").get<float>();
    view.ShotMaxVoices=fields.at("ShotMaxVoices").get<int>();
    view.TailMaxVoices=fields.at("TailMaxVoices").get<int>();
    view.TailFadeTime=fields.at("TailFadeTime").get<float>();
    view.TailMinInterval=fields.at("TailMinInterval").get<float>();
    view.TailDuckPerVoice=fields.at("TailDuckPerVoice").get<float>();
    view.TailEvery=fields.at("TailEvery").get<int>();
    view.FarEvery=fields.at("FarEvery").get<int>();
    view.BurstGap=fields.at("BurstGap").get<float>();
    view.DataFile=AssetDatabase::FollowRef(fields.at("DataFile").value("path",std::string{}),fields.at("DataFile").value("pathGuid",std::string{}));
    view.EnvEnabled=fields.at("EnvEnabled").get<bool>();
    view.EnvRayCount=fields.at("EnvRayCount").get<int>();
    view.EnvMaxDistance=fields.at("EnvMaxDistance").get<float>();
    view.EnvIndoorCover=fields.at("EnvIndoorCover").get<float>();
    view.EnvUrbanWall=fields.at("EnvUrbanWall").get<float>();
    view.EnvUrbanDistance=fields.at("EnvUrbanDistance").get<float>();
    view.EnvLargeRoomDistance=fields.at("EnvLargeRoomDistance").get<float>();
    view.EnvBlendFraction=fields.at("EnvBlendFraction").get<float>();
    view.EnvBlendDistance=fields.at("EnvBlendDistance").get<float>();
    view.EnvRefreshInterval=fields.at("EnvRefreshInterval").get<float>();
    view.EnvRefreshMoveDistance=fields.at("EnvRefreshMoveDistance").get<float>();
    view.EnvMatchRadius=fields.at("EnvMatchRadius").get<float>();
    view.EnvTailGainOutdoorOpen=fields.at("EnvTailGainOutdoorOpen").get<float>();
    view.EnvTailGainOutdoorUrban=fields.at("EnvTailGainOutdoorUrban").get<float>();
    view.EnvTailGainIndoorSmall=fields.at("EnvTailGainIndoorSmall").get<float>();
    view.EnvTailGainIndoorLarge=fields.at("EnvTailGainIndoorLarge").get<float>();
    view.EnvDebugDraw=fields.at("EnvDebugDraw").get<bool>();
}
void Read(FoleyAudioComponent& view,const Json& fields) {
    view.Enabled=fields.at("Enabled").get<bool>();
    view.Volume=fields.at("Volume").get<float>();
    view.WalkVolume=fields.at("WalkVolume").get<float>();
    view.RunVolume=fields.at("RunVolume").get<float>();
    view.CrouchVolume=fields.at("CrouchVolume").get<float>();
    view.VolumeJitterDb=fields.at("VolumeJitterDb").get<float>();
    view.PitchMin=fields.at("PitchMin").get<float>();
    view.PitchMax=fields.at("PitchMax").get<float>();
    view.StepStrideScale=fields.at("StepStrideScale").get<float>();
    view.CrouchStrideScale=fields.at("CrouchStrideScale").get<float>();
    view.MinStepSpeed=fields.at("MinStepSpeed").get<float>();
    view.RunSpeed=fields.at("RunSpeed").get<float>();
    view.StepsFromFeet=fields.at("StepsFromFeet").get<bool>();
    view.FootLiftHeight=fields.at("FootLiftHeight").get<float>();
    view.FootLiftMoving=fields.at("FootLiftMoving").get<float>();
    view.FootContactHeight=fields.at("FootContactHeight").get<float>();
    view.JumpVolume=fields.at("JumpVolume").get<float>();
    view.LandVolume=fields.at("LandVolume").get<float>();
    view.LandMinSpeed=fields.at("LandMinSpeed").get<float>();
    view.LandFullSpeed=fields.at("LandFullSpeed").get<float>();
    view.LandHeavySpeed=fields.at("LandHeavySpeed").get<float>();
    view.NpcStepVolume=fields.at("NpcStepVolume").get<float>();
    view.NpcStepMaxDistance=fields.at("NpcStepMaxDistance").get<float>();
    view.NpcStepMinDistance=fields.at("NpcStepMinDistance").get<float>();
    view.DefaultSurface=fields.at("DefaultSurface").get<std::string>();
    view.SurfaceTable=fields.at("SurfaceTable").get<std::string>();
}
void Read(FxHudSettingsComponent& view,const Json& fields) {
    view.FlashTime=fields.at("FlashTime").get<float>();
    view.PlayerFlashScale=fields.at("PlayerFlashScale").get<float>();
    view.FlameGlow=fields.at("FlameGlow").get<float>();
    view.FlameScale=fields.at("FlameScale").get<float>();
    view.MuzzleStyle=fields.at("MuzzleStyle").get<int>();
    view.BeamRange=fields.at("BeamRange").get<float>();
    view.BeamHalfWidth=fields.at("BeamHalfWidth").get<float>();
    view.BeamFalloff=fields.at("BeamFalloff").get<float>();
    view.BeamBend=fields.at("BeamBend").get<float>();
    view.FeedLife=fields.at("FeedLife").get<float>();
    view.StreakWindow=fields.at("StreakWindow").get<float>();
}
void Read(ImpactAudioComponent& view,const Json& fields) {
    view.Enabled=fields.at("Enabled").get<bool>();
    view.SurfaceTable=fields.at("SurfaceTable").get<std::string>();
    view.DefaultSurface=fields.at("DefaultSurface").get<std::string>();
    view.CasingsEnabled=fields.at("CasingsEnabled").get<bool>();
    view.CasingMaxContacts=fields.at("CasingMaxContacts").get<int>();
    view.CasingMinSpeed=fields.at("CasingMinSpeed").get<float>();
    view.CasingFullSpeed=fields.at("CasingFullSpeed").get<float>();
    view.CasingGainMin=fields.at("CasingGainMin").get<float>();
    view.CasingVolume=fields.at("CasingVolume").get<float>();
    view.CasingMinDistance=fields.at("CasingMinDistance").get<float>();
    view.CasingMaxDistance=fields.at("CasingMaxDistance").get<float>();
    view.CasingMaxVoices=fields.at("CasingMaxVoices").get<int>();
    view.ShellRadius=fields.at("ShellRadius").get<float>();
    view.ImpactsEnabled=fields.at("ImpactsEnabled").get<bool>();
    view.ImpactVolume=fields.at("ImpactVolume").get<float>();
    view.ImpactMinDistance=fields.at("ImpactMinDistance").get<float>();
    view.ImpactMaxDistance=fields.at("ImpactMaxDistance").get<float>();
    view.ImpactMaxVoices=fields.at("ImpactMaxVoices").get<int>();
    view.ImpactMinInterval=fields.at("ImpactMinInterval").get<float>();
    view.FleshUsesRecordings=fields.at("FleshUsesRecordings").get<bool>();
    view.FleshVolume=fields.at("FleshVolume").get<float>();
    view.FlybyEnabled=fields.at("FlybyEnabled").get<bool>();
    view.FlybyRadius=fields.at("FlybyRadius").get<float>();
    view.FlybyVolume=fields.at("FlybyVolume").get<float>();
    view.FlybyMinInterval=fields.at("FlybyMinInterval").get<float>();
    view.FlybyMinDistance=fields.at("FlybyMinDistance").get<float>();
    view.FlybyMaxDistance=fields.at("FlybyMaxDistance").get<float>();
    view.FlybyFarGain=fields.at("FlybyFarGain").get<float>();
}
template<typename View> void Sync(World& world,entt::entity entity,const CSharpScriptComponent* scripts,const char* className) {
    if(!world.Registry.all_of<View>(entity) && (!scripts || (scripts->ClassName!=className && scripts->Scripts.find(className)==std::string::npos)))return;
    if(scripts)for(const auto& slot:GetSlots(*scripts)) {
        if(slot.Class!=className || !slot.Enabled)continue;
        auto& view=world.Registry.get_or_emplace<View>(entity);
        if(view.ResolvedScriptFields==slot.Fields && view.ResolvedCodeGeneration==CodeGeneration() && view.ResolvedAssetRevision==AssetDatabase::Revision())return;
        std::string resolved;if(!ResolveScriptFields(className,slot.Fields,resolved))throw std::runtime_error("Project NPC configuration unavailable");
        std::string normalized=resolved;if(std::string(className)=="Tartarus.Gameplay.NpcDefinition" || std::string(className)=="Tartarus.Gameplay.SquadDefinition")if(!RequestProject("npc.config",Json{{"class",className},{"fields",Json::parse(resolved)}}.dump(),normalized))throw std::runtime_error("Project NPC configuration invalid");
        Read(view,Json::parse(normalized));view.ResolvedScriptFields=slot.Fields;view.ResolvedCodeGeneration=CodeGeneration();view.ResolvedAssetRevision=AssetDatabase::Revision();return;
    }
    if(auto* view=world.Registry.try_get<View>(entity);view && !view->ResolvedScriptFields.empty())world.Registry.remove<View>(entity);
}
}
void SyncNpcDefinition(World& world,entt::entity entity) {
    const auto* scripts=world.Registry.try_get<CSharpScriptComponent>(entity);
    Sync<ImpactAudioComponent>(world,entity,scripts,"Tartarus.Gameplay.ImpactAudioDefinition");
    Sync<WeaponAudioComponent>(world,entity,scripts,"Tartarus.Gameplay.WeaponAudioDefinition");
    Sync<FoleyAudioComponent>(world,entity,scripts,"Tartarus.Gameplay.FoleyDefinition");
    Sync<FxHudSettingsComponent>(world,entity,scripts,"Tartarus.Gameplay.EffectsDefinition");
    Sync<NpcSpawnComponent>(world,entity,scripts,"Tartarus.Gameplay.NpcDefinition");
    Sync<SquadSettingsComponent>(world,entity,scripts,"Tartarus.Gameplay.SquadDefinition");
}
void SyncNpcDefinitions(World& world) {
    for(auto e:world.Registry.view<CSharpScriptComponent>())SyncNpcDefinition(world,e);
    for(auto e:world.Registry.view<ImpactAudioComponent>(entt::exclude<CSharpScriptComponent>))SyncNpcDefinition(world,e);
    for(auto e:world.Registry.view<WeaponAudioComponent>(entt::exclude<CSharpScriptComponent>))SyncNpcDefinition(world,e);
    for(auto e:world.Registry.view<FoleyAudioComponent>(entt::exclude<CSharpScriptComponent>))SyncNpcDefinition(world,e);
    for(auto e:world.Registry.view<FxHudSettingsComponent>(entt::exclude<CSharpScriptComponent>))SyncNpcDefinition(world,e);
    for(auto e:world.Registry.view<NpcSpawnComponent>(entt::exclude<CSharpScriptComponent>))SyncNpcDefinition(world,e);
    for(auto e:world.Registry.view<SquadSettingsComponent>(entt::exclude<CSharpScriptComponent>))SyncNpcDefinition(world,e);
}
SquadSettingsComponent DefaultSquadDefinition() {
    std::string resolved;if(!ResolveScriptFields("Tartarus.Gameplay.SquadDefinition","{}",resolved))throw std::runtime_error("Project squad defaults unavailable");
    SquadSettingsComponent view;Read(view,Json::parse(resolved));return view;
}
ImpactAudioComponent DefaultImpactAudioDefinition(){ImpactAudioComponent view;std::string resolved;if(ResolveScriptFields("Tartarus.Gameplay.ImpactAudioDefinition","{}",resolved))Read(view,Json::parse(resolved));return view;}
WeaponAudioComponent DefaultWeaponAudioDefinition() {
    WeaponAudioComponent view;std::string resolved;if(ResolveScriptFields("Tartarus.Gameplay.WeaponAudioDefinition","{}",resolved))Read(view,Json::parse(resolved));return view;
}
FoleyAudioComponent DefaultFoleyDefinition() {
    FoleyAudioComponent view;std::string resolved;if(ResolveScriptFields("Tartarus.Gameplay.FoleyDefinition","{}",resolved))Read(view,Json::parse(resolved));return view;
}
FxHudSettingsComponent DefaultEffectsDefinition() {
    FxHudSettingsComponent view;std::string resolved;if(ResolveScriptFields("Tartarus.Gameplay.EffectsDefinition","{}",resolved))Read(view,Json::parse(resolved));return view;
}
}
