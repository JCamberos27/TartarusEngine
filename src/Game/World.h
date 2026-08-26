#pragma once
#include <vector>
#include <string>
#include <memory>
#include <glm/glm.hpp>
#include "AABB.h"

class Model;

// An imported model placed in the scene by the editor. Visual only (no collision) —
// intended for props, characters, and animated set-dressing rather than level geometry.
struct PlacedModel {
    std::shared_ptr<Model> ModelRef;
    std::string Name;
    glm::vec3 Position{0.0f};
    glm::vec3 RotationEuler{0.0f}; // degrees
    glm::vec3 Scale{1.0f};
    std::string SoundPath; // optional clip triggered from the editor Inspector
};

struct WorldBox {
    glm::vec3 Center;
    glm::vec3 Size;
    glm::vec3 Color;
    glm::vec3 RotationEuler{0.0f}; // degrees; visual only, collision stays axis-aligned
    bool Alive = true;
    std::string Name; // empty = display falls back to "Box N" in the Hierarchy/Inspector

    AABB Bounds() const { return AABB::FromCenterSize(Center, Size); }
};

// Simple static level: a ground plane and a handful of solid boxes.
// Good enough as level geometry for collision + raycast targets.
class World {
public:
    World();

    std::vector<WorldBox> Boxes;
    std::vector<PlacedModel> Models;
    // Vertical gradient sky (see Sky.h) — horizon at the world's XZ plane, zenith straight up.
    glm::vec3 SkyHorizonColor{0.53f, 0.72f, 0.86f};
    glm::vec3 SkyZenithColor{0.20f, 0.40f, 0.75f};

    // Resolves a moving AABB against all alive solid boxes, in place.
    // Returns true if the mover was grounded (resting on a box top) this call.
    bool ResolveCollisions(AABB& mover, glm::vec3& velocity) const;

    // Casts a ray against all alive boxes; returns index of closest hit, or -1.
    int Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist, float& outDist) const;
};

// Position * RotationY * RotationX * RotationZ * Scale — the one true transform composition
// order used everywhere a PlacedModel or WorldBox is turned into a world matrix (rendering,
// viewport picking, drag-drop placement, vertex snapping). Keeping it in one place means all
// of those stay consistent with each other by construction instead of by copy-paste diligence.
glm::mat4 ComposeTransform(const glm::vec3& position, const glm::vec3& rotationEulerDegrees, const glm::vec3& scale);
