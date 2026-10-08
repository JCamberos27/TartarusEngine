#include "CombatHud.h"
#include "AI/NpcDirector.h"
#include "Scripting/GameFrames.h"
#include "Scripting/ScriptRuntime.h"
#include "Scripting/RuntimeCanvas.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <stdexcept>
namespace {
void Invoke(Scripting::CombatHudFrame& frame) {
    if(!Scripting::InvokeProject("hud",&frame,sizeof frame))throw std::runtime_error("Project HUD unavailable");
}
}
int CombatHud::NextStreak(const FxHudSettingsComponent& s,int streak,float sinceLastKill) {
    Scripting::CombatHudFrame f;f.Operation=3;f.Now=sinceLastKill;f.StreakWindow=s.StreakWindow;f.Streak=streak;Invoke(f);return f.Result;
}
bool CombatHud::FeedExpired(const FxHudSettingsComponent& s,float age) {
    Scripting::CombatHudFrame f;f.Operation=4;f.Now=age;f.FeedLife=s.FeedLife;Invoke(f);return f.Result!=0;
}
void CombatHud::Reset() {
    m_Kills=m_FeedRows=0;Scripting::CombatHudFrame f;f.Operation=0;
    Scripting::InvokeProject("hud",&f,sizeof f);
}
void CombatHud::OnKill(const NpcDirector& npcs,unsigned entity,bool head) {
    Scripting::CombatHudFrame f;f.Operation=1;f.Unit=-1;f.Head=head;f.Now=npcs.Now();f.StreakWindow=Settings.StreakWindow;
    for(const auto& n:npcs.Npcs())if(n && entt::to_integral(n->Root)==entity){f.Unit=UnitNumber(*n);break;}
    Invoke(f);m_Kills=f.Kills;m_FeedRows=f.Rows;
}
void CombatHud::Draw(const CombatHudInput& in,NpcDirector& npcs) {
    if(in.Width<=0 || in.Height<=0)return;
    static thread_local std::vector<Scripting::NpcHudFrame> records;records.clear();
    for(const auto& n:npcs.Npcs())if(n) {
        Scripting::NpcHudFrame f;f.Feet={n->Feet.x,n->Feet.y,n->Feet.z};f.Health=n->Health;f.Awareness=n->Mem.Awareness;f.Suppression=n->Suppression;
        f.Dead=n->Dead;f.Known=n->Mem.Known;f.Visible=n->Mem.Visible;f.Crouched=n->Crouched;
        f.Attack=n->HasAttackToken;f.Flank=n->HasFlankToken;f.Push=n->HasPushToken;f.Behaviour=(int)n->Doing;f.Role=(int)n->Role;
        f.Name=reinterpret_cast<std::uintptr_t>(n->Name.c_str());f.Why=reinterpret_cast<std::uintptr_t>(n->Why.c_str());records.push_back(f);
    }
    m_Lines.clear();if(in.AiOverlay)npcs.DebugLines(m_Lines);
    Scripting::CombatHudFrame f;f.Operation=2;f.Now=npcs.Now();f.RealTime=in.RealTime;f.CameraYaw=in.CamYawDeg;
    f.Camera={in.CamPos.x,in.CamPos.y,in.CamPos.z};f.FeedLife=Settings.FeedLife;f.StreakWindow=Settings.StreakWindow;
    f.PlayerDead=in.PlayerDead;f.Armed=in.Armed;f.Ammo=in.Ammo;f.Magazine=in.Magazine;f.Reloading=in.Reloading;f.God=in.God;f.InfiniteAmmo=in.InfiniteAmmo;f.AiOverlay=in.AiOverlay;
    f.FireMode=reinterpret_cast<std::uintptr_t>(in.FireMode);f.Npcs=reinterpret_cast<std::uintptr_t>(records.data());f.NpcCount=(int)records.size();
    f.DebugLines=reinterpret_cast<std::uintptr_t>(m_Lines.data());f.DebugFloatCount=(int)m_Lines.size();
    std::copy_n(glm::value_ptr(in.ViewProj),16,f.ViewProjection);m_Text.Begin(in.Width,in.Height);
    {Scripting::ScopedRuntimeCanvas scope(m_Text);Invoke(f);}
    m_Kills=f.Kills;m_FeedRows=f.Rows;m_Text.Flush(in.Fbo);
}
