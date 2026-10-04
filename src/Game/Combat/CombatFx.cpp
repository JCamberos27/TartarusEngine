#include "CombatFx.h"

#include "AudioEngine.h"
#include "Audio/FoleyAudio.h"
#include "Audio/ImpactAudio.h"
#include "Audio/WeaponAudio.h"
#include "Components.h"
#include "FxSprites.h"
#include "ProjectPaths.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kFlashLights = 6;
constexpr float kFlameAhead = 0.01f;   // the flame's seat ahead of the muzzle face

// The recorded one-shots CombatFx plays itself (manifest keys, tools/audio build_ui.py): the hitmarker ticks and a body falling.
// Each is a keyed set of WeaponAudio, so the manifest's mix_db is its level; nothing here scales it.
SoundSet* UiSet(const char* key) {
    return WeaponAudio::Get().KeySet(key, [](SoundSet& s) {
        s.MaxVoices = 4;
        s.StealFadeTime = 0.01f;
        s.ReverbSend = 0.0f; // a HUD tick is not in the room
        s.Occlusion = 0;
    });
}
SoundSet* BodyFallSet() {
    return WeaponAudio::Get().KeySet("snd.body_fall", [](SoundSet& s) {
        s.MinDistance = 2.0f;
        s.MaxDistance = 30.0f;
        s.MaxVoices = 4;
        s.PitchMin = 0.95f;
        s.PitchMax = 1.05f;
        s.VolumeJitterDb = 1.0f;
    });
}
void PlaySet(SoundSet* set, const glm::vec3& pos, bool at2D, float volume) {
    if (set && !set->Files.empty()) WeaponAudio::Get().PlayKeyed(*set, pos, at2D, volume);
}

entt::entity MakeParticles(World& world, const char* name, float life, float startSize, float endSize, const glm::vec3& c0,
                           const glm::vec3& c1, float a0, float a1, float intensity, int blend) {
    const entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), name);
    auto& ps = world.Registry.emplace<ParticleSystemComponent>(e);
    ps.Emitting = false; // CombatFx puts every particle in by hand
    ps.MaxParticles = 2000;
    ps.Lifetime = life;
    ps.StartSize = startSize;
    ps.EndSize = endSize;
    ps.StartColor = c0;
    ps.EndColor = c1;
    ps.StartAlpha = a0;
    ps.EndAlpha = a1;
    ps.Intensity = intensity;
    ps.BlendMode = blend;
    return e;
}

glm::vec3 AnyPerpendicular(const glm::vec3& d) {
    const glm::vec3 a = std::abs(d.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    return glm::normalize(glm::cross(d, a));
}

} // namespace

float CombatFx::Rand01() {
    m_Rng ^= m_Rng << 13;
    m_Rng ^= m_Rng >> 17;
    m_Rng ^= m_Rng << 5;
    return (float)(m_Rng & 0xFFFFFFu) / (float)0x1000000u;
}

