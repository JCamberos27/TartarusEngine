#include "../Scripting/NpcDefinitions.h"
#include "../Scripting/GameFrames.h"
#include "../Scripting/ScriptRuntime.h"
#include <json.hpp>
#include <stdexcept>
#include "ImpactAudio.h"

#include "FoleyAudio.h"
#include "World.h"

#include <algorithm>
#include <cmath>

namespace {
Scripting::ImpactFrame Rule(int operation,const ImpactAudioComponent& t,float value=0){
    Scripting::ImpactFrame f;f.Operation=operation;f.Enabled=t.Enabled;f.CasingsEnabled=t.CasingsEnabled;f.MaxContacts=t.CasingMaxContacts;f.Speed=value;f.MinSpeed=t.CasingMinSpeed;f.FullSpeed=t.CasingFullSpeed;f.Volume=t.CasingVolume;f.GainMin=t.CasingGainMin;f.Radius=value;f.ShellRadius=t.ShellRadius;f.Miss=value;f.FlybyRadius=t.FlybyRadius;f.FlybyVolume=t.FlybyVolume;f.FarGain=t.FlybyFarGain;return f;
}
void Resolve(Scripting::ImpactFrame& f){if(!Scripting::InvokeProject("audio.impact",&f,sizeof f))throw std::runtime_error("Project impact policy unavailable");}
nlohmann::json SetPolicy(const char* kind,const std::string& surface,bool shell,float min,float max,int voices){std::string result;if(!Scripting::RequestProject("audio.impact-set",nlohmann::json{{"kind",kind},{"surface",surface},{"shell",shell},{"min",min},{"max",max},{"voices",voices}}.dump(),result))throw std::runtime_error("Project sound set unavailable");return nlohmann::json::parse(result);}
}

ImpactAudio& ImpactAudio::Get() {
    static ImpactAudio instance;
    return instance;
}

void ImpactAudio::Start(World& world) {
    Stop();
    Scripting::SyncNpcDefinitions(world);m_T=Scripting::DefaultImpactAudioDefinition();
    if (const auto first = world.Registry.view<ImpactAudioComponent>(); first.begin() != first.end()) { // the first one counts
        const entt::entity e = *first.begin();
        m_T = world.Registry.get<ImpactAudioComponent>(e);
    }
    m_LastImpact.clear();
    m_LastFlyby = -1e9;
    m_Played = Counts{};
    m_Active = true;
}

void ImpactAudio::Stop() {
    m_Active = false;
}

bool ImpactAudio::CasingContactAudible(const ImpactAudioComponent& t, int contactIndex, float speed) {
    auto f=Rule(0,t,speed);f.Contact=contactIndex;Resolve(f);return f.Result!=0;
}

float ImpactAudio::CasingGain(const ImpactAudioComponent& t, float speed) {
    auto f=Rule(1,t,speed);Resolve(f);return f.Gain;
}

std::string ImpactAudio::ResolveSurface(const std::string& surface, const std::vector<std::string>& available, const std::string& fallback) {
    std::string result;if(!Scripting::RequestProject("audio.impact-surface",nlohmann::json{{"surface",surface},{"available",available},{"fallback",fallback}}.dump(),result))throw std::runtime_error("Project impact surface unavailable");return nlohmann::json::parse(result).get<std::string>();
}

float ImpactAudio::FlybyGain(const ImpactAudioComponent& t, float miss) {
    auto f=Rule(3,t,miss);Resolve(f);return f.Gain;
}

bool ImpactAudio::IsShell(const ImpactAudioComponent& t,float radius){auto f=Rule(2,t,radius);Resolve(f);return f.Result!=0;}

std::string ImpactAudio::SurfaceOf(const World& world, std::uint32_t entity) const {
    const auto e = static_cast<entt::entity>(entity);
    if (entity == 0xFFFFFFFFu || !world.Registry.valid(e)) return m_T.DefaultSurface;
    std::string name;
    if (const auto* c = world.Registry.try_get<ColliderComponent>(e)) name += c->Material + " ";
    if (const auto* t = world.Registry.try_get<TagComponent>(e)) name += t->Tag + " ";
    if (const auto* n = world.Registry.try_get<NameComponent>(e)) name += n->Name;
    return FoleyAudio::SurfaceFromName(m_T.SurfaceTable, name, m_T.DefaultSurface);
}

