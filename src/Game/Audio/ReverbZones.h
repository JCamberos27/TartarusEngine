#pragma once

#include "Components.h"
#include "EnvironmentProbe.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

class World;

// Designer-placed spaces (Reverb Zone components) and the openings between them (Reverb Portal components).
//
// Zones: a shot's space is, in order, the zones around the shooter, layered by priority and faded in over each one's Fade
// Distance, and for whatever share of the mix no zone claims (everything, outside every zone) the shooter's raycast probe
// (EnvironmentProbe). The mix is four class weights that sum to 1. Zones move: Refresh() re-reads the ones whose entity moved or
// whose component changed, and rebuilds only when zones appear, disappear or change priority.
//
// Portals: a room is a zone (or "outside": no zone). A portal joins the two rooms on its sides. FindPath() takes a source and a
// listener in different rooms through up to three portals; the sound is then heard from the last portal, as far away as the
// whole path is long, quieter and darker by each portal's open amount and the bend it has to make.

constexpr unsigned kNoRoom = 0xFFFFFFFFu; // "outside": the point is in no zone

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
    ReverbPreset Reverb;       // the resolved reverb (the class default, or the zone's own)
    std::string Ambience;      // the ambience bed's sound key ("" = none)
    float AmbienceVolume = 1.0f;
    unsigned Entity = 0xFFFFFFFFu;
    glm::mat4 Placed{1.0f};            // the world matrix it was built from (Refresh compares against the entity's)
    ReverbZoneComponent Src;           // the component it was built from (Refresh compares)

    // Metres inside the surface at `p` (<= 0: outside).
    float Depth(const glm::vec3& p) const;
    // 0 outside / on the surface, rising (smoothstep) to 1 FadeDistance inside; 1 anywhere inside with no fade.
    float Weight(const glm::vec3& p) const;
};

// What one zone claimed at a point: its index in ReverbZones::Zones() and the share of the mix it took.
struct ReverbZoneClaim {
    int Zone = -1;
    float Weight = 0.0f;
};

struct ReverbZoneMix {
    static constexpr int kMaxClaims = 8;
    float Weights[kSpaceClassCount] = {0, 0, 0, 0}; // what the zones claim, per class
    float ProbeShare = 1.0f;           // the rest, left to the probe (Weights + ProbeShare = 1)
    float Gain = 1.0f;                 // the zones' tail gain, weighted (the probe's share counts 1)
    ReverbZoneClaim Claims[kMaxClaims]; // the zones that took a share, highest priority first (the reverb and the ambience follow these)
    int ClaimCount = 0;
    int Zones = 0;                     // zones with any weight at the point
};

struct ReverbPortalVolume {
    glm::vec3 Center{0.0f}, Right{1, 0, 0}, Up{0, 1, 0}, Normal{0, 0, 1}; // the opening's frame in the world; Normal faces room B
    glm::vec3 Extents{0.5f, 1.0f, 0.1f};
    float Open = 1.0f;
    float ProbeDistance = 0.5f;
    float ClosedGainDb = -30.0f, ClosedCutoff = 500.0f, DiffractionCutoff = 2500.0f, DiffractionMaxAngle = 120.0f, DiffractionGainDb = -6.0f;
    unsigned RoomA = kNoRoom, RoomB = kNoRoom; // zone entities on the -Normal / +Normal side (kNoRoom = outside)
    unsigned Entity = 0xFFFFFFFFu;
    glm::mat4 Placed{1.0f};
    ReverbPortalComponent Src;

    // What this portal does to a sound entering from `from` and leaving towards `to` (both world points): gain (linear) and the
    // low-pass cutoff (Hz) - the open amount and the bend round the opening (the angle between the two legs).
    void Loss(const glm::vec3& from, const glm::vec3& to, float& gain, float& cutoffHz, float* bendDegrees = nullptr) const;
};

// A sound's way from `source` to `listener` through portals.
struct PortalPath {
    bool Valid = false;
    int Count = 0;
    int Portals[3] = {-1, -1, -1};     // indices into ReverbZones::Portals(), source side first
    glm::vec3 Virtual{0.0f};           // where it is heard from: the listener + the direction of the last portal x the path's length
    float Length = 0.0f;               // metres, source -> portals -> listener
    float Gain = 1.0f;                 // linear, all the portals
    float CutoffHz = 20000.0f;         // the tightest portal's low-pass
    unsigned SourceRoom = kNoRoom, ListenerRoom = kNoRoom;
};

class ReverbZones {
public:
    // Every enabled Reverb Zone and Reverb Portal in the world, placed by its entity's world transform, highest priority first.
    void Build(const World& world);
    // The cheap per-frame update: zones / portals whose entity moved or whose component changed are re-read in place (no
    // allocation); a zone or portal that appeared, went, was enabled / disabled or changed priority rebuilds everything (rare).
    // True when anything changed. `LastRefreshRebuilt()` says whether it was a rebuild.
    bool Refresh(const World& world);
    bool LastRefreshRebuilt() const { return m_Rebuilt; }
    void Set(std::vector<ReverbZoneVolume> zones); // tests / scripts; sorted by priority
    void SetPortals(std::vector<ReverbPortalVolume> portals) { m_Portals = std::move(portals); }
    const std::vector<ReverbZoneVolume>& Zones() const { return m_Zones; }
    const std::vector<ReverbPortalVolume>& Portals() const { return m_Portals; }
    bool Empty() const { return m_Zones.empty(); }
    // The zones' layered mix at `p`: from the highest priority down, each takes its weight of what is still unclaimed.
    ReverbZoneMix Mix(const glm::vec3& p) const;
    // The room at `p`: the highest-priority zone that contains it (its entity), else kNoRoom.
    unsigned RoomAt(const glm::vec3& p) const;
    const ReverbZoneVolume* ZoneOfRoom(unsigned room) const;
    // The shortest portal path between two points in different rooms; false when they are in the same room or no portals join
    // them (the caller then uses the direct path).
    bool FindPath(const glm::vec3& source, const glm::vec3& listener, PortalPath& out) const;
    // Wire boxes / spheres of the zones and the portals' openings (7 floats per vertex: xyz rgba, two vertices per line).
    void DebugLines(std::vector<float>& out) const;

private:
    std::vector<ReverbZoneVolume> m_Zones;
    std::vector<ReverbPortalVolume> m_Portals;
    bool m_Rebuilt = false;
    void ResolveRooms(ReverbPortalVolume& p, const World* world) const;
};
