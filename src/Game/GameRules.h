#pragma once
#include "GameFrames.h"

struct GameModuleAPI;

// The first-person rules as the host calls them: the loaded game module's copies when it provides
// them (so editing WeaponRules.cpp / BodyLocomotion.cpp hot-reloads), else the copies built into
// the engine. HotReloadGameModule points this at each module it loads and clears it before
// unloading one.
namespace GameRules {
void UseModule(const GameModuleAPI* api);
const GameModuleAPI* Module(); // the module last passed to UseModule
bool FromModule();             // true while the module's rules are the ones in use
void UpdateWeapon(Gameplay::WeaponFrame& frame);
void UpdateWeaponLoadout(Gameplay::WeaponLoadoutFrame& frame);
void UpdateBodyMotion(Gameplay::BodyMotionFrame& frame);
}
