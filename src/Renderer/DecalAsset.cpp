#include "DecalAsset.h"
#include "AtomicFile.h"
#include "Log.h"
#include "SubstrateJson.h"

#include <json.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace {

// Type-checked reads over one JSON object: a missing key or a wrong type falls back to the
// default (with a warning for the wrong type), so a hand-edited .decal never throws out of Load.
struct Reader {
    const json& obj;
    const std::string& path;

    const json* Find(const char* k) const {
        if (!obj.is_object()) return nullptr;
        auto it = obj.find(k);
        return it == obj.end() || it->is_null() ? nullptr : &*it;
    }
    void Warn(const char* k, const char* want) const {
        Log::Warn("DecalAsset: '" + path + "': \"" + k + "\" should be " + want + " - using the default.",
                  LogContext::Asset(path));
    }
    Reader Sub(const char* k) const {
        const json* v = Find(k);
        static const json kEmpty = json::object();
        if (v && !v->is_object()) Warn(k, "an object");
        return {v && v->is_object() ? *v : kEmpty, path};
    }
    float Num(const char* k, float def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_number()) return v->get<float>();
        Warn(k, "a number"); return def;
    }
    int Int(const char* k, int def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_number()) return (int)v->get<double>();
        Warn(k, "a number"); return def;
    }
    bool Bool(const char* k, bool def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_boolean()) return v->get<bool>();
        if (v->is_number()) return v->get<double>() != 0.0;
        Warn(k, "true/false"); return def;
    }
    std::string Str(const char* k) const {
        const json* v = Find(k);
        if (!v) return {};
        if (v->is_string()) return v->get<std::string>();
        Warn(k, "a string"); return {};
    }
    // N numbers; anything shorter or non-numeric keeps the default.
    template <int N> glm::vec<N, float> Vec(const char* k, const glm::vec<N, float>& def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_array() && (int)v->size() >= N &&
            std::all_of(v->begin(), v->begin() + N, [](const json& e) { return e.is_number(); })) {
            glm::vec<N, float> out;
            for (int i = 0; i < N; ++i) out[i] = (*v)[i].get<float>();
            return out;
        }
        Warn(k, N == 2 ? "an array of 2 numbers" : "an array of 3 numbers"); return def;
    }
};

// Tier is a slot size in the decal arrays; anything else snaps to the nearest valid size.
int SnapTier(int t) {
    static constexpr int kTiers[] = {512, 1024, 2048, 4096};
    int best = kTiers[0];
    for (int s : kTiers)
        if (std::abs(s - t) < std::abs(best - t)) best = s;
    return best;
}

float Unit(float v) { return std::clamp(v, 0.0f, 1.0f); }

std::string TrimCopy(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

} // namespace

