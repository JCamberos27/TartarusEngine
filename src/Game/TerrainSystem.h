#pragma once
#include <cstdint>
#include <memory>
#include <glm/glm.hpp>

class World;
struct TerrainComponent;
struct TerrainGrid;

// Keeps every TerrainComponent's mesh in step with its fields: decodes the heights and builds the model
// ("primitive://terrain#<key>") when the fields change or the entity's Renderable lost it (a scene load
// gives it a flat stand-in, a duplicate a fresh one). Cheap when nothing changed. Runs every frame
// (Application, before the transform cache) and right after a scene loads, so Play's colliders see the mesh.
namespace TerrainSystem {
void Sync(World& world);
// A key over the fields the mesh depends on (sampled, not a full hash of the heights).
std::uint64_t Key(const TerrainComponent& t);
// The first active terrain with a decoded grid, and its entity's world origin; null if none.
std::shared_ptr<const TerrainGrid> Find(const World& world, glm::vec3& outOrigin, std::uint64_t* outKey = nullptr);
}
