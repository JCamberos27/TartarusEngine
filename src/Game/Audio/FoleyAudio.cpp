#include "FoleyAudio.h"

#include "SurfaceLookup.h"
#include "GameModuleAPI.h"
#include "PhysicsWorld.h"
#include "World.h"
#include "Core/Env.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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
    m_T = FoleyAudioComponent{};
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

std::string FoleyAudio::SurfaceFromName(const std::string& table, const std::string& name, const std::string& fallback) {
    static const SurfaceLookup lookup;
    return lookup.FromName(table, name, fallback);
}

float FoleyAudio::StepDistance(const FoleyAudioComponent& t, const FoleyPlayerInput& in) {
    const float stride = in.Sprinting ? in.SprintStride : in.WalkStride;
    return std::max(0.05f, 0.5f * stride * t.StepStrideScale * (in.Crouched ? t.CrouchStrideScale : 1.0f));
}

float FoleyAudio::LandGain(const FoleyAudioComponent& t, float fallSpeed) {
    if (fallSpeed < t.LandMinSpeed) return 0.0f;
    const float k = std::clamp((fallSpeed - t.LandMinSpeed) / std::max(t.LandFullSpeed - t.LandMinSpeed, 1e-3f), 0.0f, 1.0f);
    return t.LandVolume * (0.3f + 0.7f * k);
}

std::string FoleyAudio::SurfaceAt(World& world, const glm::vec3& feet) const {
    if (!PhysicsWorld::IsActive()) return m_T.DefaultSurface;
    const float origin[3] = {feet.x, feet.y + 0.3f, feet.z};
    const float down[3] = {0.0f, -1.0f, 0.0f};
    QueryFilter filter;
    filter.HitTriggers = 0;
    RaycastHit hit;
    if (!PhysicsWorld::RaycastFiltered(origin, down, 1.5f, filter, hit) || !hit.Hit) return m_T.DefaultSurface;
    return m_Surfaces.FromEntity(world, hit.Entity, m_T.SurfaceTable, m_T.DefaultSurface);
}

void FoleyAudio::Play(const std::string& surface, const std::string& element, float gain, bool at2D, const glm::vec3& pos) {
    WeaponAudio& wa = WeaponAudio::Get();
    if (!m_T.Enabled || !wa.Active()) return; // disabled foley is silent, but steps are still detected (the blood's footprints)
    // `surface` is the foley category: step_<surface> for footsteps, move for the body.
    SoundSet* base = wa.FoleySet(surface, element);
    if (base->Files.empty() && element == "crouch") // no crouch takes: the walk's, quieter by the crouch volume
        if (SoundSet* walk = wa.FoleySet(surface, "walk"); !walk->Files.empty()) base = walk;
    // A missing take: barefoot falls back to the boots' on the same surface, then either to the default surface's.
    const bool bare = surface.rfind("barefoot_", 0) == 0;
    if (base->Files.empty() && bare) base = wa.FoleySet("step_" + surface.substr(9), element);
    if (base->Files.empty() && bare && surface != "barefoot_" + m_T.DefaultSurface)
        base = wa.FoleySet("barefoot_" + m_T.DefaultSurface, element);
    if (base->Files.empty() && surface != "step_" + m_T.DefaultSurface && (bare || surface.rfind("step_", 0) == 0))
        base = wa.FoleySet("step_" + m_T.DefaultSurface, element);
    SoundPlayer::Request r;
    r.Override = true;
    r.OverridePitchMin = std::min(m_T.PitchMin, m_T.PitchMax);
    r.OverridePitchMax = std::max(m_T.PitchMin, m_T.PitchMax);
    r.OverrideJitterDb = m_T.VolumeJitterDb;
    if (!at2D) {
        r.OverrideDistance = true;
        r.OverrideMinDistance = m_T.NpcStepMinDistance;
        r.OverrideMaxDistance = m_T.NpcStepMaxDistance;
    }
    r.At2D = at2D;
    r.Position = pos;
    r.Gain = gain * m_T.Volume;
    wa.Note(base->Key, at2D);
    wa.Player().Play(*base, r);
}

