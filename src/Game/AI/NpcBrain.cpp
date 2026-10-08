#include "NpcBrain.h"

#include "FirstPersonPresentation.h"
#include "NpcDirector.h"
#include "../Scripting/GameFrames.h"
#include "../Scripting/ScriptRuntime.h"
#include <stdexcept>

#include <algorithm>
#include <cmath>

namespace {


float Flat(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); }

glm::vec3 FlatDir(const glm::vec3& from, const glm::vec3& to) {
    glm::vec3 d(to.x - from.x, 0.0f, to.z - from.z);
    const float l = glm::length(d);
    return l > 1e-4f ? d / l : glm::vec3(0.0f, 0.0f, 1.0f);
}

float Rand01(NpcDirector& d);

} // namespace

// The director's RNG and internals are the brain's to use (friend).
namespace {
std::mt19937* g_Rng = nullptr;
float Rand01(NpcDirector&) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(*g_Rng); }
} // namespace

int NpcBrain::FindCover(NpcDirector& d,Npc& n,CoverGoal goal,const glm::vec3& threat) {
    return Execute(d,n,PlayerSnapshot{},2,static_cast<int>(goal),&threat);
}

void NpcBrain::Think(NpcDirector& d, World& world, Npc& n, const PlayerSnapshot& p, float dt) {
    g_Rng = &d.m_Rng;
    if (d.m_Now >= n.NextThink) {
        Choose(d, n, p);
    }
    Run(d, world, n, p, dt);
}

void NpcBrain::Choose(NpcDirector& d, Npc& n, const PlayerSnapshot& p) {
    Scripting::AiChoiceFrame f;
    f.Random=Rand01(d);
    f.Now=d.m_Now;
    f.LastSeen=n.Mem.LastSeen;
    f.LastHeard=n.Mem.LastHeard;
    f.Health=n.Health;
    f.MaxHealth=n.MaxHealth;
    f.Suppression=n.Suppression;
    f.LastHurt=n.LastHurt;
    f.Awareness=n.Mem.Awareness;
    f.Morale=n.Morale;
    f.CoverCheckAt=n.CoverCheckAt;
    f.LastOwnSight=n.LastOwnSight;
    f.NoCoverUntil=n.NoCoverUntil;
    f.PlayerStill=d.m_PlayerStill;
    f.PlayerUnseen=d.m_PlayerUnseen;
    f.LimpUntil=n.LimpUntil;
    f.DoingSince=n.DoingSince;
    f.Known=n.Mem.Known;
    f.Visible=n.Mem.Visible;
    f.Wounded=n.Wounded;
    f.Doing=static_cast<int>(n.Doing);
    f.Phase=n.Phase;
    f.Role=static_cast<int>(n.Role);
    f.Shotgun=n.Class==WeaponClass::Shotgun;
    f.Cover=n.Cover;
    f.CoverGood=n.CoverGood;
    f.HasFlank=n.HasFlankToken;
    f.HasPush=n.HasPushToken;
    f.Retreated=n.Retreated;
    f.CoverSearchBlocked=d.m_CoverSearchFrame==d.m_Frame;
    const glm::vec3 threat=n.Mem.Known?n.Mem.Predicted(d.m_Now):n.Mem.LastKnown;
    f.Distance=Flat(n.Feet,threat);
    if(n.Cover>=0)f.DistanceToCover=Flat(n.Feet,d.m_Cover.Points()[static_cast<size_t>(n.Cover)].Pos);
    auto invoke=[&](int operation) {f.Operation=operation;if(!Scripting::InvokeProject("ai.choose",&f,sizeof f))throw std::runtime_error("Project NPC decisions unavailable");};
    invoke(0);
    if(f.NeedsCoverCheck) {
        const auto& cover=d.m_Cover.Points()[static_cast<size_t>(n.Cover)];
        n.CoverCheckAt=f.CoverCheckAt=d.m_Now;
        n.CoverGood=CoverSystem::Shielded(cover.Pos,cover.High?1.55f:0.95f,threat+glm::vec3(0,1.6f,0));
    f.CoverGood=n.CoverGood;
    }
    invoke(1);
    n.NextThink=f.NextThink;for(int i=0;i<kBehaviourCount;i++)n.Scores[i]=f.Scores[i];
    if(f.Change)Enter(d,n,f.Best,p);
}

