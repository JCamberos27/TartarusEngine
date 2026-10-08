#pragma once
#include <json.hpp>
namespace Scripting {
// Read compatibility only. New game data is authored through ordinary project script slots.
bool MigrateLegacyProjectComponents(nlohmann::json& entity);
}
