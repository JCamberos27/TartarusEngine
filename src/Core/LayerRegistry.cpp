#include "LayerRegistry.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "AtomicFile.h"

#include <json.hpp>

#include <array>
#include <algorithm>
#include <cctype>
#include <fstream>

using json = nlohmann::json;

namespace LayerRegistry {
namespace {

// Slot 0 is fixed to "Default" and never written to / read from disk; slots 1..kCount-1 are
// user-authored and default to empty ("unnamed", shown as "Layer N").
std::array<std::string, kCount> g_Names;

const std::string& DefaultName() {
    static const std::string kDefault = "Default";
    return kDefault;
}

const std::string& LayersPath() {
    static const std::string path = ProjectPaths::Resolve("layers.json");
    return path;
}

std::string Sanitize(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != '\x7f') s += c;
    }
    // trim
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    if (s.size() > 24) s.resize(24);
    return s;
}

} // namespace

bool IsValid(int layer) { return layer >= 0 && layer < kCount; }
bool IsRenamable(int layer) { return layer >= 1 && layer < kCount; }

const std::string& Name(int layer) {
    if (layer == 0) return DefaultName();
    if (!IsValid(layer)) {
        static const std::string kEmpty;
        return kEmpty;
    }
    return g_Names[layer];
}

std::string DisplayName(int layer) {
    if (layer == 0) return DefaultName();
    if (!IsValid(layer)) return "Layer " + std::to_string(layer);
    const std::string& n = g_Names[layer];
    return n.empty() ? ("Layer " + std::to_string(layer)) : n;
}

void SetName(int layer, const std::string& name) {
    if (!IsRenamable(layer)) return; // slot 0 and out-of-range are fixed
    g_Names[layer] = Sanitize(name);
}

void Load() {
    g_Names.fill(std::string());

    std::ifstream in(LayersPath());
    if (!in.is_open()) return; // no file yet — defaults stand, not an error

    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        Log::Warn(std::string("LayerRegistry: failed to parse '") + LayersPath() + "': " + e.what());
        return;
    }

    // Format: { "names": ["Default", "Enemies", "", ...] } — index 0 is ignored on read (always
    // "Default"); extra entries beyond kCount are dropped; a shorter array leaves the tail empty.
    if (const auto it = root.find("names"); it != root.end() && it->is_array()) {
        const json& arr = *it;
        for (int i = 1; i < kCount && i < static_cast<int>(arr.size()); ++i) {
            if (arr[i].is_string()) g_Names[i] = Sanitize(arr[i].get<std::string>());
        }
    }
}

void Save() {
    json arr = json::array();
    arr.push_back(DefaultName());
    for (int i = 1; i < kCount; ++i) arr.push_back(g_Names[i]);

    // Atomic: a crash mid-write must not truncate the layer-name table (audit CPP-206).
    if (!AtomicFile::WriteJson(LayersPath(), json{{"names", arr}}))
        Log::Warn(std::string("LayerRegistry: could not write '") + LayersPath() + "'");
}

} // namespace LayerRegistry
