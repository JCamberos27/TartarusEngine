#pragma once

#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include <string>

class World;

// Light circuits (LightCircuitComponent) and the switches that work them (LightSwitchComponent): a wall switch whose
// rocker tips over, or a lamp switched on and off by Interact on the lamp itself. A circuit that is off costs nothing:
// its Lights are deactivated (the light gather skips them) and its fittings show their materials with the emission
// taken out (a second material set made once per fitting, so batching is unchanged).
namespace LightSwitch {

// The nearest started switch under the crosshair within Openable::kReach and not behind a wall (its distance in
// `outDist`), or null.
entt::entity Pick(World& world, const glm::vec3& eye, const glm::vec3& forward, float* outDist = nullptr);

// One Play frame: switches and circuits start (each circuit set as its switches' StartOn says); with `interact`, the
// switch under the crosshair flips its circuit.
void Update(World& world, const glm::vec3& eye, const glm::vec3& forward, bool interact);

// Turns `circuit` on or off at once: its lights, its fittings' glow, every switch on it. False when no switch or
// circuit piece by that name has started.
bool Set(World& world, const std::string& circuit, bool on);

// Every circuit on or off, outside Play too (it starts what hasn't started): --smoke-shots with TARTARUS_LIGHTS_ON=1
// shows the whole house lit.
void SetAll(World& world, bool on);

// Whether `circuit` is on (false for one that hasn't started).
bool IsOn(World& world, const std::string& circuit);

} // namespace LightSwitch
