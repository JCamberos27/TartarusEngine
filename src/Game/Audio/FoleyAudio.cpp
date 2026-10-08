#include "../Scripting/GameFrames.h"
#include "../Scripting/ScriptRuntime.h"
#include <json.hpp>
#include <stdexcept>
#include "../Scripting/NpcDefinitions.h"
#include "FoleyAudio.h"

#include "GameModuleAPI.h"
#include "PhysicsWorld.h"
#include "World.h"

#include <algorithm>
#include <map>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
} // namespace

int FoleyStepper::Advance(float distance, float stepDistance) {
    if (stepDistance <= 1e-4f || distance <= 0.0f) return 0;
    Accum += distance;
    const int n = (int)(Accum / stepDistance);
    Accum -= (float)n * stepDistance;
    return n;
}

int FootContactDetector::Update(const float height[2], float dt, float lift, float contact) {
    int down = 0;
    for (int s = 0; s < 2; ++s) {
        const float h = height[s];
        if (!Primed) { // the first frame only learns where the feet are
            Floor[s] = Prev[s] = h;
            Lifted[s] = Falling[s] = false;
            SinceContact[s]=1.0f;
            continue;
        }
        const float vy = dt > 1e-5f ? (h - Prev[s]) / dt : 0.0f;
        Prev[s] = h;
        SinceContact[s]+=std::max(dt,0.0f);
        if (!Lifted[s]) {
            Floor[s] = std::min(Floor[s] + kRelax * std::max(dt, 0.0f), h);
            if (SinceContact[s]>=kContactGuard && h > Floor[s] + lift) {
                Lifted[s] = true;
                Falling[s] = false;
                Peak[s] = h;
            }
            continue;
        }
        Peak[s] = std::max(Peak[s], h);
        if (vy < -kFallSpeed) Falling[s] = true;
        const bool backDown = h <= Floor[s] + contact;
        // An upward rebound is not a landing. Stairs can plant above the previous floor,
        // but the foot must actually come to rest there, rather than reverse direction.
        const bool stopped=Falling[s] && std::abs(vy)<kStopSpeed && Peak[s]-h>=0.5f*lift;
        if (backDown || stopped) {
            Lifted[s] = false;
            Floor[s] = h;
            SinceContact[s]=0.0f;
            down |= 1 << s;
        }
    }
    Primed = true;
    return down;
}

FoleyAudio& FoleyAudio::Get() {
    static FoleyAudio instance;
    return instance;
}

void FoleyAudio::Start(World& world) {
    Stop();
    Scripting::SyncNpcDefinitions(world);
    m_T = Scripting::DefaultFoleyDefinition();
    if (const auto first = world.Registry.view<FoleyAudioComponent>(); first.begin() != first.end()) { // the first one counts
        const entt::entity e = *first.begin();
        m_T = world.Registry.get<FoleyAudioComponent>(e);
    }
    m_Stepper.Reset();
    m_Feet.Reset();
    m_PrevGrounded = true;
    m_PrevVy = 0.0f;
    m_Steps = 0;
    m_Active = true;
}

void FoleyAudio::Stop() {
    m_NpcSteppers.clear();
    m_NpcFeet.clear();
    m_Active = false;
}

namespace {
Scripting::FoleyFrame FoleyRule(int operation,const FoleyAudioComponent& t) {
    Scripting::FoleyFrame f;f.Operation=operation;f.StepStrideScale=t.StepStrideScale;f.CrouchStrideScale=t.CrouchStrideScale;
    f.LandMinSpeed=t.LandMinSpeed;f.LandFullSpeed=t.LandFullSpeed;f.LandVolume=t.LandVolume;f.LandHeavySpeed=t.LandHeavySpeed;
    f.MinStepSpeed=t.MinStepSpeed;f.StepsFromFeet=t.StepsFromFeet;f.FootLiftMoving=t.FootLiftMoving;f.FootLiftHeight=t.FootLiftHeight;
    f.RunSpeed=t.RunSpeed;f.CrouchVolume=t.CrouchVolume;f.RunVolume=t.RunVolume;f.WalkVolume=t.WalkVolume;f.NpcStepVolume=t.NpcStepVolume;return f;
}
void ResolveFoley(Scripting::FoleyFrame& f) {if(!Scripting::InvokeProject("audio.foley",&f,sizeof f))throw std::runtime_error("Project foley policy unavailable");}
std::string FoleyElement(int id){static std::map<int,std::string> cache;static std::uint64_t generation=~std::uint64_t{};if(generation!=Scripting::CodeGeneration()){cache.clear();generation=Scripting::CodeGeneration();}if(auto it=cache.find(id);it!=cache.end())return it->second;std::string result;if(!Scripting::RequestProject("audio.foley-element",nlohmann::json(id).dump(),result))throw std::runtime_error("Project foley event unavailable");return cache[id]=nlohmann::json::parse(result).get<std::string>();}

}
std::string FoleyAudio::SurfaceFromName(const std::string& table,const std::string& name,const std::string& fallback) {
    std::string result;if(!Scripting::RequestProject("audio.surface",nlohmann::json{{"table",table},{"name",name},{"fallback",fallback}}.dump(),result))throw std::runtime_error("Project surface table unavailable");return nlohmann::json::parse(result).get<std::string>();
}
float FoleyAudio::LiftHeight(const FoleyAudioComponent& t,float speed){auto f=FoleyRule(7,t);f.Speed=speed;ResolveFoley(f);return f.Result;}
float FoleyAudio::StepDistance(const FoleyAudioComponent& t,const FoleyPlayerInput& in) {
    auto f=FoleyRule(0,t);f.Sprinting=in.Sprinting;f.Crouched=in.Crouched;f.WalkStride=in.WalkStride;f.SprintStride=in.SprintStride;ResolveFoley(f);return f.Result;
}
float FoleyAudio::LandGain(const FoleyAudioComponent& t,float speed) {auto f=FoleyRule(1,t);f.FallSpeed=speed;ResolveFoley(f);return f.Result;}

