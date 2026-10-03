#pragma once

#include "Components.h"
#include "EnvironmentProbe.h"

#include <glm/glm.hpp>

#include <vector>

class World;

// Designer-placed spaces (Reverb Zone components) for the gunshot tails. A shot's space is, in order: the zones around the
// shooter, layered by priority and faded in over each one's Fade Distance, and for whatever share of the mix no zone claims
// (everything, outside every zone) the shooter's raycast probe (EnvironmentProbe). The mix is four class weights that sum to 1.

struct ReverbZoneVolume {
    glm::vec3 Center{0.0f};
    glm::mat3 ToLocal{1.0f};           // world -> zone-local rotation
    int Shape = 0;                     // 0 box, 1 sphere
    glm::vec3 Extents{1.0f};
    float Radius = 1.0f;
    int Priority = 0;
    float FadeDistance = 0.0f;
    SpaceClass Class = SpaceClass::IndoorSmall;
    float TailGain = 1.0f;
    ReverbPreset Reverb;
    unsigned Entity = 0xFFFFFFFFu;

    // Metres inside the surface at `p` (<= 0: outside).
    float Depth(const glm::vec3& p) const;
    // 0 outside / on the surface, rising (smoothstep) to 1 FadeDistance inside; 1 anywhere inside with no fade.
    float Weight(const glm::vec3& p) const;
};

struct ReverbZoneMix {
    float Weights[kSpaceClassCount] = {0, 0, 0, 0}; // what the zones claim, per class
    float ProbeShare = 1.0f;           // the rest, left to the probe (Weights + ProbeShare = 1)
    float Gain = 1.0f;                 // the zones' tail gain, weighted (the probe's share counts 1)
    int Zones = 0;                     // zones with any weight at the point
};

class ReverbZones {
public:
    // Every enabled Reverb Zone in the world, placed by its entity's world transform (position and rotation), highest
    // priority first. Read once when Play starts (zones do not move).
    void Build(World& world);
    void Set(std::vector<ReverbZoneVolume> zones); // tests / scripts; sorted by priority
    const std::vector<ReverbZoneVolume>& Zones() const { return m_Zones; }
    bool Empty() const { return m_Zones.empty(); }
    // The zones' layered mix at `p`: from the highest priority down, each takes its weight of what is still unclaimed.
    ReverbZoneMix Mix(const glm::vec3& p) const;
    // Wire boxes / spheres of the zones for an overlay (7 floats per vertex: xyz rgba, two vertices per line).
    void DebugLines(std::vector<float>& out) const;

private:
    std::vector<ReverbZoneVolume> m_Zones;
};
