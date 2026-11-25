#include "SceneSerializerInternal.h"
#include "SceneFileJson.h"
#include "SceneSnapshotJson.h"

#include <chrono>
#include <stdexcept>

using namespace SceneDetail;


bool SceneSerializer::Save(const World& world, const AssetLibrary& assets, const std::string& path) {
    const std::function<bool()> write = PrepareSave(world, assets, path);
    return write && write();
}

std::function<bool()> SceneSerializer::PrepareSave(const World& world, const AssetLibrary& assets, const std::string& path) {
    // #82 - build the whole document in memory FIRST, and only then touch the file, through
    // AtomicFile (temp + flush + atomic replace). The old version opened a truncating ofstream
    // before building, so any exception while building (e.g. a corrupt prefab) left the scene
    // file empty; and it never checked the write, so a full disk / locked file still reported
    // success.
    auto root = std::make_shared<json>();
    try {
        *root = BuildSceneJson(world);
        AppendAssetLibraryJson(*root, assets);
        WarnNonPortablePaths(*root, ""); // audit #364 - surface any absolute/'..' path before it's written
    } catch (const std::exception& e) {
        Log::Error("Scene: couldn't serialize the scene for '" + path + "' (file left untouched): " + e.what());
        return {};
    }
    // The tree is the scene's own copy: formatting and writing it no longer touch the World.
    return [root, path]() {
        std::string text;
        try {
            text = root->dump(2);
        } catch (const std::exception& e) { // e.g. invalid UTF-8 in a name
            Log::Error("Scene: couldn't format the scene for '" + path + "' (file left untouched): " + e.what());
            return false;
        }
        if (!AtomicFile::WriteBytes(std::filesystem::u8path(path), text, /*binary=*/false)) {
            Log::Error("Scene: failed to write '" + path + "' - the previous version on disk was kept.");
            return false;
        }
        return true;
    };
}

std::string SceneSerializer::TakeLoadWarning() {
    std::string warning = std::move(g_LastLoadWarning);
    g_LastLoadWarning.clear();
    return warning;
}

namespace {
// The file's size and write time, as ReadSceneFile records them; false when it can't be stat'ed.
bool StatSceneFile(const std::string& path, std::uintmax_t& size, std::filesystem::file_time_type& time) {
    std::error_code ec;
    const auto p = std::filesystem::u8path(path);
    size = std::filesystem::file_size(p, ec);
    if (ec) return false;
    time = std::filesystem::last_write_time(p, ec);
    return !ec;
}
} // namespace

std::shared_ptr<SceneSerializer::ParsedSceneFile> SceneSerializer::ReadSceneFile(const std::string& path) {
    auto out = std::make_shared<ParsedSceneFile>();
    // Stat before reading: a write that lands during the read changes the time, and Load re-reads.
    if (!StatSceneFile(path, out->Size, out->WriteTime)) return nullptr;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f.is_open()) return nullptr;
    out->Root = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (out->Root.is_discarded()) return nullptr;
    return out;
}

