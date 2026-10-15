#pragma once
#include <string>
#include <vector>
#include <entt/entt.hpp>
#include <json.hpp>
struct MuzzleEffectSettings;
struct FirstPersonWeaponGameplay;
class World;
class AssetLibrary;
namespace Gameplay {
bool ResolveWeaponPrefab(const std::string& prefab, std::string& animationSet, std::string& error,
                         MuzzleEffectSettings* muzzle = nullptr);
// The prefab's Weapon Definition gameplay numbers, normalized.
bool ResolveWeaponGameplay(const std::string& prefab,FirstPersonWeaponGameplay& gameplay,std::string& error);
// Reads an old .fpsanim "gameplay" block over the defaults, then normalizes (error says what is wrong).
bool ImportLegacyWeaponGameplay(const nlohmann::json& block,FirstPersonWeaponGameplay& gameplay,std::string& error);
// Load particle systems and attachment descendants below Weapon Definition.
// The returned root is attached to the animated weapon by the presentation.
entt::entity InstantiateWeaponParticles(World& world, AssetLibrary& assets, const std::string& prefab,
                                       std::vector<entt::entity>& emitters, std::string& error, bool loadMeshes = true);
}
