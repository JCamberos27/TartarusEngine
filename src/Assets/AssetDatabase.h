#pragma once
#include "AssetGuid.h"
#include <string>
#include <vector>

// Host-only asset identity layer. Maintains a GUID ↔ path bimap backed by .meta sidecar
// files committed alongside each asset. AssetLibrary stays path-keyed; this namespace sits
// beside it and provides stable identity across renames/moves (PR 2+).
//
// Call order:
//   1. AssetDatabase::BeginScanProject() — at start-up (#132: on a worker thread; every lookup
//      waits for it). ScanProject() is the synchronous form (--build, a watcher overflow).
//   2. AssetDatabase::EnsureGuid()   — called by every AssetLibrary::Load* and ImportDroppedFile
//   3. AssetDatabase::NotifyMoved()  — whenever a file moves (EditorLayer::SyncProjectChanges
//      applies moves made outside the editor, reported by ProjectWatcher)
//
// Tracked (#132): textures, models, audio, prefabs, materials, HDRIs, shaders and their GLSL
// sources, Animator Controllers, scripts, and scenes (.json under project/scenes). References to
// them are stored as path + GUID: scenes (asset fields, models, prefabs, sky, clip / controller /
// script paths), .mat textures and shaders, .controller clips, the Build Settings scene list and
// the editor's last-opened scene - so a rename or move outside the editor keeps every link.
namespace AssetDatabase {

// Walks ProjectPaths::Root() and calls EnsureGuid on every file with a known asset extension,
// creating missing .meta files. Safe to call multiple times (idempotent per path).
void ScanProject();

// #132 - ScanProject on a worker thread, so a big project's scan overlaps window / GL / shader
// start-up instead of blocking it. Every lookup below (from another thread) waits for the scan
// to finish, so callers never see a half-registered project. WaitForScan blocks explicitly.
void BeginScanProject();
void WaitForScan();
bool IsScanning();

// The asset type a path would be registered as ("texture", "model", "scene", ...), or "" when
// it isn't an asset (unknown extension, or a .json outside project/scenes).
std::string AssetType(const std::string& path);

// The canonical spelling of a path used for comparisons: absolute, normalised, '/' separators,
// lower-case on Windows. Two spellings of one file compare equal.
std::string PathKey(const std::string& path);

// Drops `path` from the GUID maps (its file was deleted outside the editor). Leaves any .meta
// on disk alone; the next scan prunes it if the asset really is gone.
void ForgetPath(const std::string& path);

// #132 - references stored as project-relative path strings, optionally with a "#name" suffix
// (an animation clip inside a model file). RefGuid: the GUID of the referenced file as a string,
// or "" when it isn't a registered file. FollowRef: `ref` itself while its file exists, else its
// file's new project-relative location (suffix kept) when `guid` resolves, else `ref` unchanged.
std::string RefGuid(const std::string& ref);
std::string FollowRef(const std::string& ref, const std::string& guid);

// Returns the GUID for `path`, creating a .meta sidecar if one doesn't exist yet.
// Returns an invalid guid for synthetic paths (IsSynthetic returns true).
AssetGuid EnsureGuid(const std::string& path);

// Looks up the cached GUID for `path`. Returns an invalid guid if the path hasn't been
// registered yet (call EnsureGuid to register).
AssetGuid GuidForPath(const std::string& path);

// Looks up the path for a GUID. Returns an empty string if unknown.
std::string PathForGuid(AssetGuid guid);

// If `guid` is valid and known, returns PathForGuid(guid). Otherwise returns `fallbackPath`.
// Use in serializer read paths: GUID wins if resolvable, falls back to path, no assert.
std::string Resolve(AssetGuid guid, const std::string& fallbackPath);

// Updates the GUID ↔ path bimap and renames the .meta sidecar to track the new path.
// Call whenever a real file on disk is renamed or moved. No-op if oldPath isn't registered.
void NotifyMoved(const std::string& oldPath, const std::string& newPath);

// Returns true for synthetic paths like "primitive://cube" that represent generated geometry
// and must never receive a .meta file or a GUID.
bool IsSynthetic(const std::string& path);

// Returns the full content of the .meta sidecar for `path` as a JSON-object string, or "{}"
// on any error (file missing, parse failure, synthetic path). Includes guid/type/metaVersion
// plus any extra fields written by MergeMetaFields. Thread-safe.
std::string ReadMetaFields(const std::string& path);

// Merges `fieldsJson` (a JSON-object string, e.g. {"importer":{"isSRGB":true}}) into the
// .meta sidecar for `path`, preserving guid/type/metaVersion. `path` must already be
// registered via EnsureGuid or ScanProject. Returns false on I/O error or unregistered path.
// Silently no-ops for synthetic paths. Thread-safe.
bool MergeMetaFields(const std::string& path, const std::string& fieldsJson);

} // namespace AssetDatabase
