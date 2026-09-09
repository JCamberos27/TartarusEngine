#include "AssetDatabase.h"
#include "Log.h"
#include "ProjectPaths.h"
#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <unordered_map>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// AssetGuid implementation
// ---------------------------------------------------------------------------

std::string AssetGuid::ToString() const {
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)Value);
    return buf;
}

AssetGuid AssetGuid::FromString(const std::string& s) {
    if (s.size() != 16) return {};
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
    }
    AssetGuid g;
    g.Value = std::stoull(s, nullptr, 16);
    return g;
}

AssetGuid AssetGuid::Generate() {
    static std::mt19937_64 rng(std::random_device{}());
    AssetGuid g;
    do { g.Value = rng(); } while (g.Value == 0);
    return g;
}

// ---------------------------------------------------------------------------
// AssetDatabase internals
// ---------------------------------------------------------------------------

namespace AssetDatabase {
namespace {

std::mutex g_Mutex;
std::unordered_map<std::string, AssetGuid> g_PathToGuid;
std::unordered_map<AssetGuid, std::string> g_GuidToPath;

// Extensions that get a .meta sidecar (binary and non-asset types are excluded).
const std::unordered_map<std::string, std::string>& KnownExtensions() {
    static const std::unordered_map<std::string, std::string> kExts = {
        {".png",   "texture"}, {".jpg",  "texture"}, {".jpeg", "texture"},
        {".tga",   "texture"}, {".bmp",  "texture"},
        {".fbx",   "model"},   {".obj",  "model"},
        {".gltf",  "model"},   {".glb",  "model"},
        {".wav",   "audio"},   {".mp3",  "audio"},
        {".ogg",   "audio"},   {".flac", "audio"},
        {".prefab","prefab"},
        {".mat",   "material"},
    };
    return kExts;
}

std::string MetaPath(const std::string& assetPath) {
    return assetPath + ".meta";
}

// Reads the full .meta file; returns an empty object on any error.
json ReadMetaFull(const std::string& metaPath) {
    std::ifstream f(metaPath);
    if (!f.is_open()) return {};
    try { return json::parse(f); } catch (...) { return {}; }
}

// Reads the GUID from an existing .meta file. Returns invalid guid on any error.
AssetGuid ReadMetaGuid(const std::string& metaPath) {
    json j = ReadMetaFull(metaPath);
    if (!j.contains("guid") || !j["guid"].is_string()) return {};
    return AssetGuid::FromString(j["guid"].get<std::string>());
}

// Writes a new .meta sidecar. Returns false on I/O error.
bool WriteMetaFile(const std::string& metaPath, AssetGuid guid, const std::string& type) {
    std::ofstream f(metaPath);
    if (!f.is_open()) return false;
    json j;
    j["metaVersion"] = 1;
    j["guid"] = guid.ToString();
    j["type"] = type;
    f << j.dump(2) << "\n";
    return f.good();
}

// Registers guid ↔ path in both maps (caller holds g_Mutex).
void Register(const std::string& path, AssetGuid guid) {
    g_PathToGuid[path] = guid;
    g_GuidToPath[guid] = path;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool IsSynthetic(const std::string& path) {
    return path.rfind("primitive://", 0) == 0;
}

AssetGuid EnsureGuid(const std::string& path) {
    if (IsSynthetic(path)) return {};

    std::lock_guard<std::mutex> lk(g_Mutex);

    auto it = g_PathToGuid.find(path);
    if (it != g_PathToGuid.end()) return it->second;

    // Determine asset type from extension (fall back to "asset").
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    const auto& known = KnownExtensions();
    std::string type = "asset";
    auto extIt = known.find(ext);
    if (extIt != known.end()) type = extIt->second;

    // Only create a .meta if the source file actually exists on disk. Missing assets (stale
    // scene references, paths from a different checkout) are tracked in-memory only so they
    // don't produce stale .meta files, and the caller can still load/fail gracefully.
    std::error_code existEc;
    if (!std::filesystem::exists(path, existEc) || existEc) return {};

    std::string meta = MetaPath(path);
    AssetGuid guid = ReadMetaGuid(meta);
    if (!guid.IsValid()) {
        guid = AssetGuid::Generate();
        if (!WriteMetaFile(meta, guid, type)) {
            Log::Warn("AssetDatabase: failed to write .meta for '" + path + "'");
            return {};
        }
    }

    Register(path, guid);
    return guid;
}

AssetGuid GuidForPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_Mutex);
    auto it = g_PathToGuid.find(path);
    return it != g_PathToGuid.end() ? it->second : AssetGuid{};
}

std::string PathForGuid(AssetGuid guid) {
    std::lock_guard<std::mutex> lk(g_Mutex);
    auto it = g_GuidToPath.find(guid);
    return it != g_GuidToPath.end() ? it->second : std::string{};
}

std::string Resolve(AssetGuid guid, const std::string& fallbackPath) {
    if (guid.IsValid()) {
        std::string path = PathForGuid(guid);
        if (!path.empty()) return path;
    }
    return fallbackPath;
}

void NotifyMoved(const std::string& oldPath, const std::string& newPath) {
    if (oldPath == newPath) return;

    std::lock_guard<std::mutex> lk(g_Mutex);

    auto it = g_PathToGuid.find(oldPath);
    if (it == g_PathToGuid.end()) return;

    AssetGuid guid = it->second;
    g_PathToGuid.erase(it);
    g_GuidToPath.erase(guid);
    Register(newPath, guid);

    // Move the .meta sidecar alongside the renamed asset.
    std::error_code ec;
    std::filesystem::rename(MetaPath(oldPath), MetaPath(newPath), ec);
    if (ec) {
        Log::Warn("AssetDatabase: failed to move .meta from '" + oldPath + "' to '" + newPath + "'");
    }
}

void ScanProject() {
    namespace fs = std::filesystem;
    const std::string root = ProjectPaths::Root();
    const auto& known = KnownExtensions();

    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (ec) { ec.clear(); continue; }
        if (!entry.is_regular_file(ec)) { ec.clear(); continue; }

        std::string path = entry.path().lexically_normal().string();

        // Skip .meta files themselves and anything already scanned.
        if (path.size() > 5 && path.substr(path.size() - 5) == ".meta") continue;

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        if (known.find(ext) == known.end()) continue;

        EnsureGuid(path);
    }
}

