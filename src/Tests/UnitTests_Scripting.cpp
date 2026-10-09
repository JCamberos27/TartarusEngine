#include "UnitTestSupport.h"
#include "../Game/Scripting/ScriptRuntime.h"
#include "../Game/Scripting/PlayerDefinition.h"
#include "../Game/Scripting/NpcDefinitions.h"
#include "../Game/Scripting/RuntimeCanvas.h"
#include "../Renderer/HudText.h"
#include "../Game/Scripting/WeaponPrefab.h"
#include "FirstPersonAnimation.h"
#include "../Game/Scripting/ProjectComponentMigration.h"
#include "../Game/Scripting/GameFrames.h"
#include "../Game/Scripting/WeaponAttachments.h"
#include "MaterialAsset.h"
#include "ShaderLibrary.h"
#include "../Game/Scripting/ScriptComponent.h"
#include "ComponentRegistry.h"
#include "../Game/Scripting/ScriptReferences.h"
#include "EnginePaths.h"
#include "ProjectPaths.h"
#include <filesystem>
#include <json.hpp>
#include "World.h"
#include "AssetLibrary.h"
#include "SceneSerializer.h"
#include "ParticleSystem.h"
#include <fstream>
#include "Player.h"
#include "PhysicsWorld.h"
#include "GameModuleAPI.h"
#include <chrono>
#include <thread>
#include <set>
#include <cmath>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
void TestSceneReferences() {
    using namespace Scripting;
    World world;
    const auto root=world.CreateEmptyEntity({}, {}, {1,1,1}, "Weapon");
    const auto muzzle=world.CreateEmptyEntity({}, {}, {1,1,1}, "Muzzle Attachment");
    const auto flash=world.CreateEmptyEntity({}, {}, {1,1,1}, "Muzzle Flash");
    const auto optic=world.CreateEmptyEntity({}, {}, {1,1,1}, "Optic");
    world.AttachChildRaw(muzzle,root);world.AttachChildRaw(flash,muzzle);world.AttachChildRaw(optic,root);
    world.Registry.emplace<ParticleSystemComponent>(flash);
    CHECK(SceneReferencePath(world,muzzle,flash)==std::optional<std::string>("Muzzle Flash"));
    CHECK(ResolveSceneReference(world,muzzle,"Muzzle Flash")==flash);
    CHECK(ResolveSceneReference(world,muzzle,"")==entt::null);
    CHECK(AcceptsSceneReference(world,muzzle,flash,"Particle System",true));
    CHECK(!AcceptsSceneReference(world,muzzle,muzzle,"Particle System",true));
    CHECK(AcceptsSceneReference(world,muzzle,muzzle,"Transform",true));
    CHECK(SceneReferencePath(world,muzzle,muzzle)==std::optional<std::string>("."));
    CHECK(!AcceptsSceneReference(world,muzzle,optic,"Transform",true));
    CHECK(AcceptsSceneReference(world,muzzle,optic,"Transform",false));
    CHECK(SceneReferencePath(world,muzzle,optic)==std::optional<std::string>("../Optic"));
    const auto unrelated=world.CreateEmptyEntity({}, {}, {1,1,1}, "Other weapon");
    CHECK(!AcceptsSceneReference(world,muzzle,unrelated,"Transform",false));
    // Resolve the saved reference in another instance, with different entity IDs.
    const auto clone=world.CreateEmptyEntity({}, {}, {1,1,1}, "Muzzle Attachment");
    const auto cloneFlash=world.CreateEmptyEntity({}, {}, {1,1,1}, "Muzzle Flash");
    world.AttachChildRaw(cloneFlash,clone);
    CHECK(ResolveSceneReference(world,clone,*SceneReferencePath(world,muzzle,flash))==cloneFlash);
    const auto duplicate=world.CreateEmptyEntity({}, {}, {1,1,1}, "Muzzle Flash");
    world.AttachChildRaw(duplicate,muzzle);
    CHECK(!SceneReferencePath(world,muzzle,duplicate));
    CHECK(ResolveSceneReference(world,muzzle,"Missing")==entt::null);
}
void TestManagedGameplay() {
    using namespace Scripting;
    CHECK(EnsureLoaded());
    WeaponFrame w; w.Equipped=1; w.Chambered=1; w.Ammo=30; w.Magazine=30; w.Rpm=700;
    w.AllowFullAuto=1; w.RecoilProfile=1; w.Tags=32;
    w.Operation=1; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Result==1 && (w.Commands&2));
    w.Operation=2; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Ammo==29);
    w.Operation=5; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Result==1 && (w.Commands&64));
    w.Operation=8; w.Events=1; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Ammo==30);
    w.Operation=6; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.FullAuto==1);
    w.Operation=3; w.Held=1; w.Cooldown=.05f; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Result==0);
    w.Operation=7; w.Dt=.1f; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Cooldown==0);
    w.Operation=3; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Result==1);
    // The shotgun commits a shell, delays cycling, and chambers only after the pump state finishes.
    w.FullAuto=0; w.AllowFullAuto=0; w.PerRound=1; w.CycleAfterShot=1;
    w.CycleDelay=.12f; w.Magazine=6; w.Ammo=6; w.Operation=2;
    CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Ammo==5 && w.Chambered==0);
    w.Operation=1; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Result==0);
    w.Operation=7; w.Dt=.2f; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Commands&16);
    w.Tags=8; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.CycleSeen==1 && w.Chambered==0);
    w.Tags=32; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Chambered==1);
    w.Tags=2; w.Operation=1; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Result==0 && w.StopReload==1);
    w.Ammo=0; w.Chambered=0; w.Events=2; w.Operation=8;
    CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Ammo==1 && w.Chambered==1);
    w.Operation=9; w.Held=1; w.Dt=.1f; w.HoldSeconds=.35f; CHECK(InvokeProject("weapon",&w,sizeof w));
    w.Held=0; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Result==1);
    w.Held=1; w.Dt=.4f; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Commands==0);
    CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Commands&128);
    w.Held=0; CHECK(InvokeProject("weapon",&w,sizeof w)); CHECK(w.Result==0);
    CHECK(!InvokeProject("weapon",&w,sizeof w-1)); // a mismatched ABI is rejected before reading memory
    World world; Player player; player.Gravity=0; player.Cam.Yaw=-90; player.Cam.Position={0,2,0};
    player.ScriptedMove=true; player.ScriptMove={0,1}; player.MoveSpeed=6;
    player.Update(.1f,world,nullptr,false);
    CHECK(std::abs(player.Velocity.z+6)<.001f);
    CHECK(std::abs(player.Cam.Position.z+.6f)<.001f);
}
void TestManagedComponentsAndPrefabs() {
    World world; AssetLibrary assets;
    auto e=world.CreateEmptyEntity({0,0,0},{0,0,0},{1,1,1},"Script");
    auto& script=world.Registry.emplace<CSharpScriptComponent>(e);
    script.ClassName="Tartarus.Gameplay.Bob";
    script.Fields="{\"Height\":0.5,\"Speed\":2}";
    Scripting::Attach(script,"assets/Scripts/WeaponDefinition.cs","Tartarus.Gameplay.WeaponDefinition");
    auto slots=Scripting::GetSlots(script);
    slots.back().Fields=nlohmann::json{{"Description","870 pump shotgun"},{"AnimationSet",{{"path","assets/Weapons/Remington870/Remington870.fpsanim"},{"pathGuid",""}}}}.dump();
    Scripting::SetSlots(script,slots);
    Scripting::Attach(script,"assets/Scripts/PlayerDefinition.cs","Tartarus.Gameplay.PlayerDefinition");
    slots=Scripting::GetSlots(script);slots.back().Fields=R"({"PrimaryWeaponPrefab":{"path":"assets/Weapons/AKS74U/AKS74U.prefab","pathGuid":""}})";
    Scripting::SetSlots(script,slots);Scripting::SyncPlayerDefinitions(world);
    auto& smoke=world.Registry.emplace<ParticleSystemComponent>(e);
    smoke.Emitting=false;smoke.ShaderMode=1;smoke.SmokeDensity=2.3f;smoke.SmokeNoiseScale=4.2f;
    smoke.SmokeTurbulence=.47f;smoke.SmokeSoftness=.72f;smoke.SmokeEvolution=.83f;
    const auto snapshot=SceneSerializer::SaveToString(world);
    CHECK(SceneSerializer::LoadFromString(world,assets,snapshot));
    auto view=world.Registry.view<CSharpScriptComponent,FirstPersonControllerComponent>();
    CHECK(view.begin()!=view.end());
    if(view.begin()==view.end())return;
    auto restored=*view.begin();
    CHECK(world.Registry.get<CSharpScriptComponent>(restored).Fields=="{\"Height\":0.5,\"Speed\":2}");
    const auto restoredSlots=Scripting::GetSlots(world.Registry.get<CSharpScriptComponent>(restored));
    CHECK(restoredSlots.size()==3 && restoredSlots[1].Class=="Tartarus.Gameplay.WeaponDefinition" && restoredSlots[2].Class=="Tartarus.Gameplay.PlayerDefinition");
    CHECK(nlohmann::json::parse(restoredSlots[1].Fields).at("Description")=="870 pump shotgun");
    CHECK(world.Registry.get<FirstPersonControllerComponent>(restored).PrimaryWeaponPrefab=="assets/Weapons/AKS74U/AKS74U.prefab");
    const auto& restoredSmoke=world.Registry.get<ParticleSystemComponent>(restored);
    CHECK(restoredSmoke.ShaderMode==1 && restoredSmoke.Texture.empty());
    CHECK(restoredSmoke.SmokeDensity==2.3f && restoredSmoke.SmokeNoiseScale==4.2f && restoredSmoke.SmokeTurbulence==.47f && restoredSmoke.SmokeSoftness==.72f && restoredSmoke.SmokeEvolution==.83f);
    Scripting::Tick(world,assets,.5f);
    CHECK(world.Registry.get<TransformComponent>(restored).Position.y>.4f);
    CHECK(Scripting::Invoke(5,nullptr,0));
    Scripting::Tick(world,assets,.5f);
    CHECK(std::abs(world.Registry.get<TransformComponent>(restored).Position.y-.5f*std::sin(2.0f))<.001f);
    char invalid[]="missing-gameplay-assembly.dll";
    CHECK(!Scripting::Invoke(0,invalid,Scripting::kVersion));
    Scripting::Tick(world,assets,.5f);
    CHECK(std::abs(world.Registry.get<TransformComponent>(restored).Position.y-.5f*std::sin(3.0f))<.001f);
    Scripting::Stop(&world,&assets);
    std::string set,error;
    for(const char* name:{"AKS74U","Remington870"}) {
        const std::string stem=std::string("assets/Weapons/")+name+"/"+name;
        if(!std::filesystem::exists(ProjectPaths::Resolve(stem+".prefab"))) continue; // licensed weapon assets aren't in the distributable sample
        MuzzleEffectSettings muzzle;
        CHECK(Scripting::ResolveWeaponPrefab(stem+".prefab",set,error,&muzzle)); CHECK(set==stem+".fpsanim");
        CHECK(muzzle.PrefabParticles);
        std::vector<entt::entity> emitters;
        const auto particleRoot = Scripting::InstantiateWeaponParticles(world,assets,stem+".prefab",emitters,error,false);
        CHECK(world.Registry.valid(particleRoot) && !emitters.empty());
        Scripting::WeaponAttachments attachments; attachments.Load(world,particleRoot);
        CHECK(attachments.Count(Scripting::AttachmentKind::Muzzle)==1);
        CHECK(attachments.Count(Scripting::AttachmentKind::Grip)==1);
        glm::mat4 attachmentPose(1);
        CHECK(attachments.Pose(world,particleRoot,Scripting::AttachmentKind::Muzzle,attachmentPose));
        if(std::string(name)=="AKS74U") {
            // Iron sights only (the red dot came off): the selected optic is the reference one.
            CHECK(attachments.Count(Scripting::AttachmentKind::Optic)==1);
            CHECK(attachments.Pose(world,particleRoot,Scripting::AttachmentKind::Optic,attachmentPose));
            const glm::vec3 selectedAim(attachmentPose[3]);
            CHECK(attachments.ReferenceOpticPose(world,particleRoot,attachmentPose));
            CHECK(glm::length(glm::vec3(attachmentPose[3])-selectedAim)<.01f);
        }
        for (auto emitter : emitters) {
            auto& ps = world.Registry.get<ParticleSystemComponent>(emitter);
            // The AK's chamber smoke is authored outside the selected muzzle attachment.
            const bool chamberSmoke=world.Registry.get<NameComponent>(emitter).Name=="Chamber Smoke";
            CHECK(attachments.OwnsEmitter(world,emitter)==!chamberSmoke);
            CHECK(ps.Shape==0 && ps.Alignment==0 && !ps.Emitting);
            if(ps.ShaderMode==1) CHECK(ps.Texture.empty() && ps.BlendMode==0 && !ps.LocalSpace);
            EmitParticleBurst(world,emitter,ps.BurstCount);
            const glm::vec3 origin = ps.LocalSpace ? glm::vec3(0) : glm::vec3(world.ComposeWorldTransform(emitter)[3]);
            CHECK(ps.Live.size()==size_t(ps.BurstCount) && !ps.Live.empty() && ps.Live[0].Local==ps.LocalSpace && glm::length(ps.Live[0].Pos-origin)<1e-6f);
        }
        if (world.Registry.valid(particleRoot)) world.DestroyEntityAndChildren(particleRoot);
    }
}
void TestCSharpWeaponDefinitionMigration() {
    using Json=nlohmann::json;
    using namespace Scripting;
    CHECK(EnsureLoaded());
    for(const auto& component:ComponentRegistry::All())CHECK(std::string(component.Meta.Name)!="Weapon Definition");
    std::string resolved;
    CHECK(ResolveScriptFields("Tartarus.Gameplay.WeaponDefinition","{}",resolved));
    const auto defaults=Json::parse(resolved);
    CHECK(defaults.at("AnimationSet").at("path")=="" && defaults.at("LightIntensity")==18);
    CHECK(defaults.at("MuzzleEnabled")==true && defaults.at("FlameGlow")==150 && defaults.at("SparkCount")==9);
    const std::string guid="123456789abcdef0";
    Json node={{"id",0},{"name","Legacy weapon"},{"Weapon Definition",{
        {"Description","Authored description"},{"Animation Set",{{"path","old.fpsanim"},{"pathGuid",guid}}},
        {"Light Color",{.2,.4,.6}},{"Light Intensity",27},{"Flame Glow",99},{"Smoke",false}}},
        {"C# Script",{{"Class","Tartarus.Gameplay.Bob"},{"Fields JSON","{\"Height\":0.75}"},
            {"Scripts",Json::array({{{"id",9},{"class","Tartarus.Gameplay.OpticAttachment"},{"fields",{{"AimTransform","Sight"}}}}}).dump()},
            {"Next Script ID",2}}}};
    CHECK(MigrateLegacyWeaponDefinition(node));CHECK(!MigrateLegacyWeaponDefinition(node));
    const auto& script=node.at("C# Script");
    CHECK(script.at("Class")=="Tartarus.Gameplay.Bob" && script.at("Fields JSON")=="{\"Height\":0.75}");
    const auto extra=Json::parse(script.at("Scripts").get<std::string>());
    CHECK(extra.size()==2 && extra[0]["id"]==9 && extra[1]["id"]==10 && script["Next Script ID"]==11);
    const auto& fields=extra[1].at("fields");
    CHECK(fields.at("AnimationSet").at("pathGuid")==guid && fields.at("LightColor").at("Y")==.4);
    CHECK(ResolveScriptFields("Tartarus.Gameplay.WeaponDefinition",fields.dump(),resolved));
    const auto settings=Json::parse(resolved);
    CHECK(settings.at("Description")=="Authored description" && settings.at("LightIntensity")==27 && settings.at("FlameGlow")==99 && settings.at("Smoke")==false);
    // Older snapshots migrate on read and save as ordinary C# slots, without losing other scripts.
    auto legacy=node;legacy["Weapon Definition"]={{"Description","Legacy fallback"}};
    World world;AssetLibrary assets;
    CHECK(SceneSerializer::LoadFromString(world,assets,Json{{"formatVersion",4},{"empties",Json::array({legacy})}}.dump()));
    const auto saved=Json::parse(SceneSerializer::SaveToString(world));
    CHECK(!saved.at("empties").at(0).contains("Weapon Definition"));
    const auto savedExtra=Json::parse(saved.at("empties").at(0).at("C# Script").at("Scripts").get<std::string>());
    CHECK(savedExtra.size()==2 && savedExtra[1].at("fields").at("Description")=="Authored description");
    // Keep an incomplete user script setup intact rather than replacing its primary slot.
    Json incomplete={{"Weapon Definition",{{"Description","Legacy"}}},
        {"C# Script",{{"Class",""},{"Source","assets/Scripts/Unfinished.cs"},{"Fields JSON","{}"}}}};
    CHECK(MigrateLegacyWeaponDefinition(incomplete));
    CHECK(incomplete["C# Script"]["Class"]=="" && incomplete["C# Script"]["Source"]=="assets/Scripts/Unfinished.cs");
    CHECK(Json::parse(incomplete["C# Script"]["Scripts"].get<std::string>()).at(0).at("class")=="Tartarus.Gameplay.WeaponDefinition");
    const auto metadata=Json::parse(Describe("Tartarus.Gameplay.WeaponDefinition"));
    bool asset=false,color=false,hidden=false;
    for(const auto& f:metadata.at("fields")) {
        if(f.at("name")=="AnimationSet")asset=f.at("kind")=="asset-ref" && f.at("extensions")==Json::array({".fpsanim"});
        if(f.at("name")=="LightColor")color=f.at("kind")=="color";
        if(f.at("name")=="FlameGlow")hidden=true;
    }
    CHECK(asset && color && !hidden);
    FirstPersonWeaponGameplay weaponStats;
    std::string statsError;
    if(std::filesystem::exists(ProjectPaths::Resolve("assets/Weapons/Remington870/Remington870.prefab"))) {
        CHECK(ResolveWeaponGameplay("assets/Weapons/Remington870/Remington870.prefab",weaponStats,statsError));
        CHECK(weaponStats.Magazine==6 && weaponStats.Pellets==8 && weaponStats.Reload==FirstPersonWeaponGameplay::ReloadMode::PerRound);
        FirstPersonAnimationSet animation;
        CHECK(FirstPersonAnimationSet::LoadFile(ProjectPaths::Resolve("assets/Weapons/Remington870/Remington870.fpsanim"),animation,&statsError));
        CHECK(!nlohmann::json::parse(animation.ToJsonString()).contains("gameplay"));
    }
}
void TestWeaponPrefabParticles() {
    using Json = nlohmann::json;
    const auto path = std::filesystem::temp_directory_path() / "tartarus-weapon-particle-test.prefab";
    Json data = {{"formatVersion",4},{"models",Json::array()}, {"empties",Json::array({
        {{"id",0},{"parentId",-1},{"name","Weapon"},{"Weapon Definition",{{"Animation Set","test.fpsanim"}}}},
        {{"id",1},{"parentId",0},{"name","Muzzle"},{"position",{0,0,-.7}}},
        {{"id",2},{"parentId",1},{"name","Flash"},{"Particle System",{
            {"Emitting",true},{"Looping",false},{"Rate",0},{"Burst Count",2},{"Start Speed",0},
            {"Local Space",true},{"Alignment","Billboard"},{"Texture Channels","Red Mask"},{"Pivot Y",.85},{"Lifetime",.08}}}},
        {{"id",3},{"parentId",-1},{"name","Unrelated"},{"Particle System",{{"Burst Count",99}}}}
    })}};
    { std::ofstream file(path); file << data.dump(); }
    std::string set, error; MuzzleEffectSettings muzzle;
    CHECK(Scripting::ResolveWeaponPrefab(path.u8string(),set,error,&muzzle));
    CHECK(muzzle.PrefabParticles);
    CHECK(Scripting::MigrateLegacyWeaponDefinition(data["empties"][0]));
    CHECK(!Scripting::MigrateLegacyWeaponDefinition(data["empties"][0]));
    CHECK(!data["empties"][0].contains("Weapon Definition"));
    {std::ofstream file(path);file<<data.dump();}
    CHECK(Scripting::ResolveWeaponPrefab(path.u8string(),set,error,&muzzle));
    CHECK(muzzle.PrefabParticles && muzzle.LightIntensity==18 && set=="test.fpsanim");
    World world; AssetLibrary assets;
    const auto weapon = world.CreateEmptyEntity({2,3,4},{0,90,0},{1,1,1},"Live weapon");
    std::vector<entt::entity> emitters;
    const auto root = Scripting::InstantiateWeaponParticles(world,assets,path.u8string(),emitters,error);
    std::filesystem::remove(path);
    CHECK(error.empty()); CHECK(world.Registry.valid(root)); CHECK(emitters.size()==1);
    if (emitters.empty() || !world.Registry.valid(root)) return;
    world.AttachChildRaw(root,weapon);
    auto& ps = world.Registry.get<ParticleSystemComponent>(emitters[0]);
    CHECK(!ps.Emitting && ps.Alignment==0 && ps.TextureChannels==1 && ps.PivotY==.85f);
    CHECK(ps.ShaderMode==0); // older prefabs retain their sprite renderer
    EmitParticleBurst(world,emitters[0],ps.BurstCount);
    CHECK(ps.Live.size()==2);
    CHECK(ps.Live[0].Local && glm::length(ps.Live[0].Pos)<1e-6f);
    const glm::vec3 expected = glm::vec3(world.ComposeWorldTransform(weapon)*glm::vec4(0,0,-.7,1));
    CHECK(glm::length(glm::vec3(world.ComposeWorldTransform(emitters[0])[3])-expected)<1e-5f);
    world.Registry.get<TransformComponent>(weapon).Position.x+=1;
    CHECK(glm::length(glm::vec3(world.ComposeWorldTransform(emitters[0])[3])-expected-glm::vec3(1,0,0))<1e-5f);
    UpdateParticleSystems(world,.1f); CHECK(ps.Live.empty()); // shot ends without continuous emission
    world.DestroyEntityAndChildren(weapon);
    CHECK(!world.Registry.valid(emitters[0]));
}

