#pragma once
#include "PlayerConfig.h"

#include <json.hpp>

// The shared reader/writer for PlayerBuildFields (player.json and settings.json "build").
// Missing or wrong-typed keys keep the current value; sizes are clamped; blank scenes dropped.
void BuildFieldsFromJson(const nlohmann::json& j, PlayerBuildFields& f);
void BuildFieldsToJson(const PlayerBuildFields& f, nlohmann::json& j);
