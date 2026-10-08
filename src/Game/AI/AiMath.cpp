#include "AiMath.h"
#include "../Scripting/GameFrames.h"
#include "../Scripting/ScriptRuntime.h"
#include <stdexcept>

#include <algorithm>
#include <cmath>

float Smoothstep01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

float AiFollow(float dt, float timeConstant) {
    return timeConstant > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / timeConstant) : 1.0f;
}

namespace {
using Frame=Scripting::AiRuleFrame;
Scripting::Vec3 Pack(const glm::vec3& v) {return {v.x,v.y,v.z};}
glm::vec3 Unpack(const Scripting::Vec3& v) {return {v.x,v.y,v.z};}
void Run(Frame& f,int operation) {
    f.Operation=operation;if(!Scripting::InvokeProject("ai.rules",&f,sizeof f))throw std::runtime_error("Project AI rules unavailable");
}
Frame Memory(const TargetMemory& m) {
    Frame f;f.Known=m.Known;f.Visible=m.Visible;f.Awareness=m.Awareness;f.Position=Pack(m.LastKnown);f.Velocity=Pack(m.LastVelocity);
    f.LastSeen=m.LastSeen;f.LastHeard=m.LastHeard;f.Uncertainty=m.Uncertainty;return f;
}
void Apply(TargetMemory& m,const Frame& f) {
    m.Known=f.Known!=0;m.Visible=f.Visible!=0;m.Awareness=f.Awareness;m.LastKnown=Unpack(f.Position);m.LastVelocity=Unpack(f.Velocity);
    m.LastSeen=f.LastSeen;m.LastHeard=f.LastHeard;m.Uncertainty=f.Uncertainty;
}
}
float DetectionRate(const DetectionInput& input,const PerceptionSettings& settings) {
    Frame f;f.Distance=input.Distance;f.Angle=input.AngleDeg;f.VisiblePoints=input.VisiblePoints;f.Speed=input.TargetSpeed;
    f.Crouched=input.TargetCrouched;f.Firing=input.TargetFiring;f.Suppression=input.Suppression;f.Alertness=input.Alertness;
    f.Focal=settings.FocalHalfAngle;f.Peripheral=settings.PeripheralHalfAngle;f.Range=settings.Range;f.BaseRate=settings.BaseRate;Run(f,0);return f.Result;
}
glm::vec3 TargetMemory::Predicted(float now) const {auto f=Memory(*this);f.Now=now;Run(f,1);return Unpack(f.Position);}
bool UpdateMemory(TargetMemory& memory,bool visible,const glm::vec3& seen,const glm::vec3& velocity,float rate,float now,float dt) {
    auto f=Memory(memory);f.Visible=visible;f.Source=Pack(seen);f.Listener=Pack(velocity);f.Rate=rate;f.Now=now;f.Dt=dt;Run(f,2);Apply(memory,f);return f.BecameKnown!=0;
}
float HearNoise(TargetMemory& memory,const glm::vec3& listener,const glm::vec3& source,float radius,float loudness,float now) {
    auto f=Memory(memory);f.Listener=Pack(listener);f.Source=Pack(source);f.Radius=radius;f.Rate=loudness;f.Now=now;Run(f,3);Apply(memory,f);return f.Result;
}
float HitProbability(const AccuracyInput& input) {
    Frame f;f.Distance=input.Distance;f.Speed=input.TargetSpeed;f.TimeOnTarget=input.TimeOnTarget;f.SelfSpeed=input.SelfSpeed;
    f.Suppression=input.Suppression;f.Skill=input.Skill;f.Difficulty=input.Difficulty;f.VisibleFraction=input.VisibleFraction;
    f.Crouched=input.TargetCrouched;f.OutsideView=input.OutsideTargetView;f.Flinching=input.Flinching;f.Weapon=static_cast<int>(input.Weapon);Run(f,4);return f.Result;
}
float ReactionTime(float skill,float difficulty,bool peripheral,float random) {Frame f;f.Skill=skill;f.Difficulty=difficulty;f.OutsideView=peripheral;f.Random=random;Run(f,5);return f.Result;}
bool MayBound(bool exposed,bool coverFire,float waited,float maxWait) {Frame f;f.Exposed=exposed;f.CoverFire=coverFire;f.Waited=waited;f.MaxWait=maxWait;Run(f,6);return f.Result!=0;}
bool WantsBlindFire(float suppression,float pinnedFor,bool known,float sinceSeen) {Frame f;f.Suppression=suppression;f.PinnedFor=pinnedFor;f.KnowsThreat=known;f.SinceSeen=sinceSeen;Run(f,7);return f.Result!=0;}
bool WantsMelee(float distance,float facing,float sinceStrike,float reach,float cooldown) {Frame f;f.Distance=distance;f.Facing=facing;f.SinceStrike=sinceStrike;f.Reach=reach;f.Cooldown=cooldown;Run(f,8);return f.Result!=0;}
float DangerScale(const glm::vec3& pos,const glm::vec3* deaths,const float* times,int count,float now,float radius,float memory) {
    static_assert(sizeof(glm::vec3)==sizeof(Scripting::Vec3));Frame f;f.Position=Pack(pos);f.Positions=reinterpret_cast<const Scripting::Vec3*>(deaths);f.Times=times;
    f.Count=count;f.Now=now;f.Radius=radius;f.Memory=memory;Run(f,9);return f.Result;
}
float UtilityCurve::Eval(float x) const {Frame f;f.Distance=x;f.CurveKind=static_cast<int>(K);f.M=M;f.K=Kk;f.B=B;f.C=C;Run(f,10);return f.Result;}
float CombineScores(const float* scores,int count) {Frame f;f.Values=scores;f.Count=count;Run(f,11);return f.Result;}
