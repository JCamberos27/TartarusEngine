#include "SceneSerializerInternal.h"

namespace SceneDetail {

std::string g_LastLoadWarning;
std::vector<std::string> g_MigrationLog;
std::unordered_map<std::string, json> g_prefabPristineCache;


json Vec3ToJson(const glm::vec3& v) {
    return json::array({FiniteOr(v.x, 0.0f, "vector"), FiniteOr(v.y, 0.0f, "vector"), FiniteOr(v.z, 0.0f, "vector")});
}

glm::vec3 JsonToVec3(const json& j, const glm::vec3& fallback) {
    if (!j.is_array() || j.size() != 3) return fallback;
    if (!j[0].is_number() || !j[1].is_number() || !j[2].is_number()) return fallback;
    return glm::vec3(FiniteOr(j[0].get<float>(), fallback.x, "vector"),
                     FiniteOr(j[1].get<float>(), fallback.y, "vector"),
                     FiniteOr(j[2].get<float>(), fallback.z, "vector"));
}

json QuatToJson(const glm::quat& value) {
    const glm::quat q = NormalizeRotation(value);
    return json::array({FiniteOr(q.x, 0.0f, "quaternion"),
                        FiniteOr(q.y, 0.0f, "quaternion"),
                        FiniteOr(q.z, 0.0f, "quaternion"),
                        FiniteOr(q.w, 1.0f, "quaternion")});
}

glm::quat JsonToQuat(const json& j) {
    if (!j.is_array() || j.size() != 4 ||
        !j[0].is_number() || !j[1].is_number() || !j[2].is_number() || !j[3].is_number())
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return NormalizeRotation(glm::quat(FiniteOr(j[3].get<float>(), 1.0f, "quaternion"),
                                       FiniteOr(j[0].get<float>(), 0.0f, "quaternion"),
                                       FiniteOr(j[1].get<float>(), 0.0f, "quaternion"),
                                       FiniteOr(j[2].get<float>(), 0.0f, "quaternion")));
}

// New data is a quaternion. A three-number value is a pre-v4 YXZ Euler rotation and is the
// load-time migration path for scenes, prefabs and clipboard fragments.
void SetTransformRotationFromJson(TransformComponent& transform, const json& value,
                                  bool* migratedEuler) {
    if (value.is_array() && value.size() == 4) {
        transform.SetRotationQuaternion(JsonToQuat(value));
        return;
    }
    if (value.is_array() && value.size() == 3) {
        transform.SetRotationEuler(JsonToVec3(value));
        if (migratedEuler) *migratedEuler = true;
        return;
    }
    transform.SetRotationQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
}

glm::quat RotationFromJson(const json& value) {
    if (value.is_array() && value.size() == 4) return JsonToQuat(value);
    if (value.is_array() && value.size() == 3) return QuaternionFromEulerYXZ(JsonToVec3(value));
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

bool RotationJsonNearlyEqual(const json& a, const json& b) {
    return SameRotation(RotationFromJson(a), RotationFromJson(b), 1e-5f);
}

std::string PathOrEmpty(const std::shared_ptr<Texture>& tex) {
    return tex ? tex->Path() : std::string();
}

// --- Portable asset paths (audit #364 / BUG-101) -----------------------------------------
// Serialized scenes must not carry machine-absolute paths, or they break on any other machine
// or checkout location. On WRITE, a path under the project root becomes a '/'-normalised
// project-relative path; a path outside the project tree (a shared asset library elsewhere) is
// left absolute rather than turned into a pile of "../..". On READ, a relative path resolves
// against the current project root, and a legacy absolute path that no longer exists is rebased
// by matching its longest tail that does exist under this project root.
// A synthetic reference, not a filesystem path: World's "primitive://..." level-geometry ids,
// or any other "scheme:"-prefixed id. Never rewritten on the way in or out.
bool IsSyntheticRef(const std::string& p) {
    if (p.rfind("primitive:", 0) == 0) return true;
    // scheme:// with a multi-character scheme (a lone "C:" drive letter is not one).
    auto colon = p.find(':');
    if (colon != std::string::npos && colon >= 2) {
        bool alnum = true;
        for (size_t i = 0; i < colon; ++i) if (!std::isalnum((unsigned char)p[i])) { alnum = false; break; }
        if (alnum && colon + 1 < p.size() && p[colon + 1] == '/') return true;
    }
    return false;
}

std::string AssetPathForWriteUncached(const std::string& path, const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::path(path), ec);
    if (ec) abs = fs::path(path);
    fs::path rel = abs.lexically_relative(root);
    if (rel.empty() || rel.begin() == rel.end()) return path;
    if (rel.begin()->string() == "..") return path; // outside the project tree
    return rel.generic_string();
}

const AssetRefWriteCache::Ref& AssetRefWriteCache::Get(const std::string& path) {
    auto it = m_Refs.find(path);
    if (it != m_Refs.end()) return it->second;
    Ref r;
    r.WritePath = AssetPathForWrite(path);
    if (const AssetGuid g = AssetDatabase::GuidForPath(path); g.IsValid()) r.Guid = g.ToString();
    return m_Refs.emplace(path, std::move(r)).first->second;
}

// weakly_canonical opens each path on disk (CreateFile + GetFinalPathNameByHandle), and every undo
// snapshot writes every asset reference in the scene: in a big scene that was most of the time of
// each edit's commit. The answers are memoized per project root; a path string's answer only moves
// if a folder on it is renamed by case alone, and a renamed asset gets a new path string anyway.
std::string AssetPathForWrite(const std::string& path) {
    if (path.empty() || IsSyntheticRef(path)) return path;
    namespace fs = std::filesystem;
    static std::mutex mutex;
    static std::string cachedRootKey;
    static fs::path cachedRoot;
    static bool cachedRootOk = false;
    static std::unordered_map<std::string, std::string> cache;
    std::lock_guard<std::mutex> lock(mutex);
    const std::string& rootKey = ProjectPaths::Root();
    if (rootKey != cachedRootKey || cachedRootKey.empty()) {
        std::error_code ec;
        cachedRootKey = rootKey;
        cachedRoot = fs::weakly_canonical(fs::path(rootKey), ec);
        cachedRootOk = !ec;
        cache.clear();
    }
    if (!cachedRootOk) return path;
    if (auto it = cache.find(path); it != cache.end()) return it->second;
    if (cache.size() > 65536) cache.clear();
    return cache.emplace(path, AssetPathForWriteUncached(path, cachedRoot)).first->second;
}

std::string AssetPathForRead(const std::string& path) {
    if (path.empty() || IsSyntheticRef(path)) return path;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(path);
    fs::path root(ProjectPaths::Root());
    if (p.is_relative())
        return (root / p).lexically_normal().string();
    if (fs::exists(p, ec)) return path;
    // Legacy absolute path authored elsewhere: try each suffix of it under this project root.
    std::vector<fs::path> parts(p.begin(), p.end());
    for (size_t i = 0; i < parts.size(); ++i) {
        fs::path tail;
        for (size_t j = i; j < parts.size(); ++j) tail /= parts[j];
        fs::path cand = (root / tail).lexically_normal();
        if (fs::exists(cand, ec)) return cand.string();
    }
    return path; // unresolved — downstream logs a concrete "not found"
}

// PR 2 (#333): path string → {"path":"...", "pathGuid":"..."} for WRITE. Returns a plain
// string if the GUID isn't known yet so that v1-era paths still round-trip cleanly.
// The stored path is always project-relative when possible (audit #364).
json PathRef(const std::string& path) {
    if (path.empty()) return path;
    std::string stored = AssetPathForWrite(path);
    AssetGuid g = AssetDatabase::GuidForPath(path); // GUID lookup needs the real (absolute) path
    if (g.IsValid()) return json{{"path", stored}, {"pathGuid", g.ToString()}};
    return stored;
}

// PR 2 (#333): dual path+guid READ. Prefers GUID if present and resolvable, falls back to
// the plain path. Accepts both the v2 {"path","pathGuid"} object and the v1 plain string.
// Same result as AssetDatabase::Resolve(guid, AssetPathForRead(path)), but the stored path is only
// normalised when the GUID doesn't resolve (most refs carry a known GUID; a scene load does this
// per model and per material slot).
static std::string ResolveGuidOrPath(const json& obj, const char* pathKey, const char* guidKey) {
    if (const AssetGuid g = AssetGuid::FromString(obj.value(guidKey, std::string())); g.IsValid())
        if (std::string path = AssetDatabase::PathForGuid(g); !path.empty()) return path;
    return AssetPathForRead(obj.value(pathKey, std::string()));
}
std::string ResolveAssetRef(const json& val) {
    if (val.is_string()) return AssetPathForRead(val.get<std::string>());
    if (!val.is_object()) return {};
    return ResolveGuidOrPath(val, "path", "pathGuid");
}
// Overload for objects that carry the path and guid as sibling string keys.
std::string ResolveAssetRef(const json& obj, const char* pathKey, const char* guidKey) {
    return ResolveGuidOrPath(obj, pathKey, guidKey);
}

std::shared_ptr<Texture> LoadIfPresent(AssetLibrary& assets, const json& obj, const char* key) {
    if (!obj.contains(key)) return nullptr;
    std::string path = ResolveAssetRef(obj[key]);
    if (path.empty()) return nullptr;
    return assets.LoadTexture(path);
}

} // namespace SceneDetail
