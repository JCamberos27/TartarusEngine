#pragma once
#include <string>
#include <vector>
#include <entt/entt.hpp>
#include <json.hpp>
struct MuzzleEffectSettings;
struct FirstPersonWeaponGameplay;
class World;
class AssetLibrary;
namespace Scripting {
bool ResolveWeaponPrefab(const std::string& prefab, std::string& animationSet, std::string& error,
                         MuzzleEffectSettings* muzzle = nullptr);
// Read compatibility only: new saves contain an ordinary project C# script.
bool MigrateLegacyWeaponDefinition(nlohmann::json& entity);
bool ResolveWeaponGameplay(const std::string& prefab,FirstPersonWeaponGameplay& gameplay,std::string& error);
void ReadProjectWeaponGameplay(FirstPersonWeaponGameplay& gameplay,const nlohmann::json& fields);
// Load particle systems and Attachment-script descendants below Weapon Definition.
// The returned root is attached to the animated weapon by the presentation.
entt::entity InstantiateWeaponParticles(World& world, AssetLibrary& assets, const std::string& prefab,
                                       std::vector<entt::entity>& emitters, std::string& error, bool loadMeshes = true);
}
