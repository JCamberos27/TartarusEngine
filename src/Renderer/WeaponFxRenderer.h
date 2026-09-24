#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>

class Shader;

// The weapon's world-space effects, drawn into the HDR scene target with the scene's depth (from
// RenderFrameContext::WorldOverlay, before the view model): bullet holes - dark, ragged decals
// lying on the surface - and the laser - a thin beam that glows brightest near the emitter and
// catches the air's dust along its length, and a hot spot where it lands, both additive so they
// bloom. Queue with the Add* calls each frame, then Draw (which clears the queue).
class WeaponFxRenderer {
public:
    WeaponFxRenderer();
    ~WeaponFxRenderer();
    WeaponFxRenderer(const WeaponFxRenderer&) = delete;
    WeaponFxRenderer& operator=(const WeaponFxRenderer&) = delete;

    // `color` is linear HDR; the beam's own brightness falls off with distance from `from`.
    // `emitterScale` != 1: the emitter is drawn through another FOV (the view model's) - the
    // beam's near end is scaled about the view axis by it, so it leaves the barrel the player
    // sees along that barrel's line, easing onto its true path over the first few metres.
    void AddBeam(const glm::vec3& from, const glm::vec3& to, const glm::vec3& color, float emitterScale = 1.0f);
    // The dot where the beam lands, lying on the surface (so it stretches on a slope).
    void AddSpot(const glm::vec3& center, const glm::vec3& normal, const glm::vec3& color);
    // `radius` is the black hole's; the darkened ring around it reaches about 2.5x that.
    void AddHole(const glm::vec3& center, const glm::vec3& normal, const glm::vec3& tangent, float radius, float seed);

    // `viewportHeight` in pixels keeps the beam and spot at least a pixel or so across far off.
    void Draw(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye, int viewportHeight, float time);

private:
    struct Beam { glm::vec3 From, To, Color; float EmitterScale; };
    struct Decal { glm::vec3 Center, Normal, Tangent, Color; float Radius, Seed; };
    std::vector<Beam> m_Beams;
    std::vector<Decal> m_Spots, m_Holes;

    std::unique_ptr<Shader> m_Shader;
    unsigned int m_VAO = 0;
    unsigned int m_VBO = 0;
    size_t m_Capacity = 0; // floats
    std::vector<float> m_Verts;
};
