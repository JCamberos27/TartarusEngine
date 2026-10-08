#include "PlayerDefinition.h"
#include "ScriptComponent.h"
#include "ScriptRuntime.h"
#include "World.h"
#include "Components.h"
#include "AssetDatabase.h"
#include <json.hpp>
#include <stdexcept>

namespace {
using Json=nlohmann::json;
std::string Asset(const Json& value) {return AssetDatabase::FollowRef(value.value("path",std::string{}),value.value("pathGuid",std::string{}));}
void Read(FirstPersonControllerComponent& view,const Json& fields) {
    view.PrimaryWeaponPrefab=Asset(fields.at("PrimaryWeaponPrefab"));
    view.SecondaryWeaponPrefab=Asset(fields.at("SecondaryWeaponPrefab"));
    view.MoveSpeed=fields.at("MoveSpeed").get<float>();
    view.SprintMultiplier=fields.at("SprintMultiplier").get<float>();
    view.JumpSpeed=fields.at("JumpSpeed").get<float>();
    view.JumpBufferTime=fields.at("JumpBufferTime").get<float>();
    view.CoyoteTime=fields.at("CoyoteTime").get<float>();
    view.GroundAccelTime=fields.at("GroundAccelTime").get<float>();
    view.GroundDecelTime=fields.at("GroundDecelTime").get<float>();
    view.AirAccelTime=fields.at("AirAccelTime").get<float>();
    view.EyeHeight=fields.at("EyeHeight").get<float>();
    view.CapsuleRadius=fields.at("CapsuleRadius").get<float>();
    view.CapsuleHeight=fields.at("CapsuleHeight").get<float>();
    view.MouseSensitivity=fields.at("MouseSensitivity").get<float>();
    view.InvertY=fields.at("InvertY").get<bool>();
    view.FieldOfView=fields.at("FieldOfView").get<float>();
    view.StickLookDegPerSec=fields.at("StickLookDegPerSec").get<float>();
    view.EyeRadius=fields.at("EyeRadius").get<float>();
    view.KillY=fields.at("KillY").get<float>();
    view.GravityGun=fields.at("GravityGun").get<bool>();
    view.Gravity=fields.at("Gravity").get<float>();
    view.MinThrowSpeed=fields.at("MinThrowSpeed").get<float>();
    view.MaxThrowSpeed=fields.at("MaxThrowSpeed").get<float>();
    view.ThrowChargeTime=fields.at("ThrowChargeTime").get<float>();
    view.ThrowBackspin=fields.at("ThrowBackspin").get<float>();
    view.GrabRange=fields.at("GrabRange").get<float>();
    view.AssistRange=fields.at("AssistRange").get<float>();
    view.AssistConeDeg=fields.at("AssistConeDeg").get<float>();
    view.ScrollTurnDeg=fields.at("ScrollTurnDeg").get<float>();
    view.AnimationSet=Asset(fields.at("AnimationSet"));
    view.SecondaryAnimationSet=Asset(fields.at("SecondaryAnimationSet"));
    view.CameraBone=fields.at("CameraBone").get<std::string>();
    view.ViewModelOffset={fields.at("ViewModelOffset").at("X").get<float>(),fields.at("ViewModelOffset").at("Y").get<float>(),fields.at("ViewModelOffset").at("Z").get<float>()};
    view.ViewModelRotation={fields.at("ViewModelRotation").at("X").get<float>(),fields.at("ViewModelRotation").at("Y").get<float>(),fields.at("ViewModelRotation").at("Z").get<float>()};
    view.ViewModelScale=fields.at("ViewModelScale").get<float>();
    view.ViewModelFov=fields.at("ViewModelFov").get<float>();
    view.MaxHealth=fields.at("MaxHealth").get<float>();
    view.RegenDelay=fields.at("RegenDelay").get<float>();
    view.RegenRate=fields.at("RegenRate").get<float>();
    view.RespawnDelay=fields.at("RespawnDelay").get<float>();
    view.SpawnProtection=fields.at("SpawnProtection").get<float>();
}
}
FirstPersonControllerComponent::FirstPersonControllerComponent() {
    std::string resolved;if(!Scripting::ResolveScriptFields("Tartarus.Gameplay.PlayerDefinition","{}",resolved))throw std::runtime_error("Project player defaults unavailable");
    Read(*this,Json::parse(resolved));
}
namespace Scripting {
bool HasPlayerDefinition(const CSharpScriptComponent& scripts) {
    for(const auto& slot:GetSlots(scripts))if(slot.Class=="Tartarus.Gameplay.PlayerDefinition")return true;return false;
}
void SyncPlayerDefinition(World& world,entt::entity entity) {
    const auto* scripts=world.Registry.try_get<CSharpScriptComponent>(entity);
    if(!world.Registry.all_of<FirstPersonControllerComponent>(entity) && (!scripts || (scripts->ClassName!="Tartarus.Gameplay.PlayerDefinition" && scripts->Scripts.find("Tartarus.Gameplay.PlayerDefinition")==std::string::npos)))return;
    if(scripts)for(const auto& slot:GetSlots(*scripts)) {
        if(slot.Class!="Tartarus.Gameplay.PlayerDefinition" || !slot.Enabled)continue;
        auto& view=world.Registry.get_or_emplace<FirstPersonControllerComponent>(entity);
        if(view.ResolvedScriptFields==slot.Fields && view.ResolvedCodeGeneration==CodeGeneration() && view.ResolvedAssetRevision==AssetDatabase::Revision())return;
        std::string resolved;if(!ResolveScriptFields(slot.Class,slot.Fields,resolved))throw std::runtime_error("Cannot resolve project player definition");
        Read(view,Json::parse(resolved));view.ResolvedScriptFields=slot.Fields;view.ResolvedCodeGeneration=CodeGeneration();view.ResolvedAssetRevision=AssetDatabase::Revision();return;
    }
    if(auto* view=world.Registry.try_get<FirstPersonControllerComponent>(entity);view && !view->ResolvedScriptFields.empty())world.Registry.remove<FirstPersonControllerComponent>(entity);
}
void SyncPlayerDefinitions(World& world) {
    for(auto entity:world.Registry.view<CSharpScriptComponent>())SyncPlayerDefinition(world,entity);
    // Removing the complete script component must also remove its cached runtime view.
    for(auto entity:world.Registry.view<FirstPersonControllerComponent>(entt::exclude<CSharpScriptComponent>))SyncPlayerDefinition(world,entity);
}
}
