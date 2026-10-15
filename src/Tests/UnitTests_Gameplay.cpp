#include "UnitTestSupport.h"
#include "WeaponPrefab.h"
#include "FirstPersonAnimation.h"
#include "ProjectComponentMigration.h"
#include "GameFrames.h"
#include "../Game/WeaponRules.h"
#include "../Game/BodyLocomotion.h"
#include "WeaponAttachments.h"
#include "MaterialAsset.h"
#include "ShaderLibrary.h"
#include "ComponentRegistry.h"
#include "SceneReferences.h"
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
#include "GameRules.h"
#include <chrono>
#include <thread>
#include <set>
#include <cmath>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
void TestSceneReferences() {
    using namespace Gameplay;
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
void TestNativeWeaponRules() {
    using namespace Gameplay;
    WeaponFrame w; w.Equipped=1; w.Chambered=1; w.Ammo=30; w.Magazine=30; w.Rpm=700;
    w.AllowFullAuto=1; w.RecoilProfile=1; w.Tags=32;
    w.Operation=1; WeaponRules::Update(w); CHECK(w.Result==1 && (w.Commands&2));
    w.Operation=2; WeaponRules::Update(w); CHECK(w.Ammo==29);
    w.Operation=5; WeaponRules::Update(w); CHECK(w.Result==1 && (w.Commands&64));
    w.Operation=8; w.Events=1; WeaponRules::Update(w); CHECK(w.Ammo==30);
    w.Operation=6; WeaponRules::Update(w); CHECK(w.FullAuto==1);
    w.Operation=3; w.Held=1; w.Cooldown=.05f; WeaponRules::Update(w); CHECK(w.Result==0);
    w.Operation=7; w.Dt=.1f; WeaponRules::Update(w); CHECK(w.Cooldown==0);
    w.Operation=3; WeaponRules::Update(w); CHECK(w.Result==1);
    // The shotgun commits a shell, delays cycling, and chambers only after the pump state finishes.
    w.FullAuto=0; w.AllowFullAuto=0; w.PerRound=1; w.CycleAfterShot=1;
    w.CycleDelay=.12f; w.Magazine=6; w.Ammo=6; w.Operation=2;
    WeaponRules::Update(w); CHECK(w.Ammo==5 && w.Chambered==0);
    w.Operation=1; WeaponRules::Update(w); CHECK(w.Result==0);
    w.Operation=7; w.Dt=.2f; WeaponRules::Update(w); CHECK(w.Commands&16);
    w.Tags=8; WeaponRules::Update(w); CHECK(w.CycleSeen==1 && w.Chambered==0);
    w.Tags=32; WeaponRules::Update(w); CHECK(w.Chambered==1);
    w.Tags=2; w.Operation=1; WeaponRules::Update(w); CHECK(w.Result==0 && w.StopReload==1);
    w.Ammo=0; w.Chambered=0; w.Events=2; w.Operation=8;
    WeaponRules::Update(w); CHECK(w.Ammo==1 && w.Chambered==1);
    w.Operation=9; w.Held=1; w.Dt=.1f; w.HoldSeconds=.35f; WeaponRules::Update(w);
    w.Held=0; WeaponRules::Update(w); CHECK(w.Result==1);
    w.Held=1; w.Dt=.4f; WeaponRules::Update(w); CHECK(w.Commands==0);
    WeaponRules::Update(w); CHECK(w.Commands&128);
    w.Held=0; WeaponRules::Update(w); CHECK(w.Result==0);
    World world; Player player; player.Gravity=0; player.Cam.Yaw=-90; player.Cam.Position={0,2,0};
    player.ScriptedMove=true; player.ScriptMove={0,1}; player.MoveSpeed=6;
    player.Update(.1f,world,nullptr,false);
    CHECK(std::abs(player.Velocity.z+6)<.001f);
    CHECK(std::abs(player.Cam.Position.z+.6f)<.001f);
}
void TestWeaponPrefabsAndComponents() {
    World world; AssetLibrary assets;
    auto e=world.CreateEmptyEntity({0,0,0},{0,0,0},{1,1,1},"Weapon");
    auto& weapon=world.Registry.emplace<WeaponDefinitionComponent>(e);
    weapon.Description="870 pump shotgun";weapon.AnimationSet="assets/Weapons/Remington870/Remington870.fpsanim";
    world.Registry.emplace<FirstPersonControllerComponent>(e).PrimaryWeaponPrefab="assets/Weapons/AKS74U/AKS74U.prefab";
    auto& smoke=world.Registry.emplace<ParticleSystemComponent>(e);
    smoke.Emitting=false;smoke.ShaderMode=1;smoke.SmokeDensity=2.3f;smoke.SmokeNoiseScale=4.2f;
    smoke.SmokeTurbulence=.47f;smoke.SmokeSoftness=.72f;smoke.SmokeEvolution=.83f;
    const auto snapshot=SceneSerializer::SaveToString(world);
    CHECK(SceneSerializer::LoadFromString(world,assets,snapshot));
    auto view=world.Registry.view<WeaponDefinitionComponent,FirstPersonControllerComponent>();
    CHECK(view.begin()!=view.end());
    if(view.begin()==view.end())return;
    auto restored=*view.begin();
    const auto* restoredWeapon=world.Registry.try_get<WeaponDefinitionComponent>(restored);
    CHECK(restoredWeapon && restoredWeapon->Description=="870 pump shotgun" && restoredWeapon->AnimationSet=="assets/Weapons/Remington870/Remington870.fpsanim");
    CHECK(world.Registry.get<FirstPersonControllerComponent>(restored).PrimaryWeaponPrefab=="assets/Weapons/AKS74U/AKS74U.prefab");
    const auto& restoredSmoke=world.Registry.get<ParticleSystemComponent>(restored);
    CHECK(restoredSmoke.ShaderMode==1 && restoredSmoke.Texture.empty());
    CHECK(restoredSmoke.SmokeDensity==2.3f && restoredSmoke.SmokeNoiseScale==4.2f && restoredSmoke.SmokeTurbulence==.47f && restoredSmoke.SmokeSoftness==.72f && restoredSmoke.SmokeEvolution==.83f);
    std::string set,error;
    for(const char* name:{"AKS74U","Remington870"}) {
        const std::string stem=std::string("assets/Weapons/")+name+"/"+name;
        if(!std::filesystem::exists(ProjectPaths::Resolve(stem+".prefab"))) continue; // licensed weapon assets aren't in the distributable sample
        MuzzleEffectSettings muzzle;
        CHECK(Gameplay::ResolveWeaponPrefab(stem+".prefab",set,error,&muzzle)); CHECK(set==stem+".fpsanim");
        CHECK(muzzle.PrefabParticles);
        std::vector<entt::entity> emitters;
        const auto particleRoot = Gameplay::InstantiateWeaponParticles(world,assets,stem+".prefab",emitters,error,false);
        CHECK(world.Registry.valid(particleRoot) && !emitters.empty());
        Gameplay::WeaponAttachments attachments; attachments.Load(world,particleRoot);
        CHECK(attachments.Count(Gameplay::AttachmentKind::Muzzle)==1);
        CHECK(attachments.Count(Gameplay::AttachmentKind::Grip)==1);
        glm::mat4 attachmentPose(1);
        CHECK(attachments.Pose(world,particleRoot,Gameplay::AttachmentKind::Muzzle,attachmentPose));
        if(std::string(name)=="AKS74U") {
            // Iron sights only (the red dot came off): the selected optic is the reference one.
            CHECK(attachments.Count(Gameplay::AttachmentKind::Optic)==1);
            CHECK(attachments.Pose(world,particleRoot,Gameplay::AttachmentKind::Optic,attachmentPose));
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
void TestScriptDefinitionConversion() {
    using Json=nlohmann::json;
    using namespace Gameplay;
    bool registered=false;
    for(const auto& component:ComponentRegistry::All())registered|=std::string(component.Meta.Name)=="Weapon Definition";
    CHECK(registered);
    const std::string guid="123456789abcdef0";
    const Json weaponFields={{"Description","Authored description"},{"AnimationSet",{{"path","old.fpsanim"},{"pathGuid",guid}}},
        {"LightColor",{{"X",.2},{"Y",.4},{"Z",.6}}},{"LightIntensity",27},{"FlameGlow",99},{"Smoke",false},{"Magazine",6},{"Reload",1}};
    Json node={{"id",0},{"name","Legacy weapon"},
        {"C# Script",{{"Class","Tartarus.Gameplay.Bob"},{"Fields JSON","{\"Height\":0.75}"},
            {"Scripts",Json::array({
                {{"id",9},{"class","Tartarus.Gameplay.OpticAttachment"},{"fields",{{"AimPoint","Sight"},{"AimBlendTime",.3}}},{"enabled",true}},
                {{"id",10},{"class","Tartarus.Gameplay.GripAttachment"},{"fields",Json::object()},{"enabled",false}},
                {{"id",11},{"class","Tartarus.Gameplay.WeaponDefinition"},{"fields",weaponFields},{"enabled",true}},
                {{"id",12},{"class","Tartarus.Gameplay.Unrelated"},{"fields",Json::object()},{"enabled",true}}}).dump()},
            {"Next Script ID",13}}}};
    CHECK(ConvertScriptDefinitions(node));CHECK(!ConvertScriptDefinitions(node));
    // Ordinary scripts stay where they were; only the definitions leave the C# component.
    const auto& script=node.at("C# Script");
    CHECK(script.at("Class")=="Tartarus.Gameplay.Bob" && script.at("Fields JSON")=="{\"Height\":0.75}");
    const auto extra=Json::parse(script.at("Scripts").get<std::string>());
    CHECK(extra.size()==1 && extra[0].at("id")==12);
    const auto& weapon=node.at("Weapon Definition");
    CHECK(weapon.at("Description")=="Authored description" && weapon.at("Animation Set").at("pathGuid")==guid);
    CHECK(weapon.at("Light Color")==Json::array({.2,.4,.6}) && weapon.at("Light Intensity")==27 && weapon.at("Smoke")==false);
    CHECK(node.at("Optic Attachment").at("Aim Point")=="Sight" && node.at("Grip Attachment").at("Enabled")==false);
    // A definition that was the only script removes the C# component entirely.
    Json only={{"C# Script",{{"Class","Tartarus.Gameplay.SquadDefinition"},{"Fields JSON",R"({"SquadSize":6,"CoverSpacing":1.2})"},{"Enabled",true}}}};
    CHECK(ConvertScriptDefinitions(only));
    CHECK(!only.contains("C# Script") && only.at("Squad Settings").at("Squad Size")==6 && only.at("Squad Settings").at("Cover Sample Spacing")==1.2);
    // Older snapshots convert on read and save as native components.
    Json legacy=node;legacy.erase("Weapon Definition");legacy.erase("Optic Attachment");legacy.erase("Grip Attachment");
    legacy["C# Script"]["Scripts"]=Json::array({{{"id",11},{"class","Tartarus.Gameplay.WeaponDefinition"},{"fields",weaponFields},{"enabled",true}}}).dump();
    World world;AssetLibrary assets;
    CHECK(SceneSerializer::LoadFromString(world,assets,Json{{"formatVersion",4},{"empties",Json::array({legacy})}}.dump()));
    auto loaded=world.Registry.view<WeaponDefinitionComponent>();
    CHECK(loaded.begin()!=loaded.end());
    if(loaded.begin()!=loaded.end()) {
        const auto& w=loaded.get<WeaponDefinitionComponent>(loaded.front());
        CHECK(w.Description=="Authored description" && w.AnimationSet=="old.fpsanim" && w.Muzzle.LightIntensity==27 && !w.Muzzle.Smoke);
        CHECK(std::abs(w.Muzzle.LightColor.y-.4f)<1e-6f && w.Gameplay.Magazine==6 && w.Gameplay.Reload==FirstPersonWeaponGameplay::ReloadMode::PerRound);
    }
    const auto saved=Json::parse(SceneSerializer::SaveToString(world));
    CHECK(saved.at("empties").at(0).at("Weapon Definition").at("Description")=="Authored description");
    CHECK(!saved.at("empties").at(0).contains("C# Script")); // the leftover C# behaviour has no runtime any more
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
    CHECK(Gameplay::ResolveWeaponPrefab(path.u8string(),set,error,&muzzle));
    CHECK(muzzle.PrefabParticles && muzzle.LightIntensity==18 && set=="test.fpsanim");
    World world; AssetLibrary assets;
    const auto weapon = world.CreateEmptyEntity({2,3,4},{0,90,0},{1,1,1},"Live weapon");
    std::vector<entt::entity> emitters;
    const auto root = Gameplay::InstantiateWeaponParticles(world,assets,path.u8string(),emitters,error);
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
    using namespace Gameplay;
    World world; AssetLibrary assets;
    auto root=world.CreateEmptyEntity({2,3,4},{0,90,0},{1,1,1},"Weapon");
    auto child=[&](entt::entity parent,const char* name,glm::vec3 position=glm::vec3(0)) {
        auto e=world.CreateEmptyEntity(position,{0,0,0},{1,1,1},name);
        world.AttachChildRaw(e,parent); return e;
    };
    auto muzzleAttachment=[&](entt::entity owner,const char* sounds,const char* profile) {
        auto& m=world.Registry.emplace<MuzzleAttachmentComponent>(owner);
        m.MuzzleTransform="Flash"; m.MuzzleFlash="Flash"; m.FiringSounds=sounds; m.FiringSoundProfile=profile;
    };
    auto opticAttachment=[&](entt::entity owner,bool reference,float blend) -> OpticAttachmentComponent& {
        auto& o=world.Registry.emplace<OpticAttachmentComponent>(owner);
        o.AimPoint="Aim"; o.DefaultOptic=reference; o.AimBlendTime=blend; return o;
    };
    auto muzzle=child(root,"Standard muzzle"), otherMuzzle=child(root,"Suppressor");
    auto flash=child(muzzle,"Flash",{0,0,-.7f}), otherFlash=child(otherMuzzle,"Flash",{0,0,-.9f});
    world.Registry.emplace<ParticleSystemComponent>(flash);
    world.Registry.emplace<ParticleSystemComponent>(otherFlash);
    muzzleAttachment(muzzle,"","ak");
    muzzleAttachment(otherMuzzle,"quiet1.wav;quiet2.wav","");
    world.Registry.emplace<DeactivatedTag>(otherMuzzle);
    auto grip=child(root,"Grip"); world.Registry.emplace<GripAttachmentComponent>(grip);
    auto irons=child(root,"Irons"), optic=child(root,"Red dot");
    child(irons,"Aim",{0,.1f,0}); child(optic,"Aim",{0,.2f,-.1f});
    opticAttachment(irons,true,.2f);
    opticAttachment(optic,false,.3f);
    world.Registry.emplace<DeactivatedTag>(irons);
    // A switched-off attachment is not one of the choices.
    auto disabled=child(root,"Disabled optic"); opticAttachment(disabled,false,.2f).Enabled=false;
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

// vInspector (Phase 3b): the C# attributes come through Describe, and the edit-mode button /
// ShowInInspector calls run on a temporary instance holding the saved fields.
}
namespace {
void TestProjectWeaponLoadout(){
    using namespace Gameplay;
    int ammo[2]={3,7};WeaponLoadoutFrame loadout;loadout.Operation=3;loadout.Count=2;loadout.SlotAmmo=reinterpret_cast<std::uintptr_t>(ammo);loadout.Active=1;loadout.Magazine=30;loadout.CycleWait=2;WeaponRules::UpdateLoadout(loadout);CHECK(ammo[0]==-1 && ammo[1]==-1 && loadout.Ammo==30 && loadout.Chambered && loadout.CycleWait==0);
    loadout.Operation=0;loadout.Slot=0;loadout.Pending=-1;loadout.Value=1;loadout.Equipped=1;WeaponRules::UpdateLoadout(loadout);CHECK(loadout.Pending==1 && !loadout.Equipped && loadout.Commands==12);
    loadout.Operation=2;loadout.Requested=0;loadout.Burst=3;WeaponRules::UpdateLoadout(loadout);CHECK(loadout.Pending==-1 && loadout.Burst==0 && loadout.Commands==6);
}
void TestProjectLocomotion() {
    using namespace Gameplay;
    BodyMotionFrame body;body.Dt=.01f;body.Grounded=1;body.IsLocomotion=1;body.TurnThreshold=55;body.TurnLagMargin=60;body.TurnLagFloor=90;body.TurnMinTime=.1f;body.TurnTimeout=2;body.TurnEndAngle=20;body.ViewYaw=glm::radians(100.0f);
    BodyLocomotion::Update(body);CHECK(body.Turning && body.SetTurnAngle && std::abs(body.TurnAngle-100)<.01f);
    body.IsTurn=1;body.IsLocomotion=0;body.Dt=.11f;body.RootYaw=60;body.StateTime=.4f;BodyLocomotion::Update(body);CHECK(body.Turning && std::abs(body.Yaw-glm::radians(60.0f))<.001f);
    body.RootYaw=30;BodyLocomotion::Update(body);CHECK(!body.Turning && std::abs(body.TurnDone-glm::radians(90.0f))<.001f);
    body={};body.Dt=.01f;body.Grounded=1;body.IsLocomotion=1;body.StartStopClips=1;body.IdleTime=2;body.StartIdleTime=.5f;body.StartMaxMove=.7f;body.ParamSmoothing=.2f;body.PlayerRunSpeed=4;body.PlayerSprintSpeed=6;body.RunSpeed=3.264f;body.ClipSprint=4.736f;body.WishVelocity={0,0,4};body.MaxPlayRate=2;
    BodyLocomotion::Update(body);CHECK((body.Triggers&4) && body.StartDir.y==1 && body.Moving);
    CHECK(body.StartTurn==0 && body.StartGait==1);
    // Moving off toward a view the body lags by ~90 degrees: a start that turns it, to the left, ~2 steps of 45.
    body.Triggers=0;body.IdleTime=2;body.Move={};body.TurnThreshold=30;body.TurnLagFloor=90;body.TurnMoveEase=.08f;body.ViewYaw=glm::radians(100.0f);
    body.WishVelocity={4*std::sin(glm::radians(100.0f)),0,4*std::cos(glm::radians(100.0f))};
    BodyLocomotion::Update(body);CHECK((body.Triggers&4) && body.StartTurn>1.8f && body.StartTurn<2.3f && body.StartTurnAmount==body.StartTurn);
    // Jogging forward, the input reverses: a pivot from the forward travel (the jog's).
    body={};body.Dt=.01f;body.Grounded=1;body.IsLocomotion=1;body.StartStopClips=1;body.PlayerRunSpeed=4;body.RunSpeed=3.264f;body.ParamSmoothing=.2f;body.Move={0,3,0};body.WishVelocity={0,0,-4};
    BodyLocomotion::Update(body);CHECK((body.Triggers&64) && body.PivotDir.y>.99f && body.PivotGait==1);
    // In the pivot, still going the old way at 2 m/s toward 3 the other way (accel 0.1 s): 4.7 cm to the turnaround.
    body.Triggers=0;body.IsPivot=1;body.IsLocomotion=0;body.Velocity={0,0,2};body.WishVelocity={0,0,-3};body.AccelTime=.1f;
    BodyLocomotion::Update(body);CHECK(std::abs(body.PivotDistance+(.2f-.3f*std::log(1+2.0f/3)))<1e-4f && !body.PivotReversed);
    body.Velocity={0,0,-1};BodyLocomotion::Update(body);CHECK(body.PivotReversed && std::abs(body.PivotDistance-.01f)<1e-4f);
    // A tap: let go 0.2 s into a start - one step its way.
    body={};body.Dt=.06f;body.Grounded=1;body.IsStart=1;body.StartStopClips=1;body.StopDebounce=.05f;body.MoveTime=.2f;body.MoveDistance=.3f;body.LastDir={1,0,0};
    BodyLocomotion::Update(body);CHECK((body.Triggers&128) && !(body.Triggers&8) && body.StepDir.x==1);
    // Stood still long enough with a gun: one of the ready stance's fidgets; busy with the gun, none.
    body={};body.Dt=.01f;body.Grounded=1;body.IsLocomotion=1;body.Armed=1;body.Random=.5f;body.FidgetNext=1;body.FidgetTime=.995f;
    BodyLocomotion::Update(body);CHECK((body.Triggers&256) && body.FidgetIndex>=0 && body.FidgetIndex<=1 && body.FidgetTime==0);
    body.Triggers=0;body.Busy=1;body.FidgetNext=1;body.FidgetTime=.995f;BodyLocomotion::Update(body);CHECK(!(body.Triggers&256) && body.FidgetTime==0);
    body={};body.Grounded=1;body.IsLocomotion=1;body.StartStopClips=1;body.Crouched=1;BodyLocomotion::Update(body);CHECK(body.Triggers==1 && body.WasCrouched);
}
void TestProjectNpcConfiguration() {
    using namespace Gameplay;using Json=nlohmann::json;
    // Scenes saved while NPC Spawn and Squad Settings were C# definitions read back as the native components.
    Json legacy={{"id",0},{"name","NPC config"},
        {"C# Script",{{"Class","Tartarus.Gameplay.Bob"},{"Enabled",false},{"Fields JSON","{}"},{"Next Script ID",9},{"Scripts",Json::array({
            {{"id",7},{"class","Tartarus.Gameplay.NpcDefinition"},{"fields",{{"Weapon",1},{"Brain",1},{"Skill",.7},{"Squad",2}}},{"enabled",true}},
            {{"id",8},{"class","Tartarus.Gameplay.SquadDefinition"},{"fields",{{"SquadSize",6},{"NpcDamageScale",.8},{"CoverSpacing",1.2},{"CoverKneeHeight",.75}}},{"enabled",true}}}).dump()}}}};
    World world;AssetLibrary assets;
    CHECK(SceneSerializer::LoadFromString(world,assets,Json{{"formatVersion",4},{"empties",Json::array({legacy})}}.dump()));
    auto converted=world.Registry.view<NpcSpawnComponent,SquadSettingsComponent>();
    CHECK(converted.begin()!=converted.end());
    if(converted.begin()==converted.end())return;
    const auto entity=converted.front();
    const auto& spawn=world.Registry.get<NpcSpawnComponent>(entity);
    const auto& squad=world.Registry.get<SquadSettingsComponent>(entity);
    CHECK(spawn.Weapon==1 && spawn.Brain==1 && std::abs(spawn.Skill-.7f)<1e-6f && spawn.Squad==2);
    CHECK(squad.SquadSize==6 && std::abs(squad.NpcDamageScale-.8f)<1e-6f && std::abs(squad.CoverSpacing-1.2f)<1e-6f && std::abs(squad.CoverKneeHeight-.75f)<1e-6f);
    const auto snapshot=SceneSerializer::SaveToString(world);
    CHECK(snapshot.find("NpcDefinition")==std::string::npos && snapshot.find("SquadDefinition")==std::string::npos);
    World restored;CHECK(SceneSerializer::LoadFromString(restored,assets,snapshot));
    const auto views=restored.Registry.view<NpcSpawnComponent,SquadSettingsComponent>();CHECK(views.begin()!=views.end());
    if(views.begin()!=views.end()) {
        CHECK(views.get<NpcSpawnComponent>(views.front()).Squad==2);
        CHECK(std::abs(views.get<SquadSettingsComponent>(views.front()).NpcDamageScale-.8f)<1e-6f);
    }
    const SquadSettingsComponent defaults;
    CHECK(defaults.SquadSize==4 && defaults.Difficulty==1 && defaults.NpcDamageScale==.45f);
    CHECK(defaults.BodyPrefab=="assets/AI/Soldier.json" && !defaults.RiflePrefab.empty());
}
void TestLightFlickerMigration() {
    using namespace Gameplay;using Json=nlohmann::json;
    // The C# LightFlicker script became the native Light Flicker component.
    Json node={{"id",0},{"name","Tube"},{"C# Script",{{"Class","Tartarus.Gameplay.LightFlicker"},{"Source","assets/Scripts/LightFlicker.cs"},
        {"Fields JSON",R"({"Mode":2,"MinInterval":0.5,"Depth":0.25,"Seed":4})"},{"Enabled",true},{"Scripts","[]"}}}};
    Json converted=node;
    CHECK(ConvertScriptDefinitions(converted));CHECK(!converted.contains("C# Script"));
    const auto& block=converted.at("Light Flicker");
    CHECK(block.at("Mode")==2 && block.at("Min Interval")==0.5 && block.at("Depth")==0.25 && block.at("Seed")==4);
    World world;AssetLibrary assets;
    CHECK(SceneSerializer::LoadFromString(world,assets,Json{{"formatVersion",4},{"empties",Json::array({node})}}.dump()));
    auto flickers=world.Registry.view<LightFlickerComponent>();CHECK(flickers.size()==1);
    if(flickers.size()==1) {
        const auto& f=flickers.get<LightFlickerComponent>(flickers.front());
        CHECK(f.Mode==2 && f.MinInterval==.5f && f.MaxInterval==15.0f && f.Depth==.25f && f.Seed==4);
    }
    const auto saved=SceneSerializer::SaveToString(world);
    CHECK(saved.find("Light Flicker")!=std::string::npos && saved.find("C# Script")==std::string::npos);
}
void TestHealthMigration() {
    using namespace Gameplay;using Json=nlohmann::json;
    // A Health script slot from the C# era becomes the native Health component, next to the scripts that stay.
    Json node={{"C# Script",{{"Class","Tartarus.Gameplay.Bob"},{"Fields JSON","{}"},{"Next Script ID",9},
        {"Scripts",Json::array({{{"id",8},{"class","Tartarus.Gameplay.Health"},{"fields",{{"Max",150},{"Invulnerable",true}}},{"enabled",true}}}).dump()}}}};
    CHECK(ConvertScriptDefinitions(node));CHECK(!ConvertScriptDefinitions(node));
    CHECK(node.at("C# Script").at("Class")=="Tartarus.Gameplay.Bob" && Json::parse(node.at("C# Script").at("Scripts").get<std::string>()).empty());
    CHECK(node.at("Health").at("Max")==150 && node.at("Health").at("Invulnerable")==true);
    // The native block loads as the component again (Current starts unset: Play fills it).
    World world;AssetLibrary assets;
    CHECK(SceneSerializer::LoadFromString(world,assets,Json{{"formatVersion",4},{"empties",Json::array({{{"id",0},{"name","Crate"},{"Health",{{"Max",40},{"Invulnerable",false}}}}})}}.dump()));
    auto healthy=world.Registry.view<HealthComponent>();CHECK(healthy.size()==1);
    if(healthy.size()==1){const auto& h=healthy.get<HealthComponent>(healthy.front());CHECK(h.Max==40 && !h.Invulnerable && h.Current<0);}
}
}
// The host calls a loaded game module's weapon/body rules only when the module was built against
// the same frames, and falls back to its built-in copies otherwise.
void TestGameRulesDispatch() {
    const GameModuleAPI* previous = GameRules::Module();
    struct Probe { static void Weapon(Gameplay::WeaponFrame& w) { w.Result = 777; } };
    GameModuleAPI fake;
    fake.FramesLayout = Gameplay::FramesLayout();
    fake.UpdateWeapon = &Probe::Weapon;

    Gameplay::WeaponFrame w;
    GameRules::UseModule(nullptr);
    CHECK(!GameRules::FromModule());
    w.Operation = 7; GameRules::UpdateWeapon(w);
    CHECK(w.Result != 777);

    GameRules::UseModule(&fake);
    CHECK(GameRules::FromModule());
    GameRules::UpdateWeapon(w);
    CHECK(w.Result == 777);

    fake.FramesLayout ^= 1u; // built against other frames
    GameRules::UseModule(&fake);
    CHECK(!GameRules::FromModule());
    w.Result = 0; GameRules::UpdateWeapon(w);
    CHECK(w.Result != 777);

    GameRules::UseModule(previous);
}

void RegisterGameplayTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("Game module weapon/body rules dispatch",TestGameRulesDispatch);
    tests.emplace_back("Native weapon loadout rules",TestProjectWeaponLoadout);
    tests.emplace_back("Native body locomotion actions",TestProjectLocomotion);
    tests.emplace_back("Project NPC configuration and scene migration",TestProjectNpcConfiguration);
    tests.emplace_back("Health script slot becomes the native component",TestHealthMigration);
    tests.emplace_back("LightFlicker script becomes the native Light Flicker",TestLightFlickerMigration);
    tests.emplace_back("Prefab-relative draggable script references",TestSceneReferences);
    tests.emplace_back("Native weapon rules, fire modes and pump",TestNativeWeaponRules);
    tests.emplace_back("Weapon prefabs, attachments and component round trip",TestWeaponPrefabsAndComponents);
    tests.emplace_back("C# definition slots convert to native components",TestScriptDefinitionConversion);
    tests.emplace_back("Weapon prefab particle systems",TestWeaponPrefabParticles);
    tests.emplace_back("Weapon attachment selection, poses and world clone",TestWeaponAttachments);
    tests.emplace_back("Material shader selection and red-dot defaults",TestMaterialShaderSelection);
}