void FoleyAudio::UpdatePlayer(World& world, float dt, const FoleyPlayerInput& in) {
    if (!m_Active || dt <= 0.0f) return;
    const float speed = glm::length(glm::vec2(in.Velocity.x, in.Velocity.z));
    // Landing: the fall speed the frame before touching down.
    if (in.Grounded && !m_PrevGrounded) {
        const float fall = std::max(0.0f, -m_PrevVy), g = LandGain(m_T, fall);
        if (g > 0.0f && m_T.Enabled) {
            Play("move", fall >= m_T.LandHeavySpeed ? "land_heavy" : "land_light", g, true, in.Feet);
            Play(StepCategory(SurfaceAt(world, in.Feet), m_T.Footwear), "land", g, true, in.Feet);
        }
        m_Stepper.Reset();
        m_Feet.Reset(); // the landing is the touch-down: the feet coming to rest after it are not steps
    }
    if (in.Jumped && m_T.Enabled) Play("move", "jump", m_T.JumpVolume, true, in.Feet);
    const bool fromFeet = m_T.StepsFromFeet && in.HaveFootHeights;
    if (!in.Grounded || !fromFeet) m_Feet.Reset();
    int n = 0;
    if (in.Grounded && fromFeet) {
        // The body's own feet: a step each time one touches down, at any speed (turning on the spot steps too).
        const int down = m_Feet.Update(in.FootHeight, dt, LiftHeight(m_T, speed), m_T.FootContactHeight);
        n = (down & 1) + ((down >> 1) & 1);
        if (m_StepListener)
            for (int f = 0; f < 2; ++f)
                if (down & (1 << f)) m_StepListener(-1, in.Feet, in.Velocity, f);
    } else if (in.Grounded && speed >= m_T.MinStepSpeed) {
        n = m_Stepper.Advance(speed * dt, StepDistance(m_T, in));
        if (m_StepListener)
            for (int i = 0; i < n; ++i) { m_PlayerFoot = !m_PlayerFoot; m_StepListener(-1, in.Feet, in.Velocity, m_PlayerFoot ? 0 : 1); }
    }
    if (n > 0 && m_T.Enabled) {
        const bool run = !in.Crouched && (in.Sprinting || speed >= m_T.RunSpeed);
        const char* element = in.Crouched ? "crouch" : run ? "run" : "walk";
        const float gain = in.Crouched ? m_T.CrouchVolume : run ? m_T.RunVolume : m_T.WalkVolume;
        const std::string surface = StepCategory(SurfaceAt(world, in.Feet), m_T.Footwear);
        for (int i = 0; i < n; ++i) Play(surface, element, gain, true, in.Feet);
        m_Steps += n;
    }
    // FOLEY_FEET_LOG=1: the feet the steps are read from, a line a frame (time, heights, planted heights, swinging, steps, speed).
#pragma warning(suppress : 4996)
    static const bool feetLog = Env::Flag(Env::Var::FoleyFeetLog);
    if (feetLog && fromFeet) {
        static double clock = 0.0;
        clock += dt;
        std::printf("[Feet] %.3f h %.3f %.3f floor %.3f %.3f up %d %d steps %d speed %.2f grounded %d\n", clock, in.FootHeight[0],
                    in.FootHeight[1], m_Feet.Floor[0], m_Feet.Floor[1], (int)m_Feet.Lifted[0], (int)m_Feet.Lifted[1], n, speed, (int)in.Grounded);
    }
    m_PrevGrounded = in.Grounded;
    m_PrevVy = in.Grounded ? 0.0f : std::min(m_PrevVy, in.Velocity.y); // the fastest fall since leaving the ground
}

void FoleyAudio::NpcStep(World& world, const glm::vec3& feet, bool sprint) {
    if (!m_Active || !m_T.Enabled) return;
    Play(StepCategory(SurfaceAt(world, feet), m_T.NpcFootwear), sprint ? "run" : "walk", m_T.NpcStepVolume * (sprint ? m_T.RunVolume : m_T.WalkVolume) / std::max(m_T.WalkVolume, 0.01f), false, feet);
}

void FoleyAudio::NpcGone(int id) {
    m_NpcSteppers.erase(id);
    m_NpcFeet.erase(id);
}

void FoleyAudio::NpcWalk(World& world, int id, const glm::vec3& feet, const glm::vec3& velocity, bool sprint, float dt) {
    if (!m_Active || dt <= 0.0f) return;
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
    if (!m_Active) return true;
    if (!m_T.StepsFromFeet) return false;
    // Tracked at any range (the feet stay primed), heard within the NPC step range.
    FootContactDetector& fd = m_NpcFeet[id];
    const int down = fd.Update(height, dt, LiftHeight(m_T, speed), m_T.FootContactHeight);
#pragma warning(suppress : 4996)
    static const bool feetLog = Env::Flag(Env::Var::FoleyFeetLog);
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
