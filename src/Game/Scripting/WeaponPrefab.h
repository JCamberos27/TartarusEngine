#pragma once
#include <string>
struct WeaponDefinitionComponent;
namespace Scripting {
bool ResolveWeaponPrefab(const std::string& prefab, std::string& animationSet, std::string& error,
                         WeaponDefinitionComponent* definition = nullptr);
}