std::string FoleyAudio::SurfaceAt(World& world, const glm::vec3& feet) const {
    if (!PhysicsWorld::IsActive()) return m_T.DefaultSurface;
    const float origin[3] = {feet.x, feet.y + 0.3f, feet.z};
    const float down[3] = {0.0f, -1.0f, 0.0f};
    QueryFilter filter;
    filter.HitTriggers = 0;
    RaycastHit hit;
    if (!PhysicsWorld::RaycastFiltered(origin, down, 1.5f, filter, hit) || !hit.Hit) return m_T.DefaultSurface;
    const auto e = static_cast<entt::entity>(hit.Entity);
    if (!world.Registry.valid(e)) return m_T.DefaultSurface;
    std::string name;
    if (const auto* c = world.Registry.try_get<ColliderComponent>(e)) name += c->Material + " ";
    if (const auto* t = world.Registry.try_get<TagComponent>(e)) name += t->Tag + " ";
    if (const auto* n = world.Registry.try_get<NameComponent>(e)) name += n->Name;
    return SurfaceFromName(m_T.SurfaceTable, name, m_T.DefaultSurface);
}

void FoleyAudio::Play(const std::string& surface, const std::string& element, float gain, bool at2D, const glm::vec3& pos) {
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return;
    // `surface` is the foley category: step_<surface> for footsteps, move for the body.
    std::string choices;if(!Scripting::RequestProject("audio.foley-candidates",nlohmann::json{{"surface",surface},{"element",element},{"fallback",m_T.DefaultSurface}}.dump(),choices))return;
    SoundSet* base=nullptr;for(const auto& choice:nlohmann::json::parse(choices)){auto* candidate=wa.FoleySet(choice.at("surface").get<std::string>(),choice.at("element").get<std::string>());if(choice.value("selectEmpty",false) || !candidate->Files.empty())base=candidate;if(base && !base->Files.empty())break;}if(!base)return;
    SoundSet set = *base;
    set.PitchMin = std::min(m_T.PitchMin, m_T.PitchMax);
    set.PitchMax = std::max(m_T.PitchMin, m_T.PitchMax);
    set.VolumeJitterDb = m_T.VolumeJitterDb;
    if (!at2D) {
        set.MinDistance = m_T.NpcStepMinDistance;
        set.MaxDistance = m_T.NpcStepMaxDistance;
    }
    SoundPlayer::Request r;
    r.At2D = at2D;
    r.Position = pos;
    r.Gain = gain * m_T.Volume;
    wa.Note(base->Key, at2D);
    wa.Player().Play(set, r);
}