std::string ReadMetaFields(const std::string& path) {
    if (IsSynthetic(path)) return "{}";
    std::lock_guard<std::mutex> lk(g_Mutex);
    json j = ReadMetaFull(MetaPath(path));
    return j.empty() ? "{}" : j.dump();
}

bool MergeMetaFields(const std::string& path, const std::string& fieldsJson) {
    if (IsSynthetic(path)) return false;

    json incoming;
    try { incoming = json::parse(fieldsJson); } catch (...) { return false; }
    if (!incoming.is_object()) return false;

    std::lock_guard<std::mutex> lk(g_Mutex);

    auto it = g_PathToGuid.find(path);
    if (it == g_PathToGuid.end()) return false;
    AssetGuid guid = it->second;

    std::string meta = MetaPath(path);
    json j = ReadMetaFull(meta);

    if (!j.contains("metaVersion")) j["metaVersion"] = 1;
    if (!j.contains("guid"))        j["guid"] = guid.ToString();
    if (!j.contains("type")) {
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        const auto& known = KnownExtensions();
        auto extIt = known.find(ext);
        j["type"] = (extIt != known.end()) ? extIt->second : std::string("asset");
    }

    for (auto& [key, val] : incoming.items()) j[key] = val;

    std::ofstream f(meta);
    if (!f.is_open()) return false;
    f << j.dump(2) << "\n";
    return f.good();
}

} // namespace AssetDatabase
