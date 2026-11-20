#include "SceneSerializerInternal.h"

namespace SceneDetail {


// EnTT views iterate a pool newest-entity-first. Writing in that order would reverse the scene
// on every save (load re-creates in file order, the next save reverses it again), so the file
// churns and the Hierarchy's ordering flips each time. Collecting and reversing restores
// creation order, which is stable across any number of round-trips.
template <typename View>
std::vector<entt::entity> InCreationOrder(const entt::registry& reg, View view) {
    std::vector<entt::entity> entities(view.begin(), view.end());
    std::sort(entities.begin(), entities.end(), [&](entt::entity a, entt::entity b) {
        const auto* oa = reg.try_get<OrderComponent>(a);
        const auto* ob = reg.try_get<OrderComponent>(b);
        int va = oa ? oa->Value : 0, vb = ob ? ob->Value : 0;
        return va != vb ? va < vb : a < b; // stable tiebreak for pre-OrderComponent data
    });
    return entities;
}

// When `only` is non-null, just the entities it names are written (an entity-subset fragment
// for the clipboard or a prefab) and the sky settings are left out, since pasting a couple of
// objects must not also overwrite the destination scene's environment.
// flattenPrefabInstances: write prefab-instance subtrees in full (as plain entities, no stub,
// no link) instead of collapsing them to a stub. Used when saving a .prefab file — a prefab
// asset must be self-contained; nested prefab links are stage 4.
// The physical sky's settings (World::Sky) <-> a "sky" JSON object. One table of fields so the
// two directions can't drift apart; anything missing on load keeps its default, so older scenes
// and hand-edited files load cleanly.
struct SkyFloatField { const char* key; float SkySettings::* member; float lo, hi; };
struct SkyBoolField { const char* key; bool SkySettings::* member; };
struct SkyVec3Field { const char* key; glm::vec3 SkySettings::* member; };
const SkyFloatField kSkyFloatFields[] = {
    {"timeOfDayHours", &SkySettings::TimeOfDayHours, 0.0f, 24.0f},
    {"dayOfYear", &SkySettings::DayOfYear, 1.0f, 365.0f},
    {"latitude", &SkySettings::LatitudeDegrees, -90.0f, 90.0f},
    {"northOffset", &SkySettings::NorthOffsetDegrees, -360.0f, 360.0f},
    {"dayLengthMinutes", &SkySettings::DayLengthMinutes, 0.0f, 1440.0f},
    {"sunDiscBrightness", &SkySettings::SunDiscBrightness, 0.0f, 100.0f},
    {"sunDiscScale", &SkySettings::SunDiscScale, 0.05f, 50.0f},
    {"moonBrightness", &SkySettings::MoonBrightness, 0.0f, 1.0f},
    {"moonPhaseOffset", &SkySettings::MoonPhaseOffset, 0.0f, 1.0f},
    {"moonDiscScale", &SkySettings::MoonDiscScale, 0.05f, 50.0f},
    {"starsBrightness", &SkySettings::StarsBrightness, 0.0f, 100.0f},
    {"nightGlow", &SkySettings::NightGlow, 0.0f, 10.0f},
    {"nightBrightness", &SkySettings::NightBrightness, 0.0f, 12.0f},
    {"skyIntensity", &SkySettings::SkyIntensity, 0.0f, 100.0f},
    {"rayleighScale", &SkySettings::RayleighScale, 0.0f, 100.0f},
    {"mieScale", &SkySettings::MieScale, 0.0f, 100.0f},
    {"mieAnisotropy", &SkySettings::MieAnisotropy, 0.0f, 0.99f},
    {"mieAbsorption", &SkySettings::MieAbsorption, 0.0f, 100.0f},
    {"ozoneScale", &SkySettings::OzoneScale, 0.0f, 100.0f},
    {"planetRadiusKm", &SkySettings::PlanetRadiusKm, 10.0f, 100000.0f},
    {"atmosphereHeightKm", &SkySettings::AtmosphereHeightKm, 1.0f, 1000.0f},
    {"rayleighHeightKm", &SkySettings::RayleighHeightKm, 0.1f, 100.0f},
    {"mieHeightKm", &SkySettings::MieHeightKm, 0.05f, 100.0f},
    {"multipleScattering", &SkySettings::MultipleScattering, 0.0f, 10.0f},
    {"aerialPerspectiveScale", &SkySettings::AerialPerspectiveScale, 0.0f, 1000.0f},
    {"seaLevel", &SkySettings::SeaLevel, -1.0e6f, 1.0e6f},
    {"altitudeOffsetMeters", &SkySettings::AltitudeOffsetMeters, -1000.0f, 50000.0f},
    {"cloudCoverage", &SkySettings::CloudCoverage, 0.0f, 1.0f},
    {"cloudDensity", &SkySettings::CloudDensity, 0.0f, 20.0f},
    {"cloudType", &SkySettings::CloudType, 0.0f, 1.0f},
    {"cloudBaseMeters", &SkySettings::CloudBaseMeters, 0.0f, 20000.0f},
    {"cloudThicknessMeters", &SkySettings::CloudThicknessMeters, 50.0f, 20000.0f},
    {"cloudScale", &SkySettings::CloudScale, 0.05f, 20.0f},
    {"cloudDetail", &SkySettings::CloudDetail, 0.0f, 1.0f},
    {"cloudWindSpeed", &SkySettings::CloudWindSpeed, 0.0f, 500.0f},
    {"cloudWindDirection", &SkySettings::CloudWindDirectionDegrees, -360.0f, 360.0f},
    {"windStrength", &SkySettings::WindStrength, 0.0f, 4.0f},
    {"windGustiness", &SkySettings::WindGustiness, 0.0f, 1.0f},
    {"windGustSpeed", &SkySettings::WindGustSpeed, 0.0f, 30.0f},
    {"cloudWindShear", &SkySettings::CloudWindShear, 0.0f, 20.0f},
    {"cloudForwardScattering", &SkySettings::CloudForwardScattering, 0.0f, 0.95f},
    {"cloudAmbient", &SkySettings::CloudAmbient, 0.0f, 10.0f},
    {"cloudPowder", &SkySettings::CloudPowder, 0.0f, 1.0f},
    {"cloudMultiScattering", &SkySettings::CloudMultiScattering, 0.0f, 1.5f},
    {"cloudHorizonHaze", &SkySettings::CloudHorizonHaze, 0.0f, 1.0f},
    {"cloudSeed", &SkySettings::CloudSeed, -1.0e6f, 1.0e6f},
    {"cloudShadowStrength", &SkySettings::CloudShadowStrength, 0.0f, 1.0f},
    {"cirrusCoverage", &SkySettings::CirrusCoverage, 0.0f, 1.0f},
    {"cirrusAltitudeMeters", &SkySettings::CirrusAltitudeMeters, 1000.0f, 30000.0f},
    {"cirrusScale", &SkySettings::CirrusScale, 0.05f, 20.0f},
};
const SkyBoolField kSkyBoolFields[] = {
    {"timeOfDay", &SkySettings::TimeOfDayEnabled},
    {"animateInEditor", &SkySettings::AnimateInEditor},
    {"atmosphereTintsSun", &SkySettings::AtmosphereTintsSun},
    {"showGround", &SkySettings::ShowGround},
    {"moon", &SkySettings::MoonEnabled},
    {"clouds", &SkySettings::CloudsEnabled},
    {"cloudShadows", &SkySettings::CloudShadows},
};
const SkyVec3Field kSkyVec3Fields[] = {
    {"rayleighTint", &SkySettings::RayleighTint},
    {"groundAlbedo", &SkySettings::GroundAlbedo},
};

json SkySettingsToJson(const SkySettings& s) {
    json j;
    for (const auto& f : kSkyFloatFields) j[f.key] = s.*(f.member);
    for (const auto& f : kSkyBoolFields) j[f.key] = s.*(f.member);
    for (const auto& f : kSkyVec3Fields) j[f.key] = Vec3ToJson(s.*(f.member));
    j["cloudQuality"] = s.CloudQuality;
    return j;
}

SkySettings SkySettingsFromJson(const json& j) {
    SkySettings s;
    if (!j.is_object()) return s;
    for (const auto& f : kSkyFloatFields) {
        auto it = j.find(f.key);
        if (it != j.end() && it->is_number()) s.*(f.member) = std::clamp(it->get<float>(), f.lo, f.hi);
    }
    for (const auto& f : kSkyBoolFields) {
        auto it = j.find(f.key);
        if (it != j.end() && it->is_boolean()) s.*(f.member) = it->get<bool>();
    }
    for (const auto& f : kSkyVec3Fields) {
        auto it = j.find(f.key);
        if (it != j.end()) s.*(f.member) = glm::max(JsonToVec3(*it, s.*(f.member)), glm::vec3(0.0f));
    }
    if (auto it = j.find("cloudQuality"); it != j.end() && it->is_number_integer())
        s.CloudQuality = std::clamp(it->get<int>(), 0, 3);
    return s;
}

json BuildSceneJson(const World& world, const std::set<entt::entity>* only,
                    bool flattenPrefabInstances) {
    json root;
    AssetRefWriteCache refs; // per-path write path + GUID, worked out once per build
    std::unordered_map<std::string, std::optional<json>> prefabFiles; // each instance's .prefab, parsed once (nullopt: unreadable)
    auto included = [&](entt::entity e) { return !only || only->count(e) > 0; };
    // v4 changes the shape of every "rotation" value, so fragments and prefabs now need the
    // version too even though they still omit scene environment settings.
    root["formatVersion"] = kSceneFormatVersion;
    if (!only) {
        root["skyHorizonColor"] = Vec3ToJson(world.SkyHorizonColor);
        root["skyZenithColor"] = Vec3ToJson(world.SkyZenithColor);
        root["skyAmbientIntensity"] = world.SkyAmbientIntensity; // #196
        // PR13: HDRI sky source (defaults omitted for backwards compatibility)
        if (world.SkySourceMode != World::SkySource::Procedural)
            root["skySource"] = (int)world.SkySourceMode;
        // The physical sky's settings: written when the scene uses it or has changed them.
        if (world.SkySourceMode == World::SkySource::Atmosphere || !(world.Sky == SkySettings{}))
            root["sky"] = SkySettingsToJson(world.Sky);
        if (!world.SkyHdriPath.empty()) {
            root["skyHdriPath"] = AssetPathForWrite(world.SkyHdriPath); // audit #364 — was stored absolute
            // #132 — and by GUID, so renaming/moving the .hdr doesn't break the sky.
            AssetGuid g = AssetDatabase::GuidForPath(world.SkyHdriPath); // registered by ScanProject
            if (g.IsValid()) root["skyHdriGuid"] = g.ToString();
        }
        if (world.SkyRotationDegrees != 0.0f)
            root["skyRotationDegrees"] = world.SkyRotationDegrees;
        if (world.SkyHdriSun != World::HdriSunMode::Auto)
            root["skyHdriSun"] = (int)world.SkyHdriSun; // #277
        if (world.SkyHdriSunThreshold != 50.0f)
            root["skyHdriSunThreshold"] = world.SkyHdriSunThreshold;

        // #9, Phase M item 1 — post-processing/shadow settings, scene-authored since v3.
        root["exposureEV"] = world.ExposureEV;
        root["tonemapOperator"] = world.TonemapOperator;
        root["msaaSamples"] = world.MsaaSamples;
        root["ssaoEnabled"] = world.SsaoEnabled;
        root["ssaoRadius"] = world.SsaoRadius;
        root["ssaoBias"] = world.SsaoBias;
        root["ssaoIntensity"] = world.SsaoIntensity;
        root["bloomEnabled"] = world.BloomEnabled;
        root["bloomThreshold"] = world.BloomThreshold;
        root["bloomKnee"] = world.BloomKnee;
        root["bloomIntensity"] = world.BloomIntensity;
        // #162
        root["fxaa"] = world.FxaaEnabled;
        root["autoExposure"] = world.AutoExposure;
        root["autoExposureMinEV"] = world.AutoExposureMinEV;
        root["autoExposureMaxEV"] = world.AutoExposureMaxEV;
        root["autoExposureSpeedUp"] = world.AutoExposureSpeedUp;
        root["autoExposureSpeedDown"] = world.AutoExposureSpeedDown;
        root["gradeTemperature"] = world.GradeTemperature;
        root["gradeTint"] = world.GradeTint;
        root["gradeContrast"] = world.GradeContrast;
        root["gradeSaturation"] = world.GradeSaturation;
        root["gradeColorFilter"] = {world.GradeColorFilter.r, world.GradeColorFilter.g, world.GradeColorFilter.b};
    root["depthOfField"] = world.DepthOfField;
    root["focusDistance"] = world.FocusDistance;
    // Anti-aliasing mode: written only when it says more than msaaSamples + fxaa already do (TAA), so
    // scenes that never touch it save byte-for-byte as before. Rain wetness likewise only when set.
    if (world.AntiAliasing != World::LegacyAntiAliasing(world.MsaaSamples, world.FxaaEnabled)) {
        static const char* const kAa[] = {"Off", "FXAA", "MSAA", "TAA"};
        root["antiAliasing"] = kAa[std::clamp((int)world.AntiAliasing, 0, 3)];
    }
    if (world.TaaSharpness != 0.35f) root["taaSharpness"] = world.TaaSharpness;
    if (world.Wetness != 0.0f) root["wetness"] = world.Wetness;
    if (world.PuddleAmount != 0.0f) root["puddleAmount"] = world.PuddleAmount;
    if (world.PuddleScale != 4.0f) root["puddleScale"] = world.PuddleScale;
    if (world.PuddleSsr) root["puddleSsr"] = true;
    root["focusRange"] = world.FocusRange;
    root["dofMaxBlur"] = world.DofMaxBlur;
        root["fogEnabled"] = world.FogEnabled;
        root["fogMode"] = world.FogMode;
        root["fogColor"] = {world.FogColor.r, world.FogColor.g, world.FogColor.b};
        root["fogDensity"] = world.FogDensity;
        root["fogStart"] = world.FogStart;
        root["fogEnd"] = world.FogEnd;
        root["fogHeightFalloff"] = world.FogHeightFalloff;
        root["fogBaseHeight"] = world.FogBaseHeight;
        root["volumetricEnabled"] = world.VolumetricEnabled; // VolumetricFog
        root["volDensity"] = world.VolumetricDensity;
        root["volAnisotropy"] = world.VolumetricAnisotropy;
        root["volHeightFalloff"] = world.VolumetricHeightFalloff;
        root["volBaseHeight"] = world.VolumetricBaseHeight;
        root["volAlbedo"] = {world.VolumetricAlbedo.r, world.VolumetricAlbedo.g, world.VolumetricAlbedo.b};
        root["volDistance"] = world.VolumetricDistance;
        root["volQuality"] = world.VolumetricQuality;
        root["shadowsEnabled"] = world.ShadowsEnabled;
        root["shadowResolution"] = world.ShadowResolution;
        root["shadowCascades"] = world.ShadowCascades;
        root["shadowDistance"] = world.ShadowDistance;
        root["shadowCascadeSplit"] = world.ShadowCascadeSplit;
        root["maxSpotShadows"] = world.MaxSpotShadows;
        root["maxPointShadows"] = world.MaxPointShadows;
    }

    // Every box/model entity gets a stable 0-based id (assigned in the exact order written
    // below) so HierarchyComponent parent links — engine-internal entt::entity handles — can
    // round-trip through JSON as plain integers instead.
    std::unordered_map<entt::entity, int> idOf;
    int nextId = 0;
    auto assignId = [&](entt::entity e) { idOf[e] = nextId++; };
    auto parentIdOf = [&](entt::entity e) -> int {
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        if (!hier || hier->Parent == entt::null) return -1;
        auto it = idOf.find(hier->Parent);
        return it != idOf.end() ? it->second : -1;
    };

    // An entity whose real parent isn't part of this subset becomes a "false root" once
    // written — parentIdOf reports -1 for it, exactly like a genuinely unparented entity. But
    // its TransformComponent is still LOCAL (relative to that real, excluded parent); writing
    // those raw numbers under a parentId of -1 would have a reload place it using local
    // coordinates as if they were world coordinates, landing it wherever that ratio happens to
    // put it instead of where it visually was. Decomposing the live world-space matrix instead
    // keeps a copy/prefab of a child object anchored where it actually appeared. An entity
    // whose parent IS included, or that's genuinely unparented, is untouched (returned as-is) —
    // this only ever changes behavior for the subset case (`only != nullptr`), never a full
    // scene save, since `included()` is unconditionally true there.
    auto effectiveTransform = [&](entt::entity e) -> TransformComponent {
        const auto& t = world.Registry.get<TransformComponent>(e);
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        bool becomingFalseRoot = hier && hier->Parent != entt::null && !included(hier->Parent);
        if (!becomingFalseRoot) return t;

        glm::mat4 worldMatrix = world.ComposeWorldTransform(e);
        glm::vec3 pos, scale, skew; glm::vec4 persp; glm::quat rot;
        glm::decompose(worldMatrix, scale, rot, pos, skew, persp);
        TransformComponent out;
        out.Position = pos;
        out.Scale = scale;
        out.SetRotationQuaternion(rot);
        return out;
    };

    json boxes = json::array();
    // #122 — NameComponent is not required: an entity without one (created by code, or a future
    // component-only path) used to be silently dropped from every save. It is written as
    // "GameObject" instead.
    auto nameOf = [&](entt::entity e) -> std::string {
        const auto* nc = world.Registry.try_get<NameComponent>(e);
        return nc ? nc->Name : std::string("GameObject");
    };
    auto boxView = world.Registry.view<const TransformComponent,
        const RenderableComponent, const LevelGeometryTag>();
    auto modelViewForIds = world.Registry.view<const TransformComponent,
        const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    // Entities with no mesh at all — lights and plain empties used as grouping pivots. They
    // live in their own array rather than "models" because every "models" entry needs a `path`
    // to reload geometry from, and these have none.
    auto emptyViewForIds = world.Registry.view<const TransformComponent>(
        entt::exclude<RenderableComponent>);

    std::vector<entt::entity> boxEntities = InCreationOrder(world.Registry, boxView);
    std::vector<entt::entity> modelEntities = InCreationOrder(world.Registry, modelViewForIds);
    std::vector<entt::entity> emptyEntities = InCreationOrder(world.Registry, emptyViewForIds);

    // #236 A2 — a live prefab instance (root has PrefabInstanceComponent) is written as one
    // compact stub in "prefabInstances"; its descendants are not written at all. The owned set
    // is recomputed here from each root's HierarchyComponent, so no per-descendant marker is
    // needed. `flattenPrefabInstances` (saving a .prefab) disables all of this.
    std::set<entt::entity> prefabOwned;       // descendants — skipped entirely
    std::vector<entt::entity> prefabRoots;    // holders — written as stubs, in creation order
    if (!flattenPrefabInstances) {
        std::function<void(entt::entity)> markSubtree = [&](entt::entity e) {
            const auto* h = world.Registry.try_get<HierarchyComponent>(e);
            if (!h) return;
            for (entt::entity c : h->Children) {
                if (!included(c) || !prefabOwned.insert(c).second) continue;
                markSubtree(c);
            }
        };
        auto collect = [&](const std::vector<entt::entity>& v) {
            for (entt::entity e : v) {
                if (!included(e) || !world.Registry.all_of<PrefabInstanceComponent>(e)) continue;
                prefabRoots.push_back(e);
                markSubtree(e);
            }
        };
        collect(boxEntities); collect(modelEntities); collect(emptyEntities);
    }
    // An id goes to every entity that will be referenced by a parentId — normal writable
    // entities and prefab-instance roots (stubs), but not pure prefab descendants.
    auto skipWrite = [&](entt::entity e) {
        return !included(e) || prefabOwned.count(e) > 0;
    };
    auto isPrefabRoot = [&](entt::entity e) {
        return !flattenPrefabInstances && world.Registry.all_of<PrefabInstanceComponent>(e);
    };

    for (auto entity : boxEntities)   { if (!skipWrite(entity)) assignId(entity); }
    for (auto entity : modelEntities) { if (!skipWrite(entity)) assignId(entity); }
    for (auto entity : emptyEntities) { if (!skipWrite(entity)) assignId(entity); }

    for (auto entity : boxEntities) {
        if (skipWrite(entity) || isPrefabRoot(entity)) continue; // #236 A2
        TransformComponent transform = effectiveTransform(entity);
        const std::string name = nameOf(entity);
        const auto& renderable = boxView.get<const RenderableComponent>(entity);
        if (!renderable.ModelRef) continue; // a box is always built with its cube; nothing to write without it
        json b = {
            {"position", Vec3ToJson(transform.Position)},
            {"scale", Vec3ToJson(transform.Scale)},
            {"color", Vec3ToJson(renderable.ModelRef->MeshMaterial(0).BaseColor)},
            {"rotation", QuatToJson(transform.Rotation)},
            {"name", name},
            {"id", idOf[entity]},
            {"parentId", parentIdOf(entity)},
        };
        if (json slots = MaterialSlotsToJson(renderable.Materials, &refs); !slots.is_null()) b["materials"] = std::move(slots); // #120
        WriteRendererFlags(b, renderable); // #163
        WriteCommonComponents(b, world, entity);
        boxes.push_back(std::move(b));
    }
    root["boxes"] = std::move(boxes);

    json empties = json::array();
    for (auto entity : emptyEntities) {
        if (skipWrite(entity) || isPrefabRoot(entity)) continue; // #236 A2
        TransformComponent transform = effectiveTransform(entity);
        json e = {
            {"name", nameOf(entity)},
            {"position", Vec3ToJson(transform.Position)},
            {"rotation", QuatToJson(transform.Rotation)},
            {"scale", Vec3ToJson(transform.Scale)},
            {"id", idOf[entity]},
            {"parentId", parentIdOf(entity)},
        };
        WriteCommonComponents(e, world, entity);
        empties.push_back(std::move(e));
    }
    root["empties"] = std::move(empties);

    json models = json::array();
    for (auto entity : modelEntities) {
        if (skipWrite(entity) || isPrefabRoot(entity)) continue; // #236 A2
        TransformComponent transform = effectiveTransform(entity);
        const auto& renderable = modelViewForIds.get<const RenderableComponent>(entity);
        if (!renderable.ModelRef) continue; // nothing to reload it from (a load always makes a Model)

        json m;
        const AssetRefWriteCache::Ref& ref = refs.Get(renderable.ModelRef->Path());
        m["path"] = ref.WritePath; // audit #364
        if (!ref.Guid.empty()) m["pathGuid"] = ref.Guid;
        m["name"] = nameOf(entity);
        m["position"] = Vec3ToJson(transform.Position);
        m["rotation"] = QuatToJson(transform.Rotation);
        m["scale"] = Vec3ToJson(transform.Scale);
        // AudioSourceComponent moved onto reflection (#302 Wave 3): it now round-trips through
        // the generic "Audio Source" block written by WriteCommonComponents below (for every
        // entity kind, not just models). The old flat "sound*" keys are still READ.
        m["id"] = idOf[entity];
        m["parentId"] = parentIdOf(entity);

        if (json slots = MaterialSlotsToJson(renderable.Materials, &refs); !slots.is_null()) m["materials"] = std::move(slots);
        WriteRendererFlags(m, renderable); // #163
        WriteCommonComponents(m, world, entity);
        models.push_back(std::move(m));
    }
    root["models"] = std::move(models);

    // #236 A2 — prefab-instance stubs. Just the source path plus the root's own transform /
    // name / tags (the transform-only overrides); the subtree is rebuilt from the .prefab on
    // load and these re-applied on top.
    if (!prefabRoots.empty()) {
        json instances = json::array();
        for (entt::entity e : prefabRoots) {
            const auto& pi = world.Registry.get<PrefabInstanceComponent>(e);
            TransformComponent t = effectiveTransform(e);
            json s;
            s["source"] = AssetPathForWrite(pi.SourcePath); // audit #364
            {
                AssetGuid g = AssetDatabase::GuidForPath(pi.SourcePath);
                if (g.IsValid()) s["sourceGuid"] = g.ToString();
            }
            s["name"]     = nameOf(e);
            s["position"] = Vec3ToJson(t.Position);
            s["rotation"] = QuatToJson(t.Rotation);
            s["scale"]    = Vec3ToJson(t.Scale);
            s["id"]       = idOf[e];
            s["parentId"] = parentIdOf(e);
            if (const auto* o = world.Registry.try_get<OrderComponent>(e)) s["order"] = o->Value;
            if (world.Registry.all_of<DeactivatedTag>(e)) s["active"] = false;
            if (world.Registry.all_of<StaticTag>(e)) s["static"] = true;
            if (const auto* lc = world.Registry.try_get<LayerComponent>(e); lc && lc->Layer != 0)
                s["layer"] = lc->Layer;
            if (const auto* tag = world.Registry.try_get<TagComponent>(e)) s["tag"] = tag->Tag;
            // #122 — the root's editor visibility / lock (non-stub entities get these through
            // WriteCommonComponents).
            if (world.Registry.all_of<HiddenInSceneTag>(e)) s["sceneHidden"] = true;
            if (world.Registry.all_of<SceneLockedTag>(e)) s["sceneLocked"] = true;
            if (const auto* hs = world.Registry.try_get<HierarchyStyleComponent>(e); hs && !hs->IsEmpty())
                s["hierStyle"] = HierStyleToJson(*hs); // a styled prefab-instance root keeps its row style

            // #302 Part B — per-field overrides: diff each live instance entity against its
            // pristine counterpart in the .prefab file, by prefab-local index.
            if (!pi.InstanceEntities.empty()) {
                // Read once per save, however many instances of it the scene holds (the save runs on every undo commit).
                auto [slot, fresh] = prefabFiles.try_emplace(pi.SourcePath);
                if (fresh) {
                    std::ifstream pf(std::filesystem::u8path(pi.SourcePath));
                    if (pf.is_open()) {
                        // #82 — a corrupt / merge-conflicted .prefab must not abort the whole scene
                        // save: skip this instance's override diff (its stub still saves) and warn.
                        try { json parsed; pf >> parsed; UpgradeLegacyBoxKeys(parsed); slot->second = std::move(parsed); }
                        catch (const std::exception& ex) {
                            Log::Warn("Scene: couldn't parse prefab '" + pi.SourcePath +
                                      "' while saving - its per-field overrides were not re-diffed: " + ex.what());
                        }
                    }
                }
                const bool prefabOk = slot->second.has_value();
                if (prefabOk) {
                    const json& prefab = *slot->second;
                    std::vector<const json*> pristineEnts = PrefabEntityObjects(prefab);
                    json overrides = json::array();
                    const std::size_t n = std::min(pi.InstanceEntities.size(), pristineEnts.size());
                    for (std::size_t i = 0; i < n; ++i) {
                        entt::entity le = pi.InstanceEntities[i];
                        if (!world.Registry.valid(le)) continue;
                        DiffPrefabEntity(world, le, *pristineEnts[i], (int)i, /*isRoot=*/le == e,
                                         effectiveTransform, overrides);
                    }
                    if (!overrides.empty()) s["overrides"] = std::move(overrides);
                }
            }

            instances.push_back(std::move(s));
        }
        root["prefabInstances"] = std::move(instances);
    }

    return root;
}

// audit #364 validation guard: after a scene JSON is built, warn about any string value that
// still looks machine-absolute (drive letter, UNC prefix) or contains a "../" traversal. These
// don't survive a move to another machine/checkout. A path to a genuinely out-of-project shared
// asset library legitimately stays absolute, so this warns (names the JSON pointer) rather than
// failing the save.
bool LooksNonPortable(const std::string& s) {
    if (s.size() >= 2 && std::isalpha((unsigned char)s[0]) && s[1] == ':' &&
        (s.size() == 2 || s[2] == '/' || s[2] == '\\'))
        return true;                                   // C:\ or C:/
    if (s.rfind("\\\\", 0) == 0) return true;          // \\server\share (UNC)
    if (s.find("/../") != std::string::npos || s.find("\\..\\") != std::string::npos ||
        s.rfind("../", 0) == 0 || s.rfind("..\\", 0) == 0)
        return true;
    return false;
}

void WarnNonPortablePaths(const json& node, const std::string& pointer) {
    if (node.is_string()) {
        const std::string& s = node.get_ref<const std::string&>();
        if (LooksNonPortable(s))
            Log::Warn("Scene: non-portable path at '" + pointer + "': '" + s +
                      "' — will not resolve on another machine/checkout (audit #364).");
    } else if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it)
            WarnNonPortablePaths(it.value(), pointer + "/" + it.key());
    } else if (node.is_array()) {
        for (size_t i = 0; i < node.size(); ++i)
            WarnNonPortablePaths(node[i], pointer + "/" + std::to_string(i));
    }
}


} // namespace SceneDetail
