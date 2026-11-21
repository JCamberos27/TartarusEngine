// SceneSerializer's internals, shared by its files (SceneSerializer.cpp is the public API; SceneJsonCommon,
// SceneMaterialJson, SceneComponentJson, ScenePrefabOverrides, SceneBuild, SceneApply, SceneAssetLibraryJson
// hold the rest by topic). Not for use outside them.
#pragma once

#include "LayerRegistry.h"
#include "SceneSerializer.h"
#include "TerrainSystem.h"
#include "ComponentJson.h"
#include "AtomicFile.h"
#include "World.h"
#include "AssetLibrary.h"
#include "ComponentRegistry.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"
#include "MaterialAsset.h"
#include "SubstrateJson.h"
#include "AssetDatabase.h"
#include "AssetGuid.h"
#include "Core/Paths.h"
#include "GameShell.h"
#include "ProjectComponentMigration.h"

#include "Log.h"
#include "SpotShadowMap.h"
#include "PointShadowMap.h"
#include "OutfitSystem.h"

#include <json.hpp>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <optional>
#include <unordered_set>
#include <set>
#include <mutex>
#include <functional>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <cctype>
#include <glm/gtx/matrix_decompose.hpp>
#include <cmath>


using json = nlohmann::json;

namespace SceneDetail {


// Bumped whenever a scene-file change would make an older build misread a newer save (a field
// whose meaning changed, not merely a new optional field — those need no bump at all, since every
// read here already defaults gracefully when a key is absent). #195: scenes previously carried no
// version at all, so there's no way to tell "old build, old file" apart from "old build, file from
// a build that changed something incompatible" — this is the fix. Starting at 1 rather than 0 so
// that 0 unambiguously means "no formatVersion field was written" (a legacy pre-#195 file), not
// "written by version 0".
// v3 (#9, Phase M item 1): post-processing/shadow settings move from editor_prefs.json onto the
// scene itself (see World.h's ExposureEV et al.) — they're scene-authored content, not per-user
// editor prefs. One-way: a pre-v3 file has its values migrated forward from editor_prefs.json on
// load (see ApplySceneJson's MigratePostProcessSettingsFromPrefs call) and is immediately
// re-saved by SceneSerializer::Load(), after writing a ".bak" of the original.
// v4 (#123): every entity "rotation" changes from YXZ Euler degrees [x,y,z] to a normalized
// quaternion [x,y,z,w]. Older scenes are converted on load and immediately re-saved.
constexpr int kSceneFormatVersion = 4;

// Set by ApplySceneJson when the file being loaded declares a formatVersion newer than this build
// understands; read (and cleared) via SceneSerializer::TakeLoadWarning() so a caller like the
// editor can pop a dialog in addition to the Console line ApplySceneJson already logs. Plain
// (non-thread-local) static: scene loads happen on the main thread only.
extern std::string g_LastLoadWarning;

// Populated by ApplySceneJson during a pre-v3 migration; drained by SceneSerializer::Load() to
// log exactly what moved (review §7 Q13: "logs exactly what moved"). Plain static — same
// single-threaded assumption as g_LastLoadWarning above.
extern std::vector<std::string> g_MigrationLog;


// #302 Part B — the parsed .prefab content the scene's live instances were built from, by
// SourcePath: filled by InstantiatePrefab, read by the Inspector's per-field override check
// (every frame per visible field) and by the save-time override diff. Not the file as it is now:
// an instance built from the old content and diffed against an edited file would save every
// change the edit made as an override of its own, and never pick the edit up. The editor
// rebuilds the instances when the file changes (EditorLayer::RefreshPrefabInstances), which
// refills it. Cleared on full scene load (ApplySceneJson) and via ClearPrefabPristineCache().
extern std::unordered_map<std::string, json> g_prefabPristineCache;
// The cached content for `path`, read from disk when nothing is cached yet; null if unreadable.
const json* CachedPrefabJson(const std::string& path);


// A non-finite component anywhere in the scene (nan/inf slipped past the Inspector, or a
// corrupt file) serializes as JSON `null` / a bare `nan` token, neither of which reloads —
// the whole scene is then lost. Scrub to 0 at the one choke point every vector passes
// through, on both write and read, and say so in the Console (#34 D4).
inline float FiniteOr(float x, float fallback, const char* what) {
    if (std::isfinite(x)) return x;
    Log::Warn(std::string("Scene: non-finite ") + what + " value replaced with " + std::to_string(fallback));
    return fallback;
}

// Used across the files (defined in the file named by its topic).
json Vec3ToJson(const glm::vec3& v);
glm::vec3 JsonToVec3(const json& j, const glm::vec3& fallback = glm::vec3(0.0f));
json QuatToJson(const glm::quat& value);
void SetTransformRotationFromJson(TransformComponent& transform, const json& value,
                                  bool* migratedEuler = nullptr);
bool RotationJsonNearlyEqual(const json& a, const json& b);
std::string PathOrEmpty(const std::shared_ptr<Texture>& tex);
std::string AssetPathForWrite(const std::string& path);
// One scene build's asset reference answers, per path string: the written path
// (AssetPathForWrite) and the GUID string ("" when unregistered). A big scene places the same few
// models and materials thousands of times; each answer used to be worked out again (two locked
// map lookups and a key normalisation) for every placement.
struct AssetRefWriteCache {
    struct Ref { std::string WritePath, Guid; };
    const Ref& Get(const std::string& path);
private:
    std::unordered_map<std::string, Ref> m_Refs;
};
std::string AssetPathForRead(const std::string& path);
json PathRef(const std::string& path);
std::string ResolveAssetRef(const json& val);
std::string ResolveAssetRef(const json& obj, const char* pathKey, const char* guidKey);
std::shared_ptr<Texture> LoadIfPresent(AssetLibrary& assets, const json& obj, const char* key);
json MaterialSlotsToJson(const std::vector<std::shared_ptr<MaterialAsset>>& matSlots,
                         AssetRefWriteCache* refs = nullptr);
void WriteRendererFlags(json& obj, const RenderableComponent& rc);
void ReadRendererFlags(const json& obj, RenderableComponent& rc);
void ReadMaterialSlots(AssetLibrary& assets, const json& obj, RenderableComponent& rc);
json ReflectFieldToJson(const ReflectField& f, const void* fp);
void ReflectFieldFromJson(const ReflectField& f, void* fp, const json& v, AssetLibrary* assets);
bool ReflectJsonNearlyEqual(ReflectFieldType t, const json& a, const json& b);
json HierStyleToJson(const HierarchyStyleComponent& st);
void ReadHierStyle(const json& j, World& world, entt::entity entity);
void WriteCommonComponents(json& j, const World& world, entt::entity entity);
ColliderComponent ColliderFromJson(const json& c);
JointComponent JointFromJson(const json& jc);
void ReadCommonComponents(const json& j, World& world, AssetLibrary& assets, entt::entity entity);
void UpgradeLegacyBoxKeys(json& root);
std::vector<const json*> PrefabEntityObjects(const json& prefab);
void DiffPrefabEntity(const World& world, entt::entity live, const json& pristine,
                      int localIndex, bool isRoot,
                      const std::function<TransformComponent(entt::entity)>& effectiveTransform,
                      json& outOverrides);
void ApplyPrefabOverride(World& world, AssetLibrary& assets, entt::entity e,
                         const std::string& comp, const std::string& field, const json& v);
SkySettings SkySettingsFromJson(const json& j);
json BuildSceneJson(const World& world, const std::set<entt::entity>* only = nullptr,
                    bool flattenPrefabInstances = false);
void WarnNonPortablePaths(const json& node, const std::string& pointer);
bool ApplySceneJson(World& world, AssetLibrary& assets, const json& root,
    bool clearFirst = true, std::vector<entt::entity>* outCreated = nullptr,
    std::unordered_map<int, entt::entity>* outSourceOrder = nullptr);
void AppendAssetLibraryJson(json& root, const AssetLibrary& assets, bool includeAssetMetadata = false);
void ApplyAssetLibraryJson(AssetLibrary& assets, const json& root, bool fromUndoSnapshot = false,
                           std::vector<std::shared_ptr<void>>* hold = nullptr, bool writeMeta = true);

} // namespace SceneDetail
