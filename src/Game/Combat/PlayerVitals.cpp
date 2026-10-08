#include "PlayerVitals.h"
#include "Damage.h"
#include "Scripting/ScriptRuntime.h"
#include <algorithm>
#include <stdexcept>

// Native presentation view of state transformed exclusively by project C#.
PlayerVitals::PlayerVitals() = default;
void PlayerVitals::Run(int operation) {
    m_Frame.Operation=operation;m_Frame.GodMode=GodMode;
    if(!Scripting::InvokeProject("vitals",&m_Frame,sizeof m_Frame))throw std::runtime_error("Project vitals implementation unavailable");
    m_Indicators.clear();
    for(int i=0;i<m_Frame.Indicators;i++)m_Indicators.push_back({{m_Frame.SourceX[i],m_Frame.SourceY[i],m_Frame.SourceZ[i]},m_Frame.Age[i],m_Frame.Strength[i]});
}
void PlayerVitals::Reset(const PlayerVitalsSettings& settings) {
    m_Frame.MaxHealth=settings.MaxHealth;m_Frame.RegenDelay=settings.RegenDelay;m_Frame.RegenRate=settings.RegenRate;
    m_Frame.RespawnDelay=settings.RespawnDelay;m_Frame.SpawnProtection=settings.SpawnProtection;Run(0);
}
float PlayerVitals::ApplyDamage(float amount,const glm::vec3& source) {
    m_Frame.Amount=amount;m_Frame.Source={source.x,source.y,source.z};Run(1);return m_Frame.Taken;
}
void PlayerVitals::Tick(float dt) {m_Frame.Dt=dt;Run(2);}
void PlayerVitals::Respawned() {Run(3);}
float PlayerVitals::RespawnProgress() const {return m_Frame.RespawnProgress;}
void PlayerVitals::MarkHit(bool kill,bool head) {m_Frame.HitmarkerKill=kill;m_Frame.HitmarkerHead=head;Run(4);}

PlayerHudState MakePlayerHudState(const PlayerVitals& v, const glm::vec3& cameraPos, float cameraYawDeg) {
    auto frame=v.Snapshot();frame.Operation=5;frame.Source={cameraPos.x,cameraPos.y,cameraPos.z};frame.CameraYaw=cameraYawDeg;
    if(!Scripting::InvokeProject("vitals",&frame,sizeof frame))throw std::runtime_error("Project vitals HUD state unavailable");
    PlayerHudState s;
    s.Visible = true;
    s.Health01 = v.Health01();
    s.HurtFlash = v.HurtFlash();
    s.Dead = v.IsDead();
    s.DeathFade = frame.DeathFade;
    s.RespawnProgress = v.RespawnProgress();
    s.Protected = v.Protected();
    for(int i=0;i<frame.Indicators;i++) {
        s.ArcAngle[s.Arcs] = frame.ArcAngle[i];
        s.ArcAlpha[s.Arcs] = frame.ArcAlpha[i];
        ++s.Arcs;
    }
    s.Hitmarker = v.Hitmarker();
    s.HitmarkerKill = v.HitmarkerKill();
    s.HitmarkerHead = v.HitmarkerHead();
    return s;
}
