#pragma once
#include <json.hpp>

// Reads a registered component's block, in the scene/prefab encoding, onto an existing instance
// of it (`component` is a ComponentRegistry name). Fields the block doesn't have keep their
// values. False when no registered component has that name. Defined in SceneSerializer.cpp.
bool ReadComponentJson(const char* component, const nlohmann::json& block, void* instance);
