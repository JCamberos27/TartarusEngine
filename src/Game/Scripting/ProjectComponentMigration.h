#pragma once
#include <json.hpp>
namespace Scripting {
// Read compatibility only: the C# definition slots (PlayerDefinition, NpcDefinition, WeaponDefinition,
// the attachments, the audio and effects definitions, Health) become their native component blocks, and the
// slots are removed. Returns whether anything changed.
bool ConvertScriptDefinitions(nlohmann::json& entity);
}
