#include "AssetDatabase.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "EnginePaths.h"
#include "AtomicFile.h"
#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <cctype>
#include <sstream>
#include <regex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
    // #131 — case-insensitive: a hand-edited or tool-written uppercase GUID used to read as
    // invalid, so the asset got a brand-new GUID and every reference to it broke.
    for (char c : s) {
        if (!std::isxdigit((unsigned char)c)) return {};
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

// #131 — last resort for a .meta that no longer parses as JSON (merge conflict markers, a
// truncated write, a stray edit): pull the first "guid": "<16 hex>" out of the raw text, so
// the asset keeps its identity instead of being handed a new GUID.
AssetGuid SalvageMetaGuid(const std::string& metaPath) {
    std::ifstream f(metaPath, std::ios::binary);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    static const std::regex kGuid("\"guid\"\\s*:\\s*\"([0-9a-fA-F]{16})\"");
    std::smatch m;
    const std::string text = ss.str();
    if (!std::regex_search(text, m, kGuid)) return {};
    return AssetGuid::FromString(m[1].str());
}

// Writes a new .meta sidecar. Returns false on I/O error.
bool WriteMetaFile(const std::string& metaPath, AssetGuid guid, const std::string& type) {
    json j;
    j["metaVersion"] = 1;
    j["guid"] = guid.ToString();
    j["type"] = type;
    // Atomic: a crash mid-write must not truncate a .meta — the asset would lose its GUID and
    // every scene reference to it would break (audit CPP-206).
    return AtomicFile::WriteJson(metaPath, j);
}

// #132 — the path-map key: absolute, lexically normalised, '/' separators, and lower-case on
// Windows (case-insensitive filesystem). Without it "project/a.png", "project\\a.png" and
// "Project/A.png" were three different keys for one file, so GuidForPath missed and one asset
// could be registered several times. Lexical only (no filesystem hit), so it stays cheap on
// hot lookups. g_GuidToPath keeps the caller's own spelling for display/IO.
std::string Key(const std::string& path) {
    std::error_code ec;
    std::string k = std::filesystem::absolute(path, ec).lexically_normal().generic_string();
    if (ec || k.empty()) k = std::filesystem::path(path).lexically_normal().generic_string();
#ifdef _WIN32
    for (char& c : k) c = (char)std::tolower((unsigned char)c);
#endif
    return k;
}

// Registers guid ↔ path in both maps (caller holds g_Mutex).
void Register(const std::string& path, AssetGuid guid) {
    g_PathToGuid[Key(path)] = guid;
    g_GuidToPath[guid] = path;
}

// File creation time as a comparable tick count, or 0 if unknown. A copy made in Explorer
// keeps the source's modified time but gets a fresh creation time, so this is what tells
// the original from the copy (#130).
unsigned long long CreationTime(const std::string& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(std::filesystem::path(path).wstring().c_str(), GetFileExInfoStandard, &data))
        return 0;
    return ((unsigned long long)data.ftCreationTime.dwHighDateTime << 32) | data.ftCreationTime.dwLowDateTime;
#else
    (void)path;
    return 0;
#endif
}

// Gives `path` a brand-new GUID and rewrites its .meta, keeping every other field (import
// settings etc.). Returns the new GUID, or an invalid one on I/O failure.
AssetGuid RegenerateGuid(const std::string& path) {
    const std::string meta = MetaPath(path);
    json j = ReadMetaFull(meta);
    AssetGuid guid;
    do { guid = AssetGuid::Generate(); } while (g_GuidToPath.count(guid));
    j["guid"] = guid.ToString();
    if (!j.contains("metaVersion")) j["metaVersion"] = 1;
    if (!AtomicFile::WriteJson(meta, j)) return {};
    return guid;
}

