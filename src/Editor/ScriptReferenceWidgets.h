#pragma once
#include <entt/entt.hpp>
#include <json.hpp>
class World;
struct GLFWwindow;
// Shared by default and managed custom Inspectors. Uses stable prefab-relative references.
bool DrawScriptReferenceField(World& world, entt::entity owner, const nlohmann::json& metadata,
                              nlohmann::json& value, GLFWwindow* window);
