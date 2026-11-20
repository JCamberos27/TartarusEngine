#pragma once

// What ComponentRegistry.cpp and its per-area registration files share (not the registry's interface: ComponentRegistry.h).

#include "ComponentRegistry.h"

#include <vector>

namespace ComponentRegistry {

// Every registered component, in registration order (All() hands it out read-only).
std::vector<RegisteredComponent>& Storage();

// RegisterEngineComponents' parts, called in this order (the order is the Add Component menu's and the serializer's).
void RegisterGameplayComponents();
void RegisterCharacterComponents();
void RegisterSceneComponents();
void RegisterPhysicsComponents();
void RegisterAudioComponents();

} // namespace ComponentRegistry