SoundSet* ImpactAudio::ImpactSet(const std::string& surface) {
    auto policy=SetPolicy("impact",surface,false,m_T.ImpactMinDistance,m_T.ImpactMaxDistance,m_T.ImpactMaxVoices);const auto key=policy.at("key").get<std::string>();
    return WeaponAudio::Get().KeySet(key,[&](SoundSet& set){set=SoundSet::FromJson(key,policy.at("set").dump(),set);});
}

SoundSet* ImpactAudio::CasingSet(bool shell, const std::string& surface) {
    auto policy=SetPolicy("casing",surface,shell,m_T.CasingMinDistance,m_T.CasingMaxDistance,m_T.CasingMaxVoices);const auto key=policy.at("key").get<std::string>();
    return WeaponAudio::Get().KeySet(key,[&](SoundSet& set){set=SoundSet::FromJson(key,policy.at("set").dump(),set);});
}

bool ImpactAudio::CasingContact(const World& world, const glm::vec3& pos, float speed, std::uint32_t groundEntity, bool shell, int contactIndex) {
    if (!m_Active || !CasingContactAudible(m_T, contactIndex, speed)) return false;
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return false;
    SoundSet* set = CasingSet(shell, SurfaceOf(world, groundEntity));
    if (set->Files.empty()) set = CasingSet(shell, m_T.DefaultSurface); // a surface with no takes: the default's
    if (set->Files.empty()) return false;
    wa.PlayKeyed(*set, pos, false, CasingGain(m_T, speed));
    ++m_Played.Casings;
    return true;
}

bool ImpactAudio::Impact(const World& world, std::uint32_t entity, const glm::vec3& pos) {
    if (!m_Active || !m_T.Enabled || !m_T.ImpactsEnabled) return false;
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return false;
    const std::string surface = SurfaceOf(world, entity);
    SoundSet* set = ImpactSet(surface);
    std::string used = surface;
    if (set->Files.empty()) { // tile / carpet / ... have no impact takes: the default surface's
        used = m_T.DefaultSurface;
        set = ImpactSet(used);
    }
    if (set->Files.empty()) return false;
    const double now = wa.Player().Now();
    double& last = m_LastImpact.try_emplace(used, -1e9).first->second;
    auto cadence=Rule(4,m_T);cadence.Now=now;cadence.Last=last;cadence.Interval=m_T.ImpactMinInterval;Resolve(cadence);if(!cadence.Result)return false;last=cadence.Last;
    wa.PlayKeyed(*set, pos, false, m_T.ImpactVolume);
    ++m_Played.Impacts;
    return true;
}

bool ImpactAudio::Gore(const glm::vec3& pos, float gain) {
    if (!m_Active || !m_T.Enabled || !m_T.FleshUsesRecordings) return false;
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return false;
    SoundSet* set = ImpactSet("gore");
    if (set->Files.empty()) return false;
    wa.PlayKeyed(*set, pos, false, m_T.FleshVolume * gain);
    ++m_Played.Flesh;
    return true;
}

bool ImpactAudio::Flesh(const glm::vec3& pos, bool at2D, float gain) {
    if (!m_Active || !m_T.Enabled || !m_T.FleshUsesRecordings) return false;
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return false;
    SoundSet* set = ImpactSet("flesh");
    if (set->Files.empty()) return false;
    wa.PlayKeyed(*set, pos, at2D, m_T.FleshVolume * gain);
    ++m_Played.Flesh;
    return true;
}

bool ImpactAudio::Flyby(const glm::vec3& point, float miss) {
    if (!m_Active || !m_T.Enabled || !m_T.FlybyEnabled) return false;
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return false;
    auto policy=SetPolicy("flyby","",false,m_T.FlybyMinDistance,m_T.FlybyMaxDistance,4);const auto key=policy.at("key").get<std::string>();
    SoundSet* set=wa.KeySet(key,[&](SoundSet& value){value=SoundSet::FromJson(key,policy.at("set").dump(),value);});
    if (set->Files.empty()) return false;
    const float g = FlybyGain(m_T, miss);
    const double now = wa.Player().Now();
    if (g <= 0.0f) return false;
    auto cadence=Rule(4,m_T);cadence.Now=now;cadence.Last=m_LastFlyby;cadence.Interval=m_T.FlybyMinInterval;Resolve(cadence);if(!cadence.Result)return true; // a burst reads as a few cracks: this one is swallowed, not replaced by the placeholder
    m_LastFlyby=cadence.Last;
    wa.PlayKeyed(*set, point, false, g);
    ++m_Played.Flybys;
    return true;
}
