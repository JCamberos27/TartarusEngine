#include "DecalPalette.h"
#include "AtomicFile.h"
#include "Log.h"

#include <json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace {

constexpr const char* kSiteNames[8] = {"wall", "base", "corner", "window", "door", "pipe", "ceiling", "floor"};

void Warn(const std::string& path, const char* what, const char* want) {
    Log::Warn("DecalPalette: '" + path + "': " + what + " should be " + want + " - using the default.",
              LogContext::Asset(path));
}

// A number, or `def` (warning when the key is there but not a number).
float Num(const json& o, const char* k, float def, const std::string& path) {
    auto it = o.find(k);
    if (it == o.end() || it->is_null()) return def;
    if (it->is_number()) return it->get<float>();
    Warn(path, k, "a number");
    return def;
}

// A [min, max] pair (or one number for both), ordered and clamped to [lo, hi].
glm::vec2 Range(const json& o, const char* k, const glm::vec2& def, float lo, float hi, const std::string& path) {
    auto it = o.find(k);
    if (it == o.end() || it->is_null()) return def;
    glm::vec2 r = def;
    if (it->is_number()) {
        r = glm::vec2(it->get<float>());
    } else if (it->is_array() && it->size() >= 2 && (*it)[0].is_number() && (*it)[1].is_number()) {
        r = glm::vec2((*it)[0].get<float>(), (*it)[1].get<float>());
    } else {
        Warn(path, k, "a number or an array of 2 numbers");
        return def;
    }
    r = glm::clamp(r, glm::vec2(lo), glm::vec2(hi));
    return glm::vec2(std::min(r.x, r.y), std::max(r.x, r.y));
}

} // namespace

const char* DecalSites::Name(uint32_t site) {
    for (int i = 0; i < 8; ++i)
        if (site == (1u << i)) return kSiteNames[i];
    return "";
}

uint32_t DecalSites::FromName(const std::string& name) {
    for (int i = 0; i < 8; ++i)
        if (name == kSiteNames[i]) return 1u << i;
    return 0;
}

float DecalPalette::TotalWeight(uint32_t site) const {
    float sum = 0.0f;
    for (const DecalPaletteEntry& e : Entries)
        if (site == 0 || e.Sites == 0 || (e.Sites & site)) sum += e.Weight;
    return sum;
}

int DecalPalette::Pick(float roll, uint32_t site) const {
    const float total = TotalWeight(site);
    if (total <= 0.0f) return -1;
    float at = std::clamp(roll, 0.0f, 0.999999f) * total;
    int last = -1;
    for (int i = 0; i < (int)Entries.size(); ++i) {
        const DecalPaletteEntry& e = Entries[i];
        if (e.Weight <= 0.0f || (site != 0 && e.Sites != 0 && !(e.Sites & site))) continue;
        last = i;
        if (at < e.Weight) return i;
        at -= e.Weight;
    }
    return last; // float slop at the very top of the range
}

std::shared_ptr<DecalPalette> DecalPalette::Load(const std::string& path) {
    auto p = std::make_shared<DecalPalette>();
    p->Path = path;
    p->Name = std::filesystem::path(path).stem().string();

    std::ifstream f(path);
    if (!f.is_open()) {
        p->Missing = true;
        return p;
    }
    json j;
    try { j = json::parse(f); } catch (const std::exception& e) {
        Log::Error("DecalPalette: JSON parse error in '" + path + "': " + e.what(), LogContext::Asset(path));
        p->Missing = true;
        return p;
    }
    if (!j.is_object()) {
        Log::Error("DecalPalette: '" + path + "' is not a JSON object.", LogContext::Asset(path));
        p->Missing = true;
        return p;
    }
    if (Num(j, "paletteVersion", 1.0f, path) > 1.0f)
        Log::Warn("DecalPalette: '" + path + "' is from a newer engine (paletteVersion > 1); unknown keys are ignored.",
                  LogContext::Asset(path));

    if (auto it = j.find("name"); it != j.end() && it->is_string() && !it->get<std::string>().empty())
        p->Name = it->get<std::string>();
    if (auto it = j.find("description"); it != j.end() && it->is_string()) p->Description = it->get<std::string>();

    auto entries = j.find("entries");
    if (entries == j.end() || !entries->is_array()) {
        if (entries != j.end() && !entries->is_null()) Warn(path, "entries", "an array");
        return p;
    }
    for (const json& e : *entries) {
        if (!e.is_object()) continue;
        auto d = e.find("decal");
        if (d == e.end() || !d->is_string() || d->get<std::string>().empty()) continue; // nothing to paint with
        DecalPaletteEntry out;
        out.DecalPath = d->get<std::string>();
        out.Weight = std::max(Num(e, "weight", out.Weight, path), 0.0f);
        out.Scale = Range(e, "scale", out.Scale, 0.05f, 20.0f, path);
        out.Opacity = Range(e, "opacity", out.Opacity, 0.0f, 1.0f, path);
        if (auto t = e.find("tint"); t != e.end() && !t->is_null()) {
            if (t->is_array() && t->size() >= 3 && (*t)[0].is_number() && (*t)[1].is_number() && (*t)[2].is_number())
                out.Tint = glm::clamp(glm::vec3((*t)[0].get<float>(), (*t)[1].get<float>(), (*t)[2].get<float>()),
                                      glm::vec3(0.0f), glm::vec3(4.0f));
            else
                Warn(path, "tint", "an array of 3 numbers");
        }
        if (auto s = e.find("sites"); s != e.end() && !s->is_null()) {
            if (s->is_array()) {
                for (const json& n : *s) {
                    const uint32_t bit = n.is_string() ? DecalSites::FromName(n.get<std::string>()) : 0u;
                    if (bit) out.Sites |= bit;
                    else Warn(path, "a site name", "one of wall, base, corner, window, door, pipe, ceiling, floor");
                }
            } else {
                Warn(path, "sites", "an array of names");
            }
        }
        p->Entries.push_back(std::move(out));
    }
    return p;
}

bool DecalPalette::Save() const {
    using ojson = nlohmann::ordered_json;
    ojson j;
    j["paletteVersion"] = 1;
    j["name"] = Name;
    j["description"] = Description;
    ojson entries = ojson::array();
    for (const DecalPaletteEntry& e : Entries) {
        ojson o;
        o["decal"] = e.DecalPath;
        o["weight"] = e.Weight;
        o["scale"] = {e.Scale.x, e.Scale.y};
        o["opacity"] = {e.Opacity.x, e.Opacity.y};
        o["tint"] = {e.Tint.r, e.Tint.g, e.Tint.b};
        ojson sites = ojson::array();
        for (int i = 0; i < 8; ++i)
            if (e.Sites & (1u << i)) sites.push_back(kSiteNames[i]);
        o["sites"] = std::move(sites);
        entries.push_back(std::move(o));
    }
    j["entries"] = std::move(entries);
    return AtomicFile::WriteBytes(Path, j.dump(2) + "\n");
}
