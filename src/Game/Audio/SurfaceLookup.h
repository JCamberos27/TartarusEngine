#pragma once

#include <cstdint>
#include <string>
#include <vector>

class World;

// An entity's surface ("wood", "metal", ...) for the footstep, impact and bullet-hole systems: the words of its collider's physics
// material, its tag and its name matched against a surface table ("wood=wood,plank;metal=metal,steel"), the first surface with a
// word in the text winning. The table is parsed once and kept; it is parsed again only when the string handed in changes (the
// component it comes from was edited), so each system holds its own SurfaceLookup and asks it with its table every time.
class SurfaceLookup {
public:
    // The first surface in `table` with a word in `name` (case-insensitive), else `fallback`.
    std::string FromName(const std::string& table, const std::string& name, const std::string& fallback) const;
    // The surface of `entity`'s collider material, tag and name, in that order; `fallback` for no entity, a dead one or no match.
    std::string FromEntity(const World& world, std::uint32_t entity, const std::string& table, const std::string& fallback) const;

private:
    struct Entry {
        std::string Surface;
        std::vector<std::string> Words; // lower case, trimmed
    };
    void Parse(const std::string& table) const;
    mutable bool m_Parsed = false;
    mutable std::string m_Table;
    mutable std::vector<Entry> m_Entries;
};
