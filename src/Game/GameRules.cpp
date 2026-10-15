#include "GameRules.h"
#include "BodyLocomotion.h"
#include "GameModuleAPI.h"
#include "Log.h"
#include "WeaponRules.h"

namespace GameRules {
namespace {
void (*g_Weapon)(Gameplay::WeaponFrame&) = &WeaponRules::Update;
void (*g_Loadout)(Gameplay::WeaponLoadoutFrame&) = &WeaponRules::UpdateLoadout;
void (*g_Body)(Gameplay::BodyMotionFrame&) = &BodyLocomotion::Update;
bool g_FromModule = false;
const GameModuleAPI* g_Module = nullptr;
}

void UseModule(const GameModuleAPI* api) {
    g_Weapon = &WeaponRules::Update;
    g_Loadout = &WeaponRules::UpdateLoadout;
    g_Body = &BodyLocomotion::Update;
    g_FromModule = false;
    g_Module = api;
    if (!api || !(api->UpdateWeapon || api->UpdateWeaponLoadout || api->UpdateBodyMotion)) return;
    if (api->FramesLayout != Gameplay::FramesLayout()) {
        Log::Warn("Hot reload: the game module was built against different gameplay frames; the weapon and body rules "
                  "stay on the engine's copy until the engine is rebuilt too.");
        return;
    }
    if (api->UpdateWeapon) g_Weapon = api->UpdateWeapon;
    if (api->UpdateWeaponLoadout) g_Loadout = api->UpdateWeaponLoadout;
    if (api->UpdateBodyMotion) g_Body = api->UpdateBodyMotion;
    g_FromModule = true;
}

const GameModuleAPI* Module() { return g_Module; }
bool FromModule() { return g_FromModule; }
void UpdateWeapon(Gameplay::WeaponFrame& frame) { g_Weapon(frame); }
void UpdateWeaponLoadout(Gameplay::WeaponLoadoutFrame& frame) { g_Loadout(frame); }
void UpdateBodyMotion(Gameplay::BodyMotionFrame& frame) { g_Body(frame); }
}
