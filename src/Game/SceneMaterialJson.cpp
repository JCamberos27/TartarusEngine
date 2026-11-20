#include "SceneSerializerInternal.h"

namespace SceneDetail {


// Material slots of a RenderableComponent <-> JSON. Shared by boxes and models: #120 — boxes
// used to write only their colour, so a .mat or an embedded PBR material assigned to a
// level-geometry box was silently dropped on save, on every undo/redo and on Play -> Stop.
json MaterialSlotsToJson(const std::vector<std::shared_ptr<MaterialAsset>>& matSlots, AssetRefWriteCache* refs) {
    if (matSlots.empty()) return nullptr;
    AssetRefWriteCache local; // a caller without a build-wide cache
    if (!refs) refs = &local;
    json arr = json::array();
    for (const auto& slot : matSlots) {
        if (!slot) { arr.push_back(nullptr); continue; }
        if (!slot->Path.empty()) {
            json entry;
            const AssetRefWriteCache::Ref& r = refs->Get(slot->Path);
            if (!r.Guid.empty()) entry["guid"] = r.Guid;
            entry["path"] = r.WritePath; // audit #364
            arr.push_back(std::move(entry));
        } else {
            const auto& smat = slot->Mat;
            json em = {
                {"factorsScaleMaps", true}, // #102 — see ReadEmbeddedMat
                {"baseColor", Vec3ToJson(smat.BaseColor)},
                {"metallic", smat.Metallic},
                {"roughness", smat.Roughness},
                {"emissiveColor", Vec3ToJson(smat.EmissiveColor)},
                {"emissiveStrength", smat.EmissiveStrength},
                {"triplanar", smat.Triplanar},
                {"triplanarScale", smat.TriplanarScale},
                {"albedoMap",            PathRef(PathOrEmpty(smat.AlbedoMap))},
                {"normalMap",            PathRef(PathOrEmpty(smat.NormalMap))},
                {"metallicRoughnessMap", PathRef(PathOrEmpty(smat.MetallicRoughnessMap))},
                {"metallicMap",          PathRef(PathOrEmpty(smat.MetallicMap))},
                {"roughnessMap",         PathRef(PathOrEmpty(smat.RoughnessMap))},
                {"aoMap",                PathRef(PathOrEmpty(smat.AOMap))},
                {"emissiveMap",          PathRef(PathOrEmpty(smat.EmissiveMap))},
            };
            // #120 — the advanced lobes and surface settings were not persisted for embedded
            // materials either. Written only when not default, to keep scene diffs small.
            if (smat.ClearCoat != 0.0f)            em["clearCoat"] = smat.ClearCoat;
            if (smat.ClearCoatRoughness != 0.5f)   em["clearCoatRoughness"] = smat.ClearCoatRoughness;
            if (smat.Anisotropy != 0.0f)           em["anisotropy"] = smat.Anisotropy;
            if (smat.AnisotropyRotation != 0.0f)   em["anisotropyRotation"] = smat.AnisotropyRotation;
            if (smat.Sheen != glm::vec3(0.0f))     em["sheen"] = Vec3ToJson(smat.Sheen);
            if (smat.SheenRoughness != 0.5f)       em["sheenRoughness"] = smat.SheenRoughness;
            if (smat.SubsurfaceEnabled)            em["subsurface"] = true;
            if (smat.SubsurfaceColor != glm::vec3(1.0f, 0.8f, 0.6f)) em["subsurfaceColor"] = Vec3ToJson(smat.SubsurfaceColor);
            if (smat.Thickness != 0.5f)            em["thickness"] = smat.Thickness;
            if (smat.TransmissionStrength != 0.0f) em["transmission"] = smat.TransmissionStrength;
            if (smat.IOR != 1.5f)                  em["ior"] = smat.IOR;
            if (smat.TriplanarBreakup != 0.0f)     em["triplanarBreakup"] = smat.TriplanarBreakup;
            if (smat.ReflectionProbes)             em["reflectionProbes"] = true;
            // #102 / #113 surface options
            if (smat.UVTiling != glm::vec2(1.0f))  em["uvTiling"] = {smat.UVTiling.x, smat.UVTiling.y};
            if (smat.UVOffset != glm::vec2(0.0f))  em["uvOffset"] = {smat.UVOffset.x, smat.UVOffset.y};
            if (smat.NormalStrength != 1.0f)       em["normalStrength"] = smat.NormalStrength;
            if (smat.NormalFlipY)                  em["normalFlipY"] = true;
            if (smat.DoubleSided)                  em["doubleSided"] = true;
            if (smat.UseVertexColor)               em["vertexColors"] = true;
            if (smat.HeightMap)                    em["heightMap"] = PathRef(PathOrEmpty(smat.HeightMap));
            if (smat.ParallaxScale != 0.02f)       em["parallaxScale"] = smat.ParallaxScale;
            if (smat.DetailAlbedoMap)              em["detailAlbedoMap"] = PathRef(PathOrEmpty(smat.DetailAlbedoMap));
            if (smat.DetailNormalMap)              em["detailNormalMap"] = PathRef(PathOrEmpty(smat.DetailNormalMap));
            if (smat.DetailTiling != glm::vec2(4.0f)) em["detailTiling"] = {smat.DetailTiling.x, smat.DetailTiling.y};
            // Grunge and Receive Decals (docs/DECALS.md), as a .mat writes them.
            if (smat.GrungeMap)                    em["grungeMap"] = PathRef(PathOrEmpty(smat.GrungeMap));
            if (smat.GrungeStrength != 0.0f)       em["grungeStrength"] = smat.GrungeStrength;
            if (smat.GrungeScale != 0.5f)          em["grungeScale"] = smat.GrungeScale;
            if (!smat.ReceiveDecals)               em["receiveDecals"] = false;
            if (smat.NoWetness)                    em["noWetness"] = true;
            // Shell fur (Material::FurDesc), non-defaults only, as a .mat writes it.
            {
                const Material::FurDesc def;
                if (smat.Fur.Enabled)                  em["furEnabled"] = true;
                if (smat.Fur.Length != def.Length)     em["furLength"] = smat.Fur.Length;
                if (smat.Fur.Density != def.Density)   em["furDensity"] = smat.Fur.Density;
                if (smat.Fur.Shells != def.Shells)     em["furShells"] = smat.Fur.Shells;
                if (smat.Fur.Gravity != def.Gravity)   em["furGravity"] = smat.Fur.Gravity;
                if (smat.Fur.Tint != def.Tint)         em["furTint"] = {smat.Fur.Tint.x, smat.Fur.Tint.y, smat.Fur.Tint.z};
                if (smat.Fur.Rim != def.Rim)           em["furRim"] = smat.Fur.Rim;
                if (smat.Fur.Mask)                     em["furMask"] = PathRef(PathOrEmpty(smat.Fur.Mask));
            }
            if (!smat.Substrate.Albedo.empty())
                em["substrate"] = SubstrateJson::Write<json>(smat.Substrate, [](const std::string& p) { return PathRef(ProjectPaths::Resolve(p)); });
            if (smat.AlphaCutoff != 0.5f)          em["alphaCutoff"] = smat.AlphaCutoff;
            if (slot->RenderQueue != MaterialAsset::Queue::Opaque) em["renderQueue"] = (int)slot->RenderQueue;
            if (slot->Opacity != 1.0f)             em["opacity"] = slot->Opacity;
            arr.push_back({{"embedded", std::move(em)}});
        }
    }
    return arr;
}

std::shared_ptr<MaterialAsset> ReadEmbeddedMat(AssetLibrary& assets, const json& mj) {
    auto ma = std::make_shared<MaterialAsset>();
    ma->Mat.BaseColor       = JsonToVec3(mj.value("baseColor", json::array({1, 1, 1})), glm::vec3(1.0f));
    ma->Mat.Metallic        = mj.value("metallic", 0.0f);
    ma->Mat.Roughness       = mj.value("roughness", 0.5f);
    ma->Mat.EmissiveColor   = JsonToVec3(mj.value("emissiveColor", json::array({0, 0, 0})));
    ma->Mat.EmissiveStrength= mj.value("emissiveStrength", 1.0f);
    ma->Mat.Triplanar       = mj.value("triplanar", false);
    ma->Mat.TriplanarScale  = mj.value("triplanarScale", 1.0f);
    ma->Mat.TriplanarBreakup = mj.value("triplanarBreakup", 0.0f);
    ma->Mat.AlbedoMap            = LoadIfPresent(assets, mj, "albedoMap");
    ma->Mat.NormalMap            = LoadIfPresent(assets, mj, "normalMap");
    ma->Mat.MetallicRoughnessMap = LoadIfPresent(assets, mj, "metallicRoughnessMap");
    ma->Mat.MetallicMap          = LoadIfPresent(assets, mj, "metallicMap");
    ma->Mat.RoughnessMap         = LoadIfPresent(assets, mj, "roughnessMap");
    ma->Mat.AOMap                = LoadIfPresent(assets, mj, "aoMap");
    ma->Mat.EmissiveMap          = LoadIfPresent(assets, mj, "emissiveMap");
    MaterialAsset::UpgradeLegacyMapFactors(ma->Mat, mj.value("factorsScaleMaps", false));
    ma->Mat.ClearCoat            = mj.value("clearCoat", 0.0f);
    ma->Mat.ClearCoatRoughness   = mj.value("clearCoatRoughness", 0.5f);
    ma->Mat.Anisotropy           = mj.value("anisotropy", 0.0f);
    ma->Mat.AnisotropyRotation   = mj.value("anisotropyRotation", 0.0f);
    ma->Mat.Sheen                = JsonToVec3(mj.value("sheen", json::array({0, 0, 0})));
    ma->Mat.SheenRoughness       = mj.value("sheenRoughness", 0.5f);
    ma->Mat.SubsurfaceEnabled    = mj.value("subsurface", false);
    ma->Mat.SubsurfaceColor      = JsonToVec3(mj.value("subsurfaceColor", json::array({1.0, 0.8, 0.6})), glm::vec3(1.0f, 0.8f, 0.6f));
    ma->Mat.Thickness            = mj.value("thickness", 0.5f);
    ma->Mat.TransmissionStrength = mj.value("transmission", 0.0f);
    ma->Mat.IOR                  = mj.value("ior", 1.5f);
    ma->Mat.ReflectionProbes     = mj.value("reflectionProbes", false);
    { // #102 / #113 surface options
        auto vec2Or = [&](const char* k, glm::vec2 def) {
            const auto it = mj.find(k);
            if (it == mj.end() || !it->is_array() || it->size() < 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) return def;
            return glm::vec2((*it)[0].get<float>(), (*it)[1].get<float>());
        };
        ma->Mat.UVTiling       = vec2Or("uvTiling", glm::vec2(1.0f));
        ma->Mat.UVOffset       = vec2Or("uvOffset", glm::vec2(0.0f));
        ma->Mat.DetailTiling   = vec2Or("detailTiling", glm::vec2(4.0f));
        ma->Mat.NormalStrength = mj.value("normalStrength", 1.0f);
        ma->Mat.NormalFlipY    = mj.value("normalFlipY", false);
        ma->Mat.DoubleSided    = mj.value("doubleSided", false);
        ma->Mat.UseVertexColor = mj.value("vertexColors", false);
        ma->Mat.ParallaxScale  = mj.value("parallaxScale", 0.02f);
        ma->Mat.HeightMap       = LoadIfPresent(assets, mj, "heightMap");
        ma->Mat.DetailAlbedoMap = LoadIfPresent(assets, mj, "detailAlbedoMap");
        ma->Mat.DetailNormalMap = LoadIfPresent(assets, mj, "detailNormalMap");
        // Grunge and Receive Decals (docs/DECALS.md). The grime map is colour, as MaterialAsset loads it.
        ma->Mat.GrungeMap       = LoadIfPresent(assets, mj, "grungeMap");
        ma->Mat.GrungeStrength  = mj.value("grungeStrength", 0.0f);
        ma->Mat.GrungeScale     = mj.value("grungeScale", 0.5f);
        ma->Mat.ReceiveDecals   = mj.value("receiveDecals", true);
        ma->Mat.NoWetness       = mj.value("noWetness", false);
        {
            Material::FurDesc& fur = ma->Mat.Fur;
            const Material::FurDesc def;
            fur.Enabled = mj.value("furEnabled", def.Enabled);
            fur.Length  = std::max(0.0f, mj.value("furLength", def.Length));
            fur.Density = std::max(1.0f, mj.value("furDensity", def.Density));
            fur.Shells  = std::clamp(mj.value("furShells", def.Shells), 0, Material::kMaxFurShells);
            fur.Gravity = mj.value("furGravity", def.Gravity);
            if (auto t = mj.find("furTint"); t != mj.end() && t->is_array() && t->size() >= 3)
                fur.Tint = glm::vec3((*t)[0].get<float>(), (*t)[1].get<float>(), (*t)[2].get<float>());
            fur.Rim  = std::max(0.0f, mj.value("furRim", def.Rim));
            fur.Mask = LoadIfPresent(assets, mj, "furMask");
        }
        // Substrate (Substrate.h): {path, pathGuid} refs, kept project-relative in memory like a .decal's.
        if (auto sub = mj.find("substrate"); sub != mj.end())
            SubstrateJson::Read(*sub, ma->Mat.Substrate, [](const json& v) {
                const std::string p = ResolveAssetRef(v);
                return p.empty() ? p : ProjectPaths::Relativize(p);
            });
    }
    ma->Mat.AlphaCutoff          = mj.value("alphaCutoff", 0.5f);
    ma->RenderQueue              = (MaterialAsset::Queue)std::clamp(mj.value("renderQueue", 0), 0, 2);
    ma->Mat.AlphaClip            = ma->RenderQueue == MaterialAsset::Queue::AlphaTest;
    ma->Opacity                  = mj.value("opacity", 1.0f);
    return ma;
}

// #163 - Cast/Receive Shadows. Written only when not the default, so untouched scenes don't change.
void WriteRendererFlags(json& obj, const RenderableComponent& rc) {
    if (rc.CastShadows != RenderableComponent::ShadowCasting::On) obj["castShadows"] = (int)rc.CastShadows;
    if (!rc.ReceiveShadows) obj["receiveShadows"] = false;
}
void ReadRendererFlags(const json& obj, RenderableComponent& rc) {
    const auto it = obj.find("castShadows");
    rc.CastShadows = (it != obj.end() && it->is_number_integer())
        ? (RenderableComponent::ShadowCasting)std::clamp(it->get<int>(), 0, 3)
        : RenderableComponent::ShadowCasting::On;
    rc.ReceiveShadows = obj.value("receiveShadows", true);
}

void ReadMaterialSlots(AssetLibrary& assets, const json& obj, RenderableComponent& rc) {
    if (obj.contains("materials") && obj["materials"].is_array()) {
        for (const auto& slot_j : obj["materials"]) {
            if (slot_j.is_null()) { rc.Materials.push_back(nullptr); continue; }
            if (slot_j.contains("embedded")) {
                rc.Materials.push_back(ReadEmbeddedMat(assets, slot_j["embedded"]));
            } else {
                std::string path = ResolveAssetRef(slot_j);
                rc.Materials.push_back(path.empty() ? nullptr : assets.LoadMaterial(path));
            }
        }
    } else if (obj.contains("material")) {
        // Legacy pre-PR5 format: single embedded Material -> slot 0
        rc.Materials.assign(1, ReadEmbeddedMat(assets, obj["material"]));
    }
}

} // namespace SceneDetail

using namespace SceneDetail;

std::string SceneSerializer::EmbeddedMaterialToJson(const MaterialAsset& material) {
    auto copy = std::make_shared<MaterialAsset>(material);
    copy->Path.clear(); // an embedded slot, not a reference to its .mat
    const json slots = MaterialSlotsToJson({copy});
    return slots.at(0).at("embedded").dump();
}

std::shared_ptr<MaterialAsset> SceneSerializer::EmbeddedMaterialFromJson(AssetLibrary& assets, const std::string& embeddedJson) {
    try {
        const json mj = json::parse(embeddedJson);
        return mj.is_object() ? ReadEmbeddedMat(assets, mj) : nullptr;
    } catch (const std::exception&) {
        return nullptr;
    }
}