void TestWeaponAttachments() {
    using namespace Scripting;
    using Json = nlohmann::json;
    World world; AssetLibrary assets;
    auto root=world.CreateEmptyEntity({2,3,4},{0,90,0},{1,1,1},"Weapon");
    auto child=[&](entt::entity parent,const char* name,glm::vec3 position=glm::vec3(0)) {
        auto e=world.CreateEmptyEntity(position,{0,0,0},{1,1,1},name);
        world.AttachChildRaw(e,parent); return e;
    };
    auto script=[&](entt::entity owner,const char* type,Json fields) {
        auto& c=world.Registry.emplace<CSharpScriptComponent>(owner);
        c.ClassName=std::string("Tartarus.Gameplay.")+type; c.Fields=fields.dump();
    };
    auto muzzle=child(root,"Standard muzzle"), otherMuzzle=child(root,"Suppressor");
    auto flash=child(muzzle,"Flash",{0,0,-.7f}), otherFlash=child(otherMuzzle,"Flash",{0,0,-.9f});
    world.Registry.emplace<ParticleSystemComponent>(flash);
    world.Registry.emplace<ParticleSystemComponent>(otherFlash);
    script(muzzle,"MuzzleAttachment",{{"MuzzleTransform","Flash"},{"MuzzleFlash","Flash"},{"FiringSoundProfile","ak"}});
    script(otherMuzzle,"MuzzleAttachment",{{"MuzzleTransform","Flash"},{"MuzzleFlash","Flash"},{"FiringSounds","quiet1.wav;quiet2.wav"}});
    world.Registry.emplace<DeactivatedTag>(otherMuzzle);
    auto grip=child(root,"Grip"); script(grip,"GripAttachment",Json::object());
    auto irons=child(root,"Irons"), optic=child(root,"Red dot");
    child(irons,"Aim",{0,.1f,0}); child(optic,"Aim",{0,.2f,-.1f});
    script(irons,"OpticAttachment",{{"AimPoint","Aim"},{"DefaultOptic",true}});
    script(optic,"OpticAttachment",{{"AimPoint","Aim"},{"AimBlendTime",.3f}});
    world.Registry.emplace<DeactivatedTag>(irons);
    // Malformed authoring must not prevent the valid attachments from loading.
    auto broken=child(root,"Broken optic"); script(broken,"OpticAttachment",{{"AimPoint",17}});
    WeaponAttachments attachments; attachments.Load(world,root);
    CHECK(attachments.Count(AttachmentKind::Muzzle)==2 && attachments.Count(AttachmentKind::Optic)==2);
    CHECK(attachments.Selected(AttachmentKind::Muzzle)->Entity==muzzle);
    CHECK(attachments.Selected(AttachmentKind::Optic)->Entity==optic);
    CHECK(attachments.OwnsEmitter(world,flash) && !attachments.OwnsEmitter(world,otherFlash));
    glm::mat4 pose(1);
    CHECK(attachments.Pose(world,root,AttachmentKind::Muzzle,pose));
    CHECK(glm::length(glm::vec3(pose[3])-glm::vec3(0,0,-.7f))<1e-5f);
    CHECK(attachments.ReferenceOpticPose(world,root,pose));
    CHECK(glm::length(glm::vec3(pose[3])-glm::vec3(0,.1f,0))<1e-5f);
    CHECK(attachments.Cycle(world,AttachmentKind::Muzzle));
    CHECK(attachments.Selected(AttachmentKind::Muzzle)->FiringSounds=="quiet1.wav;quiet2.wav");
    CHECK(!attachments.OwnsEmitter(world,flash) && attachments.OwnsEmitter(world,otherFlash));
    CHECK(attachments.Cycle(world,AttachmentKind::Muzzle));
    CHECK(attachments.Selected(AttachmentKind::Muzzle)->FiringSoundProfile=="ak");
    CHECK(!attachments.Cycle(world,AttachmentKind::Grip));
    CHECK(attachments.Cycle(world,AttachmentKind::Optic));
    CHECK(world.Registry.all_of<InactiveTag>(optic) && !world.Registry.all_of<InactiveTag>(irons));
    CHECK(attachments.Cycle(world,AttachmentKind::Optic));
    CHECK(std::abs(attachments.Selected(AttachmentKind::Optic)->AimBlendTime-.3f)<1e-5f);
    const auto saved=SceneSerializer::SaveEntitiesToString(world,{root});
    World clone; std::vector<entt::entity> created;
    CHECK(SceneSerializer::AppendEntitiesFromString(clone,assets,saved,created));
    entt::entity cloneRoot=entt::null;
    for(auto e:created) if(clone.Registry.get<HierarchyComponent>(e).Parent==entt::null) cloneRoot=e;
    WeaponAttachments mirrored; mirrored.Load(clone,cloneRoot,attachments.Indices());
    CHECK(mirrored.Indices()==attachments.Indices());
    CHECK(mirrored.Pose(clone,cloneRoot,AttachmentKind::Optic,pose));
    CHECK(glm::length(glm::vec3(pose[3])-glm::vec3(0,.2f,-.1f))<1e-5f);
}

