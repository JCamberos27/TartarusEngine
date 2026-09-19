#include "PhysicMaterialAsset.h"

#include "AtomicFile.h"
#include "Components.h"
#include "Log.h"
#include "ProjectPaths.h"

#include <json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace fs = std::filesystem;

bool PhysicMaterialAsset::LoadFile(const std::string& absPath, PhysicMaterialAsset& out, std::string* error) {
    std::ifstream in(fs::u8path(absPath), std::ios::binary);
    if (!in) {
        if (error) *error = "can't open the file";
        return false;
    }
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        if (error) *error = "not a JSON object";
        return false;
    }
    PhysicMaterialAsset m;
    auto num = [&j](const char* key, float fallback, float lo, float hi) {
        const auto it = j.find(key);
        return (it != j.end() && it->is_number()) ? std::clamp(it->get<float>(), lo, hi) : fallback;
    };
    auto mode = [&j](const char* key) {
        const auto it = j.find(key);
        const int v = (it != j.end() && it->is_number_integer()) ? it->get<int>() : 0;
        return (v >= 0 && v <= 3) ? v : 0;
    };
    m.DynamicFriction = num("dynamicFriction", m.DynamicFriction, 0.0f, 10.0f);
    m.StaticFriction  = num("staticFriction", m.DynamicFriction, 0.0f, 10.0f);
    m.Bounciness      = num("bounciness", m.Bounciness, 0.0f, 1.0f);
    m.FrictionCombine = mode("frictionCombine");
    m.BounceCombine   = mode("bounceCombine");
    out = m;
    return true;
}

bool PhysicMaterialAsset::SaveFile(const std::string& absPath) const {
    const nlohmann::json j = {
        {"dynamicFriction", DynamicFriction},
        {"staticFriction", StaticFriction},
        {"bounciness", Bounciness},
        {"frictionCombine", FrictionCombine},
        {"bounceCombine", BounceCombine},
    };
    return AtomicFile::WriteJson(fs::u8path(absPath), j);
}

void PhysicMaterialAsset::ApplyTo(ColliderComponent& c) const {
    c.Friction = DynamicFriction;
    c.StaticFriction = StaticFriction;
    c.Bounciness = Bounciness;
    c.FrictionCombine = FrictionCombine;
    c.BounceCombine = BounceCombine;
}

std::vector<std::string> FindPhysicMaterials() {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path root(ProjectPaths::Root());
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() == 0 && it->is_directory() && it->path().filename() == "Library") {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() && it->path().extension() == ".physicmaterial")
            out.push_back(fs::relative(it->path(), root, ec).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

ColliderComponent ResolvePhysicMaterial(const ColliderComponent& c) {
    if (c.Material.empty()) return c;
    PhysicMaterialAsset m;
    std::string err;
    if (!PhysicMaterialAsset::LoadFile(ProjectPaths::Resolve(c.Material), m, &err)) {
        static std::unordered_set<std::string> warned;
        if (warned.insert(c.Material).second)
            Log::Warn("Physic Material '" + c.Material + "': " + err + " - the collider's own values are used.");
        return c;
    }
    ColliderComponent out = c;
    m.ApplyTo(out);
    return out;
}