bool SceneSerializer::Load(World& world, AssetLibrary& assets, const std::string& path, bool persistMigration,
                           const ParsedSceneFile* parsed) {
    const auto loadStart = std::chrono::steady_clock::now();
    if (parsed) {
        std::uintmax_t size = 0;
        std::filesystem::file_time_type time{};
        if (!StatSceneFile(path, size, time) || size != parsed->Size || time != parsed->WriteTime) parsed = nullptr;
    }
    std::ifstream in;
    if (!parsed) {
        in.open(std::filesystem::u8path(path));
        if (!in.is_open()) return false; // (before anything touches the world: a missing file leaves it as it was)
    }
    OutfitSystem::CancelPending(world); // the registry is about to be replaced
    struct SyncActiveOnExit { World& W; ~SyncActiveOnExit() { W.SyncActiveInHierarchy(); } } syncActive{world}; // #201

    json read;
    if (!parsed) {
        try {
            in >> read;
        } catch (const std::exception& e) {
            Log::Error("Scene: failed to parse '" + path + "': " + e.what());
            return false;
        }
        // Release the file now: the migration write-back below replaces `path` atomically, and on
        // Windows that fails with "Access is denied" while this stream still holds it open.
        in.close();
    }
    const json& root = parsed ? parsed->Root : read;
    // Valid JSON but the wrong shape (e.g. a bare `[]` or a number) parses fine above but throws
    // a json::type_error the moment anything below calls .value()/.contains() on it, since those
    // require an object. Reject it here as a load failure — same contract as a parse error —
    // instead of letting that exception escape uncaught all the way to main()'s outermost catch
    // and take down the whole process/--smoke-test batch over one bad scene (audit #76).
    if (!root.is_object()) {
        Log::Error("Scene: '" + path + "' is valid JSON but not a scene object (top-level type is " +
                   std::string(root.type_name()) + ").");
        return false;
    }

    // #9, Phase M item 1: one .bak of the whole file before a pre-v3 load mutates anything in
    // memory, per the review's Q13 "one backup" requirement. .json.bak is already in .gitignore.
    // Skipped when the caller won't persist the migration either (audit #77) — no write-back
    // coming means there's nothing for the backup to protect against.
    const int formatVersion = root.value("formatVersion", 0);
    const bool migrating = formatVersion < kSceneFormatVersion;
    if (migrating && persistMigration) {
        std::error_code ec;
        std::filesystem::copy_file(std::filesystem::u8path(path), std::filesystem::u8path(path + ".bak"), std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) Log::Warn("Scene: couldn't write a backup of '" + path + "' before migrating it: " + ec.message());
    }

    const AssetLibrary::CacheKeys cacheBefore = assets.SnapshotCacheKeys();
    g_MigrationLog.clear();
    // #83 — keep the current world so a malformed file can't leave a half-loaded one behind.
    std::string previous;
    try { previous = SaveToString(world, assets); }
    catch (...) { Log::Warn("Scene: couldn't snapshot the current world before loading '" + path + "'; a failed load can't restore it."); }
    bool ok = true;
    std::vector<std::shared_ptr<void>> libraryHold; // the file's own library entries: never released below
    try {
        ApplyAssetLibraryJson(assets, root, false, &libraryHold, /*writeMeta=*/persistMigration); // before ApplySceneJson: library assets should exist first
    } catch (const std::exception& e) {
        Log::Error("Scene: asset library data in '" + path + "' is malformed: " + e.what());
        ok = false;
    }
    if (ok) ok = ApplySceneJson(world, assets, root);
    if (!ok) {
        Log::Error("Scene: '" + path + "' could not be loaded; the previous scene was kept.");
        if (!previous.empty() && !LoadFromString(world, assets, previous))
            Log::Error("Scene: restoring the previous scene failed too; the world may be incomplete.");
        return false;
    }

    {
        std::set<std::string> keepModels;
        for (auto [e, rc] : world.Registry.view<RenderableComponent>().each())
            if (rc.ModelRef) keepModels.insert(rc.ModelRef->Path());
        const int dropped = assets.ReleaseUnreferenced(cacheBefore, keepModels);
        if (dropped) Log::Info("Scene: released " + std::to_string(dropped) + " cached asset(s) the new scene doesn't use.");
    }

    if (migrating && !g_MigrationLog.empty()) {
        Log::Info((persistMigration ? "Scene: upgraded '" : "Scene: would upgrade (not persisting) '") + path +
                   "' to format v" + std::to_string(kSceneFormatVersion) + ":");
        for (const auto& line : g_MigrationLog) Log::Info("  - " + line);
        if (persistMigration) {
            // Write the upgrade back immediately (review §7 Q13: "one build upgrade" moves the
            // project in one pass) so a second load doesn't re-read the now-stale prefs values.
            Save(world, assets, path);
        }
        // persistMigration == false: the migrated data lives only in `world`/`assets` for this
        // run (a --smoke-test scene) — `path` on disk is never touched.
    }
    char took[64];
    std::snprintf(took, sizeof(took), "%.0f ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count());
    Log::Info("Scene: loaded '" + std::filesystem::u8path(path).filename().u8string() + "' in " + took + ".");
    return ok;
}

std::string SceneSerializer::SaveToString(const World& world) {
    return BuildSceneJson(world).dump();
}

bool SceneSerializer::SaveSnapshotToFile(const std::string& entitySnapshot, const AssetLibrary& assets,
                                         const std::string& path) {
    std::string text;
    try {
        json root = json::parse(entitySnapshot);
        if (!root.is_object()) return false;
        AppendAssetLibraryJson(root, assets);
        text = root.dump(2);
    } catch (const std::exception& e) {
        Log::Error("Scene: couldn't build the snapshot for '" + path + "': " + e.what());
        return false;
    }
    return AtomicFile::WriteBytes(std::filesystem::u8path(path), text, /*binary=*/false);
}

namespace {
// The keys SaveToString(world, assets) adds on top of the scene: its own marker and every key
// AppendAssetLibraryJson writes.
constexpr const char* kLibraryKeys[] = {"hasAssetLibrarySnapshot", "libraryModels", "libraryTextures", "libraryMaterials",
                                        "librarySounds", "libraryPrefabs", "assetFolders", "removedDiskFolders", "assetMeta"};
} // namespace

bool SceneSerializer::SameSceneContent(const std::string& a, const std::string& b) {
    if (a == b) return true;
    try {
        json ja = json::parse(a), jb = json::parse(b);
        for (const char* k : kLibraryKeys) { ja.erase(k); jb.erase(k); }
        return ja == jb;
    } catch (const std::exception&) {
        return false;
    }
}

namespace {
// #15 - takes out of a SaveToString snapshot what the outfit system builds: every Outfit Piece and
// what's under it, and each Character Outfit's gender and race (set from the pieces' request). The
// ids left are numbered again in the order written, as SaveToString numbers them, so an entity
// written after a piece that came or went still compares equal.
void StripOutfits(json& scene) {
    constexpr const char* kEntityArrays[] = {"boxes", "models", "empties", "prefabInstances"};
    auto entries = [&](auto&& each) {
        for (const char* k : kEntityArrays)
            if (auto it = scene.find(k); it != scene.end() && it->is_array())
                for (auto& e : *it)
                    if (e.is_object()) each(e);
    };
    std::set<int> dropped;
    // A child can be written before its parent: go round until nothing more joins.
    for (bool grew = true; grew;) {
        grew = false;
        entries([&](const json& e) {
            const int id = e.value("id", -1);
            if (dropped.count(id)) return;
            if (e.contains("Outfit Piece") || dropped.count(e.value("parentId", -1))) { dropped.insert(id); grew = true; }
        });
    }
    std::unordered_map<int, int> renumbered;
    int next = 0;
    for (const char* k : kEntityArrays) {
        auto it = scene.find(k);
        if (it == scene.end() || !it->is_array()) continue;
        json kept = json::array();
        for (auto& e : *it) {
            if (e.is_object() && dropped.count(e.value("id", -1))) continue;
            if (e.is_object()) renumbered[e.value("id", -1)] = next++;
            kept.push_back(std::move(e));
        }
        *it = std::move(kept);
    }
    entries([&](json& e) {
        e["id"] = renumbered[e.value("id", -1)];
        const auto parent = renumbered.find(e.value("parentId", -1));
        e["parentId"] = parent != renumbered.end() ? parent->second : -1;
        if (auto outfit = e.find("Character Outfit"); outfit != e.end() && outfit->is_object()) {
            outfit->erase("Gender");
            outfit->erase("Race");
        }
    });
}
} // namespace

bool SceneSerializer::SameSceneContentApartFromOutfits(const std::string& a, const std::string& b) {
    if (a == b) return true;
    try {
        json ja = json::parse(a), jb = json::parse(b);
        for (const char* k : kLibraryKeys) { ja.erase(k); jb.erase(k); }
        StripOutfits(ja);
        StripOutfits(jb);
        return ja == jb;
    } catch (const std::exception&) {
        return false;
    }
}

std::string SceneSerializer::WithLibraryState(const std::string& scene, const std::string& library) {
    try {
        json out = json::parse(scene);
        const json lib = json::parse(library);
        for (const char* k : kLibraryKeys) {
            out.erase(k);
            if (const auto it = lib.find(k); it != lib.end()) out[k] = *it;
        }
        return out.dump();
    } catch (const std::exception&) {
        return {};
    }
}

// The keys SaveToString(world, assets) writes that Save() doesn't: the undo snapshot's marker and the
// AssetLibrary state that lives in .meta files / the session (AppendAssetLibraryJson's metadata half).
// UnitTests_Editor checks that a snapshot stripped of these and given Save()'s library half is
// Save()'s output byte for byte, with every optional key present, so a new undo-only key fails there.
constexpr const char* kUndoOnlyKeys[] = {"hasAssetLibrarySnapshot", "libraryModels", "libraryTextures",
                                         "assetFolders", "removedDiskFolders", "assetMeta"};

std::function<bool()> SceneSerializer::PrepareSaveFromSnapshot(std::string snapshot, const AssetLibrary& assets,
                                                              const std::string& path) {
    auto library = std::make_shared<json>(json::object());
    AppendAssetLibraryJson(*library, assets); // what Save() writes: no per-asset metadata (#121)
    auto text = std::make_shared<std::string>(std::move(snapshot));
    return [library, text, path]() {
        std::string out;
        try {
            json root = json::parse(*text);
            text->clear();
            text->shrink_to_fit();
            if (!root.is_object()) throw std::runtime_error("the snapshot isn't a JSON object");
            for (const char* k : kUndoOnlyKeys) root.erase(k);
            for (const char* k : kLibraryKeys) root.erase(k); // the library half comes from the live library below
            for (auto& [k, v] : library->items()) root[k] = v;
            WarnNonPortablePaths(root, ""); // audit #364, as Save() does
            out = root.dump(2);
        } catch (const std::exception& e) {
            Log::Error("Scene: couldn't build '" + path + "' from the undo snapshot (file left untouched): " + e.what());
            return false;
        }
        if (!AtomicFile::WriteBytes(std::filesystem::u8path(path), out, /*binary=*/false)) {
            Log::Error("Scene: failed to write '" + path + "' - the previous version on disk was kept.");
            return false;
        }
        return true;
    };
}

std::string SceneSerializer::SnapshotAfterLoad(const json& loaded, const std::string& loadedText, const AssetLibrary& assets) {
    json live;
    live["hasAssetLibrarySnapshot"] = true;
    AppendAssetLibraryJson(live, assets, /*includeAssetMetadata=*/true);
    bool same = true;
    for (const char* k : kLibraryKeys) {
        const bool inLoaded = loaded.contains(k), inLive = live.contains(k);
        if (inLoaded != inLive || (inLoaded && loaded.at(k) != live.at(k))) { same = false; break; }
    }
    if (same) return loadedText;
    json out = loaded;
    for (const char* k : kLibraryKeys) {
        out.erase(k);
        if (const auto it = live.find(k); it != live.end()) out[k] = *it;
    }
    return out.dump();
}

bool SceneSerializer::ApplyEntityTransformAndName(World& world, entt::entity entity, const json& entityJson) {
    if (!entityJson.is_object() || !world.Registry.valid(entity)) return false;
    // The same reads, defaults and non-finite scrub as ApplySceneJson's per-kind loops.
    auto& t = world.Registry.get_or_emplace<TransformComponent>(entity);
    t.Position = JsonToVec3(entityJson.value("position", json::array({0, 0, 0})));
    t.Scale = JsonToVec3(entityJson.value("scale", json::array({1, 1, 1})), glm::vec3(1.0f));
    if (const auto it = entityJson.find("rotation"); it != entityJson.end()) SetTransformRotationFromJson(t, *it);
    else t.SetRotationQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    if (const auto it = entityJson.find("name"); it != entityJson.end() && it->is_string())
        world.Registry.emplace_or_replace<NameComponent>(entity, NameComponent{it->get<std::string>()});
    return true;
}

std::string SceneSerializer::SaveToString(const World& world, const AssetLibrary& assets) {
    json root = BuildSceneJson(world);
    // Marks this snapshot as carrying real AssetLibrary state, so LoadFromString below knows to
    // treat it as a full replace rather than mistaking an entity-only snapshot (the play-mode
    // one) for "nothing changed, don't touch the library."
    root["hasAssetLibrarySnapshot"] = true;
    AppendAssetLibraryJson(root, assets, /*includeAssetMetadata=*/true); // #121 - undo only, never a file
    return root.dump();
}

namespace {
json LibraryStateJson(const AssetLibrary& assets) {
    json lib = json::object();
    lib["hasAssetLibrarySnapshot"] = true;
    AppendAssetLibraryJson(lib, assets, /*includeAssetMetadata=*/true);
    return lib;
}
double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
} // namespace

void SceneSerializer::SaveToStrings(const World& world, std::string& entityOnly, const AssetLibrary* assets,
                                    std::string* withLibrary, SnapshotTimes* times) {
    SnapshotTimes local;
    SnapshotTimes& t = times ? *times : local;
    auto t0 = std::chrono::steady_clock::now();
    json root = BuildSceneJson(world);
    t.BuildMs = MsSince(t0);
    t0 = std::chrono::steady_clock::now();
    entityOnly = root.dump();
    t.DumpMs = MsSince(t0);
    if (!assets || !withLibrary) return;
    // SaveToString(world, assets) is this root plus the marker and the library keys: added on top here (an object's
    // keys are kept sorted, so the order they go in doesn't change the text).
    t0 = std::chrono::steady_clock::now();
    json lib = LibraryStateJson(*assets);
    t.LibraryMs = MsSince(t0);
    t0 = std::chrono::steady_clock::now();
    for (auto it = lib.begin(); it != lib.end(); ++it) root[it.key()] = std::move(*it);
    *withLibrary = root.dump();
    t.LibraryDumpMs = MsSince(t0);
}

bool SceneSerializer::LoadFromString(World& world, AssetLibrary& assets, const std::string& data) {
    json root;
    try {
        root = json::parse(data);
    } catch (const std::exception& e) {
        Log::Error(std::string("Scene: failed to parse snapshot: ") + e.what());
        return false;
    }
    return LoadFromJson(world, assets, root);
}

bool SceneSerializer::LoadFromJson(World& world, AssetLibrary& assets, const json& root) {
    OutfitSystem::CancelPending(world); // the registry is about to be replaced
    struct SyncActiveOnExit { World& W; ~SyncActiveOnExit() { W.SyncActiveInHierarchy(); } } syncActive{world}; // #201
    // Same guard as Load() (audit #76): valid-but-non-object JSON parses fine but throws on the
    // first .value()/.contains() call below, which would otherwise escape uncaught.
    if (!root.is_object()) {
        Log::Error(std::string("Scene: snapshot is valid JSON but not a scene object (top-level type is ") +
                   root.type_name() + ").");
        return false;
    }
    // Only a snapshot built via the AssetLibrary-aware SaveToString() overload (undo/redo)
    // carries this - the entity-only overload (the play-mode snapshot) has no library state at
    // all, and touching the library there would affect the Asset Browser on exiting Play for no
    // reason. This is a TRUE replace (an asset created/deleted/renamed after this snapshot was
    // taken ends up correctly gone/restored, not merged with whatever's here now) but a CHEAP
    // one: PruneToKeepSet only drops cache entries for assets that shouldn't survive - it never
    // touches anything that's staying, so a texture/model that's already loaded and still wanted
    // is never re-decoded or re-imported from disk. Only genuinely-missing entries (something
    // that was deleted after this snapshot, now being undone back) cost a real reimport, via the
    // ordinary LoadModel/LoadTexture calls ApplyAssetLibraryJson below already makes - an earlier
    // version of this used a full assets.Clear() instead, which reimported the ENTIRE library on
    // every single undo/redo step, however small the actual edit — a real, serious regression
    // (multi-second stalls long enough to trip a GPU driver watchdog on a real project's worth
    // of assets), not just a style choice being reverted here.
    if (root.value("hasAssetLibrarySnapshot", false)) {
        // #81 — library entries are written through PathRef(), so each one is either a plain
        // project-relative string or a {"path","pathGuid"} object; and the live library keys its
        // assets by the (usually absolute) path they were loaded from. Match the two in the
        // WRITTEN form (AssetPathForWrite, or the GUID when both sides have one) and hand
        // PruneToKeepSet the live path, so a still-wanted asset is recognised and kept instead of
        // the old raw get<std::string>() throwing on the first object entry (crashing Undo) or,
        // for plain strings, never matching and re-importing the whole library every step.
        auto keepSet = [&](const char* key, const std::vector<std::string>& livePaths) {
            std::set<std::string> keep;
            auto it = root.find(key);
            if (it == root.end() || !it->is_array()) return keep;
            std::set<std::string> storedPaths, storedGuids;
            for (const auto& p : *it) {
                if (p.is_string()) storedPaths.insert(p.get<std::string>());
                else if (p.is_object()) {
                    const auto path = p.find("path");
                    if (path != p.end() && path->is_string()) storedPaths.insert(path->get<std::string>());
                    const auto guid = p.find("pathGuid");
                    if (guid != p.end() && guid->is_string()) storedGuids.insert(guid->get<std::string>());
                }
            }
            for (const auto& live : livePaths) {
                const AssetGuid g = AssetDatabase::GuidForPath(live);
                if (storedPaths.count(AssetPathForWrite(live)) || storedPaths.count(live) ||
                    (g.IsValid() && storedGuids.count(g.ToString())))
                    keep.insert(live);
            }
            return keep;
        };
        std::vector<std::string> liveModels, liveTextures, liveMaterials;
        for (const auto& m : assets.Models()) liveModels.push_back(m->Path());
        for (const auto& t : assets.Textures()) liveTextures.push_back(t->Path());
        for (const auto& m : assets.Materials()) liveMaterials.push_back(m->Path);
        std::set<std::string> keepModels = keepSet("libraryModels", liveModels);
        std::set<std::string> keepTextures = keepSet("libraryTextures", liveTextures);
        std::set<std::string> keepSounds = keepSet("librarySounds", assets.Sounds());
        std::set<std::string> keepPrefabs = keepSet("libraryPrefabs", assets.Prefabs());
        std::set<std::string> keepMaterials = keepSet("libraryMaterials", liveMaterials);
        // #121 - an undo snapshot still carries the folder list, so undoing a folder create /
        // rename / delete still works. A snapshot WITHOUT the key is not "no folders": keep what
        // the library has rather than deleting every folder in the project.
        std::set<std::string> keepFolders;
        if (auto it = root.find("assetFolders"); it != root.end() && it->is_array()) {
            for (const auto& f : *it) if (f.is_string()) keepFolders.insert(f.get<std::string>());
        } else {
            // Braced on purpose: unbraced, this else bound to the inner `if (f.is_string())`, so a
            // snapshot with no folder list kept nothing and PruneToKeepSet dropped every folder.
            keepFolders.insert(assets.Folders().begin(), assets.Folders().end());
        }
        assets.PruneToKeepSet(keepModels, keepTextures, keepSounds, keepPrefabs, keepFolders, keepMaterials);
        try { ApplyAssetLibraryJson(assets, root, /*fromUndoSnapshot=*/true); }
        catch (const std::exception& e) {
            Log::Error(std::string("Scene: snapshot asset library data is malformed: ") + e.what()); // #83
            return false;
        }
    }
    return ApplySceneJson(world, assets, root);
}

std::string SceneSerializer::SaveEntitiesToString(const World& world,
    const std::vector<entt::entity>& entities, bool flattenPrefabInstances) {
    // Descendants come along automatically: a fragment that kept a parent but dropped its
    // children would paste back as a visibly different object than the one that was copied.
    std::set<entt::entity> included;
    std::function<void(entt::entity)> addWithChildren = [&](entt::entity e) {
        if (e == entt::null || !world.Registry.valid(e) || !included.insert(e).second) return;
        if (const auto* hier = world.Registry.try_get<HierarchyComponent>(e)) {
            for (entt::entity child : hier->Children) addWithChildren(child);
        }
    };
    for (entt::entity e : entities) addWithChildren(e);

    return BuildSceneJson(world, &included, flattenPrefabInstances).dump();
}

bool SceneSerializer::AppendEntitiesFromString(World& world, AssetLibrary& assets,
    const std::string& data, std::vector<entt::entity>& outCreated,
    std::unordered_map<int, entt::entity>* outSourceOrder) {
    struct SyncActiveOnExit { World& W; ~SyncActiveOnExit() { W.SyncActiveInHierarchy(); } } syncActive{world}; // #201
    json root;
    try {
        root = json::parse(data);
    } catch (const std::exception& e) {
        Log::Error(std::string("Scene: failed to parse entity fragment: ") + e.what());
        return false;
    }
    return ApplySceneJson(world, assets, root, /*clearFirst=*/false, &outCreated, outSourceOrder);
}

bool SceneSerializer::SavePrefab(const World& world, entt::entity root, const std::string& path) {
    if (!world.Registry.valid(root)) return false;
    // Re-parsed and re-dumped with indentation so a prefab file is human-readable/diffable,
    // unlike the compact in-memory clipboard form the same function produces. flatten=true: a
    // .prefab asset is self-contained — if `root` (or a child) is itself a prefab instance it is
    // baked in fully here, not left as a nested link (#236 A2 — nested prefabs are stage 4).
    // #82 — atomic write, and the write result is checked.
    if (!AtomicFile::WriteJson(std::filesystem::u8path(path),
            json::parse(SaveEntitiesToString(world, {root}, /*flattenPrefabInstances=*/true)))) {
        Log::Error("Prefab: failed to write '" + path + "'.");
        return false;
    }
    Log::Info("Saved prefab '" + path + "'.");
    return true;
}

entt::entity SceneSerializer::InstantiatePrefab(World& world, AssetLibrary& assets,
    const std::string& path, std::vector<entt::entity>* outAll) {
    struct SyncActiveOnExit { World& W; ~SyncActiveOnExit() { W.SyncActiveInHierarchy(); } } syncActive{world}; // #201
    std::ifstream in(std::filesystem::u8path(path));
    if (!in.is_open()) {
        Log::Error("Prefab: '" + path + "' could not be opened.");
        return entt::null;
    }
    // Parsed once: built from here, then kept as the content this instance was built from.
    json parsed;
    try {
        in >> parsed;
    } catch (const std::exception& e) {
        Log::Error("Prefab: '" + path + "' could not be parsed: " + e.what());
        return entt::null;
    }

    std::vector<entt::entity> created;
    if (!ApplySceneJson(world, assets, parsed, /*clearFirst=*/false, &created, nullptr) || created.empty()) {
        return entt::null;
    }
    if (outAll) outAll->insert(outAll->end(), created.begin(), created.end());
    // Its overrides are diffed against that content (not the file, which may be edited before the
    // next save). The first instance of a path caches it; later ones were built from the same
    // content unless the file changed, and then the editor rebuilds them all
    // (EditorLayer::RefreshPrefabInstances).
    if (!g_prefabPristineCache.count(path)) {
        UpgradeLegacyBoxKeys(parsed); // #122
        g_prefabPristineCache.emplace(path, std::move(parsed));
    }

    // The prefab's root is whichever created entity has no parent inside the fragment.
    entt::entity root = created.front();
    for (entt::entity e : created) {
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        if (!hier || hier->Parent == entt::null) { root = e; break; }
    }
    // #236 A2 — link the instance to its source. Applies to every path that stamps a prefab:
    // drag-drop placement, "Place Instance", and scene-load stub expansion. The scene then
    // stores this instance as a stub, and edits to the .prefab propagate on the next load.
    // `InstanceEntities` keeps the full creation order (== the .prefab file's entity order) so
    // save-time per-field override diffing can pair each live entity with its pristine
    // counterpart in the file by index (#302 Part B).
    PrefabInstanceComponent pi{path};
    pi.InstanceEntities = created;
    world.Registry.emplace_or_replace<PrefabInstanceComponent>(root, std::move(pi));
    return root;
}