void TestMaterialShaderSelection() {
    // Descriptor parsing and property preservation do not require a GL context.
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
    ShaderLibrary::Init((std::filesystem::path(executable).parent_path()/"assets/shaders").u8string());
    AssetLibrary assets; MaterialAsset material;
    material.Mat.BaseColor={.2f,.4f,.6f}; material.Mat.Roughness=.65f;
    CHECK(material.SetShader("engine://RedDot.shader",assets));
    CHECK(material.RenderQueue==MaterialAsset::Queue::Transparent && material.QueueIndex==3000);
    CHECK(material.Mat.ExtraProps.at("_ReticleBrightness").F==5);
    CHECK(material.Mat.ExtraProps.at("_BlurSamples").I==8);
    CHECK(!material.Mat.ExtraProps.at("_UseTextureColor").B);
    material.Mat.ExtraProps.at("_ReticleBrightness").F=7;
    CHECK(material.SetShader("",assets));
    CHECK(material.ShaderPath=="engine://Standard.shader" && material.RenderQueue==MaterialAsset::Queue::Opaque);
    CHECK(material.SetShader("engine://RedDot.shader",assets));
    CHECK(material.Mat.ExtraProps.at("_ReticleBrightness").F==7);
    CHECK(material.Mat.BaseColor==glm::vec3(.2f,.4f,.6f) && material.Mat.Roughness==.65f);
    CHECK(!material.SetShader("engine://MissingShader.shader",assets));
    CHECK(material.ShaderPath=="engine://RedDot.shader");
}

