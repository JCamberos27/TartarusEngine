#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include <algorithm>
#include "WeaponFxSettings.h"

class Shader;

// The weapon's world-space effects, drawn into the HDR scene target with the scene's depth (from
// RenderFrameContext::WorldOverlay, before the view model): the laser - a thin beam that glows
// brightest near the emitter and catches the air's dust along its length, and a hot spot where it
// lands, both additive so they bloom. Queue with the Add* calls each frame, then Draw (which clears
// the queue). Bullet holes are clustered decals (BloodRenderer::HoleLook).
class WeaponFxRenderer {
public:
    WeaponFxRenderer();
    ~WeaponFxRenderer();
    WeaponFxRenderer(const WeaponFxRenderer&) = delete;
    WeaponFxRenderer& operator=(const WeaponFxRenderer&) = delete;

    // Scene tuning (beam range, width, glow falloff, bend); the defaults are the old fixed values.
    FxHudSettingsComponent Settings;
    // How much of a `distance` long beam is drawn; past the range it is gone in the haze.
    static float BeamLength(const FxHudSettingsComponent& s, float distance) { return std::min(distance, s.BeamRange); }
    // `color` is linear HDR; the beam's own brightness falls off with distance from `from`.
    // `emitterScale` != 1: the emitter is drawn through another FOV (the view model's) - the
    // beam's near end is scaled about the view axis by it, so it leaves the barrel the player
    // sees along that barrel's line, easing onto its true path over the first few metres.
    void AddBeam(const glm::vec3& from, const glm::vec3& to, const glm::vec3& color, float emitterScale = 1.0f);
    // The dot where the beam lands, lying on the surface (so it stretches on a slope).
    void AddSpot(const glm::vec3& center, const glm::vec3& normal, const glm::vec3& color);

    // `viewportHeight` in pixels keeps the beam and spot at least a pixel or so across far off.
    void Draw(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye, int viewportHeight, float time);

private:
    struct Beam { glm::vec3 From, To, Color; float EmitterScale; };
    struct Spot { glm::vec3 Center, Normal, Color; float Radius; };
    std::vector<Beam> m_Beams;
    std::vector<Spot> m_Spots;

    std::unique_ptr<Shader> m_Shader;
    unsigned int m_VAO = 0;
    unsigned int m_VBO = 0;
    size_t m_Capacity = 0; // floats
    std::vector<float> m_Verts;
};