std::shared_ptr<DecalAsset> DecalAsset::Load(const std::string& path) {
    auto d = std::make_shared<DecalAsset>();
    d->Path = path;
    d->Name = std::filesystem::path(path).stem().string();

    std::ifstream f(path);
    if (!f.is_open()) {
        d->Missing = true;
        return d;
    }
    json j;
    try { j = json::parse(f); } catch (const std::exception& e) {
        Log::Error("DecalAsset: JSON parse error in '" + path + "': " + e.what(), LogContext::Asset(path));
        d->Missing = true;
        return d;
    }
    if (!j.is_object()) {
        Log::Error("DecalAsset: '" + path + "' is not a JSON object.", LogContext::Asset(path));
        d->Missing = true;
        return d;
    }

    const Reader r{j, path};
    if (r.Int("decalVersion", 1) > 1)
        Log::Warn("DecalAsset: '" + path + "' is from a newer engine (decalVersion > 1); unknown keys are ignored.",
                  LogContext::Asset(path));

    const Reader tex = r.Sub("textures");
    d->AlbedoPath = tex.Str("albedo");
    d->NormalPath = tex.Str("normal");
    d->SurfacePath = tex.Str("surface");
    d->AuxPath = tex.Str("aux");

    d->LumaMask = r.Bool("lumaMask", d->LumaMask);
    d->InvertMask = r.Bool("invertMask", d->InvertMask);
    d->Tier = SnapTier(r.Int("tier", d->Tier));
    const Reader atlas = r.Sub("atlas");
    d->AtlasCols = std::clamp(atlas.Int("cols", 1), 1, 64);
    d->AtlasRows = std::clamp(atlas.Int("rows", 1), 1, 64);
    d->DefaultSize = glm::max(r.Vec<3>("defaultSize", d->DefaultSize), glm::vec3(0.001f));

    const Reader ch = r.Sub("channels");
    d->AlbedoOp = Unit(ch.Num("albedo", d->AlbedoOp));
    d->NormalOp = Unit(ch.Num("normal", d->NormalOp));
    d->RoughnessOp = Unit(ch.Num("roughness", d->RoughnessOp));
    d->MetalOp = Unit(ch.Num("metal", d->MetalOp));
    d->AOOp = Unit(ch.Num("ao", d->AOOp));
    d->EmissiveOp = Unit(ch.Num("emissive", d->EmissiveOp));

    d->BaseColor = r.Vec<3>("baseColor", d->BaseColor);
    d->Roughness = Unit(r.Num("roughness", d->Roughness));
    d->Metal = Unit(r.Num("metal", d->Metal));
    d->Emissive = glm::max(r.Vec<3>("emissive", d->Emissive), glm::vec3(0.0f));

    const glm::vec2 angle = r.Vec<2>("angleFade", {d->AngleFadeStart, d->AngleFadeEnd});
    d->AngleFadeStart = Unit(angle.x);
    d->AngleFadeEnd = Unit(angle.y);
    d->EdgeFade = std::clamp(r.Num("edgeFade", d->EdgeFade), 0.0f, 0.5f);
    d->HeightBlend = Unit(r.Num("heightBlend", d->HeightBlend));
    d->HeightContrast = std::max(r.Num("heightContrast", d->HeightContrast), 0.0f);

    const Reader paint = r.Sub("paint");
    d->Paint = paint.Bool("enabled", d->Paint);
    d->PaintRoughness = Unit(paint.Num("roughness", d->PaintRoughness));
    d->PaintMetal = Unit(paint.Num("metal", d->PaintMetal));
    d->Relief = Unit(paint.Num("relief", d->Relief));
    d->UVPaint = paint.Bool("uv", d->UVPaint);

    const Reader wx = r.Sub("weathering");
    d->Fade = Unit(wx.Num("fade", d->Fade));
    d->Bleach = Unit(wx.Num("bleach", d->Bleach));
    d->Peel = Unit(wx.Num("peel", d->Peel));
    d->Buff = Unit(wx.Num("buff", d->Buff));
    d->SceneGrading = wx.Bool("sceneGrading", d->SceneGrading);

    if (const json* sub = r.Find("substrate")) {
        if (!SubstrateJson::Read(*sub, d->Substrate, SubstrateJson::PlainPath)) r.Warn("substrate", "an object");
    }

    d->Category = r.Str("category");
    if (const json* tags = r.Find("tags")) {
        if (tags->is_array()) {
            for (const json& t : *tags) {
                if (!t.is_string()) continue;
                const std::string s = TrimCopy(t.get<std::string>());
                if (s.empty()) continue;
                if (!d->Tags.empty()) d->Tags += ", ";
                d->Tags += s;
            }
        } else if (tags->is_string()) {
            d->Tags = tags->get<std::string>();
        } else {
            r.Warn("tags", "an array of strings");
        }
    }
    return d;
}

bool DecalAsset::Save() const {
    // ordered_json keeps the schema's key order (DecalAsset.h), so a saved file diffs cleanly
    // against a hand-written or generated one and every save of the same values is byte-identical.
    using ojson = nlohmann::ordered_json;
    ojson j;
    j["decalVersion"] = 1;
    j["textures"] = {{"albedo", AlbedoPath}, {"normal", NormalPath}, {"surface", SurfacePath}, {"aux", AuxPath}};
    j["lumaMask"] = LumaMask;
    j["invertMask"] = InvertMask;
    j["tier"] = Tier;
    j["atlas"] = {{"cols", AtlasCols}, {"rows", AtlasRows}};
    j["defaultSize"] = {DefaultSize.x, DefaultSize.y, DefaultSize.z};
    j["channels"] = {{"albedo", AlbedoOp}, {"normal", NormalOp}, {"roughness", RoughnessOp},
                     {"metal", MetalOp},   {"ao", AOOp},         {"emissive", EmissiveOp}};
    j["baseColor"] = {BaseColor.r, BaseColor.g, BaseColor.b};
    j["roughness"] = Roughness;
    j["metal"] = Metal;
    j["emissive"] = {Emissive.r, Emissive.g, Emissive.b};
    j["angleFade"] = {AngleFadeStart, AngleFadeEnd};
    j["edgeFade"] = EdgeFade;
    j["heightBlend"] = HeightBlend;
    j["heightContrast"] = HeightContrast;
    j["paint"] = {{"enabled", Paint}, {"roughness", PaintRoughness}, {"metal", PaintMetal},
                  {"relief", Relief},  {"uv", UVPaint}};
    j["weathering"] = {{"fade", Fade}, {"bleach", Bleach}, {"peel", Peel}, {"buff", Buff}, {"sceneGrading", SceneGrading}};
    if (!Substrate.Albedo.empty())
        j["substrate"] = SubstrateJson::Write<ojson>(Substrate, [](const std::string& p) { return p; });
    j["category"] = Category;
    ojson tags = ojson::array();
    size_t start = 0;
    while (start <= Tags.size()) {
        const size_t comma = std::min(Tags.find(',', start), Tags.size());
        const std::string t = TrimCopy(Tags.substr(start, comma - start));
        if (!t.empty()) tags.push_back(t);
        start = comma + 1;
    }
    j["tags"] = std::move(tags);

    // Atomic write: a crash mid-save must not truncate the .decal (audit CPP-206).
    return AtomicFile::WriteBytes(Path, j.dump(2) + "\n");
}
