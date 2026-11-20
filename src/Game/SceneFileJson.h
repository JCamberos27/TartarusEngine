#pragma once
// A scene file read and parsed ahead of SceneSerializer::Load. The editor's scene open already
// parses the file to find the assets to load first (EditorLayer_ScenePreload.cpp); handing that
// parse to Load saves parsing it a second time inside the open frame (~0.4 s on the Forest).
#include <json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace SceneSerializer {

struct ParsedSceneFile {
    nlohmann::json Root;
    // The file as it was when read: Load parses it again if either has changed since.
    std::uintmax_t Size = 0;
    std::filesystem::file_time_type WriteTime{};
};

// Null when the file is missing or isn't valid JSON (Load then reports it as before).
std::shared_ptr<ParsedSceneFile> ReadSceneFile(const std::string& path);

} // namespace SceneSerializer