void CombatFx::Start(World& world) {
    Stop(world);
    m_Active = true;
    m_Now = 0.0f;
    m_ShotsHeard = m_Whizzes = 0;
    WeaponAudio::Get().Start(world); // the guns' report layers, gear sounds and the foley (Play-mode audio, whatever else is in the scene)
    WeaponAudio::Get().SetLogFile(m_Log);
    ImpactAudio::Get().Start(world);
    FoleyAudio::Get().Start(world);
    for (int i = 0; i < kFlashLights; ++i) {
        Flash f;
        f.Light = world.CreateEmptyEntity(glm::vec3(0.0f, -100.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "[Runtime] Muzzle Light");
        auto& l = world.Registry.emplace<LightComponent>(f.Light);
        l.Kind = LightComponent::Type::Point;
        l.Color = glm::vec3(1.0f, 0.72f, 0.38f);
        l.Intensity = 0.0f;
        l.Range = 0.01f;
        m_Flashes.push_back(f);
    }
    // Sparks: the flash itself, a few hot additive specks thrown forward that die in a frame or three.
    m_Sparks = MakeParticles(world, "[Runtime] Muzzle Flash", 0.05f, 0.075f, 0.015f, glm::vec3(1.0f, 0.85f, 0.5f),
                             glm::vec3(1.0f, 0.4f, 0.1f), 1.0f, 0.0f, 9.0f, 1);
    // The muzzle flame from the Tactical Shooter pack (P_AK105_MuzzleFlash / P_SRM12_MuzzleFlash,
    // M_Muzzle): one tongue per shot (ParticleRenderer runs its curves). The colour is its emission's
    // hue, the custom-data gradient times the graph's colour; the intensity, the AK's peak, is what
    // the engine's exposure and bloom want of HDRP's ~150,000. The player's flame rides the gun (the
    // prefabs simulate in local space), see FollowMuzzle, in two copies: one in the player's own
    // camera, on the first-person gun and drawn with it in the view-model pass (its projection, its
    // depth), and one on the world copy of the gun that every other view sees.
    struct FlameSystem { entt::entity* E; const char* Name; };
    for (const FlameSystem& f : {FlameSystem{&m_Flame, "[Runtime] Muzzle Flame"}, FlameSystem{&m_PlayerFlame, "[Runtime] Player Muzzle Flame"},
                                 FlameSystem{&m_PlayerWorldFlame, "[Runtime] Player Muzzle Flame (world)"}}) {
        *f.E = MakeParticles(world, f.Name, 0.15f, 0.0f, 0.0f, glm::vec3(1.0f, 0.147f, 0.0177f), glm::vec3(0.0f), 1.0f, 0.0f, Settings.FlameGlow, 1);
        world.Registry.get<ParticleSystemComponent>(*f.E).Texture = "assets/Effects/Muzzle/T_MuzzleFlame.png";
    }
    world.Registry.emplace<ViewModelTag>(m_PlayerFlame);
    world.Registry.emplace<OwnerViewOnlyTag>(m_PlayerFlame);
    world.Registry.emplace<HiddenFromOwnerTag>(m_PlayerWorldFlame);
    m_Smoke = MakeParticles(world, "[Runtime] Muzzle Smoke", 0.9f, 0.05f, 0.3f, glm::vec3(0.45f, 0.44f, 0.43f),
                            glm::vec3(0.55f, 0.54f, 0.53f), 0.09f, 0.0f, 0.6f, 0);
    m_Tracers = MakeParticles(world, "[Runtime] Tracers", 0.06f, 0.035f, 0.025f, glm::vec3(1.0f, 0.75f, 0.35f),
                              glm::vec3(1.0f, 0.55f, 0.2f), 1.0f, 0.6f, 14.0f, 1);
}

void CombatFx::SetAudioLog(const std::string& path) {
    if (m_Log) std::fclose(m_Log);
    m_Log = nullptr;
#pragma warning(suppress : 4996)
    if (!path.empty()) m_Log = std::fopen(path.c_str(), "w");
    WeaponAudio::Get().SetLogFile(m_Log);
}

void CombatFx::Stop(World& world) {
    if (m_Log) std::fflush(m_Log);
    FoleyAudio::Get().Stop();
    ImpactAudio::Get().Stop();
    WeaponAudio::Get().Stop();
    for (Flash& f : m_Flashes)
        if (world.Registry.valid(f.Light)) world.DestroyEntityAndChildren(f.Light);
    m_Flashes.clear();
    for (entt::entity* e : {&m_Sparks, &m_Flame, &m_PlayerFlame, &m_PlayerWorldFlame, &m_Smoke, &m_Tracers}) {
        if (*e != entt::null && world.Registry.valid(*e)) world.DestroyEntityAndChildren(*e);
        *e = entt::null;
    }
    m_Active = false;
}

void CombatFx::Shot(World& world, Gun gun, const glm::vec3& origin, const glm::vec3& end, bool fromPlayer, bool tracer, std::uint32_t shooter) {
    if (!m_Active) return;
    ++m_ShotsHeard;
    const bool shotgun = gun == Gun::Shotgun;
    // The report: every layer of this gun's audio (close, mech, sub, tail, far), weighted by distance for the squad's guns.
    WeaponAudio::Get().SetListener(m_Listener);
    WeaponAudio::Get().Shot(shotgun ? "870" : "ak", origin, fromPlayer, shooter);

    glm::vec3 dir = end - origin;
    const float len = glm::length(dir);
    dir = len > 1e-4f ? dir / len : glm::vec3(0.0f, 0.0f, -1.0f);

    // The light: off the muzzle a hand's width, so the gun and the shooter's hands catch it. The
    // player's is softer: its hands are right there, and it's the flash they see most of (looking
    // down the bore, the flame mostly hides behind the gun).
    if (!m_Flashes.empty()) {
        Flash& f = m_Flashes[m_NextFlash++ % m_Flashes.size()];
        if (world.Registry.valid(f.Light)) {
            world.Registry.get<TransformComponent>(f.Light).Position = origin + dir * 0.12f;
            f.Peak = FlashPeak(Settings, shotgun, fromPlayer);
            f.Left = Settings.FlashTime;
        }
    }
    // The flash: a hot core plus a fan of sparks along the bore, then a puff of smoke drifting up.
    const glm::vec3 side = AnyPerpendicular(dir), up = glm::cross(side, dir);
    if (auto* ps = world.Registry.try_get<ParticleSystemComponent>(m_Sparks); ps && !fromPlayer) {
        const int n = shotgun ? 14 : 9;
        for (int i = 0; i < n; ++i) {
            ParticleSystemComponent::Particle p;
            const float a = Rand01() * 6.2831853f, r = Rand01() * (i == 0 ? 0.0f : 0.35f);
            const glm::vec3 v = glm::normalize(dir + (side * std::cos(a) + up * std::sin(a)) * r);
            p.Pos = origin + dir * 0.03f;
            p.Vel = v * (i == 0 ? 0.5f : 6.0f + 10.0f * Rand01());
            p.Life = (i == 0 ? 0.035f : 0.03f + 0.03f * Rand01()) * (shotgun ? 1.4f : 1.0f);
            ps->Live.push_back(p);
        }
    }
    {
        // The flame's one particle, on the muzzle's face (kFlameAhead), 0.04-0.06 m
        // wide when grown (start size 0.5-0.75, times 0.08 over its life). The AK's (P_AK105) is
        // 0.16-0.32 m long and lives 0.125-0.175 s; the shotgun's (P_SRM12's forward flame - not its
        // brake's side jets, which an 870 hasn't got) 0.2-0.28 m and 0.175-0.275 s, at half the heat.
        ParticleSystemComponent::Particle p;
        p.Pos = origin + dir * kFlameAhead;
        p.Axis = dir;
        p.Width = (0.04f + 0.02f * Rand01()) * Settings.FlameScale;
        p.Length = (shotgun ? 0.2f + 0.08f * Rand01() : 0.16f + 0.16f * Rand01()) * Settings.FlameScale;
        p.Seed = Rand01();
        // The emission is two randoms multiplied (the gradient's and M_Muzzle's); HDRP blows nearly
        // all of them out to white, so here the dimmest are kept to a third.
        p.Glow = (0.33f + 0.67f * Rand01() * Rand01()) * (shotgun ? 0.5f : 1.0f);
        p.Alpha = 0.5f + 0.5f * Rand01();
        p.Life = shotgun ? 0.175f + 0.1f * Rand01() : 0.125f + 0.05f * Rand01();
        p.Age = 0.008f; // half a frame in (a burst comes out partly simulated): grown from nothing it'd be unseen the frame it fires
        for (entt::entity e : {fromPlayer ? m_PlayerFlame : m_Flame, fromPlayer ? m_PlayerWorldFlame : entt::entity(entt::null)}) {
            if (!world.Registry.valid(e)) continue;
            if (auto* ps = world.Registry.try_get<ParticleSystemComponent>(e); ps && ps->Live.size() < 100) ps->Live.push_back(p);
        }
    }
    if (auto* ps = world.Registry.try_get<ParticleSystemComponent>(m_Smoke)) {
        const int n = shotgun ? 3 : 1;
        for (int i = 0; i < n && ps->Live.size() < 1500; ++i) {
            ParticleSystemComponent::Particle p;
            p.Pos = origin + dir * (0.05f + 0.1f * Rand01());
            p.Vel = dir * (0.6f + 0.8f * Rand01()) + glm::vec3(0.0f, 0.25f + 0.2f * Rand01(), 0.0f) +
                    (side * (Rand01() - 0.5f) + up * (Rand01() - 0.5f)) * 0.3f;
            p.Life = 0.6f + 0.6f * Rand01();
            ps->Live.push_back(p);
        }
    }
    if (Settings.MuzzleStyle >= 1) MuzzleSpritesFor(shotgun, fromPlayer, origin, dir);
    // A tracer: a short dashed streak flying down the line at a few hundred metres a second.
    if (tracer && len > 2.0f) {
        if (auto* ps = world.Registry.try_get<ParticleSystemComponent>(m_Tracers)) {
            const float speed = 320.0f, life = std::min(0.5f, (len - 1.5f) / speed);
            for (int i = 0; i < 7 && ps->Live.size() < 1900; ++i) {
                ParticleSystemComponent::Particle p;
                p.Pos = origin + dir * (1.2f + 0.22f * (float)i);
                p.Vel = dir * speed;
                p.Life = std::max(0.02f, life - 0.22f * (float)i / speed);
                ps->Live.push_back(p);
            }
        }
    }
}

// PRO Effects' muzzle flash (Shoot FX: Assault Rifle / Shotgun MuzzleflashEmitter + AfterfireSmoke), as flipbook sprites over
// the flame: the star (rifle, 0.08 s, 0.8-1.1 Unity units ~ 0.25-0.35 m here, +-15 degrees) or the shotgun's burst (0.12 s),
// side jets along the bore, a core glow, the gas puff (alpha ~0.06-0.1) and a thin column of barrel smoke that thickens
// as the shots pile up.
void CombatFx::MuzzleSpritesFor(bool shotgun, bool fromPlayer, const glm::vec3& origin, const glm::vec3& dir) {
    if (!m_Sprites) return;
    FxSprites& fx = *m_Sprites;
    const int follow = fromPlayer ? 1 : 0;
    const glm::vec3 hot(1.0f, 0.62f, 0.3f);
    auto add = [&](FxSprites::Emit e) {
        e.ViewModel = fromPlayer && e.Mode == FxSprites::Shade::Additive; // the player's flash with the gun; smoke in the world
        e.Follow = e.ViewModel ? follow : 0;
        fx.Spawn(e);
        ++m_MuzzleSprites;
    };
    // Seen from behind the gun, half a metre from the eye, the flash is mostly hidden by the gun and must not white out the
    // view: the player's own is a third of the size and a fraction of the light of what a soldier's reads as across a room.
    const float k = Settings.FlameScale / 1.75f * (fromPlayer ? 0.5f : 1.0f);
    const float heat = fromPlayer ? 0.4f : 1.0f;
    if (const int flash = fx.Entry(shotgun ? "muzzle_burst" : "muzzle_star"); flash >= 0) {
        FxSprites::Emit e;
        e.Entry = flash;
        e.Mode = FxSprites::Shade::Additive;
        e.FollowAhead = shotgun ? 0.07f : 0.05f;
        e.Pos = origin + dir * e.FollowAhead;
        e.Life = shotgun ? 0.11f : 0.065f;
        e.Size0 = (shotgun ? 0.38f : 0.26f + 0.08f * Rand01()) * k;
        e.Size1 = e.Size0 * 1.15f;
        e.Rot = (Rand01() - 0.5f) * 0.52f + (shotgun ? Rand01() * 6.28f : 0.0f);
        e.Color = hot;
        e.Intensity = 14.0f * heat * (0.8f + 0.4f * Rand01());
        e.FadeOut = 0.6f;
        add(e);
    }
    if (const int jets = fx.Entry("muzzle_side"); jets >= 0 && !shotgun) {
        FxSprites::Emit e;
        e.Entry = jets;
        e.Mode = FxSprites::Shade::Additive;
        e.FollowAhead = 0.09f;
        e.Pos = origin + dir * e.FollowAhead;
        e.Axis = dir * 1.6f; // the bowtie's long side along the bore
        e.Life = 0.05f;
        e.Size0 = e.Size1 = 0.12f * k;
        e.Color = hot;
        e.Intensity = 10.0f * heat;
        e.FadeOut = 0.7f;
        add(e);
    }
    if (const int glow = fx.Entry("glow"); glow >= 0) {
        FxSprites::Emit e;
        e.Entry = glow;
        e.Mode = FxSprites::Shade::Additive;
        e.FollowAhead = 0.03f;
        e.Pos = origin + dir * e.FollowAhead;
        e.Life = 0.07f;
        e.Size0 = 0.3f * k;
        e.Size1 = 0.4f * k;
        e.Color = glm::vec3(1.0f, 0.55f, 0.25f);
        e.Intensity = 3.0f * heat;
        e.FadeOut = 1.0f;
        add(e);
    }
    if (const int puff = fx.Entry("smoke_muzzle"); puff >= 0) {
        for (int i = 0; i < (shotgun ? 2 : 1); ++i) {
            FxSprites::Emit e;
            e.Entry = puff;
            e.Mode = FxSprites::Shade::Lit;
            e.Pos = origin + dir * (0.12f + 0.1f * Rand01());
            e.Vel = dir * (1.0f + 1.5f * Rand01()) + glm::vec3(0.0f, 0.15f, 0.0f);
            e.Drag = 3.0f;
            e.Gravity = -0.02f;
            e.Life = 2.0f + Rand01();
            e.Size0 = 0.12f;
            e.Size1 = shotgun ? 0.6f : 0.45f;
            e.Rot = Rand01() * 6.2832f;
            e.Spin = (Rand01() - 0.5f) * 0.6f;
            e.BlendFrames = true;
            e.Color = glm::vec3(0.62f, 0.6f, 0.58f);
            e.Alpha = shotgun ? 0.35f : 0.18f;
            e.FadeIn = 0.05f;
            e.FadeOut = 0.7f;
            add(e);
        }
    }
    if (const int column = fx.Entry("smoke_gun"); column >= 0 && Rand01() < (shotgun ? 1.0f : 0.5f)) {
        FxSprites::Emit e; // AfterfireSmoke: a thin rising wisp off the hot barrel
        e.Entry = column;
        e.Mode = FxSprites::Shade::Lit;
        e.Pos = origin - dir * 0.05f + glm::vec3(0.0f, 0.12f, 0.0f);
        e.Vel = glm::vec3(0.0f, 0.12f, 0.0f);
        e.Life = 2.5f + Rand01();
        e.Size0 = e.Size1 = 0.35f;
        e.BlendFrames = true;
        e.Color = glm::vec3(0.7f, 0.68f, 0.66f);
        e.Alpha = shotgun ? 0.3f : 0.14f;
        e.FadeIn = 0.15f;
        e.FadeOut = 0.5f;
        add(e);
    }
}

void CombatFx::FollowMuzzle(World& world, const glm::vec3& firstPerson, const glm::vec3& worldCopy, const glm::vec3& bore) {
    if (m_Sprites) m_Sprites->Follow(1, firstPerson, bore);
    for (const auto& [e, muzzle] : {std::pair{m_PlayerFlame, firstPerson}, std::pair{m_PlayerWorldFlame, worldCopy}}) {
        if (!world.Registry.valid(e)) continue;
        if (auto* ps = world.Registry.try_get<ParticleSystemComponent>(e))
            for (auto& p : ps->Live) {
                p.Pos = muzzle + bore * kFlameAhead;
                p.Axis = bore;
            }
    }
}

float CombatFx::FlybyReach() const { return std::max(1.6f, ImpactAudio::Get().FlybyRadius()); }

void CombatFx::Impact(World& world, std::uint32_t entity, const glm::vec3& point) {
    if (!m_Active) return;
    ImpactAudio::Get().Impact(world, entity, point);
}

void CombatFx::Whizz(const glm::vec3& point, float miss) {
    if (!m_Active) return;
    const int before = ImpactAudio::Get().Played().Flybys;
    if (ImpactAudio::Get().Flyby(point, miss) && ImpactAudio::Get().Played().Flybys > before) ++m_Whizzes; // the recorded flyby (snd.flyby)
}

void CombatFx::Play(Cue cue, const glm::vec3& pos, bool at2D, float volume) {
    if (!m_Active) return;
    switch (cue) {
    // The pump, the reload and the dry fire are the weapons' own: an animator's snd.* event plays the recorded take
    // (snd.<gun>.pump_back, mag_out, dry_fire ...), so these cues make no sound of their own.
    case Cue::Pump:
    case Cue::Reload:
    case Cue::DryFire: break;
    case Cue::BodyFall: PlaySet(BodyFallSet(), pos, at2D, volume); break;
    case Cue::FleshHit: ImpactAudio::Get().Flesh(pos, at2D, volume); break; // the recorded flesh impact (snd.impact.flesh)
    case Cue::Hitmarker: PlaySet(UiSet("snd.ui.hitmarker"), pos, true, volume); break;
    case Cue::HitmarkerKill: PlaySet(UiSet("snd.ui.hitmarker_kill"), pos, true, volume); break;
    }
}

void CombatFx::Update(World& world, float dt) {
    if (!m_Active) return;
    if (m_Log)
        std::fprintf(m_Log, "L %.4f %.3f %.3f %.3f %.3f %.3f %.3f\n", m_Now, m_Listener.x, m_Listener.y, m_Listener.z, m_ListenerFwd.x,
                     m_ListenerFwd.y, m_ListenerFwd.z);
    m_Now += dt;
    WeaponAudio::Get().SetListener(m_Listener);
    WeaponAudio::Get().Update(dt);
    for (Flash& f : m_Flashes) {
        if (!world.Registry.valid(f.Light)) continue;
        auto& l = world.Registry.get<LightComponent>(f.Light);
        if (f.Left <= 0.0f) {
            l.Intensity = 0.0f;
            l.Range = 0.01f;
            continue;
        }
        // Full on the frame it fires, gone two or three frames later.
        const float k = std::clamp(f.Left / std::max(Settings.FlashTime, 1e-4f), 0.0f, 1.0f);
        l.Intensity = f.Peak * k * k;
        l.Range = 7.0f;
        f.Left -= dt;
    }
}
