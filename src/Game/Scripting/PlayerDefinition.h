#pragma once
#include <entt/entt.hpp>
class World;
struct CSharpScriptComponent;
namespace Scripting {
bool HasPlayerDefinition(const CSharpScriptComponent& scripts);
void SyncPlayerDefinition(World& world,entt::entity entity);
void SyncPlayerDefinitions(World& world);
}
