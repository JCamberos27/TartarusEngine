#pragma once

#include "WeaponAudio.h"

#include "../Components.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

class World;

// Bullet impacts, shell casings on the ground and rounds whipping past the listener.
// All 3D, all sets from the manifest (snd.impact.<surface>, snd.casing.<rifle|shell>.<surface>, snd.flyby), played through the
// weapon audio's SoundPlayer so they get the reverb send, the occlusion low-pass and the voice caps like everything else.
// The surface comes from the struck collider's physics material, tag and name against the Impact Audio component's table.
class ImpactAudio {
public:
    static ImpactAudio& Get();
    void Start(World& world);
    void Stop();
    bool Active() const { return m_Active; }
    const ImpactAudioComponent& Tuning() const { return m_T; }
    void SetTuning(const ImpactAudioComponent& t) { m_T = t; }
    void StartForTest(const ImpactAudioComponent& t) { m_T = t; m_Active = true; m_LastImpact.clear(); m_LastFlyby = -1e9; m_Played = Counts{}; }

    // --- pure rules (unit-tested) ---
    // A case's `contactIndex`-th (0-based) ground contact at `speed` m/s into the surface sounds when it is one of the first
    // CasingMaxContacts and fast enough.
    static bool CasingContactAudible(const ImpactAudioComponent& t, int contactIndex, float speed);
    // Gain of a casing contact: CasingGainMin at the minimum speed up to 1 at CasingFullSpeed, times CasingVolume.
    static float CasingGain(const ImpactAudioComponent& t, float speed);
    // Whether a shell is a shotgun shell (wider than ShellRadius) rather than a rifle case.
    static bool IsShell(const ImpactAudioComponent& t,float caseRadius);
    // A surface name for the table of sets that have files: `surface` itself when `available` lists it, else the default's.
    static std::string ResolveSurface(const std::string& surface, const std::vector<std::string>& available, const std::string& fallback);
    // A round passing the listener `miss` metres away: gain 1 at 0 falling to FlybyFarGain at FlybyRadius, 0 beyond.
    static float FlybyGain(const ImpactAudioComponent& t, float miss);

    // --- world-facing ---
    // The surface of an entity's collider (material + tag + name through the table), the default when it has none.
    std::string SurfaceOf(const World& world, std::uint32_t entity) const;
    // A spent case touched the ground at `pos` (its `contactIndex`-th contact, `speed` m/s into the surface).
    bool CasingContact(const World& world, const glm::vec3& pos, float speed, std::uint32_t groundEntity, bool shell, int contactIndex);
    // A round struck `entity` at `pos`: snd.impact.<surface>, 3D.
    bool Impact(const World& world, std::uint32_t entity, const glm::vec3& pos);
    // The flesh hit (a soldier or the player struck): snd.impact.flesh. False when the recordings are off / missing (the caller
    // then plays its placeholder, so there is never both).
    bool Flesh(const glm::vec3& pos, bool at2D, float gain);
    // The wet crunch over it when a round takes a head apart (snd.impact.gore). 3D.
    bool Gore(const glm::vec3& pos, float gain);
    // A round passed `miss` metres from the listener at `point`. False when it is out of the radius, too soon after the last,
    // or the flyby has no files (the caller's placeholder whizz plays then).
    bool Flyby(const glm::vec3& point, float miss);
    float FlybyRadius() const { return m_Active && m_T.Enabled && m_T.FlybyEnabled ? m_T.FlybyRadius : 0.0f; }

    struct Counts { int Casings = 0, Impacts = 0, Flesh = 0, Flybys = 0; };
    const Counts& Played() const { return m_Played; }

private:
    bool m_Active = false;
    ImpactAudioComponent m_T;
    std::unordered_map<std::string, double> m_LastImpact;
    double m_LastFlyby = -1e9;
    Counts m_Played;
    SoundSet* ImpactSet(const std::string& surface);
    SoundSet* CasingSet(bool shell, const std::string& surface);
};
