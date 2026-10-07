#include "UnitTestSupport.h"
#include "../Game/Scripting/ScriptRuntime.h"
#include "../Game/Scripting/WeaponPrefab.h"
#include "../Game/Scripting/ScriptComponent.h"
#include "EnginePaths.h"
#include "ProjectPaths.h"
#include <filesystem>
#include <json.hpp>
#include "World.h"
#include "AssetLibrary.h"
#include "SceneSerializer.h"
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
void TestManagedGameplay() {
    using namespace Scripting;
    CHECK(EnsureLoaded());
    WeaponFrame w; w.Equipped=1; w.Chambered=1; w.Ammo=30; w.Magazine=30; w.Rpm=700;
    w.AllowFullAuto=1; w.RecoilProfile=1; w.Tags=32;
    w.Operation=1; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Result==1 && (w.Commands&2));
    w.Operation=2; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Ammo==29);
    w.Operation=5; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Result==1 && (w.Commands&64));
    w.Operation=8; w.Events=1; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Ammo==30);
    w.Operation=6; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.FullAuto==1);
    w.Operation=3; w.Held=1; w.Cooldown=.05f; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Result==0);
    w.Operation=7; w.Dt=.1f; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Cooldown==0);
    w.Operation=3; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Result==1);
    // The shotgun commits a shell, delays cycling, and chambers only after the pump state finishes.
    w.FullAuto=0; w.AllowFullAuto=0; w.PerRound=1; w.CycleAfterShot=1;
    w.CycleDelay=.12f; w.Magazine=6; w.Ammo=6; w.Operation=2;
    CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Ammo==5 && w.Chambered==0);
    w.Operation=1; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Result==0);
    w.Operation=7; w.Dt=.2f; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Commands&16);
    w.Tags=8; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.CycleSeen==1 && w.Chambered==0);
    w.Tags=32; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Chambered==1);
    w.Tags=2; w.Operation=1; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Result==0 && w.StopReload==1);
    w.Ammo=0; w.Chambered=0; w.Events=2; w.Operation=8;
    CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Ammo==1 && w.Chambered==1);
    w.Operation=9; w.Held=1; w.Dt=.1f; w.HoldSeconds=.35f; CHECK(Invoke(2,&w,sizeof w));
    w.Held=0; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Result==1);
    w.Held=1; w.Dt=.4f; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Commands==0);
    CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Commands&128);
    w.Held=0; CHECK(Invoke(2,&w,sizeof w)); CHECK(w.Result==0);
    CHECK(!Invoke(2,&w,sizeof w-1)); // a mismatched ABI is rejected before reading memory
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
    auto& definition=world.Registry.emplace<WeaponDefinitionComponent>(e);
    definition.Description="870 pump shotgun"; definition.AnimationSet="assets/Weapons/Remington870/Remington870.fpsanim";
    auto& config=world.Registry.emplace<FirstPersonControllerComponent>(e);
    config.PrimaryWeaponPrefab="assets/Weapons/AKS74U/AKS74U.prefab";
    const auto snapshot=SceneSerializer::SaveToString(world);
    CHECK(SceneSerializer::LoadFromString(world,assets,snapshot));
    auto view=world.Registry.view<CSharpScriptComponent,WeaponDefinitionComponent,FirstPersonControllerComponent>();
    CHECK(view.begin()!=view.end());
    auto restored=*view.begin();
    CHECK(world.Registry.get<CSharpScriptComponent>(restored).Fields=="{\"Height\":0.5,\"Speed\":2}");
    CHECK(world.Registry.get<WeaponDefinitionComponent>(restored).Description=="870 pump shotgun");
    CHECK(world.Registry.get<FirstPersonControllerComponent>(restored).PrimaryWeaponPrefab=="assets/Weapons/AKS74U/AKS74U.prefab");
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
        CHECK(Scripting::ResolveWeaponPrefab(stem+".prefab",set,error)); CHECK(set==stem+".fpsanim");
    }
}
void TestManagedBallistics() {
    using namespace Scripting;
    World world;
    ShotFrame shot; shot.Direction={0,0,-1}; shot.Range=100; shot.Pellets=8; shot.Spread=3;
    shot.RandomSeed=42; shot.BulletHoleRadius=.025f;
    int count=0;
    CHECK(InvokeShot(world,shot,[&](const NativeRequest& trace) {
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
    CHECK(InvokeShot(world,shot,[&](const NativeRequest& trace) {
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
    const auto gameplay=(executableDir/"Managed/Tartarus.Gameplay.dll").u8string();
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
    const auto gameplay=(dir/"Managed/Tartarus.Gameplay.dll").u8string();CHECK(Invoke(0,const_cast<char*>(gameplay.c_str()),kVersion));
    if(!std::filesystem::exists(dir/"Managed/Tartarus.Editor.dll")) return; // editor tools are excluded from exports
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
    StopEditorScripts();
}
}
void RegisterScriptingTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("Managed gameplay ABI, fire modes and pump",TestManagedGameplay);
    tests.emplace_back("Managed components, lifecycle and weapon prefabs",TestManagedComponentsAndPrefabs);
    tests.emplace_back("Managed ballistics, spread and capped pellet impulse",TestManagedBallistics);
    tests.emplace_back("Managed editor background build and reload",TestManagedEditorBuild);
    tests.emplace_back("Managed Unity workflow, multiple scripts and component access",TestManagedUnityWorkflow);
    tests.emplace_back("Managed scene API, native fields and C# editor tools",TestManagedApiAndEditor);
    tests.emplace_back("Managed vInspector attributes, buttons and read-outs",TestManagedInspectorAttributes);
}
