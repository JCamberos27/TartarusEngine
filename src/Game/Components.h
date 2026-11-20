#pragma once

// The ECS component set every placed entity (former level-geometry box, imported model, or
// procedural primitive) is built from. Replacing the old WorldBox/PlacedModel split — see
// World.h for the registry and the free functions (ComposeTransform, systems) that operate on
// these.
//
// The set is split by domain; include the part you need, or this header for all of it.

#include <algorithm> // kept for includers that relied on them
#include <cmath>
#include "ComponentsCore.h"
#include "ComponentsRender.h"
#include "ComponentsPhysics.h"
#include "ComponentsAudio.h"
#include "ComponentsAnimation.h"
#include "ComponentsCharacter.h"
#include "ComponentsWeapons.h"
#include "ComponentsGameplay.h"
#include "WeaponFxSettings.h" // FxHudSettingsComponent (the weapon FX renderer reads it)
