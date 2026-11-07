#pragma once

#include "SoundManifest.h"

#include <json.hpp>

#include <string>

// The json reading and writing of a SoundSet, shared by the manifest, the Data Files of a weapon profile and the profile's own
// output. Only the .cpp files that parse json include this.
namespace SoundJson {
float Num(const nlohmann::json& j, const char* key, float fallback);
bool Flag(const nlohmann::json& j, const char* key, bool fallback);
std::string Str(const nlohmann::json& j, const char* key);
// `s` with the fields `j` names replaced.
SoundSet ApplySet(const nlohmann::json& j, SoundSet s);
nlohmann::json SetToJson(const SoundSet& s);
} // namespace SoundJson
