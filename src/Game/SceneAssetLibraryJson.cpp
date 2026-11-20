#include "SceneSerializerInternal.h"

namespace SceneDetail {


// The per-asset block of an undo snapshot (see AppendAssetLibraryJson): every asset with a folder, display
// name, labels or import settings in the live library.
json BuildAssetMetaJson(const AssetLibrary& assets) {
    json meta = json::array();
    std::unordered_map<std::string, std::size_t> indexByPath; // #121 - findOrCreate used to be O(n^2)
    auto findOrCreate = [&](const std::string& key) -> json& {
        const std::string stored = AssetPathForWrite(key); // audit #364 - portable path
        if (auto it = indexByPath.find(stored); it != indexByPath.end()) return meta[it->second];
        json entry{{"path", stored}};
        AssetGuid g = AssetDatabase::GuidForPath(key);
        if (g.IsValid()) entry["guid"] = g.ToString();
        indexByPath.emplace(stored, meta.size());
        meta.push_back(std::move(entry));
        return meta.back();
    };
    for (const auto& [key, folder] : assets.AssetFolders()) findOrCreate(key)["folder"] = folder;
    for (const auto& [key, name] : assets.DisplayNames()) findOrCreate(key)["displayName"] = name;
    for (const auto& [key, labels] : assets.LabelsMap()) findOrCreate(key)["labels"] = labels;
    for (const auto& [key, t] : assets.TextureSettingsMap()) {
        findOrCreate(key)["textureImport"] = {
            {"textureType", (int)t.TextureType}, {"generateMipmaps", t.GenerateMipmaps},
            {"isSRGB", t.IsSRGB}, {"filterMode", (int)t.FilterMode},
            {"wrapMode", (int)t.WrapMode}, {"maxTextureSize", t.MaxTextureSize},
            {"anisoLevel", t.AnisoLevel}, {"compression", (int)t.CompressionMode},
        };
    }
    for (const auto& [key, m] : assets.ModelSettingsMap()) {
        json& mi = findOrCreate(key)["modelImport"];
        mi = {
            {"globalScale", m.GlobalScale}, {"importNormals", m.ImportNormals},
            {"importAnimations", m.ImportAnimations}, {"importSkeleton", m.ImportSkeleton},
            {"optimizeGraph", m.OptimizeGraph}, {"materialImportMode", (int)m.MaterialImportMode},
        };
        if (m.SourceUpAxis != ModelImportSettings::UpAxis::Y) mi["upAxis"] = Model::UpAxisName(m.SourceUpAxis);
        if (m.SimplifyRatio < 1.0f) mi["simplify"] = m.SimplifyRatio;
        if (m.PivotOffset != glm::vec3(0.0f)) mi["pivotOffset"] = {m.PivotOffset.x, m.PivotOffset.y, m.PivotOffset.z};
        if (!m.ExcludeNodes.empty()) mi["excludeNodes"] = m.ExcludeNodes;
        if (!m.MeshNodes.empty()) mi["meshNodes"] = m.MeshNodes;
        if (m.RootTransform != glm::mat4(1.0f)) {
            json rt = json::array();
            for (int k = 0; k < 16; ++k) rt.push_back(m.RootTransform[k / 4][k % 4]);
            mi["rootTransform"] = rt;
        }
        if (!m.UnrotateNodes.empty()) mi["unrotateNodes"] = m.UnrotateNodes;
        if (!m.MoveNodes.empty()) {
            json moves = json::array();
            for (const auto& n : m.MoveNodes) moves.push_back({{"node", n.Node}, {"by", {n.By.x, n.By.y, n.By.z}}});
            mi["moveNodes"] = moves;
        }
        if (m.SkinIgnoresMeshNode) mi["skinIgnoresMeshNode"] = true;
        if (m.MergeByMaterial) mi["mergeByMaterial"] = true;
        if (m.RootNodeInModelSpace) mi["rootNodeInModelSpace"] = true;
        if (!m.ClipTrims.empty()) {
            json trims = json::array();
            for (const auto& t : m.ClipTrims) trims.push_back({{"clip", t.Clip}, {"start", t.StartSeconds}, {"end", t.EndSeconds}});
            mi["clipTrims"] = trims;
        }
    }
    return meta;
}

// Persists the Asset Browser's library list (not just what's placed in the scene) — otherwise an
// imported-but-unused asset would simply vanish on the next launch. Per-asset data (folder,
// display name, labels, import settings) is NOT written here: it lives in the asset's .meta
// sidecar, and the folder list lives in project/settings.json (#121).
// `includeAssetMetadata` (#121): fold in the per-asset folder/display-name/labels/import-settings
// block and the virtual-folder list. TRUE ONLY for the in-memory undo snapshot, which has to be a
// true replace so that an Asset Browser edit (move to folder, rename, relabel, change import
// settings) stays undoable. It is FALSE for every scene FILE: that duplication is what #121 is
// about - it forked the folder organisation across scenes, let one texture carry different import
// settings per scene, and produced scene merge conflicts from unrelated Asset Browser edits.
void AppendAssetLibraryJson(json& root, const AssetLibrary& assets, bool includeAssetMetadata) {
    json modelPaths = json::array();
    for (const auto& m : assets.Models()) modelPaths.push_back(PathRef(m->Path()));
    root["libraryModels"] = modelPaths;

    json texPaths = json::array();
    for (const auto& t : assets.Textures()) texPaths.push_back(PathRef(t->Path()));
    root["libraryTextures"] = texPaths;

    {
        json arr = json::array();
        for (const auto& m : assets.Materials()) arr.push_back(PathRef(m->Path));
        root["libraryMaterials"] = arr;
    }

    {
        json arr = json::array();
        for (const auto& p : assets.Sounds()) arr.push_back(PathRef(p));
        root["librarySounds"] = arr;
    }
    {
        json arr = json::array();
        for (const auto& p : assets.Prefabs()) arr.push_back(PathRef(p));
        root["libraryPrefabs"] = arr;
    }
    if (!includeAssetMetadata) return; // scene file: per-asset data lives in .meta (#121)

    root["assetFolders"] = assets.Folders();
    // Folders Delete Folder took out whose files are still on disk; only when there are any (same bytes otherwise).
    if (!assets.RemovedDiskFolders().empty()) root["removedDiskFolders"] = assets.RemovedDiskFolders();

    root["assetMeta"] = BuildAssetMetaJson(assets);
}

// `hold` (optional): receives a reference to every model/texture/material the file's library lists, so a
// caller can keep them alive across AssetLibrary::ReleaseUnreferenced - they are scene content (the next save
// writes them back) even when no entity uses them yet (wardrobe pieces, materials only Play applies).
// `writeMeta` false (a read-only load, Load's persistMigration false): the old block is not folded into any .meta.
void ApplyAssetLibraryJson(AssetLibrary& assets, const json& root, bool fromUndoSnapshot,
                           std::vector<std::shared_ptr<void>>* hold, bool writeMeta) {
    // #121 - MIGRATION ONLY. Scenes saved before this change carry an `assetMeta` block that
    // duplicates every asset's folder, display name, labels and import settings. That copy is no
    // longer written, and it is no longer applied to the AssetLibrary either: the asset's .meta
    // sidecar is the single source of truth, and the loaders below read it. Here the old block is
    // only folded INTO .meta, and only for fields the sidecar does not already have - a scene is
    // the stale copy, so it must never overwrite what the Asset Browser has since written.
    //
    // Runs before the library loads below, so the migrated values are in place by the time an
    // asset is actually imported and it gets imported once, with the right settings, rather than
    // once with defaults plus a Reimport.
    // An undo or redo of a scene edit leaves every asset's metadata as it was, and re-applying it went through each
    // asset's .meta (1,763 entries on the Forest, most of a second per step). When the snapshot's block is what the
    // live library would write now there is nothing to restore: skip it.
    const bool metaUnchanged = fromUndoSnapshot && root.contains("assetMeta") && root.at("assetMeta") == BuildAssetMetaJson(assets);
    if (root.contains("assetMeta") && !metaUnchanged) {
        // An undo snapshot is authoritative: it is the in-memory record of what the library
        // looked like a moment ago, so restoring it is a true replace. The Set* calls below
        // write straight through to each asset's .meta, so undoing an Asset Browser edit undoes
        // the sidecar write too.
        if (fromUndoSnapshot) assets.ClearMetadataOnly();

        int migrated = 0;
        for (const auto& entry : root["assetMeta"]) {
            // v2: prefer guid resolution; v1: plain path.
            AssetGuid g = AssetGuid::FromString(entry.value("guid", std::string()));
            std::string fallback = AssetPathForRead(entry.value("path", std::string())); // audit #364
            std::string path = AssetDatabase::Resolve(g, fallback);
            if (path.empty()) continue;

            if (!fromUndoSnapshot) {
                if (!writeMeta) continue;
                const std::string add =
                    SceneSerializer::MigrateAssetMetaFields(entry.dump(), AssetDatabase::ReadMetaFields(path));
                if (add != "{}" && AssetDatabase::MergeMetaFields(path, add)) ++migrated;
                continue;
            }
            // Import settings the .meta never had (a texture's guessed normal-map import is kept
            // in memory only) are restored to memory only, not written into the .meta.
            bool metaHasImporter = false;
            try { metaHasImporter = json::parse(AssetDatabase::ReadMetaFields(path)).contains("importer"); } catch (...) {}

            if (entry.contains("folder")) assets.SetAssetFolder(path, entry["folder"].get<std::string>());
            if (entry.contains("displayName")) assets.SetDisplayName(path, entry["displayName"].get<std::string>());
            if (entry.contains("labels")) {
                std::set<std::string> labels;
                for (const auto& l : entry["labels"]) labels.insert(l.get<std::string>());
                assets.SetLabels(path, labels);
            }
            if (entry.contains("textureImport")) {
                const auto& t = entry["textureImport"];
                TextureImportSettings st;
                // Clamp: a scene saved before #198 removed the unused Cubemap enum value could
                // still carry that old index (3) - fall back to Default rather than construct an
                // out-of-range enum.
                int rawType = t.value("textureType", 0);
                st.TextureType = (rawType >= 0 && rawType <= (int)TextureImportSettings::Type::Sprite2D)
                    ? (TextureImportSettings::Type)rawType : TextureImportSettings::Type::Default;
                st.GenerateMipmaps = t.value("generateMipmaps", true);
                st.IsSRGB = t.value("isSRGB", true);
                st.FilterMode = (TextureImportSettings::Filter)std::clamp(t.value("filterMode", 1), 0, 2); // #122
                st.WrapMode = (TextureImportSettings::Wrap)std::clamp(t.value("wrapMode", 0), 0, 1);
                st.MaxTextureSize = t.value("maxTextureSize", 2048);
                st.AnisoLevel = std::clamp(t.value("anisoLevel", 8), 1, 16);
                st.CompressionMode = (TextureImportSettings::Compression)std::clamp(t.value("compression", 0), 0, 2);
                assets.SetTextureSettings(path, st, metaHasImporter);
            }
            if (entry.contains("modelImport")) {
                const auto& m = entry["modelImport"];
                ModelImportSettings sm;
                sm.GlobalScale = m.value("globalScale", 1.0f);
                sm.ImportNormals = m.value("importNormals", true);
                sm.ImportAnimations = m.value("importAnimations", true);
                sm.ImportSkeleton = m.value("importSkeleton", true);
                sm.OptimizeGraph = m.value("optimizeGraph", true);
                sm.SimplifyRatio = std::clamp(m.value("simplify", 1.0f), 0.01f, 1.0f);
                sm.MaterialImportMode = (ModelImportSettings::MaterialMode)std::clamp(m.value("materialImportMode", 0), 0, 2); // #122
                sm.SourceUpAxis = Model::ParseUpAxis(m.value("upAxis", std::string("Y")));
                sm.SkinIgnoresMeshNode = m.value("skinIgnoresMeshNode", false);
                sm.MergeByMaterial = m.value("mergeByMaterial", false);
                sm.RootNodeInModelSpace = m.value("rootNodeInModelSpace", false);
                if (m.contains("pivotOffset") && m["pivotOffset"].is_array() && m["pivotOffset"].size() == 3)
                    sm.PivotOffset = {m["pivotOffset"][0].get<float>(), m["pivotOffset"][1].get<float>(), m["pivotOffset"][2].get<float>()};
                if (m.contains("excludeNodes") && m["excludeNodes"].is_array())
                    for (const auto& n : m["excludeNodes"])
                        if (n.is_string()) sm.ExcludeNodes.push_back(n.get<std::string>());
                if (m.contains("meshNodes") && m["meshNodes"].is_array())
                    for (const auto& n : m["meshNodes"])
                        if (n.is_string()) sm.MeshNodes.push_back(n.get<std::string>());
                if (m.contains("rootTransform") && m["rootTransform"].is_array() && m["rootTransform"].size() == 16)
                    for (int k = 0; k < 16; ++k) sm.RootTransform[k / 4][k % 4] = m["rootTransform"][k].get<float>();
                if (m.contains("unrotateNodes") && m["unrotateNodes"].is_array())
                    for (const auto& n : m["unrotateNodes"])
                        if (n.is_string()) sm.UnrotateNodes.push_back(n.get<std::string>());
                if (m.contains("moveNodes") && m["moveNodes"].is_array())
                    for (const auto& n : m["moveNodes"])
                        if (n.is_object() && n.contains("by") && n["by"].is_array() && n["by"].size() == 3)
                            sm.MoveNodes.push_back({n.value("node", std::string()),
                                                    {n["by"][0].get<float>(), n["by"][1].get<float>(), n["by"][2].get<float>()}});
                if (m.contains("clipTrims") && m["clipTrims"].is_array())
                    for (const auto& t : m["clipTrims"])
                        if (t.is_object() && t.value("clip", std::string()).size())
                            sm.ClipTrims.push_back({t.value("clip", std::string()), t.value("start", 0.0f), t.value("end", 0.0f)});
                assets.SetModelSettings(path, sm, metaHasImporter);
            }
        }
        if (migrated > 0)
            Log::Info("Scene: moved asset data for " + std::to_string(migrated) +
                      " asset(s) out of the scene file and into their .meta sidecars (#121). "
                      "Save the scene to drop the old block.");
    }
    if (root.contains("libraryModels")) {
        for (const auto& p : root["libraryModels"]) {
            std::string path = ResolveAssetRef(p);
            if (path.empty()) continue;
            auto a = assets.LoadModel(path);
            if (hold && a) hold->push_back(a);
        }
    }
    if (root.contains("libraryTextures")) {
        for (const auto& p : root["libraryTextures"]) {
            std::string path = ResolveAssetRef(p);
            if (path.empty()) continue;
            auto a = assets.LoadTexture(path);
            if (hold && a) hold->push_back(a);
        }
    }
    if (root.contains("libraryMaterials")) {
        for (const auto& p : root["libraryMaterials"]) {
            std::string path = ResolveAssetRef(p);
            if (path.empty()) continue;
            auto a = assets.LoadMaterial(path);
            if (hold && a) hold->push_back(a);
        }
    }
    if (root.contains("librarySounds")) {
        for (const auto& p : root["librarySounds"]) {
            std::string path = ResolveAssetRef(p);
            if (!path.empty()) assets.RegisterSound(path);
        }
    }
    if (root.contains("libraryPrefabs")) {
        for (const auto& p : root["libraryPrefabs"]) {
            std::string path = ResolveAssetRef(p);
            if (!path.empty()) assets.RegisterPrefab(path);
        }
    }
    // #121 - same migration for the virtual folder list, which now lives in
    // project/settings.json. CreateFolder writes through to it, so an old scene's folders are
    // adopted once and then persist for the project rather than for this one scene.
    if (root.contains("assetFolders")) {
        for (const auto& f : root["assetFolders"])
            if (f.is_string()) assets.CreateFolder(f.get<std::string>());
    }
    // After the folders (CreateFolder clears marks): an undo snapshot's marks are the library's again, so a redo of
    // Delete Folder keeps the folder out of the browser while its files are on disk, and an undo lists it again.
    // A snapshot without the key had none.
    if (fromUndoSnapshot && root.contains("assetFolders")) {
        std::vector<std::string> marks;
        if (auto it = root.find("removedDiskFolders"); it != root.end() && it->is_array())
            for (const auto& f : *it)
                if (f.is_string()) marks.push_back(f.get<std::string>());
        assets.SetRemovedDiskFolders(std::move(marks));
    }
}

} // namespace SceneDetail

using namespace SceneDetail;


// #121 - see the header. Parse failures on either side yield "{}": a scene or sidecar we cannot
// read is never a reason to write a guess into the sidecar.

std::string SceneSerializer::MigrateAssetMetaFields(const std::string& entryJson,
                                                    const std::string& existingMetaJson) {
    json entry, existing;
    try {
        entry = json::parse(entryJson);
        existing = json::parse(existingMetaJson);
    } catch (...) { return "{}"; }
    if (!entry.is_object() || !existing.is_object()) return "{}";

    json add = json::object();
    auto carry = [&](const char* from, const char* to) {
        if (entry.contains(from) && !existing.contains(to) && !add.contains(to)) add[to] = entry[from];
    };
    carry("folder", "folder");
    carry("displayName", "displayName");
    carry("labels", "labels");
    carry("textureImport", "importer");
    carry("modelImport", "importer");
    return add.dump();
}
