#pragma once

// The soldiers' sizes, shared by their collision capsule, the navmesh bake and the crowd.
//
// The navmesh is eroded (and the crowd agents sized) a little wider than the capsule: a path then keeps
// the capsule 2 cm clear of a doorway's jambs instead of grinding along them.
constexpr float kNpcCapsuleRadius = 0.28f; // collision
constexpr float kNpcNavRadius     = 0.30f; // navmesh bake erosion and crowd agent
