#pragma once

#include <string>
#include <vector>

struct ColliderComponent;

// #170 - Unity's Physic Material as a shareable asset: a small JSON file (.physicmaterial) that
// any number of Colliders point at (ColliderComponent::Material, project-relative). When a
// collider has one, its values replace the collider's own Friction / Bounciness / combine fields
// for the PhysX shape, so tuning "Ice" once changes every icy surface in every scene.
struct PhysicMaterialAsset {
    float DynamicFriction = 0.6f;
    float StaticFriction  = 0.6f;
    float Bounciness      = 0.0f;
    int   FrictionCombine = 0; // PxCombineMode order: Average, Minimum, Multiply, Maximum
    int   BounceCombine   = 0;

    // Wrong-typed or out-of-range values fall back / clamp; never throws.
    static bool LoadFile(const std::string& absPath, PhysicMaterialAsset& out, std::string* error = nullptr);
    bool SaveFile(const std::string& absPath) const;

    // Copies this material's values over the collider's own surface fields.
    void ApplyTo(ColliderComponent& c) const;
};

// Project-relative paths of every .physicmaterial under the project (skipping Library/), sorted.
std::vector<std::string> FindPhysicMaterials();

// The collider's effective surface: its own fields, or its Physic Material's when it has one
// that loads. `c` is left untouched when the asset is missing or unreadable (warns once).
ColliderComponent ResolvePhysicMaterial(const ColliderComponent& c);
