#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../Components.h"

class World;

// The noise and light of a firefight, Play only: gun reports (near and distant layers, 3D for the
// squad, 2D for the player), muzzle flashes (a short point light plus additive sparks and a puff of
// smoke), tracers, rounds whizzing past the player's head, flesh hits, hitmarker ticks, pumps,
// reloads and bodies falling. Every sound is a recorded set played through Game/Audio (WeaponAudio, ImpactAudio).
// Everything it creates is runtime-only and destroyed by Stop.
class CombatFx {
public:
    enum class Gun { Rifle, Shotgun };
    enum class Cue { Pump, Reload, DryFire, BodyFall, FleshHit, Hitmarker, HitmarkerKill };

    // Scene tuning (muzzle flash and flame); the defaults are the look before it was tunable. Set before Start.
    FxHudSettingsComponent Settings;
    // The Knife flipbook sprites the PRO Effects muzzle layers go to (Muzzle Style 1); none: the flame alone.
    void SetSprites(class FxSprites* fx) { m_Sprites = fx; }
    int MuzzleSprites() const { return m_MuzzleSprites; }
    // The peak intensity of a muzzle flash light: a soldier's, scaled down for the player's own gun.
    static float FlashPeak(const FxHudSettingsComponent& s, bool shotgun, bool fromPlayer) {
        return (shotgun ? 26.0f : 18.0f) * (fromPlayer ? s.PlayerFlashScale : 1.0f);
    }
    void Start(World& world);
    void Stop(World& world);
    bool Active() const { return m_Active; }

    // The listener (the player's eye) for choosing near / distant reports.
    void SetListener(const glm::vec3& eye, const glm::vec3& forward) { m_Listener = eye; m_ListenerFwd = forward; }
    // Tests: every sound started, and the listener each frame, as text lines in `path` (a video's
    // soundtrack can be mixed from it offline). Empty closes it.
    void SetAudioLog(const std::string& path);

    // A round left a muzzle at `origin` heading for `end`. `fromPlayer`: the report is 2D, the flash
    // light softer and the flame rides the gun (FollowMuzzle), `tracer`: a streak along the line. `shooter`: any stable id of
    // who fired (e.g. the soldier's index + 1) so the report's tail knows the space per shooter; 0 = unnamed (the player's
    // gun, else told apart by position).
    void Shot(World& world, Gun gun, const glm::vec3& origin, const glm::vec3& end, bool fromPlayer, bool tracer, std::uint32_t shooter = 0);
    // The player's gun this frame (FirstPersonPresentation::MuzzleFrames: the first-person gun's muzzle,
    // the world copy's, and the bore): the player's flames move with it.
    void FollowMuzzle(World& world, const glm::vec3& firstPerson, const glm::vec3& worldCopy, const glm::vec3& bore);
    // A round passing the player's head at `point`, `miss` metres from the listener: the recorded flyby (snd.flyby) within Impact
    // Audio's Flyby Radius.
    void Whizz(const glm::vec3& point, float miss = 0.0f);
    // How far from the listener a passing round is worth a Whizz call (the flyby radius, at least 1.6 m).
    float FlybyReach() const;
    // A round struck `entity` at `point` (a wall, a prop - not a soldier: the flesh hit has its own cue): snd.impact.<surface>.
    void Impact(World& world, std::uint32_t entity, const glm::vec3& point);
    // A one-shot at `pos` (3D), or on the listener when `at2D`.
    void Play(Cue cue, const glm::vec3& pos, bool at2D = false, float volume = 1.0f);

    void Update(World& world, float dt);

    // --- tests ---
    int ShotsHeard() const { return m_ShotsHeard; }
    int WhizzesHeard() const { return m_Whizzes; }

private:
    class FxSprites* m_Sprites = nullptr;
    int m_MuzzleSprites = 0;
    void MuzzleSpritesFor(bool shotgun, bool fromPlayer, const glm::vec3& origin, const glm::vec3& dir);
    struct Flash {
        entt::entity Light = entt::null;
        float Left = 0.0f;
        float Peak = 0.0f;
    };
    bool m_Active = false;
    glm::vec3 m_Listener{0.0f}, m_ListenerFwd{0.0f, 0.0f, -1.0f};
    std::FILE* m_Log = nullptr;
    std::vector<Flash> m_Flashes;
    size_t m_NextFlash = 0;
    entt::entity m_Sparks = entt::null, m_Flame = entt::null, m_PlayerFlame = entt::null, m_PlayerWorldFlame = entt::null, m_Smoke = entt::null, m_Tracers = entt::null;
    float m_LastWhizz = -1e9f, m_Now = 0.0f;
    std::uint32_t m_Rng = 0xC0FFEEu;
    int m_ShotsHeard = 0, m_Whizzes = 0;

    float Rand01();
};