void FoleyAudio::UpdatePlayer(World& world, float dt, const FoleyPlayerInput& in) {
    if (!m_Active || !m_T.Enabled || dt <= 0.0f) return;
    const float speed = glm::length(glm::vec2(in.Velocity.x, in.Velocity.z));
    auto events=FoleyRule(5,m_T);events.Dt=dt;events.Grounded=in.Grounded;events.PrevGrounded=m_PrevGrounded;events.PrevVy=m_PrevVy;events.Vy=in.Velocity.y;events.Jumped=in.Jumped;events.Speed=speed;events.HaveFootHeights=in.HaveFootHeights;ResolveFoley(events);
    if(events.Landing){const float fall=events.FallSpeed,g=LandGain(m_T,fall);if(g>0){auto rule=FoleyRule(4,m_T);rule.FallSpeed=fall;ResolveFoley(rule);Play("move",FoleyElement(rule.Element?11:10),g,true,in.Feet);Play("step_"+SurfaceAt(world,in.Feet),FoleyElement(3),g,true,in.Feet);}}
    if(events.ResetSteps)m_Stepper.Reset();if(events.Jumped)Play("move",FoleyElement(4),m_T.JumpVolume,true,in.Feet);if(events.ResetFeet)m_Feet.Reset();
    const bool fromFeet=m_T.StepsFromFeet && in.HaveFootHeights;
    int n = 0;
    if (events.UseFeet) {
        // The body's own feet: a step each time one touches down, at any speed (turning on the spot steps too).
        const int down = m_Feet.Update(in.FootHeight, dt, LiftHeight(m_T, speed), m_T.FootContactHeight);
        n = (down & 1) + ((down >> 1) & 1);
        if (m_StepListener)
            for (int f = 0; f < 2; ++f)
                if (down & (1 << f)) m_StepListener(-1, in.Feet, in.Velocity, f);
    } else if (events.UseDistance) {
        n = m_Stepper.Advance(speed * dt, StepDistance(m_T, in));
        if (m_StepListener)
            for (int i = 0; i < n; ++i) { m_PlayerFoot = !m_PlayerFoot; m_StepListener(-1, in.Feet, in.Velocity, m_PlayerFoot ? 0 : 1); }
    }
    if (n > 0) {
        auto rule=FoleyRule(2,m_T);rule.Crouched=in.Crouched;rule.Sprinting=in.Sprinting;rule.Speed=speed;ResolveFoley(rule);
        const std::string element=FoleyElement(rule.Element);const float gain=rule.Gain;
        const std::string surface = "step_" + SurfaceAt(world, in.Feet);
        for (int i = 0; i < n; ++i) Play(surface, element, gain, true, in.Feet);
        m_Steps += n;
    }
    // FOLEY_FEET_LOG=1: the feet the steps are read from, a line a frame (time, heights, planted heights, swinging, steps, speed).
#pragma warning(suppress : 4996)
    static const bool feetLog = std::getenv("FOLEY_FEET_LOG") != nullptr;
    if (feetLog && fromFeet) {
        static double clock = 0.0;
        clock += dt;
        std::printf("[Feet] %.3f h %.3f %.3f floor %.3f %.3f up %d %d steps %d speed %.2f grounded %d\n", clock, in.FootHeight[0],
                    in.FootHeight[1], m_Feet.Floor[0], m_Feet.Floor[1], (int)m_Feet.Lifted[0], (int)m_Feet.Lifted[1], n, speed, (int)in.Grounded);
    }
    m_PrevGrounded=events.PrevGrounded!=0;
    m_PrevVy=events.PrevVy; // the fastest fall since leaving the ground
}

void FoleyAudio::NpcStep(World& world, const glm::vec3& feet, bool sprint) {
    if (!m_Active || !m_T.Enabled) return;
    auto rule=FoleyRule(3,m_T);rule.Sprinting=sprint;ResolveFoley(rule);Play("step_"+SurfaceAt(world,feet),FoleyElement(rule.Element),rule.Gain,false,feet);
}

void FoleyAudio::NpcWalk(World& world, int id, const glm::vec3& feet, const glm::vec3& velocity, bool sprint, float dt) {
    if (!m_Active || !m_T.Enabled || dt <= 0.0f) return;
    const float speed = glm::length(glm::vec2(velocity.x, velocity.z));
    FoleyStepper& st = m_NpcSteppers[id];
    if (speed < m_T.MinStepSpeed) {
        st.Reset();
        return;
    }
    FoleyPlayerInput in;
    in.Sprinting = sprint;
    if (st.Advance(speed * dt, StepDistance(m_T, in)) > 0) {
        if (m_StepListener) m_StepListener(id, feet, velocity, -1);
        if (glm::length(feet - WeaponAudio::Get().m_Listener) <= m_T.NpcStepMaxDistance) NpcStep(world, feet, sprint);
    }
}

bool FoleyAudio::NpcFeet(World& world, int id, const glm::vec3& feet, const float height[2], bool sprint, float speed, float dt) {
    if (!m_Active || !m_T.Enabled) return true;
    if (!m_T.StepsFromFeet) return false;
    // Tracked at any range (the feet stay primed), heard within the NPC step range.
    FootContactDetector& fd = m_NpcFeet[id];
    const int down = fd.Update(height, dt, LiftHeight(m_T, speed), m_T.FootContactHeight);
#pragma warning(suppress : 4996)
    static const bool feetLog = std::getenv("FOLEY_FEET_LOG") != nullptr;
    if (feetLog) {
        static std::unordered_map<int, double> clock;
        std::printf("[NpcFeet] %d %.3f h %.3f %.3f floor %.3f %.3f up %d %d down %d at %.3f %.3f\n", id, clock[id] += dt, height[0], height[1],
                    fd.Floor[0], fd.Floor[1], (int)fd.Lifted[0], (int)fd.Lifted[1], down, feet.x, feet.z);
    }
    if (down && m_StepListener)
        for (int s = 0; s < 2; ++s)
            if (down & (1 << s)) m_StepListener(id, feet, glm::vec3(0.0f), s);
    if (down && glm::length(feet - WeaponAudio::Get().m_Listener) <= m_T.NpcStepMaxDistance)
        for (int s = 0; s < 2; ++s)
            if (down & (1 << s)) NpcStep(world, feet, sprint);
    return true;
}