// #130 — `path`'s .meta holds `guid`, which may already belong to another registered file
// (an asset copied together with its .meta). Resolves the clash the way Unity does: the
// original keeps the GUID and the copy is re-issued a new one, with a warning. Returns the
// GUID `path` should be registered under (caller holds g_Mutex).
AssetGuid ResolveDuplicateGuid(const std::string& path, AssetGuid guid) {
    namespace fs = std::filesystem;
    auto it = g_GuidToPath.find(guid);
    if (it == g_GuidToPath.end() || Key(it->second) == Key(path)) return guid;
    const std::string other = it->second;

    std::error_code ec;
    if (!fs::exists(other, ec) || ec || ReadMetaGuid(MetaPath(other)) != guid) {
        // The previous owner is gone or no longer claims this GUID: the file was moved or
        // renamed outside the editor. Follow it rather than treating it as a copy.
        g_PathToGuid.erase(Key(other));
        return guid;
    }
    if (fs::equivalent(other, path, ec) && !ec) {
        // Same file under a different spelling of its path — an alias, not a copy.
        g_PathToGuid[Key(path)] = guid;
        return guid;
    }

    // A real duplicate. The file created first is the original; if creation times are
    // unavailable or equal, the one already registered keeps the GUID.
    const unsigned long long tPath = CreationTime(path), tOther = CreationTime(other);
    const bool pathIsOriginal = tPath && tOther && tPath < tOther;
    const std::string& copy = pathIsOriginal ? other : path;
    const std::string& original = pathIsOriginal ? path : other;

    const AssetGuid fresh = RegenerateGuid(copy);
    if (!fresh.IsValid()) {
        Log::Error("AssetDatabase: '" + copy + "' has the same GUID " + guid.ToString() + " as '" + original +
                   "' and its .meta could not be rewritten. References by GUID may resolve to the wrong asset.");
        return pathIsOriginal ? guid : AssetGuid{};
    }
    Log::Warn("AssetDatabase: '" + copy + "' had the same GUID as '" + original +
              "' (copied together with its .meta?). Assigned it a new GUID " + fresh.ToString() +
              "; '" + original + "' keeps " + guid.ToString() + ".");

    if (!pathIsOriginal) return fresh;
    // The already-registered file turned out to be the copy: move it to its new GUID.
    Register(other, fresh);
    return guid;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace {
bool IsUnderRoot(const std::string& path, const std::string& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string r = fs::weakly_canonical(fs::absolute(root, ec), ec).generic_string();
    const std::string p = fs::weakly_canonical(fs::absolute(path, ec), ec).generic_string();
    if (r.empty() || p.size() <= r.size()) return false;
#ifdef _WIN32
    auto lower = [](std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    return lower(p).rfind(lower(r) + "/", 0) == 0;
#else
    return p.rfind(r + "/", 0) == 0;
#endif
}
} // namespace

bool IsSynthetic(const std::string& path) {
    return path.rfind("primitive://", 0) == 0;
}

AssetGuid EnsureGuid(const std::string& path) {
    if (IsSynthetic(path)) return {};

    std::lock_guard<std::mutex> lk(g_Mutex);

    auto it = g_PathToGuid.find(Key(path));
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
    std::error_code metaEc;
    if (!guid.IsValid() && std::filesystem::exists(meta, metaEc)) {
        // #131 — the sidecar exists but is unreadable. Never overwrite it blindly: keep a copy,
        // try to recover its GUID, and say so loudly.
        const std::string bak = meta + ".bak";
        std::filesystem::copy_file(meta, bak, std::filesystem::copy_options::overwrite_existing, metaEc);
        guid = SalvageMetaGuid(meta);
        if (guid.IsValid()) {
            Log::Error("AssetDatabase: '" + meta + "' is corrupt; recovered its GUID " + guid.ToString() +
                       " and rewrote it (import settings reset; original kept as " + bak + ").");
            if (!WriteMetaFile(meta, guid, type)) Log::Warn("AssetDatabase: failed to rewrite '" + meta + "'");
            guid = ResolveDuplicateGuid(path, guid);
            if (!guid.IsValid()) return {};
            Register(path, guid);
            return guid;
        }
        Log::Error("AssetDatabase: '" + meta + "' is corrupt and has no recoverable GUID - assigning a new "
                   "one. References to this asset by GUID will break; the original is kept as " + bak + ".");
    }
    if (!guid.IsValid()) {
        // #125 — never litter a folder outside the project (Downloads, another repo, read-only
        // media) with .meta sidecars. Such a file is referenced by path only; importing copies
        // it into the project first, where it gets a GUID.
        if (!IsUnderRoot(path, ProjectPaths::Root()) && !IsUnderRoot(path, EnginePaths::Resolve("assets")))
            return {};
        guid = AssetGuid::Generate();
        if (!WriteMetaFile(meta, guid, type)) {
            Log::Warn("AssetDatabase: failed to write .meta for '" + path + "'");
            return {};
        }
    } else {
        guid = ResolveDuplicateGuid(path, guid);
        if (!guid.IsValid()) return {};
    }

    Register(path, guid);
    return guid;
}

AssetGuid GuidForPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_Mutex);
    auto it = g_PathToGuid.find(Key(path));
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

    auto it = g_PathToGuid.find(Key(oldPath));
    if (it == g_PathToGuid.end()) return;
    const AssetGuid guid = it->second;

    // #132 — move the .meta sidecar FIRST and only re-key the maps if that worked (or there was
    // no sidecar to move). Updating the maps regardless left the in-memory GUID at the new path
    // while its .meta stayed behind at the old one, so the next launch minted a fresh GUID.
    std::error_code ec;
    const std::string oldMeta = MetaPath(oldPath);
    if (std::filesystem::exists(oldMeta, ec)) {
        std::filesystem::rename(oldMeta, MetaPath(newPath), ec);
        if (ec) {
            Log::Error("AssetDatabase: failed to move .meta from '" + oldPath + "' to '" + newPath +
                       "' (" + ec.message() + ") - the asset keeps its GUID only until the editor restarts.");
            return;
        }
    }
    g_PathToGuid.erase(it);
    g_GuidToPath.erase(guid);
    Register(newPath, guid);
}

