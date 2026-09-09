#pragma once
#include "AssetGuid.h"
#include <string>

// Host-only asset identity layer. Maintains a GUID ↔ path bimap backed by .meta sidecar
// files committed alongside each asset. AssetLibrary stays path-keyed; this namespace sits
// beside it and provides stable identity across renames/moves (PR 2+).
//
// Call order:
//   1. AssetDatabase::ScanProject()  — once, after ProjectSettings::Load(), before any Load*
//   2. AssetDatabase::EnsureGuid()   — called by every AssetLibrary::Load* and ImportDroppedFile
//   3. AssetDatabase::NotifyMoved()  — called by CommitRename / any file-rename path in the editor
namespace AssetDatabase {

// Walks ProjectPaths::Root() and calls EnsureGuid on every file with a known asset extension,
// creating missing .meta files. Safe to call multiple times (idempotent per path).
void ScanProject();

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

} // namespace AssetDatabase
