#include "SceneSerializerInternal.h"

namespace SceneDetail {


// #9, Phase M item 1 — a pre-v3 scene carries no post-processing/shadow keys of its own; those
// values lived in editor_prefs.json. Read them straight off that file on disk (EditorSettings no
// longer declares these fields, so Get() can't provide them) and copy the user's real authored
// values onto World, rather than silently resetting to compiled defaults.
void MigratePostProcessSettingsFromPrefs(World& world) {
    std::ifstream in(std::filesystem::u8path(GameShell::PrefsFilePath()));
    // Starts as an object, not null: with no editor_prefs.json (a fresh machine, or a built game,
    // which has its own user folder) json::value() on a null threw type_error 306 and the whole
    // pre-v3 scene failed to load. Wrong-typed values fall back to the defaults the same way.
    json prefs = json::object();
    if (in.is_open()) {
        try { in >> prefs; } catch (const std::exception&) { prefs = json::object(); }
        if (!prefs.is_object()) prefs = json::object();
    }
    auto num = [&prefs](const char* key, auto fallback) {
        const auto it = prefs.find(key);
        return (it != prefs.end() && it->is_number()) ? it->get<decltype(fallback)>() : fallback;
    };
    auto flag = [&prefs](const char* key, bool fallback) {
        const auto it = prefs.find(key);
        return (it != prefs.end() && it->is_boolean()) ? it->get<bool>() : fallback;
    };
    world.Render.ExposureEV       = num("exposureEV", 0.0f);
    world.Render.TonemapOperator  = num("tonemapOperator", 1);
    world.Render.MsaaSamples      = num("msaaSamples", 4);
    world.Render.SsaoEnabled      = flag("ssaoEnabled", false);
    world.Render.BloomEnabled     = flag("bloomEnabled", false);
    world.Render.BloomThreshold   = num("bloomThreshold", 1.0f);
    world.Render.BloomKnee        = num("bloomKnee", 0.5f);
    world.Render.BloomIntensity   = num("bloomIntensity", 0.25f);
    world.Render.ShadowsEnabled   = flag("shadowsEnabled", true);
    world.Render.ShadowResolution = num("shadowResolution", 4096);
    world.Render.ShadowCascades   = num("shadowCascades", 4);
    world.Render.ShadowDistance   = num("shadowDistance", 500.0f);
    g_MigrationLog.push_back("post-processing/shadow settings (exposure=" +
        std::to_string(world.Render.ExposureEV) + ", bloom=" + (world.Render.BloomEnabled ? "on" : "off") +
        ", ssao=" + (world.Render.SsaoEnabled ? "on" : "off") + ") migrated from editor_prefs.json");
}

// Guards ApplySceneJson against runaway recursion when a prefab instance stub expands another
// scene fragment (#236 A2). A flattened .prefab never contains a "prefabInstances" key, so in
// normal use this only ever reaches depth 2; the cap just stops a hand-broken / cyclic file
// from looping forever. Main-thread only, same as g_LastLoadWarning.
int g_ApplyDepth = 0;
constexpr int kMaxApplyDepth = 8;
struct ApplyDepthGuard {
    ApplyDepthGuard()  { ++g_ApplyDepth; }
    ~ApplyDepthGuard() { --g_ApplyDepth; }
};


// #83 — every field read below uses json::value()/get<T>(), which THROW json::type_error on a
// wrong-typed value ("tag": 5, "name": null, a bool written as "true", ...). The wrapper turns
// any such exception into an ordinary load failure with a logged reason instead of letting it
// escape to main()'s outermost catch and close the editor. Callers restore the previous world
// on failure (see SceneSerializer::Load / Undo).
bool ApplySceneJsonImpl(World& world, AssetLibrary& assets, const json& root,
    bool clearFirst, std::vector<entt::entity>* outCreated,
    std::unordered_map<int, entt::entity>* outSourceOrder);
bool ApplySceneJson(World& world, AssetLibrary& assets, const json& root,
    bool clearFirst, std::vector<entt::entity>* outCreated,
    std::unordered_map<int, entt::entity>* outSourceOrder) {
    try {
        return ApplySceneJsonImpl(world, assets, root, clearFirst, outCreated, outSourceOrder);
    } catch (const std::exception& e) {
        Log::Error(std::string("Scene: the scene data is malformed and could not be loaded: ") + e.what());
        return false;
    }
}

// `clearFirst` false ADDS to the existing scene instead of replacing it — the difference
// between loading a scene and pasting/instantiating a fragment into one. `outCreated`, when
// given, collects every entity this call created so the caller can select or offset them.
bool ApplySceneJsonImpl(World& world, AssetLibrary& assets, const json& root,
    bool clearFirst, std::vector<entt::entity>* outCreated,
    std::unordered_map<int, entt::entity>* outSourceOrder) {
    ApplyDepthGuard depthGuard; // #236 A2 — bounds prefab-stub expansion recursion
    if (g_ApplyDepth > kMaxApplyDepth) {
        Log::Error("Scene: prefab instance nesting too deep (" + std::to_string(kMaxApplyDepth) +
                   "); stopping expansion. Is a .prefab referencing itself?");
        return false;
    }
    auto created = [&](entt::entity e) { if (outCreated) outCreated->push_back(e); };

    // #195: 0 (the default when the key is absent) means a legacy pre-#195 file or an entity-subset
    // fragment (BuildSceneJson never writes the field for those) — both fall straight through to
    // the existing presence-check reads below exactly as before this field existed. A recognized
    // version in between would take its own migration branch here as new incompatible versions are
    // added; there's only ever been version 1 so far, so there's nothing to branch on yet. A
    // version newer than this build knows about is still loaded best-effort (every read below
    // already tolerates an unrecognized/missing key), but is very likely missing data this build
    // can't interpret, so it's called out loudly rather than silently: Console now, and stashed for
    // the caller (e.g. the editor) to also raise as a dialog via SceneSerializer::TakeLoadWarning().
    if (g_ApplyDepth == 1) g_LastLoadWarning.clear(); // not a nested prefab expansion / paste: it would drop the file's warning
    int formatVersion = root.value("formatVersion", 0);
    if (formatVersion > kSceneFormatVersion) {
        std::string msg = "Scene: this file's format version (" + std::to_string(formatVersion) +
            ") is newer than this build supports (" + std::to_string(kSceneFormatVersion) +
            "). It was saved by a newer version of the engine — some data may be missing or "
            "misinterpreted after loading.";
        Log::Error(msg);
        g_LastLoadWarning = msg;
    }

    if (clearFirst) { world.Registry.clear(); g_prefabPristineCache.clear(); ++world.SceneLoads; } // #302 Part B
    if (!clearFirst) {
        // A fragment carries no environment settings (BuildSceneJson omits them for subsets),
        // and must not disturb the scene's own.
    } else
    if (root.contains("skyHorizonColor") || root.contains("skyZenithColor")) {
        world.Render.SkyHorizonColor = JsonToVec3(root.value("skyHorizonColor", json::array({0.53f, 0.72f, 0.86f})), glm::vec3(0.53f, 0.72f, 0.86f));
        world.Render.SkyZenithColor = JsonToVec3(root.value("skyZenithColor", json::array({0.20f, 0.40f, 0.75f})), glm::vec3(0.20f, 0.40f, 0.75f));
    } else if (root.contains("skyColor")) {
        // Migrate scenes saved before the sky became a horizon/zenith gradient — keep the old
        // flat color as the horizon and fall back to the default zenith blue above it.
        world.Render.SkyHorizonColor = JsonToVec3(root["skyColor"], glm::vec3(0.53f, 0.72f, 0.86f));
        world.Render.SkyZenithColor = glm::vec3(0.20f, 0.40f, 0.75f);
    } else {
        world.Render.SkyHorizonColor = glm::vec3(0.53f, 0.72f, 0.86f);
        world.Render.SkyZenithColor = glm::vec3(0.20f, 0.40f, 0.75f);
    }
    // #196: scenes saved before IBL existed carry no ambient intensity — 1.0 (the physically
    // consistent value) is the right default for them, same as a brand-new scene.
    if (clearFirst) {
        world.Render.SkyAmbientIntensity    = root.value("skyAmbientIntensity",    1.0f);
        // PR13: HDRI sky source fields (absent in old scenes → Procedural defaults)
        world.Render.SkySourceMode          = (World::SkySource)std::clamp(root.value("skySource", 0), 0, 2); // #122
        world.Render.Sky                    = SkySettingsFromJson(root.value("sky", json::object()));
        world.Render.SkyHdriPath            = ResolveAssetRef(root, "skyHdriPath", "skyHdriGuid"); // audit #364, #132
        world.Render.SkyRotationDegrees     = root.value("skyRotationDegrees",     0.0f);
        world.Render.SkyHdriSun             = (World::HdriSunMode)std::clamp(root.value("skyHdriSun", 0), 0, 2); // #277
        world.Render.SkyHdriSunThreshold    = std::max(1.0f, root.value("skyHdriSunThreshold", 50.0f));
        // #110 - local-light shadow budgets; absent in older scenes -> the old fixed caps.
        world.Render.MaxSpotShadows  = std::clamp(root.value("maxSpotShadows", 4), 0, SpotShadowMap::kMaxSpots);
        world.Render.MaxPointShadows = std::clamp(root.value("maxPointShadows", 2), 0, PointShadowMap::kMaxPoints);

        // #9, Phase M item 1 — v3+ scenes own these directly; a pre-v3 file has none of these
        // keys (BuildSceneJson only started writing them at v3), which is exactly the signal to
        // migrate the user's real values forward from editor_prefs.json instead of defaulting.
        if (root.contains("exposureEV")) {
            world.Render.ExposureEV       = root.value("exposureEV", 0.0f);
            world.Render.TonemapOperator  = root.value("tonemapOperator", 1);
            world.Render.MsaaSamples      = root.value("msaaSamples", 4);
            world.Render.SsaoEnabled      = root.value("ssaoEnabled", false);
            world.Render.SsaoRadius       = std::clamp(root.value("ssaoRadius", 0.5f), 0.05f, 5.0f);
            world.Render.SsaoBias         = std::clamp(root.value("ssaoBias", 0.025f), 0.0f, 0.5f);
            world.Render.SsaoIntensity    = std::clamp(root.value("ssaoIntensity", 1.0f), 0.1f, 4.0f);
            world.Render.BloomEnabled     = root.value("bloomEnabled", false);
            world.Render.BloomThreshold   = root.value("bloomThreshold", 1.0f);
            world.Render.BloomKnee        = root.value("bloomKnee", 0.5f);
            world.Render.BloomIntensity   = root.value("bloomIntensity", 0.25f);
            // #162 - absent in older scenes: neutral.
            world.Render.FxaaEnabled        = root.value("fxaa", false);
            world.Render.AutoExposure       = root.value("autoExposure", false);
            world.Render.AutoExposureMinEV  = std::clamp(root.value("autoExposureMinEV", -4.0f), -12.0f, 12.0f);
            world.Render.AutoExposureMaxEV  = std::clamp(root.value("autoExposureMaxEV", 4.0f), -12.0f, 12.0f);
            world.Render.AutoExposureSpeedUp   = std::clamp(root.value("autoExposureSpeedUp", 2.0f), 0.0f, 20.0f);
            world.Render.AutoExposureSpeedDown = std::clamp(root.value("autoExposureSpeedDown", 1.0f), 0.0f, 20.0f);
            world.Render.GradeTemperature   = std::clamp(root.value("gradeTemperature", 0.0f), -100.0f, 100.0f);
            world.Render.GradeTint          = std::clamp(root.value("gradeTint", 0.0f), -100.0f, 100.0f);
            world.Render.GradeContrast      = std::clamp(root.value("gradeContrast", 0.0f), -100.0f, 100.0f);
            world.Render.GradeSaturation    = std::clamp(root.value("gradeSaturation", 0.0f), -100.0f, 100.0f);
            world.Render.GradeColorFilter   = glm::vec3(1.0f);
            if (root.contains("gradeColorFilter") && root["gradeColorFilter"].is_array() && root["gradeColorFilter"].size() == 3)
                for (int i = 0; i < 3; ++i)
                    if (root["gradeColorFilter"][i].is_number())
                        world.Render.GradeColorFilter[i] = std::max(0.0f, root["gradeColorFilter"][i].get<float>());
    // Volumetric fog: absent in older scenes, which load with it off.
    world.Render.VolumetricEnabled       = root.value("volumetricEnabled", false);
    world.Render.VolumetricDensity       = std::clamp(root.value("volDensity", 0.02f), 0.0f, 10.0f);
    world.Render.VolumetricAnisotropy    = std::clamp(root.value("volAnisotropy", 0.6f), -0.95f, 0.95f);
    world.Render.VolumetricHeightFalloff = std::clamp(root.value("volHeightFalloff", 0.0f), 0.0f, 10.0f);
    world.Render.VolumetricBaseHeight    = root.value("volBaseHeight", 0.0f);
    world.Render.VolumetricAlbedo        = glm::vec3(0.9f);
    if (root.contains("volAlbedo") && root["volAlbedo"].is_array() && root["volAlbedo"].size() == 3)
        for (int i = 0; i < 3; ++i)
            if (root["volAlbedo"][i].is_number())
                world.Render.VolumetricAlbedo[i] = std::clamp(root["volAlbedo"][i].get<float>(), 0.0f, 1.0f);
    world.Render.VolumetricDistance      = std::clamp(root.value("volDistance", 64.0f), 4.0f, 1000.0f);
    world.Render.VolumetricQuality       = std::clamp(root.value("volQuality", 1), 0, 2);
    world.Render.DepthOfField  = root.value("depthOfField", false);
    world.Render.FocusDistance = std::clamp(root.value("focusDistance", 10.0f), 0.0f, 10000.0f);
    // Absent: the mode the older msaaSamples / fxaa fields already describe (never TAA).
    world.Render.AntiAliasing = World::LegacyAntiAliasing(world.Render.MsaaSamples, world.Render.FxaaEnabled);
    if (const auto aa = root.find("antiAliasing"); aa != root.end() && aa->is_string()) {
        const std::string v = aa->get<std::string>();
        if (v == "Off") world.Render.AntiAliasing = World::AntiAliasingMode::Off;
        else if (v == "FXAA") world.Render.AntiAliasing = World::AntiAliasingMode::Fxaa;
        else if (v == "MSAA") world.Render.AntiAliasing = World::AntiAliasingMode::Msaa;
        else if (v == "TAA") world.Render.AntiAliasing = World::AntiAliasingMode::Taa;
    }
    world.Render.TaaSharpness = std::clamp(root.value("taaSharpness", 0.35f), 0.0f, 1.0f);
    world.Render.Wetness      = std::clamp(root.value("wetness", 0.0f), 0.0f, 1.0f);
    world.Render.PuddleAmount = std::clamp(root.value("puddleAmount", 0.0f), 0.0f, 1.0f);
    world.Render.PuddleScale  = std::clamp(root.value("puddleScale", 4.0f), 0.25f, 100.0f);
    world.Render.PuddleSsr    = root.value("puddleSsr", false);
    world.Render.FocusRange    = std::clamp(root.value("focusRange", 3.0f), 0.01f, 10000.0f);
    world.Render.DofMaxBlur    = std::clamp(root.value("dofMaxBlur", 8.0f), 0.0f, 32.0f);
            world.Render.FogEnabled       = root.value("fogEnabled", false);
            world.Render.FogMode          = std::clamp(root.value("fogMode", 2), 1, 3);
            world.Render.FogColor         = glm::vec3(0.55f, 0.62f, 0.72f);
            if (root.contains("fogColor") && root["fogColor"].is_array() && root["fogColor"].size() == 3)
                for (int i = 0; i < 3; ++i)
                    if (root["fogColor"][i].is_number())
                        world.Render.FogColor[i] = std::max(0.0f, root["fogColor"][i].get<float>());
            world.Render.FogDensity       = std::max(0.0f, root.value("fogDensity", 0.01f));
            world.Render.FogStart         = root.value("fogStart", 10.0f);
            world.Render.FogEnd           = root.value("fogEnd", 300.0f);
            world.Render.FogHeightFalloff = std::max(0.0f, root.value("fogHeightFalloff", 0.0f));
            world.Render.FogBaseHeight    = root.value("fogBaseHeight", 0.0f);
            world.Render.ShadowsEnabled   = root.value("shadowsEnabled", true);
            world.Render.ShadowResolution = root.value("shadowResolution", 4096);
            world.Render.ShadowCascades   = root.value("shadowCascades", 4);
            world.Render.ShadowDistance   = root.value("shadowDistance", 500.0f);
            world.Render.ShadowCascadeSplit = std::clamp(root.value("shadowCascadeSplit", 0.75f), 0.0f, 0.98f);
        } else {
            MigratePostProcessSettingsFromPrefs(world);
        }
    }

    // Reconstructs HierarchyComponent parent links from the "id"/"parentId" fields written by
    // BuildSceneJson — both entries are created first (order-independent), then parents are
    // applied in a second pass so a parentId can point forward or backward in the file.
    std::unordered_map<int, entt::entity> idToEntity;
    // Two entities sharing a saved id makes parent links ambiguous (the later one wins, and children of the
    // earlier one attach to the wrong entity), so say so once per id instead of silently mislinking.
    std::unordered_set<int> warnedDuplicateIds;
    auto mapId = [&](int id, entt::entity e) {
        if (id < 0) return;
        const auto [it, inserted] = idToEntity.emplace(id, e);
        if (inserted) return;
        if (warnedDuplicateIds.insert(id).second)
            Log::Warn("Scene: more than one entity has the id " + std::to_string(id) + "; children of it may attach to the wrong one. Renumber the duplicates.");
        it->second = e;
    };
    std::vector<std::pair<entt::entity, int>> pendingParents;

    // OrderComponent (see Components.h): a full load restores it verbatim so save->load->save is
    // order-stable; a paste/prefab append (!clearFirst) keeps the fresh trailing value CreateXxx
    // already assigned, so it lands after the current scene. Legacy files with no "order" field
    // get sequential file-order values here, matching the old boxes->models->empties behaviour.
    int fallbackOrder = 0;
    int maxLoadedOrder = -1;
    // #119 — append mode: the fragment's own order values -> the new copies, used below to
    // repoint intra-fragment joint references and handed back to the caller.
    std::unordered_map<int, entt::entity> sourceOrder;
    auto applyOrder = [&](entt::entity e, const json& j) {
        if (!clearFirst) {
            if (auto it = j.find("order"); it != j.end() && it->is_number_integer())
                sourceOrder[it->get<int>()] = e;
            return;
        }
        int order = j.value("order", fallbackOrder);
        fallbackOrder = std::max(fallbackOrder, order) + 1;
        maxLoadedOrder = std::max(maxLoadedOrder, order);
        world.Registry.emplace_or_replace<OrderComponent>(e, order);
    };

    bool migratedEulerRotations = false;
    auto applyRotation = [&](TransformComponent& transform, const json& object) {
        if (auto it = object.find("rotation"); it != object.end())
            SetTransformRotationFromJson(transform, *it, &migratedEulerRotations);
        else
            transform.SetRotationQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    };

    if (root.contains("boxes")) {
        for (const auto& b : root["boxes"]) {
            // "alive" is a pre-ECS field: a shot-dead box used to be soft-deleted (kept in the
            // file, flagged inert) rather than removed, so an old save can still contain one —
            // skip creating it at all, which is externally indistinguishable from the old
            // soft-deleted state (nothing before ever displayed or collided with a dead box).
            if (!b.value("alive", true)) continue;

            // #122 - "position" / "scale" like every other entity; "center" / "size" in older files.
            const char* posKey = b.contains("position") ? "position" : "center";
            const char* scaleKey = b.contains("scale") ? "scale" : "size";
            glm::vec3 center = JsonToVec3(b.value(posKey, json::array({0, 0, 0})));
            glm::vec3 size = JsonToVec3(b.value(scaleKey, json::array({1, 1, 1})), glm::vec3(1.0f));
            glm::vec3 color = JsonToVec3(b.value("color", json::array({1, 1, 1})), glm::vec3(1.0f));
            std::string name = b.value("name", std::string());
            entt::entity e = world.CreateBox(center, size, color, glm::vec3(0.0f), name);
            applyRotation(world.Registry.get<TransformComponent>(e), b);
            ReadMaterialSlots(assets, b, world.Registry.get<RenderableComponent>(e)); // #120
            ReadRendererFlags(b, world.Registry.get<RenderableComponent>(e)); // #163
            ReadCommonComponents(b, world, assets, e);
            applyOrder(e, b);
            created(e);

            int id = b.value("id", -1);
            mapId(id, e);
            int parentId = b.value("parentId", -1);
            if (parentId >= 0) pendingParents.emplace_back(e, parentId);
        }
    }

    if (root.contains("models")) {
        for (const auto& m : root["models"]) {
            std::string modelPath = ResolveAssetRef(m, "path", "pathGuid");
            if (modelPath.empty()) continue;

            auto model = assets.InstantiateModel(modelPath);

            std::string name = m.value("name", std::string("Model"));
            glm::vec3 position = JsonToVec3(m.value("position", json::array({0, 0, 0})));
            glm::vec3 scale = JsonToVec3(m.value("scale", json::array({1, 1, 1})), glm::vec3(1.0f));

            entt::entity e = world.CreateModelEntity(model, position, glm::vec3(0.0f), scale, name);
            applyRotation(world.Registry.get<TransformComponent>(e), m);
            auto& rc = world.Registry.get<RenderableComponent>(e);

            ReadMaterialSlots(assets, m, rc);
            // A slot the scene leaves empty uses the model's material remap (its .mat per imported
            // material, Unity's importer remap), as a newly placed copy does - so remap edits reach
            // objects placed before them. Slots the scene sets are kept.
            if (rc.ModelRef) assets.ApplyMaterialRemap(*rc.ModelRef, rc.Materials);
            ReadRendererFlags(m, rc); // #163
            ReadCommonComponents(m, world, assets, e); // handles the "Audio Source" block + the legacy "sound*" shim
            applyOrder(e, m);
            created(e);

            int id = m.value("id", -1);
            mapId(id, e);
            int parentId = m.value("parentId", -1);
            if (parentId >= 0) pendingParents.emplace_back(e, parentId);
        }
    }

    if (root.contains("empties")) {
        for (const auto& en : root["empties"]) {
            glm::vec3 position = JsonToVec3(en.value("position", json::array({0, 0, 0})));
            glm::vec3 scale = JsonToVec3(en.value("scale", json::array({1, 1, 1})), glm::vec3(1.0f));
            entt::entity e = world.CreateEmptyEntity(position, glm::vec3(0.0f), scale,
                en.value("name", std::string("Empty")));
            applyRotation(world.Registry.get<TransformComponent>(e), en);
            ReadCommonComponents(en, world, assets, e);
            applyOrder(e, en);
            created(e);

            int id = en.value("id", -1);
            mapId(id, e);
            int parentId = en.value("parentId", -1);
            if (parentId >= 0) pendingParents.emplace_back(e, parentId);
        }
    }

    // #236 A2 — expand prefab-instance stubs: rebuild the subtree from the .prefab, then apply
    // the stub's transform-only overrides on the root. A missing source becomes a visible
    // broken placeholder rather than a silent hole.
    if (root.contains("prefabInstances") && root["prefabInstances"].is_array()) {
        for (const auto& s : root["prefabInstances"]) {
            const std::string src = ResolveAssetRef(s, "source", "sourceGuid");
            glm::vec3 position = JsonToVec3(s.value("position", json::array({0, 0, 0})));
            glm::vec3 scale = JsonToVec3(s.value("scale", json::array({1, 1, 1})), glm::vec3(1.0f));
            const std::string name = s.value("name", std::string("Prefab Instance"));

            std::vector<entt::entity> instCreated;
            entt::entity rootE = entt::null;
            if (!src.empty())
                rootE = SceneSerializer::InstantiatePrefab(world, assets, src, &instCreated);

            const bool missing = (rootE == entt::null);
            if (missing) {
                rootE = world.CreateEmptyEntity(position, glm::vec3(0.0f), scale, name);
                applyRotation(world.Registry.get<TransformComponent>(rootE), s);
                world.Registry.emplace_or_replace<PrefabInstanceComponent>(
                    rootE, PrefabInstanceComponent{src, /*Missing=*/true});
                if (!src.empty())
                    Log::Warn("Prefab instance: source '" + src + "' could not be loaded; "
                              "inserted a placeholder so the reference isn't lost.");
                instCreated.push_back(rootE);
            } else {
                auto& t = world.Registry.get<TransformComponent>(rootE);
                t.Position = position;
                applyRotation(t, s);
                t.Scale = scale;
                world.Registry.emplace_or_replace<NameComponent>(rootE, NameComponent{name});
            }

            // Root-level overrides carried by the stub.
            if (!s.value("active", true)) world.Registry.emplace_or_replace<DeactivatedTag>(rootE);
            else                          world.Registry.remove<DeactivatedTag>(rootE);
            if (s.value("static", false)) world.Registry.emplace_or_replace<StaticTag>(rootE);
            if (const int layer = s.value("layer", 0); layer > 0 && layer < LayerRegistry::kCount)
                world.Registry.emplace_or_replace<LayerComponent>(rootE, LayerComponent{layer});
            if (s.contains("tag"))
                world.Registry.emplace_or_replace<TagComponent>(rootE, s["tag"].get<std::string>());
            if (s.value("sceneHidden", false)) world.Registry.emplace_or_replace<HiddenInSceneTag>(rootE); // #122
            if (s.value("sceneLocked", false)) world.Registry.emplace_or_replace<SceneLockedTag>(rootE);
            ReadHierStyle(s, world, rootE);

            // #302 Part B — overrides: replay onto the freshly rebuilt subtree, by prefab-local
            // index into instCreated (== the .prefab file's entity order). Two passes so a field
            // override can't land before its addComponent (matters for hand-edited files; the
            // diff already emits them in order).
            if (!missing && s.contains("overrides") && s["overrides"].is_array()) {
                auto entityFor = [&](const json& ov) -> entt::entity {
                    const int idx = ov.value("e", -1);
                    return (idx >= 0 && idx < (int)instCreated.size() && world.Registry.valid(instCreated[idx]))
                               ? instCreated[idx] : entt::null;
                };
                for (const auto& ov : s["overrides"]) {                     // pass 1: structural
                    const std::string op = ov.value("op", std::string());
                    if (op.empty()) continue;
                    entt::entity e = entityFor(ov);
                    if (e == entt::null) continue;
                    const std::string cn = ov.value("c", std::string());
                    for (const auto& rc : ComponentRegistry::All()) {
                        if (!ReflectComponentMatches(rc.Meta, cn.c_str())) continue;
                        if (op == "addComponent")         { if (!rc.Has(world.Registry, e)) rc.Add(world.Registry, e); }
                        else if (op == "removeComponent") { if (rc.Has(world.Registry, e)) rc.Remove(world.Registry, e); }
                        break;
                    }
                }
                for (const auto& ov : s["overrides"]) {                     // pass 2: field values
                    if (ov.contains("op") || !ov.contains("v")) continue;
                    entt::entity e = entityFor(ov);
                    if (e == entt::null) continue;
                    ApplyPrefabOverride(world, assets, e,
                                        ov.value("c", std::string()), ov.value("f", std::string()), ov["v"]);
                }
            }

            applyOrder(rootE, s);
            for (entt::entity e : instCreated) created(e);

            const int id = s.value("id", -1);
            mapId(id, rootE);
            const int parentId = s.value("parentId", -1);
            if (parentId >= 0) pendingParents.emplace_back(rootE, parentId);
        }
    }

    if (clearFirst && maxLoadedOrder >= 0) world.EnsureNextOrderAtLeast(maxLoadedOrder + 1);

    for (const auto& [child, parentId] : pendingParents) {
        auto it = idToEntity.find(parentId);
        if (it != idToEntity.end()) world.AttachChildRaw(child, it->second);
    }

    // #202 - AttachChildRaw only refuses self-parenting, so a file whose parentIds form a loop
    // would load a genuine cycle: those entities are unreachable from any root, never draw, and
    // the loop gets written straight back out on the next save. Break it here, once the whole
    // graph exists, rather than refusing individual links in file order (which would depend on
    // which entity happened to be read first).
    world.RepairHierarchyCycles();

    // Terrains get their mesh now (the load gave them a flat stand-in), so Play's colliders, built before the
    // next frame's Sync, collide with the real surface.
    TerrainSystem::Sync(world);

    // #202 - clip planes are the one camera field where a bad value is fatal rather than ugly:
    // far == near divides by zero and a non-positive near makes the perspective divide
    // meaningless, so the whole view goes black. The Inspector already clamps them, but nothing
    // did on load, and a reflected float is read straight out of the JSON (a registry Min/Max is
    // a drag limit for the widget, not a load-time constraint). Correct it here and say so,
    // rather than leaving a scene that silently renders nothing.
    for (entt::entity e : world.Registry.view<CameraComponent>()) {
        auto& cam = world.Registry.get<CameraComponent>(e);
        const float near0 = cam.NearPlane, far0 = cam.FarPlane, fov0 = cam.FovDegrees;
        if (!(cam.NearPlane > 0.0f)) cam.NearPlane = 0.1f;                       // also catches NaN
        if (!(cam.FarPlane > cam.NearPlane)) cam.FarPlane = cam.NearPlane + 1000.0f;
        if (!(cam.FovDegrees > 0.0f) || cam.FovDegrees >= 180.0f) cam.FovDegrees = 60.0f;
        if (cam.NearPlane != near0 || cam.FarPlane != far0 || cam.FovDegrees != fov0)
            Log::Warn("Camera: " + EntityLogRef(world.Registry, e) + " had an unusable frustum "
                      "(near " + std::to_string(near0) + ", far " + std::to_string(far0) +
                      ", fov " + std::to_string(fov0) + ") - corrected on load.",
                      EntityLogContext(world.Registry, e));
    }

    // #119 — a copied joint whose partner was copied with it must connect to the partner's copy,
    // not the original (a partner outside the fragment keeps the original reference).
    if (!clearFirst) {
        for (const auto& [oldOrder, e] : sourceOrder) {
            auto* joint = world.Registry.try_get<JointComponent>(e);
            if (!joint || joint->ConnectedOrder < 0) continue;
            auto partner = sourceOrder.find(joint->ConnectedOrder);
            if (partner == sourceOrder.end()) continue;
            if (const auto* o = world.Registry.try_get<OrderComponent>(partner->second))
                joint->ConnectedOrder = o->Value;
        }
        if (outSourceOrder) *outSourceOrder = std::move(sourceOrder);
    }

    // Even an empty v3 scene must advance to v4 on disk; otherwise it would be treated as a
    // migration candidate on every open forever. A scene with rotations sets the same message
    // through migratedEulerRotations; the format check covers the empty-scene case.
    if (migratedEulerRotations || (clearFirst && formatVersion < 4))
        g_MigrationLog.push_back("transform rotation storage migrated from YXZ Euler degrees to quaternions");

    return true;
}

} // namespace SceneDetail
