#pragma once
#include "SwayModifier.h"
#include <json.hpp>
nlohmann::json SwayModifierToJson(const SwayModifierSettings&);
bool SwayModifierFromJson(const nlohmann::json&,SwayModifierSettings&,std::string* error);
