#include "DevPanel.h"
#include "AI/NpcDirector.h"
#include "Combat/PlayerVitals.h"
#include "Scripting/GameFrames.h"
#include "Scripting/ScriptRuntime.h"
#include "Scripting/RuntimeGui.h"
#include "TimeService.h"
namespace {
int Run(DevPanel& panel,PlayerVitals& vitals,NpcDirector& npcs,int operation,World* world=nullptr,AssetLibrary* assets=nullptr,bool* blood=nullptr){
    Scripting::DevFrame f;f.Operation=operation;f.Open=panel.Open;f.InfiniteAmmo=panel.InfiniteAmmo;f.Invisible=panel.Invisible;f.AiOverlay=panel.AiOverlay;f.God=vitals.GodMode;f.Frozen=npcs.Frozen;f.HoldFire=npcs.HoldFire;f.Difficulty=npcs.Difficulty();f.TimeScale=Time::TimeScale();f.HaveBloodLab=blood!=nullptr;f.BloodLab=blood && *blood;f.Wanted=npcs.SquadSize();f.Spawns=(int)npcs.SpawnPoints().size();
    std::vector<Scripting::NpcMateFrame> members;
    for(const auto& n:npcs.Npcs())if(n){Scripting::NpcMateFrame m;m.Index=n->Index;m.Feet={n->Feet.x,n->Feet.y,n->Feet.z};m.Dead=n->Dead;members.push_back(m);if(!n->Dead){++f.Alive;if(n->Mem.Known)++f.Known;}}
    f.Count=(int)members.size();f.Members=reinterpret_cast<std::uintptr_t>(members.data());
    struct Scope {NpcDirector& Npcs;World* WorldPtr;AssetLibrary* Assets;} scope{npcs,world,assets};f.Context=reinterpret_cast<std::uintptr_t>(&scope);
    f.Services=reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t ptr,Scripting::NpcServiceFrame* request)->int {
        if(!ptr || !request)return 0;auto& c=*reinterpret_cast<Scope*>(ptr);if(!c.WorldPtr)return 0;
        if(request->Operation==0){auto* n=c.Npcs.Find(request->Index);if(!n || n->Dead)return 0;c.Npcs.Kill(*c.WorldPtr,*n,{request->A.x,request->A.y,request->A.z},{request->B.x,request->B.y,request->B.z},request->Value);return 1;}
        if(request->Operation==1 && c.Assets)return c.Npcs.Spawn(*c.WorldPtr,*c.Assets,request->Index)>=0;return 0;
    });
    if(!Scripting::InvokeProject("dev",&f,sizeof f))return 0;
    panel.Open=f.Open!=0;panel.InfiniteAmmo=f.InfiniteAmmo!=0;panel.Invisible=f.Invisible!=0;panel.AiOverlay=f.AiOverlay!=0;vitals.GodMode=f.God!=0;npcs.Frozen=f.Frozen!=0;npcs.HoldFire=f.HoldFire!=0;
    if(operation==2){npcs.SetDifficulty(f.Difficulty);Time::SetTimeScale(f.TimeScale);if(blood)*blood=f.BloodLab!=0;}
    return f.Result;
}
}
void DevPanel::Reset(PlayerVitals& vitals,NpcDirector& npcs){Run(*this,vitals,npcs,0);}
void DevPanel::Keys(PlayerVitals& vitals,NpcDirector& npcs){Run(*this,vitals,npcs,1);}
int DevPanel::KillAll(World& world,NpcDirector& npcs){DevPanel panel;PlayerVitals vitals;return Run(panel,vitals,npcs,3,&world);}
int DevPanel::RespawnSquad(World& world,AssetLibrary& assets,NpcDirector& npcs){DevPanel panel;PlayerVitals vitals;return Run(panel,vitals,npcs,4,&world,&assets);}
void DevPanel::Draw(const Context& c){if(!Open || !c.Npcs || !c.Vitals)return;Scripting::ScopedRuntimeGui gui;Run(*this,*c.Vitals,*c.Npcs,2,c.WorldPtr,c.Assets,c.BloodLab);}