void ScanProject() {
    namespace fs = std::filesystem;
    const std::string root = ProjectPaths::Root();
    const auto& known = KnownExtensions();

    std::vector<std::string> orphanMetas;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& entry = *it;
        // #133 / #132 — Library/ holds caches (thumbnails etc.) and screenshots/ holds editor
        // captures, not assets. Scanning them gave every cached thumbnail and every screenshot
        // its own .meta and GUID. (A screenshot used as a texture still gets one on first use.)
        if (it.depth() == 0 && entry.is_directory(ec)) {
            const std::string dir = entry.path().filename().string();
            if (dir == "Library" || dir == "screenshots") {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (!entry.is_regular_file(ec)) { ec.clear(); continue; }

        std::string path = entry.path().lexically_normal().string();

        // .meta files aren't assets. #132 — one whose asset is gone (deleted outside the editor)
        // is an orphan; collected here and pruned below.
        if (path.size() > 5 && path.substr(path.size() - 5) == ".meta") {
            const std::string assetPath = path.substr(0, path.size() - 5);
            std::string assetExt = fs::path(assetPath).extension().string();
            std::transform(assetExt.begin(), assetExt.end(), assetExt.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });
            std::error_code existEc;
            if (known.count(assetExt) && !fs::exists(assetPath, existEc) && !existEc)
                orphanMetas.push_back(path);
            continue;
        }

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        if (known.find(ext) == known.end()) continue;

        EnsureGuid(path);
    }

    // Unity deletes a .meta whose asset no longer exists; do the same, but say which ones.
    for (const std::string& meta : orphanMetas) {
        std::error_code rmEc;
        if (fs::remove(meta, rmEc))
            Log::Info("AssetDatabase: removed orphaned '" + ProjectPaths::Relativize(meta) + "' (its asset no longer exists).");
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

    auto it = g_PathToGuid.find(Key(path));
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

    return AtomicFile::WriteJson(meta, j); // atomic — see WriteMetaFile (audit CPP-206)
}

} // namespace AssetDatabase