void TestManagedBallistics() {
    using namespace Scripting;
    World world;
    ShotFrame shot; shot.Direction={0,0,-1}; shot.Range=100; shot.Pellets=8; shot.Spread=3;
    shot.RandomSeed=42; shot.BulletHoleRadius=.025f;
    int count=0;
    CHECK(InvokeProjectWithTrace(world,"shot",&shot,sizeof shot,[&](const NativeRequest& trace) {
        ++count; CHECK(trace.Result==0 && trace.Entity==0xFFFFFFFFu);
        const glm::vec3 delta(trace.B.x-trace.A.x,trace.B.y-trace.A.y,trace.B.z-trace.A.z);
        CHECK(std::abs(glm::length(delta)-100)<.001f);
        CHECK(glm::dot(glm::normalize(delta),glm::vec3(0,0,-1))>=std::cos(glm::radians(3.01f)));
    }));
    CHECK(count==8);
    auto target=world.CreateEmptyEntity({0,0,-5},{0,0,0},{1,1,1},"Target");
    auto& collider=world.Registry.emplace<ColliderComponent>(target); collider.HalfExtents={2,2,.5f};
    auto& body=world.Registry.emplace<RigidbodyComponent>(target);
    body.Mass=2; body.UseGravity=false; body.LinearDamping=0;
    world.SyncActiveInHierarchy(); world.RebuildWorldTransformCache(); PhysicsWorld::Create(world);
    CHECK(PhysicsWorld::IsActive());
    shot.Spread=0; shot.ImpactImpulse=100; shot.ImpactMaxSpeed=3;
    count=0;
    CHECK(InvokeProjectWithTrace(world,"shot",&shot,sizeof shot,[&](const NativeRequest& trace) {
        ++count; CHECK(trace.Result==1 && trace.Entity==entt::to_integral(target));
        CHECK(std::abs(trace.B.z+4.5f)<.001f);
    }));
    CHECK(count==8);
    PhysicsWorld::Step(.02f,world);
    BodyState state; CHECK(PhysicsWorld::GetBodyState(entt::to_integral(target),state));
    CHECK(std::abs(state.Velocity[2]+3)<.01f); // the capped impulse is shared between eight pellets
    PhysicsWorld::Destroy(); PhysicsWorld::Shutdown();
}
void TestManagedEditorBuild() {
    Scripting::RequestBuild(); CHECK(Scripting::BuildPending());
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while((Scripting::Building() || Scripting::BuildPending()) && std::chrono::steady_clock::now()<deadline) {
        Scripting::Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!Scripting::Building());
    CHECK(!Scripting::BuildPending());
    CHECK(Scripting::LastError().empty());
}
void TestManagedUnityWorkflow() {
    using namespace Scripting;
    wchar_t executable[32768]{};
    CHECK(GetModuleFileNameW(nullptr,executable,32768)!=0); // tests precede EnginePaths::Init
    const auto executableDir=std::filesystem::path(executable).parent_path();
    const auto fixture=(executableDir/"ScriptTests/Tartarus.Gameplay.Tests.dll").u8string();
    if(!std::filesystem::exists(std::filesystem::u8path(fixture))) {
        std::cout<<"[UnitTest] SKIP managed Unity fixture (not shipped in exported games)\n"; return;
    }
    CHECK(Invoke(0,const_cast<char*>(fixture.c_str()),kVersion));
    const auto metadata=nlohmann::json::parse(Describe("Tartarus.Tests.Probe"));
    CHECK(metadata.contains("fields"));
    bool gain=false,range=false,mode=false;
    for(const auto& f:metadata.value("fields",nlohmann::json::array())) {
        CHECK(f.at("name")!="updates"); // HideInInspector retains state without exposing a control
        if(f.at("name")=="gain") gain=f.at("default")==1.5;
        if(f.at("name")=="Speed") range=f.at("min")==0 && f.at("max")==10;
        if(f.at("name")=="Mode") mode=f.at("values")==nlohmann::json::array({0,7});
    }
    CHECK(gain && range && mode);
    World world; AssetLibrary assets;
    auto parent=world.CreateEmptyEntity({10,0,0},{0,0,0},{2,2,2},"Parent");
    auto entity=world.CreateEmptyEntity({0,0,0},{0,0,0},{1,1,1},"Behaviours");
    world.AttachChildRaw(entity,parent);
    auto& component=world.Registry.emplace<CSharpScriptComponent>(entity);
    Attach(component,"assets/Scripts/Bob.cs","Tartarus.Tests.Probe");
    Attach(component,"assets/Scripts/Bob.cs","Tartarus.Tests.Probe");
    auto slots=GetSlots(component); CHECK(slots.size()==2); CHECK(slots[0].Id!=slots[1].Id);
    slots[0].Fields="{\"Prefix\":\"A\",\"Speed\":2}";
    slots[1].Fields="{\"Prefix\":\"B\",\"Speed\":4}"; SetSlots(component,slots);
    world.Registry.emplace<AnimatorControllerComponent>(entity);
    world.Registry.emplace<RigidbodyComponent>(entity).Mass=2;
    const auto snapshot=SceneSerializer::SaveToString(world);
    World restored; CHECK(SceneSerializer::LoadFromString(restored,assets,snapshot));
    for(auto [e,c]:restored.Registry.view<CSharpScriptComponent>().each()) {
        const auto saved=GetSlots(c); CHECK(saved.size()==2); CHECK(saved[1].Id==slots[1].Id);
        CHECK(saved[1].Fields==slots[1].Fields); CHECK(c.NextScriptId==component.NextScriptId);
    }
    auto param=[&](const char* name) { return world.Registry.get<AnimatorControllerComponent>(entity).GetFloat(name); };
    Tick(world,assets,.5f);
    CHECK(param("AAwake")==1 && param("BAwake")==1);
    CHECK(param("AStart")==1 && param("BStart")==1);
    CHECK(param("APeers")==2 && param("BPeers")==2);
    CHECK(param("ATransform")==1 && param("AMass")==2);
    CHECK(param("ALate")==1 && param("BLate")==1);
    CHECK(std::abs(world.Registry.get<TransformComponent>(entity).Position.y-3)<.001f);
    CHECK(std::abs(world.ComposeWorldTransform(entity)[3].x-11.5f)<.001f); // world position setter converts through parent scale
    Tick(world,assets,.02f,true); CHECK(param("AFixed")==1 && param("BFixed")==1);
    component.Enabled=false; Tick(world,assets,.1f);
    CHECK(param("ADisable")==1 && param("ADestroy")==0 && param("AState")==1);
    CHECK(param("BState")==2);
    Tick(world,assets,.1f); CHECK(param("ADisable")==1);
    component.Enabled=true; Tick(world,assets,.1f);
    CHECK(param("AEnable")==2 && param("AAwake")==1 && param("AStart")==1);
    CHECK(param("AState")==2);
    CHECK(Invoke(5,nullptr,0)); Tick(world,assets,0);
    CHECK(param("AState")==3 && param("BState")==5); // private SerializeField state survives reload
    CHECK(param("AAwake")==1 && param("AStart")==1);
    // A live field edit doesn't recreate the script or reset unrelated state.
    component.Fields="{\"Prefix\":\"A\",\"Speed\":3}"; Tick(world,assets,0);
    CHECK(param("AState")==4 && param("AStart")==1);
    slots=GetSlots(component); const auto removed=slots[0].Id; slots.erase(slots.begin()); SetSlots(component,slots);
    Attach(component,"c.cs","Tartarus.Tests.Probe"); slots=GetSlots(component);
    CHECK(slots.back().Id!=removed && slots.back().Id!=slots.front().Id);
    slots.back().Fields="{\"Prefix\":\"C\"}"; SetSlots(component,slots); Tick(world,assets,0);
    CHECK(param("ADestroy")==1 && param("BAwake")==1 && param("CAwake")==1);
    CHECK(param("CPeers")==2); // a removed instance isn't returned to the new script
    Stop(&world,&assets); CHECK(param("BDestroy")==1 && param("CDestroy")==1);
    World physics;
    const auto bodyEntity=physics.CreateEmptyEntity({0,0,0},{0,0,0},{1,1,1},"Body");
    physics.Registry.emplace<ColliderComponent>(bodyEntity);
    auto& rb=physics.Registry.emplace<RigidbodyComponent>(bodyEntity); rb.Mass=2; rb.UseGravity=false; rb.LinearDamping=0;
    physics.Registry.emplace<AnimatorControllerComponent>(bodyEntity);
    Attach(physics.Registry.emplace<CSharpScriptComponent>(bodyEntity),"","Tartarus.Tests.Probe");
    physics.SyncActiveInHierarchy(); physics.RebuildWorldTransformCache(); PhysicsWorld::Create(physics);
    Tick(physics,assets,0);
    CHECK(physics.Registry.get<AnimatorControllerComponent>(bodyEntity).GetFloat("AVelocity")==2);
    CHECK(physics.Registry.get<AnimatorControllerComponent>(bodyEntity).GetFloat("ApiQueries")==1);
    PhysicsWorld::Step(.02f,physics); BodyState bodyState;
    CHECK(PhysicsWorld::GetBodyState(entt::to_integral(bodyEntity),bodyState));
    CHECK(std::abs(bodyState.Velocity[2]-2.5f)<.001f);
    Stop(&physics,&assets); PhysicsWorld::Destroy(); PhysicsWorld::Shutdown();
    const auto gameplay=ProjectPaths::Resolve("Scripts/bin/Tartarus.Gameplay.dll");
    CHECK(Invoke(0,const_cast<char*>(gameplay.c_str()),kVersion));
}
// vInspector (Phase 3b): the C# attributes come through Describe, and the edit-mode button /
// ShowInInspector calls run on a temporary instance holding the saved fields.
void TestManagedInspectorAttributes() {
    using namespace Scripting;
    using Json=nlohmann::json;
    wchar_t executable[32768]{};
    CHECK(GetModuleFileNameW(nullptr,executable,32768)!=0);
    const auto fixture=(std::filesystem::path(executable).parent_path()/"ScriptTests/Tartarus.Gameplay.Tests.dll").u8string();
    if(!std::filesystem::exists(std::filesystem::u8path(fixture))) {
        std::cout<<"[UnitTest] SKIP managed attribute fixture (not shipped in exported games)\n"; return;
    }
    CHECK(Invoke(0,const_cast<char*>(fixture.c_str()),kVersion));
    const Json& meta=DescribeJson("Tartarus.Tests.AttributeProbe");
    CHECK(&meta==&DescribeJson("Tartarus.Tests.AttributeProbe")); // parsed once, then cached
    CHECK(meta.contains("fields") && meta.contains("buttons") && meta.contains("shows"));
    auto field=[&](const char* name)->Json {
        for(const auto& f:meta.value("fields",Json::array())) if(f.value("name",std::string{})==name) return f;
        return Json();
    };
    const Json speed=field("Speed");
    CHECK(speed.value("foldout",std::string{})=="Motion" && speed.value("tab",std::string{})=="Tuning");
    CHECK(speed.value("variants",Json::array())==Json::array({1.0,5.0,10.0}));
    CHECK(field("MaxSpeed").value("onChanged",std::string{})=="ClampSpeed");
    CHECK(field("Hits").value("readOnly",false));
    CHECK(field("Note").at("hideIf")==Json({{"field","Advanced"},{"value",false}}));
    CHECK(field("Boost").at("disableIf")==Json({{"field","Mode"},{"value",0}}));
    CHECK(field("Weights").value("kind",std::string{})=="dict" && field("Weights").value("valueKind",std::string{})=="float");
    CHECK(field("Weights").value("default",Json())==Json({{"head",2.0}}));
    CHECK(field("Transient").is_null()); // NonSerialized: not an editable field...
    std::set<std::string> shows, buttons;
    for(const auto& s:meta.at("shows")) shows.insert(s.value("name",std::string{}));
    for(const auto& b:meta.at("buttons")) buttons.insert(b.value("label",std::string{})+"|"+b.value("tab",std::string{}));
    CHECK(shows.count("Doubled") && shows.count("Transient")); // ...but shown read-only
    CHECK(buttons.count("Reset Hits|") && buttons.count("DoubleSpeed|Tuning"));

    World world; AssetLibrary assets;
    std::string after;
    CHECK(InvokeEditorMethod(world,assets,"Tartarus.Tests.AttributeProbe",0,0,"{\"Hits\":9,\"Speed\":3}","ResetHits",after));
    Json saved=Json::parse(after);
    CHECK(saved.value("Hits",-1)==0 && saved.value("Speed",0.0)==3.0); // other saved values kept
    CHECK(InvokeEditorMethod(world,assets,"Tartarus.Tests.AttributeProbe",0,0,"{\"Speed\":20,\"MaxSpeed\":8}","ClampSpeed",after));
    CHECK(Json::parse(after).value("Speed",0.0)==8.0);
    CHECK(!InvokeEditorMethod(world,assets,"Tartarus.Tests.AttributeProbe",0,0,"{}","NoSuchMethod",after));
    const Json values=Json::parse(ShowValues(world,assets,"Tartarus.Tests.AttributeProbe",0,0,"{\"Speed\":4}"));
    CHECK(values.value("Doubled",std::string{})=="8" && values.value("Transient",std::string{})=="7");
    // Later tests exercise project audio/FX policies; leave the sample integration loaded.
    const auto gameplay=ProjectPaths::Resolve("Scripts/bin/Tartarus.Gameplay.dll");
    CHECK(Invoke(0,const_cast<char*>(gameplay.c_str()),kVersion));
}
void TestManagedApiAndEditor() {
    using namespace Scripting;
    wchar_t executable[32768]{};CHECK(GetModuleFileNameW(nullptr,executable,32768)!=0);
    const auto dir=std::filesystem::path(executable).parent_path();
    const auto fixture=(dir/"ScriptTests/Tartarus.Gameplay.Tests.dll").u8string();
    if(!std::filesystem::exists(std::filesystem::u8path(fixture))) return;
    CHECK(Invoke(0,const_cast<char*>(fixture.c_str()),kVersion));
    World world;AssetLibrary assets;
    const auto entity=world.CreateEmptyEntity({10,0,0},{0,0,0},{2,2,2},"API Owner");
    world.Registry.emplace<AnimatorControllerComponent>(entity);
    Attach(world.Registry.emplace<CSharpScriptComponent>(entity),"","Tartarus.Tests.ApiProbe");
    Tick(world,assets,0);
    CHECK(world.Registry.get<AnimatorControllerComponent>(entity).GetFloat("ApiPassed")==1);
    Stop(&world,&assets);
    const auto gameplay=ProjectPaths::Resolve("Scripts/bin/Tartarus.Gameplay.dll");CHECK(Invoke(0,const_cast<char*>(gameplay.c_str()),kVersion));
    if(!std::filesystem::exists(std::filesystem::u8path(ProjectPaths::Resolve("Scripts/bin/Tartarus.Editor.dll")))) return; // editor tools are excluded from exports
    entt::entity selected=entt::null;int undo=0,depth=0,launcherDraws=0;bool create=true,reset=false,closeLauncher=false;
    auto services=[&](int op,NativeRequest& r) {
        const std::string text=r.Text?r.Text:"";
        if(op==100) {++depth;bool launcher=text.rfind("C# Tools",0)==0;if(launcher)++launcherDraws;r.Result=launcher && closeLauncher?0:1;return 1;}
        if(op==101) {--depth;return 1;}
        if(op==103) {
            if(text.rfind("Open Scene Tools",0)==0) return 1;
            if(text=="Create empty object" && create) {create=false;return 1;}
            if(text=="Reset local position" && reset) {reset=false;return 1;}
            return 0;
        }
        if(op==110) {++undo;return 1;}
        if(op==111) {r.Entity=entt::to_integral(selected);return selected!=entt::null?1:0;}
        if(op==112) {selected=static_cast<entt::entity>(r.Entity);return 1;}
        return 1;
    };
    CHECK(!EditorToolsVisible());
    DrawEditorScripts(world,assets,.016f,services);
    CHECK(depth==0 && launcherDraws==0 && undo==0);
    SetEditorToolsVisible(true);CHECK(EditorToolsVisible());
    DrawEditorScripts(world,assets,.016f,services);
    CHECK(depth==0 && undo==1 && world.Registry.valid(selected));
    if(world.Registry.valid(selected)) {
        CHECK(world.Registry.get<NameComponent>(selected).Name=="C# Object");
        world.Registry.get<TransformComponent>(selected).Position={3,4,5};reset=true;
        DrawEditorScripts(world,assets,.016f,services);
        CHECK(world.Registry.get<TransformComponent>(selected).Position==glm::vec3(0));
        CHECK(depth==0 && undo==2);
    }
    closeLauncher=true;DrawEditorScripts(world,assets,.016f,services);
    CHECK(!EditorToolsVisible());const auto closedDraws=launcherDraws;
    DrawEditorScripts(world,assets,.016f,services);CHECK(launcherDraws==closedDraws && depth==0);
    // Exercise real custom inspectors without a graphics window: balanced groups,
    // native reflected fields, and an authored C# field edit returned to the host.
    int groups=0,nativeFields=0;
    std::string returnedFields;
    auto inspectorServices=[&](int op,NativeRequest& r) {
        if(op==117){++groups;return 1;}
        if(op==118){--groups;return 1;}
        if(op==125){++nativeFields;return 1;}
        if(op==108){r.Value=.75f;return 1;}
        if(op==115)return 0; // keep the search empty
        if(op==132 || op==133)return 0;
        if(op==116){returnedFields=r.Text?r.Text:"{}";r.Text=returnedFields.c_str();return 1;}
        return 1;
    };
    std::string payload=nlohmann::json{{"type","First Person Body"},{"native",true}}.dump();
    NativeRequest inspector;inspector.Entity=entt::to_integral(entity);inspector.Text=payload.c_str();
    CHECK(DrawEditorInspector(world,assets,inspector,inspectorServices));
    CHECK(groups==0 && nativeFields>0);
    payload=nlohmann::json{{"type","Tartarus.Gameplay.Bob"},{"native",false},{"values",nlohmann::json::object()},
        {"metadata",nlohmann::json::parse(Describe("Tartarus.Gameplay.Bob"))}}.dump();
    inspector={};inspector.Entity=entt::to_integral(entity);inspector.Text=payload.c_str();
    CHECK(DrawEditorInspector(world,assets,inspector,inspectorServices));
    CHECK(groups==0);
    CHECK(nlohmann::json::parse(returnedFields).value("Height",0.0f)==.75f);
    const std::string selectedAsset=R"({"path":"selected.fpsanim","pathGuid":"123456789abcdef0"})";
    auto weaponServices=[&](int op,NativeRequest& r) {
        if(op==115 && nlohmann::json::parse(r.Text).value("label",std::string{})=="Find setting") {r.Text=" ";return 1;}
        if(op==132) {r.Text=selectedAsset.c_str();return 1;}
        if(op==133) {r.A={.2f,.4f,.6f};return 1;}
        return inspectorServices(op,r);
    };
    payload=nlohmann::json{{"type","Tartarus.Gameplay.WeaponDefinition"},{"native",false},{"values",nlohmann::json::object()},
        {"metadata",nlohmann::json::parse(Describe("Tartarus.Gameplay.WeaponDefinition"))}}.dump();
    inspector={};inspector.Entity=entt::to_integral(entity);inspector.Text=payload.c_str();
    CHECK(DrawEditorInspector(world,assets,inspector,weaponServices));
    const auto weaponFields=nlohmann::json::parse(returnedFields);
    CHECK(groups==0 && weaponFields.at("AnimationSet").at("path")=="selected.fpsanim");
    CHECK(weaponFields.at("AnimationSet").at("pathGuid")=="123456789abcdef0" && std::abs(weaponFields.at("LightColor").at("Y").get<float>()-.4f)<1e-6f);
    int references=0;
    auto referenceServices=[&](int op,NativeRequest& r) {
        if(op==132) {
            const auto data=nlohmann::json::parse(r.Text);
            CHECK(data.at("metadata").at("kind")=="scene-ref" || data.at("metadata").at("kind")=="sound-refs" || data.at("metadata").at("kind")=="choice");
            CHECK(r.Entity==entt::to_integral(entity));++references;
            static std::string assigned;
            assigned=data.at("metadata").at("name")=="MuzzleFlash"?"Flash":data.at("value").get<std::string>();
            r.Text=assigned.c_str();return 1;
        }
        return inspectorServices(op,r);
    };
    const auto muzzleMetadata=nlohmann::json::parse(Describe("Tartarus.Gameplay.MuzzleAttachment"));
    payload=nlohmann::json{{"type","Tartarus.Gameplay.MuzzleAttachment"},{"native",false},{"values",{{"MuzzleFlash","Old Flash"}}},{"metadata",muzzleMetadata}}.dump();
    inspector={};inspector.Entity=entt::to_integral(entity);inspector.Text=payload.c_str();
    CHECK(DrawEditorInspector(world,assets,inspector,referenceServices));
    CHECK(references==4 && groups==0);
    CHECK(nlohmann::json::parse(returnedFields).at("MuzzleFlash")=="Flash");
    const auto opticMetadata=nlohmann::json::parse(Describe("Tartarus.Gameplay.OpticAttachment"));
    CHECK(opticMetadata.at("fields").at(0).at("kind")=="scene-ref");
    StopEditorScripts();
}
}
namespace {
void TestEmptyProject() {
    using namespace Scripting;namespace fs=std::filesystem;
    fs::path project=fs::u8path(ProjectPaths::Resolve(""));if(project.filename().empty())project=project.parent_path();
    if(project.filename()!="boundary-empty-project")return; // run only in its isolated process/project
    CHECK(EnsureLoaded());CHECK(nlohmann::json::parse(Describe()).at("classes").empty());
    PlayerFrame game;CHECK(!InvokeProject("player",&game,sizeof game));
    std::string response;CHECK(!RequestProject("weapon.resolve","{}",response));
    const auto source=project/"assets/Scripts/Ordinary.cs";fs::create_directories(source.parent_path());
    std::ofstream(source)<<"using Tartarus; public sealed class Ordinary : Script {public override void Update(float dt){var obj=gameObject;obj.name=\"ordinary script ran\";}}";
    CHECK(Build());const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    while(Building() && std::chrono::steady_clock::now()<deadline){Poll();std::this_thread::sleep_for(std::chrono::milliseconds(50));}
    CHECK(!Building());CHECK(fs::exists(project/"Scripts/bin/Tartarus.Gameplay.dll"));CHECK(Invoke(5,nullptr,0));
    const auto types=nlohmann::json::parse(Describe());CHECK(types.at("classes").size()==1);
    CHECK(!fs::exists(project/"assets/Scripts/WeaponController.cs"));CHECK(!fs::exists(project/"assets/Scripts/PlayerController.cs"));
    CHECK(!InvokeProject("npc.combat",&game,sizeof game));
    World world;AssetLibrary assets;auto entity=world.CreateEmptyEntity({},{},{1,1,1},"Plain");
    auto& script=world.Registry.emplace<CSharpScriptComponent>(entity);Attach(script,"assets/Scripts/Ordinary.cs","Ordinary");Tick(world,assets,.02f);
    CHECK(world.Registry.get<NameComponent>(entity).Name=="ordinary script ran");CHECK(world.Registry.view<FirstPersonControllerComponent>().empty());CHECK(world.Registry.view<NpcSpawnComponent>().empty());
    Stop(&world,&assets);
}
void TestExportedProjectHost() {
    using namespace Scripting;namespace fs=std::filesystem;
    fs::path project=fs::u8path(ProjectPaths::Resolve(""));if(project.filename().empty())project=project.parent_path();
    if(project.filename()!="project" || project.parent_path().filename()!="boundary-empty-export")return;
    CHECK(EnsureLoaded());
    const auto types=nlohmann::json::parse(Describe());CHECK(types.at("classes").size()==1);
    PlayerFrame frame;CHECK(!InvokeProject("player",&frame,sizeof frame));
    std::string response;CHECK(!RequestProject("weapon.resolve","{}",response));
    CHECK(!fs::exists(project/"assets/Scripts/WeaponController.cs"));CHECK(!fs::exists(project/"assets/Scripts/PlayerController.cs"));
    World world;AssetLibrary assets;auto entity=world.CreateEmptyEntity({},{},{1,1,1},"Exported plain object");
    auto& script=world.Registry.emplace<CSharpScriptComponent>(entity);Attach(script,"assets/Scripts/Ordinary.cs","Ordinary");Tick(world,assets,.02f);
    CHECK(world.Registry.get<NameComponent>(entity).Name=="ordinary script ran");
    CHECK(world.Registry.view<FirstPersonControllerComponent>().empty());CHECK(world.Registry.view<NpcSpawnComponent>().empty());
    Stop(&world,&assets);
}
void TestProjectLifecycleAndSession(){
    using namespace Scripting;
    NpcMateFrame member;member.Feet={0,0,0};NpcSpawnFrame spawn;spawn.Position={0,0,0};spawn.Yaw=0;spawn.Now=10;spawn.Index=2;spawn.Weapon=2;spawn.Random=3;spawn.Count=1;spawn.Members=reinterpret_cast<std::uintptr_t>(&member);
    CHECK(InvokeProject("npc.spawn",&spawn,sizeof spawn));CHECK(spawn.Weapon==1 && std::abs(spawn.Feet.x-1.2f)<.001f);CHECK(spawn.AimYaw==90 && std::abs(spawn.Eye.y-1.62f)<.001f);CHECK(spawn.NextThink==10.1f && spawn.AgentHeight==1.8f);
    NpcLifeFrame life;life.Now=30;life.Wounded=1;life.WoundedAt=10;life.BleedOutTime=10;life.CrawlSpeed=.6f;life.Intent.Pace=3;life.CorpseTime=5;
    CHECK(InvokeProject("npc.life",&life,sizeof life));CHECK(life.Kill && life.Speed==.6f && !life.Despawn);
    life.Wounded=0;life.Intent.Crouch=1;life.Intent.Pace=1;life.LimpUntil=40;life.LimpScale=.5f;life.StaggerUntil=40;CHECK(InvokeProject("npc.life",&life,sizeof life));CHECK(life.Crouch && std::abs(life.Speed-.19f)<.0001f);
    life={};life.Operation=1;life.Dt=.1f;life.Gravity=20;life.Want={3,0,0};life.Push={0,0,7};CHECK(InvokeProject("npc.life",&life,sizeof life));CHECK(std::abs(life.Displacement.x-.3f)<.0001f && std::abs(life.Displacement.y+.2f)<.0001f && std::abs(life.Displacement.z-.7f)<.0001f);
    life.Operation=2;life.Grounded=1;life.Intent.Move=1;life.Intent.Crouch=1;life.Intent.Pace=1;life.Intent.Aim=1;life.MeleeAt=-100;life.MeleeTime=.5f;life.FootIKRange=20;life.OnScreen=1;CHECK(InvokeProject("npc.life",&life,sizeof life));CHECK(life.FallSpeed==-1 && life.BlockedTime==.1f && life.Crouch && life.Aim && life.FootIK);
    life.Operation=3;life.Now=10;life.Origin={0,0,0};life.End={0,0,20};life.Eye={1.2f,0,10};CHECK(InvokeProject("npc.life",&life,sizeof life));CHECK(life.Memory.Known && life.Memory.Awareness==1 && life.Suppression>0);
    NpcPlayerNoiseFrame noise;noise.Now=10;noise.Dt=.1f;noise.Player.Valid=1;noise.Player.Fired=1;noise.Player.Reloading=1;noise.Player.Velocity={0,0,5};noise.Player.Feet={0,0,3};CHECK(InvokeProject("npc.player-noise",&noise,sizeof noise));CHECK(noise.Gun.Radius==85 && noise.Step.Radius==16 && noise.Reload.Source==-2 && noise.FootstepTimer==.32f && noise.Still==0);noise.Player.Crouched=1;noise.Step={};CHECK(InvokeProject("npc.player-noise",&noise,sizeof noise));CHECK(noise.Step.Radius==0);
    NpcRespawnPointFrame points[2];points[0].Position={0,0,30};points[0].Seen=1;points[1].Position={0,0,25};NpcRespawnFrame respawn;respawn.Now=10;respawn.Timer=8;respawn.Wanted=4;respawn.Player.Valid=1;respawn.Points=reinterpret_cast<std::uintptr_t>(points);respawn.Count=2;
    CHECK(InvokeProject("npc.respawn",&respawn,sizeof respawn));CHECK(respawn.Best==1 && respawn.Ready && !respawn.Discard && respawn.Timer==12);respawn.Alive=4;CHECK(InvokeProject("npc.respawn",&respawn,sizeof respawn));CHECK(respawn.Discard && !respawn.Ready);
    NpcShotEffectFrame shot;shot.Player.Valid=1;shot.Player.Eye={1,1,10};shot.Origin={0,1,0};shot.End={0,1,20};shot.FirstPellet=1;shot.Reach=5;CHECK(InvokeProject("npc.shot-effects",&shot,sizeof shot));CHECK(shot.Commands==7 && shot.Tracer==1 && shot.Miss==1);shot.Shotgun=1;CHECK(InvokeProject("npc.shot-effects",&shot,sizeof shot));CHECK(shot.Commands==5 && shot.Tracer==1);
    shot.Operation=2;shot.Friendly=1;shot.Damage=40;shot.Suppression=.9f;CHECK(InvokeProject("npc.shot-effects",&shot,sizeof shot));CHECK(shot.Damage==20 && shot.Suppression==1 && shot.Commands==(32|64));
    DevFrame dev;dev.Open=dev.God=dev.Frozen=dev.InfiniteAmmo=1;CHECK(InvokeProject("dev",&dev,sizeof dev));CHECK(!dev.Open && !dev.God && !dev.Frozen && !dev.InfiniteAmmo);
    struct Actions{int Kills=0,Spawns=0;} actions;NpcMateFrame mates[2];mates[1].Dead=1;dev.Operation=3;dev.Members=reinterpret_cast<std::uintptr_t>(mates);dev.Count=2;dev.Context=reinterpret_cast<std::uintptr_t>(&actions);
    dev.Services=reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t ptr,NpcServiceFrame* req)->int{auto& a=*reinterpret_cast<Actions*>(ptr);if(req->Operation==0){++a.Kills;return 1;}if(req->Operation==1){++a.Spawns;return a.Spawns>1;}return 0;});
    CHECK(InvokeProject("dev",&dev,sizeof dev));CHECK(dev.Result==1 && actions.Kills==1);dev.Operation=4;dev.Alive=1;dev.Wanted=3;dev.Spawns=2;CHECK(InvokeProject("dev",&dev,sizeof dev));CHECK(dev.Result==2 && actions.Spawns==3);
    GameSessionFrame session;session.HasInput=1;session.Scroll=-1;CHECK(InvokeProject("session",&session,sizeof session));CHECK(session.SlotDirection==1 && (session.Commands&8));session.HasInput=0;CHECK(InvokeProject("session",&session,sizeof session));CHECK(session.Commands==512 && session.SlotDirection==0);
    session.Operation=1;session.Invisible=1;session.InfiniteAmmo=1;session.PlayerValid=1;session.Armed=1;session.Magazine=30;CHECK(InvokeProject("session",&session,sizeof session));CHECK(!session.PlayerValid && session.Ammo==30 && session.Commands==1024);
    session={};session.Operation=2;session.Dead=1;session.WantsRespawn=1;session.RespawnFeet={2,3,4};session.EyeHeight=1.5f;CHECK(InvokeProject("session",&session,sizeof session));CHECK(session.Commands==4096 && session.Position.x==2 && session.Camera.y==4.5f);
    session.Operation=3;session.NpcHit=1;session.AliveHit=1;session.Killed=1;CHECK(InvokeProject("session",&session,sizeof session));CHECK(session.Commands==(16384|32768));session.AliveHit=0;session.Killed=0;CHECK(InvokeProject("session",&session,sizeof session));CHECK(session.Commands==0);
    int ammo[2]={3,7};WeaponLoadoutFrame loadout;loadout.Operation=3;loadout.Count=2;loadout.SlotAmmo=reinterpret_cast<std::uintptr_t>(ammo);loadout.Active=1;loadout.Magazine=30;loadout.CycleWait=2;CHECK(InvokeProject("weapon.loadout",&loadout,sizeof loadout));CHECK(ammo[0]==-1 && ammo[1]==-1 && loadout.Ammo==30 && loadout.Chambered && loadout.CycleWait==0);
    loadout.Operation=0;loadout.Slot=0;loadout.Pending=-1;loadout.Value=1;loadout.Equipped=1;CHECK(InvokeProject("weapon.loadout",&loadout,sizeof loadout));CHECK(loadout.Pending==1 && !loadout.Equipped && loadout.Commands==12);
    loadout.Operation=2;loadout.Requested=0;loadout.Burst=3;CHECK(InvokeProject("weapon.loadout",&loadout,sizeof loadout));CHECK(loadout.Pending==-1 && loadout.Burst==0 && loadout.Commands==6);
    if (!std::filesystem::exists(ProjectPaths::Resolve("assets/Editor/Content/BloodImport.json"))) return; // sample package: no catalogs
    std::string response;CHECK(RequestProject("content.blood","{}",response));CHECK(nlohmann::json::parse(response).at("Sims").size()==11);CHECK(RequestProject("content.knife","{}",response));CHECK(nlohmann::json::parse(response).at("Sources").size()==39);
}
void TestProjectNpcCombat() {
    using namespace Scripting;
    struct Context {bool Friend=false;int Rays=0;} context;
    NpcCombatFrame f;f.Context=reinterpret_cast<std::uintptr_t>(&context);
    f.Services=reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t ptr,NpcServiceFrame* r)->int {
        auto& c=*reinterpret_cast<Context*>(ptr);if(r->Operation==2){++c.Rays;return c.Friend;}r->Value=r->A.x;return 1;
    });
    f.Now=10;f.Dt=.1f;f.MeleeAt=-100;f.MeleeLanded=1;f.MeleeTime=.55f;f.MeleeHitTime=.22f;f.MeleeDamage=25;f.Difficulty=1;f.Known=1;
    f.Player.Valid=1;f.Player.Height=1.8f;f.Player.Feet={0,0,1};f.Operation=2;
    CHECK(InvokeProject("npc.combat",&f,sizeof f));CHECK(f.CallMelee && f.Melees==1 && !f.MeleeLanded && f.MeleeAt==10 && f.Damage==0);
    f.Now=10.1f;f.CallMelee=0;CHECK(InvokeProject("npc.combat",&f,sizeof f));CHECK(f.Damage==0 && !f.MeleeLanded);
    f.Now=10.23f;CHECK(InvokeProject("npc.combat",&f,sizeof f));CHECK(f.Damage==25 && f.MeleeHits==1 && f.MeleeLanded);
    f={};f.Context=reinterpret_cast<std::uintptr_t>(&context);f.Services=reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t ptr,NpcServiceFrame* r)->int {auto& c=*reinterpret_cast<Context*>(ptr);if(r->Operation==2){++c.Rays;return c.Friend;}r->Value=r->A.x;return 1;});
    f.Operation=1;f.Now=20;f.Dt=.1f;f.MeleeAt=-100;f.MeleeTime=.55f;f.Ammo=30;f.AmmoSeen=30;f.Equipped=1;f.HasAttackToken=1;f.Visible=1;f.Intent.Fire=1;f.Intent.AimPoint={0,0,20};f.Intent.Aim=1;f.Player.Valid=1;f.AllowFullAuto=1;
    CHECK(InvokeProject("npc.combat",&f,sizeof f));CHECK(f.Pressed && f.Held && f.BurstLeft==3 && f.ToggleAuto && context.Rays==1);
    f.Ammo=29;CHECK(InvokeProject("npc.combat",&f,sizeof f));CHECK(f.ShotsFired==1 && f.BurstLeft==2 && f.GunNoise && !f.FirstShot);
    context.Friend=true;CHECK(InvokeProject("npc.combat",&f,sizeof f));CHECK(!f.Held && f.BurstLeft==0 && f.BlockedTime==.3f && f.BurstPause==.35f);
    context.Friend=false;f.BurstPause=0;f.ReactionLeft=1;CHECK(InvokeProject("npc.combat",&f,sizeof f));CHECK(!f.Pressed && !f.Held);
    f.ReactionLeft=0;f.Intent.Pace=3;f.Intent.Move=1;f.Velocity={0,0,4};CHECK(InvokeProject("npc.combat",&f,sizeof f));CHECK(f.Sprinting && !f.Held);
    f.Shotgun=1;f.Reloading=1;f.Known=1;f.Player.Feet={0,0,1};CHECK(InvokeProject("npc.combat",&f,sizeof f));CHECK(f.Pressed && !f.Held);
    f.Reloading=0;f.Ammo=0;f.Intent.Pace=0;f.Intent.Move=0;CHECK(InvokeProject("npc.combat",&f,sizeof f));CHECK(f.Reload && !f.Held);
}
void TestProjectLocomotionAndCallouts() {
    using namespace Scripting;
    BodyMotionFrame body;body.Dt=.01f;body.Grounded=1;body.IsLocomotion=1;body.TurnThreshold=55;body.TurnLagMargin=60;body.TurnLagFloor=90;body.TurnMinTime=.1f;body.TurnTimeout=2;body.TurnEndAngle=20;body.ViewYaw=glm::radians(100.0f);
    CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK(body.Turning && body.SetTurnAngle && std::abs(body.TurnAngle-100)<.01f);
    body.IsTurn=1;body.IsLocomotion=0;body.Dt=.11f;body.RootYaw=60;body.StateTime=.4f;CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK(body.Turning && std::abs(body.Yaw-glm::radians(60.0f))<.001f);
    body.RootYaw=30;CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK(!body.Turning && std::abs(body.TurnDone-glm::radians(90.0f))<.001f);
    body={};body.Dt=.01f;body.Grounded=1;body.IsLocomotion=1;body.StartStopClips=1;body.IdleTime=2;body.StartIdleTime=.5f;body.StartMaxMove=.7f;body.ParamSmoothing=.2f;body.PlayerRunSpeed=4;body.PlayerSprintSpeed=6;body.RunSpeed=3.264f;body.ClipSprint=4.736f;body.WishVelocity={0,0,4};body.MaxPlayRate=2;
    CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK((body.Triggers&4) && body.StartDir.y==1 && body.Moving);
    CHECK(body.StartTurn==0 && body.StartGait==1);
    // Moving off toward a view the body lags by ~90 degrees: a start that turns it, to the left, ~2 steps of 45.
    body.Triggers=0;body.IdleTime=2;body.Move={};body.TurnThreshold=30;body.TurnLagFloor=90;body.TurnMoveEase=.08f;body.ViewYaw=glm::radians(100.0f);
    body.WishVelocity={4*std::sin(glm::radians(100.0f)),0,4*std::cos(glm::radians(100.0f))};
    CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK((body.Triggers&4) && body.StartTurn>1.8f && body.StartTurn<2.3f && body.StartTurnAmount==body.StartTurn);
    // Jogging forward, the input reverses: a pivot from the forward travel (the jog's).
    body={};body.Dt=.01f;body.Grounded=1;body.IsLocomotion=1;body.StartStopClips=1;body.PlayerRunSpeed=4;body.RunSpeed=3.264f;body.ParamSmoothing=.2f;body.Move={0,3,0};body.WishVelocity={0,0,-4};
    CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK((body.Triggers&64) && body.PivotDir.y>.99f && body.PivotGait==1);
    // In the pivot, still going the old way at 2 m/s toward 3 the other way (accel 0.1 s): 4.7 cm to the turnaround.
    body.Triggers=0;body.IsPivot=1;body.IsLocomotion=0;body.Velocity={0,0,2};body.WishVelocity={0,0,-3};body.AccelTime=.1f;
    CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK(std::abs(body.PivotDistance+(.2f-.3f*std::log(1+2.0f/3)))<1e-4f && !body.PivotReversed);
    body.Velocity={0,0,-1};CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK(body.PivotReversed && std::abs(body.PivotDistance-.01f)<1e-4f);
    // A tap: let go 0.2 s into a start - one step its way.
    body={};body.Dt=.06f;body.Grounded=1;body.IsStart=1;body.StartStopClips=1;body.StopDebounce=.05f;body.MoveTime=.2f;body.MoveDistance=.3f;body.LastDir={1,0,0};
    CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK((body.Triggers&128) && !(body.Triggers&8) && body.StepDir.x==1);
    // Stood still long enough with a gun: one of the ready stance's fidgets; busy with the gun, none.
    body={};body.Dt=.01f;body.Grounded=1;body.IsLocomotion=1;body.Armed=1;body.Random=.5f;body.FidgetNext=1;body.FidgetTime=.995f;
    CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK((body.Triggers&256) && body.FidgetIndex>=0 && body.FidgetIndex<=1 && body.FidgetTime==0);
    body.Triggers=0;body.Busy=1;body.FidgetNext=1;body.FidgetTime=.995f;CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK(!(body.Triggers&256) && body.FidgetTime==0);
    body={};body.Grounded=1;body.IsLocomotion=1;body.StartStopClips=1;body.Crouched=1;CHECK(InvokeProject("body.motion",&body,sizeof body));CHECK(body.Triggers==1 && body.WasCrouched);
    NpcCallMemberFrame members[2];NpcCallChannelFrame channel;std::fill_n(channel.LastEvent,20,-1e9f);
    for(int i=0;i<2;++i){members[i].Exists=1;members[i].Index=i;members[i].Feet={float(i),0,0};members[i].LastCallout=-1e9f;}
    NpcCalloutsFrame radio;radio.Now=10;radio.Caller=0;radio.Kind=0;radio.MemberCount=2;radio.ChannelCount=1;radio.Members=reinterpret_cast<std::uintptr_t>(members);radio.Channels=reinterpret_cast<std::uintptr_t>(&channel);radio.Random=0xBA4C;
    CHECK(InvokeProject("npc.callouts",&radio,sizeof radio));CHECK(channel.LastEvent[0]==10 && channel.OnAirPriority==7 && members[0].LastCallout==10 && members[1].GlanceUntil>10);
    radio.Now=11;radio.Caller=1;radio.Kind=11;CHECK(InvokeProject("npc.callouts",&radio,sizeof radio));CHECK(channel.LastEvent[11]<0 && members[1].LastCallout<0);
    radio.Kind=9;CHECK(InvokeProject("npc.callouts",&radio,sizeof radio));CHECK(channel.LastEvent[9]==11 && channel.OnAirPriority==9);
    members[0].Dummy=1;radio.Now=20;radio.Caller=0;CHECK(InvokeProject("npc.callouts",&radio,sizeof radio));CHECK(channel.LastEvent[9]==11);
}
void TestProjectHealthAndHud() {
    using namespace Scripting;using Json=nlohmann::json;
    Json legacy={{"Health",{{"Max",250},{"Invulnerable",true}}}};
    CHECK(MigrateLegacyProjectComponents(legacy));CHECK(!MigrateLegacyProjectComponents(legacy));
    CHECK(legacy["C# Script"]["Class"]=="Tartarus.Gameplay.Health");
    std::string output;CHECK(RequestProject("health.create",R"({"entity":4242,"fields":{"Max":250,"Invulnerable":false}})",output));
    NpcDamageFrame damage;damage.UseHealth=1;damage.Entity=4242;damage.Amount=30;damage.Zone=1;damage.Attacker=1;
    CHECK(InvokeProject("npc.damage",&damage,sizeof damage));CHECK(damage.Health==220 && damage.MaxHealth==250 && !damage.Ignore);
    // Projection inputs cannot replace authoritative health.
    damage.Health=999;damage.Amount=20;CHECK(InvokeProject("npc.damage",&damage,sizeof damage));CHECK(damage.Health==200);
    CombatHudFrame hud;hud.Operation=0;CHECK(InvokeProject("hud",&hud,sizeof hud));
    hud.Operation=1;hud.Now=10;hud.Unit=2;hud.StreakWindow=4;CHECK(InvokeProject("hud",&hud,sizeof hud));CHECK(hud.Kills==1 && hud.Rows==1);
    hud.Now=11;CHECK(InvokeProject("hud",&hud,sizeof hud));CHECK(hud.Kills==2 && hud.Rows==2);
    CHECK(Invoke(5,nullptr,0));
    damage.Operation=2;CHECK(InvokeProject("npc.damage",&damage,sizeof damage));CHECK(damage.Health==200 && damage.MaxHealth==250);
    hud.Operation=3;hud.Streak=1;hud.Now=4;CHECK(InvokeProject("hud",&hud,sizeof hud));CHECK(hud.Kills==2 && hud.Rows==2 && hud.Result==2);
    CHECK(RequestProject("health.create",R"({"entity":4242,"fields":{"Max":250,"Invulnerable":true}})",output));
    damage.Operation=0;damage.Amount=50;damage.Ignore=0;CHECK(InvokeProject("npc.damage",&damage,sizeof damage));CHECK(damage.Ignore && damage.Health==250);
    CHECK(RequestProject("health.remove",R"({"entity":4242})",output));damage.Ignore=0;damage.Operation=2;CHECK(InvokeProject("npc.damage",&damage,sizeof damage));CHECK(damage.Ignore);
    // Canvas handles work only in their drawing scope; nested scopes restore their parent.
    NativeRequest query;CHECK(!RuntimeCanvasService(query));std::uint32_t oldToken=0;
    HudText outer;outer.Begin(640,480);
    {ScopedRuntimeCanvas scope(outer);CHECK(RuntimeCanvasService(query));CHECK(query.A.x==640 && query.A.y==480);oldToken=query.Entity;
        NativeRequest measure;measure.Script=1;measure.Entity=oldToken;measure.Text="abc";measure.Value=20;CHECK(RuntimeCanvasService(measure));CHECK(std::abs(measure.Value-27)<.001f);
        HudText inner;inner.Begin(1280,720);
        {ScopedRuntimeCanvas nested(inner);CHECK(!RuntimeCanvasService(measure));NativeRequest current;CHECK(RuntimeCanvasService(current));CHECK(current.Entity!=oldToken);}
        measure.Script=2;measure.Value=20;CHECK(RuntimeCanvasService(measure));CHECK(std::abs(measure.Value-17.25f)<.001f);
    }
    query.Entity=oldToken;query.Script=2;CHECK(!RuntimeCanvasService(query));
    hud.Operation=0;CHECK(InvokeProject("hud",&hud,sizeof hud));
}
void TestProjectNpcPerceptionAndSquads() {
    using namespace Scripting;
    struct Services {int Rays=0,Randoms=0;} services;
    NpcPerceptionFrame f;f.Context=reinterpret_cast<std::uintptr_t>(&services);
    f.Services=reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t context,NpcServiceFrame* r)->int {
        auto& s=*reinterpret_cast<Services*>(context);if(r->Operation==0){++s.Randoms;r->Value=.5f;return 1;}if(r->Operation==1){++s.Rays;return 1;}return 0;
    });
    f.Now=1;f.Dt=.1f;f.Eye={0,1.7f,0};f.PinnedSince=-1;f.AimYaw=f.LookYaw=-90;f.Skill=.5f;f.Difficulty=1;
    f.Memory.LastSeen=f.Memory.LastHeard=-1e9f;f.Player.Valid=1;f.Player.Height=1.8f;f.Player.Eye={0,1.7f,-8};f.Player.Feet={0,0,-8};
    CHECK(InvokeProject("npc.perception",&f,sizeof f));CHECK(f.VisiblePoints==5 && services.Rays==5 && f.Memory.Visible && f.Memory.Awareness>0);
    f.Now+=.12f;CHECK(InvokeProject("npc.perception",&f,sizeof f));CHECK(f.Memory.Known && f.FirstShot && (f.Calls&4));
    const int rayCount=services.Rays;f.NextLook=f.Now+1;f.Suppression=.8f;CHECK(InvokeProject("npc.perception",&f,sizeof f));CHECK(services.Rays==rayCount && f.PinnedSince==f.Now);
    NpcSquadMemberFrame members[4];int ids[4]={0,1,2,3};
    for(int i=0;i<4;++i){members[i].Exists=1;members[i].Health=members[i].MaxHealth=100;members[i].Memory.Known=members[i].Memory.Visible=1;members[i].Memory.LastSeen=0;members[i].Fire=1;members[i].Feet={0,0,float(i+1)};members[i].Cover=i;members[i].Doing=4;members[i].Skill=.5f;}
    members[3].Shotgun=1;NpcSquadFrame squad;squad.Members=reinterpret_cast<std::uintptr_t>(members);squad.MemberIds=reinterpret_cast<std::uintptr_t>(ids);squad.TotalCount=squad.MemberCount=4;
    squad.Now=30;squad.Difficulty=1;squad.FlankHolder=squad.PincerHolder=squad.PushHolder=squad.CoverRequest=squad.CoverFirer=-1;squad.Player.Valid=1;squad.Player.Health=1;
    CHECK(InvokeProject("npc.squads",&squad,sizeof squad));CHECK(members[0].Role==0 && members[3].Role==2 && squad.FlankHolder==3 && members[3].HasFlank);CHECK(squad.AttackerCount==3 && members[0].HasAttack && members[2].HasAttack && !members[3].HasAttack);
    for(auto& m:members){m.Memory.Visible=0;m.Fire=0;m.TriggerHeld=0;}
    squad.Now=30.3f;squad.CoverRequest=3;squad.CoverRequestAt=squad.Now;squad.CoverFireUntil=-1e9f;
    CHECK(InvokeProject("npc.squads",&squad,sizeof squad));CHECK(squad.CoverFirer==1 && members[1].CoverFireOrder==32.8f && members[1].CallCovering && squad.CoverOrders==1);CHECK(squad.AttackerCount==0);
}
void TestProjectNpcConfiguration() {
    using namespace Scripting;using Json=nlohmann::json;
    Json legacy={{"NPC Spawn",{{"Weapon","Remington 870"},{"Brain","Training Dummy"},{"Skill",.7f},{"Squad",2}}},
        {"Squad Settings",{{"Squad Size",6},{"NPC Damage Scale",.8f},{"Cover Sample Spacing",1.2f},{"Low Cover Height",.75f}}},
        {"C# Script",{{"Class","Tartarus.Gameplay.Bob"},{"Enabled",false},{"Fields JSON","{}"},{"Scripts","[]"},{"Next Script ID",7}}}};
    CHECK(MigrateLegacyProjectComponents(legacy));CHECK(!MigrateLegacyProjectComponents(legacy));
    const auto slots=Json::parse(legacy["C# Script"]["Scripts"].get<std::string>());
    CHECK(slots.size()==2 && slots[0]["id"]==7 && slots[1]["id"]==8);
    CHECK(slots[0]["fields"]["Weapon"]==1 && slots[0]["fields"]["Brain"]==1);
    CHECK(slots[1]["fields"]["NpcDamageScale"]==.8f && slots[1]["fields"]["CoverSpacing"]==1.2f);
    World world;AssetLibrary assets;const auto entity=world.CreateEmptyEntity({},{},{1,1,1},"NPC config");
    auto& script=world.Registry.emplace<CSharpScriptComponent>(entity);
    script.ClassName="Tartarus.Gameplay.Bob";script.Enabled=false;script.Scripts=slots.dump();script.NextScriptId=9;
    SyncNpcDefinitions(world);
    CHECK((world.Registry.all_of<NpcSpawnComponent,SquadSettingsComponent>(entity)));
    CHECK(world.Registry.get<NpcSpawnComponent>(entity).Weapon==1 && world.Registry.get<NpcSpawnComponent>(entity).Brain==1);
    CHECK(world.Registry.get<NpcSpawnComponent>(entity).Skill==.7f);
    CHECK(world.Registry.get<SquadSettingsComponent>(entity).SquadSize==6 && world.Registry.get<SquadSettingsComponent>(entity).CoverKneeHeight==.75f);
    const auto snapshot=SceneSerializer::SaveToString(world);
    CHECK(snapshot.find("NPC Spawn")==std::string::npos && snapshot.find("Squad Settings")==std::string::npos);
    World restored;CHECK(SceneSerializer::LoadFromString(restored,assets,snapshot));
    const auto views=restored.Registry.view<NpcSpawnComponent,SquadSettingsComponent>();CHECK(views.begin()!=views.end());
    if(views.begin()!=views.end()) {
        CHECK(views.get<NpcSpawnComponent>(views.front()).Squad==2);
        CHECK(views.get<SquadSettingsComponent>(views.front()).NpcDamageScale==.8f);
    }
    world.Registry.remove<CSharpScriptComponent>(entity);SyncNpcDefinitions(world);
    CHECK((!world.Registry.any_of<NpcSpawnComponent,SquadSettingsComponent>(entity)));
    std::string resolved;CHECK(RequestProject("npc.config",R"({"class":"Tartarus.Gameplay.NpcDefinition","fields":{"Weapon":99,"Brain":-1,"Skill":5}})",resolved));
    auto normalized=Json::parse(resolved);CHECK(normalized["Weapon"]==2 && normalized["Brain"]==0 && normalized["Skill"]==1);
    auto defaults=DefaultSquadDefinition();CHECK(defaults.SquadSize==4 && defaults.Difficulty==1 && defaults.NpcDamageScale==.45f);
    CHECK(defaults.BodyPrefab=="assets/AI/Soldier.json" && !defaults.RiflePrefab.empty());
}
void TestProjectNpcBehaviour() {
    using namespace Scripting;
    struct Fixture {
        NpcCoverFrame Cover;
        int Index=0,Claims=0,Releases=0,Radio=0;
        bool Navigation=true,Shield=true;
    } fixture;
    fixture.Cover.Pos={0,0,6};fixture.Cover.Normal={0,0,1};fixture.Cover.ClaimedBy=-1;fixture.Cover.LastUsed=-1e9f;
    NpcBrainFrame f;f.Context=reinterpret_cast<std::uintptr_t>(&fixture);
    f.Services=reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t ptr,NpcServiceFrame* r)->int {
        auto& data=*reinterpret_cast<Fixture*>(ptr);
        switch(r->Operation) {
        case 0:r->Value=.5f;return 1;
        case 1:case 2:r->C=r->A;return data.Navigation;
        case 3:case 15:return data.Navigation;
        case 4:return data.Shield && r->Value<1.0f;
        case 5:r->Value=6;return 1;
        case 6:r->Cover=data.Cover;return 1;
        case 7:r->Data=reinterpret_cast<std::uintptr_t>(&data.Index);r->Count=1;return 1;
        case 8:++data.Claims;return 1;
        case 9:++data.Releases;return 1;
        case 12:++data.Radio;return 1;
        case 13:return 1;
        default:return 0;
        }
    });
    f.Now=10;f.Threat={0,0,20};f.Known=f.Visible=1;f.Index=3;f.Cover=-1;f.CoverCount=1;f.SquadCount=1;
    f.MemoryLastSeen=f.LastOwnSight=10;f.Ammo01=1;f.Skill=.5f;f.PlayerHeight=1.8f;
    f.CowerUntil=f.GlanceUntil=f.LastHurt=f.LastBlindFire=-1e9f;f.PinnedSince=-1;f.BoundWaitFrom=-1;
    auto invoke=[&](int operation,int target=0) {f.Operation=operation;f.TargetBehaviour=target;CHECK(InvokeProject("npc.behaviour",&f,sizeof f));};
    invoke(2);CHECK(f.Result==0 && f.CoverGood && f.SearchCoverCalled && fixture.Claims==1);
    f.Cover=-1;fixture.Shield=false;invoke(2);CHECK(f.Result==-1 && fixture.Claims==1);
    fixture.Shield=true;invoke(1,3);CHECK(f.Doing==3 && f.Cover==0 && f.Phase==0 && f.DoingSince==10);
    f.BoundWaitFrom=10;f.SquadCoverFireUntil=0;invoke(0);CHECK(!f.Intent.Move && f.Intent.Crouch && f.Intent.Fire && f.SquadCoverRequest==3);
    f.SquadCoverFireUntil=11;invoke(0);CHECK(f.Intent.Move && f.BoundWaitFrom<0 && f.CoveredBounds==1);
    f.Feet=f.Cover>=0?fixture.Cover.Pos:Vec3{};invoke(0);CHECK(f.Doing==4 && f.Phase==0 && f.PhaseUntil>10);
    f.Ammo01=.4f;invoke(0);CHECK(f.Intent.Reload && f.Intent.Crouch && !f.Intent.Fire);
    f.Ammo01=1;f.Suppression=.65f;f.PinnedSince=8;invoke(0);CHECK(f.Phase==3 && f.LastBlindFire==10 && f.BlindFires==1);
    invoke(0);CHECK(f.Intent.BlindFire && f.Intent.Suppress && f.Intent.Fire);
    f.CowerUntil=11;invoke(0);CHECK(f.Intent.Cower==1 && !f.Intent.Fire);
    f.CowerUntil=-1;f.Doing=2;f.Phase=0;f.Feet={0,0,0};f.Suppression=0;fixture.Navigation=false;invoke(0);CHECK(!f.Intent.Move);
    fixture.Navigation=true;f.Doing=5;f.SquadFlankHolder=3;f.HasFlankToken=1;invoke(1,0);CHECK(!f.HasFlankToken && f.SquadFlankHolder==-1 && f.SquadFlankDoneAt==10);
    f.Doing=7;f.Phase=1;f.SearchStep=4;f.PhaseUntil=9;invoke(0);CHECK(!f.Known && f.MemoryAwareness==.4f && f.Phase==-1);
}
void TestProjectNpcDecisions() {
    using namespace Scripting;
    AiChoiceFrame f;f.Operation=1;f.Now=20;f.LastSeen=20;f.LastHeard=-1e9f;f.Health=f.MaxHealth=100;f.Cover=-1;f.Phase=-1;f.LastHurt=-1e9f;f.LimpUntil=-1e9f;f.Known=f.Visible=1;f.Distance=20;f.LastOwnSight=20;
    auto choose=[&](){CHECK(InvokeProject("ai.choose",&f,sizeof f));};
    choose();CHECK(f.Best==3 && f.Change && f.NextThink>20); // take cover
    f.Health=20;f.LastHurt=20;choose();CHECK(f.Best==8); // hurt: retreat
    f.CoverSearchBlocked=1;choose();CHECK(f.Delay && !f.Change && f.NextThink==20); // one cover search per frame
    f.Wounded=1;choose();CHECK(f.Best==10 && f.Change); // wounded soldiers crawl instead of choosing an attack
    f.Wounded=0;f.CoverSearchBlocked=0;f.Health=100;f.LastHurt=-1e9f;f.HasFlank=1;f.PlayerStill=12;f.LimpUntil=25;choose();CHECK(f.Scores[5]==0 && f.Scores[6]==0); // limping prevents flank/push
    NpcDamageFrame hit;hit.Now=10;hit.Amount=60;hit.Health=hit.MaxHealth=100;hit.Attacker=-1;hit.Zone=0;hit.Part=-1;
    auto damage=[&](){CHECK(InvokeProject("npc.damage",&hit,sizeof hit));};
    damage();CHECK(hit.Kill && hit.Health==40 && hit.Known && hit.Hits==1); // the player's headshot executes
    hit={};hit.Now=10;hit.Amount=20;hit.Health=hit.MaxHealth=100;hit.Attacker=-1;hit.Zone=2;hit.Part=-1;hit.LimpTime=4;hit.HeavyHitDamage=15;hit.StaggerTime=.5f;hit.Direction={0,0,-1};
    damage();CHECK(!hit.Kill && hit.Health==80 && hit.LimpUntil==14 && hit.StaggerUntil==10.5f && hit.Flinch);
    CHECK(hit.PushVelocity.z<0 && hit.ReactionLeft>=.5f);
    hit={};hit.Now=10;hit.Amount=15;hit.Health=30;hit.MaxHealth=100;hit.Attacker=-1;hit.Zone=1;hit.Part=-1;hit.WoundChance=.35f;
    damage();CHECK(hit.NeedRandom && hit.WoundRolled && !hit.Kill);
    hit.Operation=1;hit.Random=.2f;damage();CHECK(hit.BecomeWounded && !hit.CallWounded);
    hit={};hit.Dead=1;hit.Amount=15;damage();CHECK(hit.Ignore && hit.Hits==0);
}
void TestProjectScoringAndConstraints() {
    using namespace Scripting;using Json=nlohmann::json;
    Json legacy={{"Goal Trigger",{{"Tag","Basketball"},{"Team","Away"},{"Points",7},{"Score Sound","goal.wav"},{"Score SoundGuid","123456789abcdef0"}}},
        {"C# Script",{{"Class","Tartarus.Gameplay.Bob"},{"Fields JSON","{}"},{"Scripts","[]"},{"Next Script ID",8}}}};
    CHECK(MigrateLegacyProjectComponents(legacy));CHECK(!MigrateLegacyProjectComponents(legacy));
    CHECK(legacy.at("C# Script").at("Class")=="Tartarus.Gameplay.Bob");
    auto extra=Json::parse(legacy.at("C# Script").at("Scripts").get<std::string>());
    CHECK(extra.size()==1 && extra[0].at("id")==8 && extra[0].at("fields").at("Team")==1);
    CHECK(extra[0].at("fields").at("ScoreSound").at("pathGuid")=="123456789abcdef0");
    World world;AssetLibrary assets;
    auto board=world.CreateEmptyEntity({0,0,0},{0,0,0},{1,1,1},"Score");
    Attach(world.Registry.emplace<CSharpScriptComponent>(board),"","Tartarus.Gameplay.Scoreboard");
    auto goal=world.CreateEmptyEntity({0,0,0},{0,0,0},{1,1,1},"Goal");
    auto& trigger=world.Registry.emplace<ColliderComponent>(goal);trigger.IsTrigger=true;trigger.HalfExtents={1,1,1};
    auto& rules=world.Registry.emplace<CSharpScriptComponent>(goal);Attach(rules,"","Tartarus.Gameplay.BasketGoal");rules.Fields="{\"Tag\":\"Basketball\",\"RequireDownward\":false}";
    auto ball=world.CreateEmptyEntity({0,0,0},{0,0,0},{1,1,1},"Ball");
    world.Registry.emplace<TagComponent>(ball,"Basketball");
    auto& shape=world.Registry.emplace<ColliderComponent>(ball);shape.Kind=ColliderComponent::Shape::Sphere;shape.HalfExtents={.12f,0,0};
    auto& body=world.Registry.emplace<RigidbodyComponent>(ball);body.UseGravity=false;body.LinearDamping=0;
    world.SyncActiveInHierarchy();world.RebuildWorldTransformCache();PhysicsWorld::Create(world);
    auto score=[]() {std::string state;CHECK(RequestProject("basketball.state","{}",state));return Json::parse(state).value("home",-1);};
    Tick(world,assets,.02f);CHECK(score()==0);
    PhysicsWorld::Step(.02f,world);Tick(world,assets,.02f);CHECK(score()==2);
    Tick(world,assets,.02f);CHECK(score()==2); // a rendered frame cannot deliver the same physics batch twice
    CHECK(Invoke(5,nullptr,0));Tick(world,assets,.02f);CHECK(score()==2); // managed reload preserves score and rebinds scene services
    const float rotation[4]={0,0,0,1},outside[3]={4,0,0},inside[3]={0,0,0};
    PhysicsWorld::SetActorPose(entt::to_integral(ball),outside,rotation,true);PhysicsWorld::Step(.02f,world);Tick(world,assets,.02f);
    PhysicsWorld::SetActorPose(entt::to_integral(ball),inside,rotation,true);PhysicsWorld::Step(.02f,world);Tick(world,assets,.02f);CHECK(score()==2);
    for(int i=0;i<60;i++) {PhysicsWorld::Step(.02f,world);Tick(world,assets,.02f);}
    PhysicsWorld::SetActorPose(entt::to_integral(ball),outside,rotation,true);PhysicsWorld::Step(.02f,world);Tick(world,assets,.02f);
    PhysicsWorld::SetActorPose(entt::to_integral(ball),inside,rotation,true);PhysicsWorld::Step(.02f,world);Tick(world,assets,.02f);CHECK(score()==4);
    GravityFrame ability;ability.Operation=0;CHECK(InvokeProject("gravity",&ability,sizeof ability));
    ability.Operation=1;ability.Dt=.1f;ability.Eye={0,0,5};ability.Forward={0,0,-1};ability.CameraRight={1,0,0};
    ability.GrabRange=100;ability.AssistRange=30;ability.AssistConeDeg=7;ability.MinThrowSpeed=4;ability.MaxThrowSpeed=18;ability.ChargeTime=1;ability.Right=1;
    CHECK(InvokeProject("gravity",&ability,sizeof ability));CHECK(PhysicsWorld::GrabbedEntity()==entt::to_integral(ball));
    ability.Left=1;for(int i=0;i<10;i++)CHECK(InvokeProject("gravity",&ability,sizeof ability));
    CHECK(ability.Charging && ability.Charge>.99f && ability.PredictSpeed==18);
    ability.Left=0;CHECK(InvokeProject("gravity",&ability,sizeof ability));CHECK(!PhysicsWorld::IsGrabbing() && !ability.Charging);
    PhysicsWorld::Step(.02f,world);
    BodyState state;CHECK(PhysicsWorld::GetBodyState(entt::to_integral(ball),state));CHECK(std::abs(state.Velocity[2]+18)<.01f);
    Stop(&world,&assets);CHECK(score()==-1);PhysicsWorld::Destroy();PhysicsWorld::Shutdown();
}
}
void RegisterScriptingTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("Project NPC lifecycle, session rules and content configuration",TestProjectLifecycleAndSession);
    tests.emplace_back("Project body actions and squad callout scheduling",TestProjectLocomotionAndCallouts);
    tests.emplace_back("Empty project host and generic bootstrap",TestEmptyProject);
    tests.emplace_back("Exported generic project host",TestExportedProjectHost);
    tests.emplace_back("Project NPC melee, burst fire and reload",TestProjectNpcCombat);
    tests.emplace_back("Project NPC behavior selection",TestProjectNpcDecisions);
    tests.emplace_back("Project NPC states, cover and navigation",TestProjectNpcBehaviour);
    tests.emplace_back("Project NPC configuration and scene migration",TestProjectNpcConfiguration);
    tests.emplace_back("Project health authority, HUD reload and canvas scopes",TestProjectHealthAndHud);
    tests.emplace_back("Project NPC perception, squad roles and covering fire",TestProjectNpcPerceptionAndSquads);
    tests.emplace_back("Project scoring, physics callbacks and carry constraint",TestProjectScoringAndConstraints);
    tests.emplace_back("Prefab-relative draggable script references",TestSceneReferences);
    tests.emplace_back("Managed gameplay ABI, fire modes and pump",TestManagedGameplay);
    tests.emplace_back("Managed components, lifecycle and weapon prefabs",TestManagedComponentsAndPrefabs);
    tests.emplace_back("C# WeaponDefinition defaults, migration and script slot preservation",TestCSharpWeaponDefinitionMigration);
    tests.emplace_back("Weapon prefab particle systems",TestWeaponPrefabParticles);
    tests.emplace_back("Weapon attachment selection, poses and world clone",TestWeaponAttachments);
    tests.emplace_back("Material shader selection and red-dot defaults",TestMaterialShaderSelection);
    tests.emplace_back("Managed ballistics, spread and capped pellet impulse",TestManagedBallistics);
    tests.emplace_back("Managed editor background build and reload",TestManagedEditorBuild);
    tests.emplace_back("Managed Unity workflow, multiple scripts and component access",TestManagedUnityWorkflow);
    tests.emplace_back("Managed scene API, native fields and C# editor tools",TestManagedApiAndEditor);
    tests.emplace_back("Managed vInspector attributes, buttons and read-outs",TestManagedInspectorAttributes);
}