void NpcBrain::Enter(NpcDirector& d,Npc& n,int behaviour,const PlayerSnapshot& p) {
    Execute(d,n,p,1,behaviour);
    char why[96];std::snprintf(why,sizeof why,"%s (%.2f)",BehaviourName(n.Doing),n.Scores[behaviour]);
    n.Why=why;
}
void NpcBrain::Run(NpcDirector& d,World& world,Npc& n,const PlayerSnapshot& p,float dt) {
    (void)world;(void)dt;Execute(d,n,p,0,0);
}
int NpcBrain::Execute(NpcDirector& d,Npc& n,const PlayerSnapshot& p,int operation,int target,const glm::vec3* threat) {
    auto pack=[](const glm::vec3& v) {return Scripting::Vec3{v.x,v.y,v.z};};
    auto unpack=[](const Scripting::Vec3& v) {return glm::vec3(v.x,v.y,v.z);};
    struct Context {NpcDirector* Director;Npc* Actor;};Context context{&d,&n};
    Scripting::NpcBrainFrame f;
    f.Operation=operation;
    f.TargetBehaviour=target;
f.Now=d.m_Now;
    f.Ammo01=n.Weapon && n.Weapon->IsActive() ? static_cast<float>(n.Weapon->Ammo())/std::max(1,n.Weapon->MagazineSize()):1.0f;
    f.PlayerHeight=p.Height;
    f.MemoryAwareness=n.Mem.Awareness;
    f.MemoryLastSeen=n.Mem.LastSeen;
    f.MemoryUncertainty=n.Mem.Uncertainty;
    f.DoingSince=n.DoingSince;
    f.BoundWaitFrom=n.BoundWaitFrom;
    f.PhaseStart=n.PhaseStart;
    f.PhaseUntil=n.PhaseUntil;
    f.NoCoverUntil=n.NoCoverUntil;
    f.IdleUntil=n.IdleUntil;
    f.BlockedTime=n.BlockedTime;
    f.LastOwnSight=n.LastOwnSight;
    f.LastHurt=n.LastHurt;
    f.Suppression=n.Suppression;
    f.CoverFireOrder=n.CoverFireOrder;
    f.PinnedSince=n.PinnedSince;
    f.LastBlindFire=n.LastBlindFire;
    f.Skill=n.Skill;
    f.MeleeAt=n.MeleeAt;
    f.CowerUntil=n.CowerUntil;
    f.GlanceUntil=n.GlanceUntil;
    f.CoverCheckAt=n.CoverCheckAt;
    f.Feet=pack(n.Feet);
    f.Eye=pack(n.Eye);
    f.Goal=pack(n.Goal);
    f.PostPos=pack(n.PostPos);
    f.GlanceAt=pack(n.GlanceAt);
    f.Threat=pack(n.Mem.Known?n.Mem.Predicted(d.m_Now):n.Mem.LastKnown);
    f.PlayerFeet=pack(p.Feet);
    f.SeenPoint=pack(n.SeenPoint);
    f.Known=n.Mem.Known;
    f.Visible=n.Mem.Visible;
    f.Shotgun=n.Class==WeaponClass::Shotgun;
    f.Reloading=n.Reloading;
    f.Retreated=n.Retreated;
    f.CoverGood=n.CoverGood;
    f.HasFlankToken=n.HasFlankToken;
    f.HasPushToken=n.HasPushToken;
    f.Doing=static_cast<int>(n.Doing);
    f.Phase=n.Phase;
    f.ShotsAtPhase=n.ShotsAtPhase;
    f.ShotsFired=n.ShotsFired;
    f.SearchStep=n.SearchStep;
    f.Squad=n.Squad;
    f.Index=n.Index;
    f.Cover=n.Cover;
    f.Role=static_cast<int>(n.Role);
    f.PeekSide=n.PeekSide;
    f.EmptyPeeks=n.EmptyPeeks;
    f.VisiblePoints=n.VisiblePoints;
    f.SquadCount=static_cast<int>(d.m_Squads.size());
    f.MateCount=static_cast<int>(d.m_Npcs.size());
    f.CoverCount=static_cast<int>(d.m_Cover.Points().size());
    f.DeathCount=d.m_DeathCount;
    f.Deaths=reinterpret_cast<const Scripting::Vec3*>(d.m_DeathPos);
    f.DeathTimes=d.m_DeathTime;
    f.Intent.MoveTarget=pack(n.Intent.MoveTarget);
    f.Intent.AimPoint=pack(n.Intent.AimPoint);
    f.Intent.LookPoint=pack(n.Intent.LookPoint);
    f.Intent.Lean=n.Intent.Lean;
    f.Intent.Cower=n.Intent.Cower;
    f.Intent.Move=n.Intent.Move;
    f.Intent.Crouch=n.Intent.Crouch;
    f.Intent.Aim=n.Intent.Aim;
    f.Intent.FaceAim=n.Intent.FaceAim;
    f.Intent.Fire=n.Intent.Fire;
    f.Intent.Suppress=n.Intent.Suppress;
    f.Intent.Reload=n.Intent.Reload;
    f.Intent.BlindFire=n.Intent.BlindFire;
    f.Intent.Pace=static_cast<int>(n.Intent.Pace);
    if(n.Squad>=0 && n.Squad<static_cast<int>(d.m_Squads.size())) {const auto& sq=d.m_Squads[static_cast<size_t>(n.Squad)];
    f.SquadCoverFireUntil=sq.CoverFireUntil;
    f.SquadCoverRequestAt=sq.CoverRequestAt;
    f.SquadPushUntil=sq.PushUntil;
    f.SquadFlankDoneAt=sq.FlankDoneAt;
    f.SquadPincerDoneAt=sq.PincerDoneAt;
    f.SquadPushHolder=sq.PushHolder;
    f.SquadFlankHolder=sq.FlankHolder;
    f.SquadPincerHolder=sq.PincerHolder;
    f.SquadCoverRequest=sq.CoverRequest;}
    if(threat)f.Threat=pack(*threat);
    f.Context=reinterpret_cast<std::uintptr_t>(&context);
    f.Services=reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t ptr,Scripting::NpcServiceFrame* r)->int {
        auto& ctx=*reinterpret_cast<Context*>(ptr);auto& director=*ctx.Director;auto& actor=*ctx.Actor;
        auto unpack=[](const Scripting::Vec3& v) {return glm::vec3(v.x,v.y,v.z);};
        auto pack=[](const glm::vec3& v) {return Scripting::Vec3{v.x,v.y,v.z};};glm::vec3 point(0.0f);
        switch(r->Operation) {
        case 0:r->Value=std::uniform_real_distribution<float>(0,1)(director.m_Rng);return 1;
        case 1:{bool ok=director.m_Nav.RandomPointNear(unpack(r->A),r->Value,r->B.x,r->B.y,point);r->C=pack(point);return ok;}
        case 2:{bool ok=director.m_Nav.Closest(unpack(r->A),point);r->C=pack(point);return ok;}
        case 3:return director.m_Nav.Walkable(unpack(r->A),unpack(r->B));
        case 4:return CoverSystem::Shielded(unpack(r->A),r->Value,unpack(r->B));
        case 5:r->Value=director.m_Nav.PathLength(unpack(r->A),unpack(r->B));return 1;
        case 6:{if(r->Index<0 || r->Index>=static_cast<int>(director.m_Cover.Points().size()))return 0;
            const auto& c=director.m_Cover.Points()[static_cast<size_t>(r->Index)];r->Cover.Pos=pack(c.Pos);r->Cover.Normal=pack(c.Normal);
            r->Cover.High=c.High;r->Cover.Peek0=c.Peek[0];r->Cover.Peek1=c.Peek[1];r->Cover.PeekLeft=pack(c.PeekPos[0]);r->Cover.PeekRight=pack(c.PeekPos[1]);r->Cover.ClaimedBy=c.ClaimedBy;r->Cover.LastUsed=c.LastUsed;return 1;}
        case 7:{static thread_local std::vector<int> near;director.m_Cover.Query(unpack(r->A),r->Value,near);r->Data=reinterpret_cast<std::uintptr_t>(near.data());r->Count=static_cast<int>(near.size());return 1;}
        case 8:return director.m_Cover.Claim(r->Index,actor.Index,director.m_Now);
        case 9:director.m_Cover.Release(actor.Index,director.m_Now);return 1;
        case 10:{if(r->Index<0 || r->Index>=static_cast<int>(director.m_Npcs.size()) || !director.m_Npcs[static_cast<size_t>(r->Index)])return 0;
            const auto& mate=*director.m_Npcs[static_cast<size_t>(r->Index)];r->Mate.Feet=pack(mate.Feet);r->Mate.Index=mate.Index;r->Mate.Squad=mate.Squad;r->Mate.Dead=mate.Dead;r->Mate.Cover=mate.Cover;r->Mate.Doing=static_cast<int>(mate.Doing);return 1;}
        case 12:director.Callout(actor,static_cast<CallKind>(r->Index));return 1;
        case 13:actor.Body.Signal(unpack(r->A));return 1;
        case 15:return director.m_Nav.Valid();
        default:return 0;
        }
    });
    if(!Scripting::InvokeProject("npc.behaviour",&f,sizeof f))throw std::runtime_error("Project NPC behaviour unavailable");
    n.LastBlindFire=f.LastBlindFire;if(f.SearchCoverCalled)d.m_CoverSearchFrame=d.m_Frame;
