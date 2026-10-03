#include "FoleyAudio.h"

#include "GameModuleAPI.h"
#include "PhysicsWorld.h"
#include "World.h"

#include <algorithm>
#include <cctype>
#include <cmath>

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

FoleyAudio& FoleyAudio::Get() {
    static FoleyAudio instance;
    return instance;
}

void FoleyAudio::Start(World& world) {
    Stop();
    m_T = FoleyAudioComponent{};
    for (const entt::entity e : world.Registry.view<FoleyAudioComponent>()) {
        m_T = world.Registry.get<FoleyAudioComponent>(e);
        break;
    }
    m_Stepper.Reset();
    m_PrevGrounded = true;
    m_PrevVy = 0.0f;
    m_Steps = 0;
    m_Active = true;
}

void FoleyAudio::Stop() {
    m_NpcSteppers.clear();
    m_Active = false;
}

std::string FoleyAudio::SurfaceFromName(const std::string& table, const std::string& name, const std::string& fallback) {
    const std::string hay = Lower(name);
    size_t pos = 0;
    while (pos < table.size()) {
        size_t end = table.find(';', pos);
        if (end == std::string::npos) end = table.size();
        const std::string entry = table.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = entry.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string surface = entry.substr(0, eq);
        size_t w = eq + 1;
        while (w <= entry.size()) {
            size_t we = entry.find(',', w);
            if (we == std::string::npos) we = entry.size();
            std::string word = Lower(entry.substr(w, we - w));
            while (!word.empty() && word.front() == ' ') word.erase(word.begin());
            while (!word.empty() && word.back() == ' ') word.pop_back();
            if (!word.empty() && hay.find(word) != std::string::npos) return surface;
            w = we + 1;
        }
    }
    return fallback;
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
    SoundSet* base = wa.FoleySet(surface, element);
    if (base->Files.empty() && element == "crouch") // no crouch takes: the walk's, quieter by the crouch volume
        if (SoundSet* walk = wa.FoleySet(surface, "walk"); !walk->Files.empty()) base = walk;
    if (base->Files.empty() && surface != "step_" + m_T.DefaultSurface && surface.rfind("step_", 0) == 0)
        base = wa.FoleySet("step_" + m_T.DefaultSurface, element);
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
    // Landing: the fall speed the frame before touching down.
    if (in.Grounded && !m_PrevGrounded) {
        const float fall = std::max(0.0f, -m_PrevVy), g = LandGain(m_T, fall);
        if (g > 0.0f) {
            Play("move", fall >= m_T.LandHeavySpeed ? "land_heavy" : "land_light", g, true, in.Feet);
            Play("step_" + SurfaceAt(world, in.Feet), "land", g, true, in.Feet);
        }
        m_Stepper.Reset();
    }
    if (in.Jumped) Play("move", "jump", m_T.JumpVolume, true, in.Feet);
    if (in.Grounded && speed >= m_T.MinStepSpeed) {
        const int n = m_Stepper.Advance(speed * dt, StepDistance(m_T, in));
        if (n > 0) {
            const bool run = !in.Crouched && (in.Sprinting || speed >= m_T.RunSpeed);
            const char* element = in.Crouched ? "crouch" : run ? "run" : "walk";
            const float gain = in.Crouched ? m_T.CrouchVolume : run ? m_T.RunVolume : m_T.WalkVolume;
            const std::string surface = "step_" + SurfaceAt(world, in.Feet);
            for (int i = 0; i < n; ++i) Play(surface, element, gain, true, in.Feet);
            m_Steps += n;
        }
    }
    m_PrevGrounded = in.Grounded;
    m_PrevVy = in.Grounded ? 0.0f : std::min(m_PrevVy, in.Velocity.y); // the fastest fall since leaving the ground
}

void FoleyAudio::NpcStep(World& world, const glm::vec3& feet, bool sprint) {
    if (!m_Active || !m_T.Enabled) return;
    Play("step_" + SurfaceAt(world, feet), sprint ? "run" : "walk", m_T.NpcStepVolume * (sprint ? m_T.RunVolume : m_T.WalkVolume) / std::max(m_T.WalkVolume, 0.01f), false, feet);
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
    if (st.Advance(speed * dt, StepDistance(m_T, in)) > 0 &&
        glm::length(feet - WeaponAudio::Get().m_Listener) <= m_T.NpcStepMaxDistance)
        NpcStep(world, feet, sprint);
}