n.Mem.Awareness=f.MemoryAwareness;
    n.DoingSince=f.DoingSince;
    n.BoundWaitFrom=f.BoundWaitFrom;
    n.PhaseStart=f.PhaseStart;
    n.PhaseUntil=f.PhaseUntil;
    n.NoCoverUntil=f.NoCoverUntil;
    n.IdleUntil=f.IdleUntil;
    n.BlockedTime=f.BlockedTime;
    n.CoverCheckAt=f.CoverCheckAt;
    n.Goal=unpack(f.Goal);
    n.Mem.Known=f.Known!=0;
    n.Retreated=f.Retreated!=0;
    n.CoverGood=f.CoverGood!=0;
    n.HasFlankToken=f.HasFlankToken!=0;
    n.HasPushToken=f.HasPushToken!=0;
    n.Doing=static_cast<Behaviour>(f.Doing);
    n.Phase=f.Phase;
    n.ShotsAtPhase=f.ShotsAtPhase;
    n.SearchStep=f.SearchStep;
    n.Cover=f.Cover;
    n.PeekSide=f.PeekSide;
    n.EmptyPeeks=f.EmptyPeeks;
    n.Intent.MoveTarget=unpack(f.Intent.MoveTarget);
    n.Intent.AimPoint=unpack(f.Intent.AimPoint);
    n.Intent.LookPoint=unpack(f.Intent.LookPoint);
    n.Intent.Lean=f.Intent.Lean;
    n.Intent.Cower=f.Intent.Cower;
    n.Intent.Move=f.Intent.Move!=0;
    n.Intent.Crouch=f.Intent.Crouch!=0;
    n.Intent.Aim=f.Intent.Aim!=0;
    n.Intent.FaceAim=f.Intent.FaceAim!=0;
    n.Intent.Fire=f.Intent.Fire!=0;
    n.Intent.Suppress=f.Intent.Suppress!=0;
    n.Intent.Reload=f.Intent.Reload!=0;
    n.Intent.BlindFire=f.Intent.BlindFire!=0;
    n.Intent.Pace=static_cast<Gait>(f.Intent.Pace);
    if(n.Squad>=0 && n.Squad<static_cast<int>(d.m_Squads.size())) {auto& sq=d.m_Squads[static_cast<size_t>(n.Squad)];
        sq.CoverFireUntil=f.SquadCoverFireUntil;
        sq.CoverRequestAt=f.SquadCoverRequestAt;
        sq.PushUntil=f.SquadPushUntil;
        sq.FlankDoneAt=f.SquadFlankDoneAt;
        sq.PincerDoneAt=f.SquadPincerDoneAt;
        sq.PushHolder=f.SquadPushHolder;
        sq.FlankHolder=f.SquadFlankHolder;
        sq.PincerHolder=f.SquadPincerHolder;
        sq.CoverRequest=f.SquadCoverRequest;}
d.m_Tactics.Bounds+=f.Bounds;
    d.m_Tactics.CoveredBounds+=f.CoveredBounds;
    d.m_Tactics.Backpedals+=f.Backpedals;
    d.m_Tactics.BlindFires+=f.BlindFires;
    d.m_Tactics.Flanks+=f.Flanks;
    d.m_Tactics.FlankFails+=f.FlankFails;
    d.m_Tactics.Pincers+=f.Pincers;
    return f.Result;
}
