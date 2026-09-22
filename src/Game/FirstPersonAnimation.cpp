#include "FirstPersonAnimation.h"

#include <json.hpp>

#include <cmath>
#include <fstream>
#include <unordered_set>

using json = nlohmann::json;

namespace {

std::string String(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool Bool(const json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

float Number(const json& j, const char* key, float fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<float>() : fallback;
}

// Optional [x, y, z] array. Absent or malformed falls back to `fallback`, so older .fpsanim
// files keep loading; the caller still gets a chance to reject non-finite values.
glm::vec3 Vec3(const json& j, const char* key, const glm::vec3& fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 3) return fallback;
    glm::vec3 out = fallback;
    for (int i = 0; i < 3; ++i) {
        if (!(*it)[i].is_number()) return fallback;
        out[i] = (*it)[i].get<float>();
    }
    return out;
}

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

} // namespace

const FirstPersonAnimationClip* FirstPersonAnimationSet::Find(const std::string& state) const {
    for (const auto& clip : Clips)
        if (clip.Name == state) return &clip;
    return nullptr;
}

bool FirstPersonAnimationSet::FromJsonString(const std::string& text, FirstPersonAnimationSet& out,
                                              std::string* error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        return Fail(error, std::string("invalid JSON: ") + e.what());
    }
    if (!root.is_object()) return Fail(error, "the root must be a JSON object");

    FirstPersonAnimationSet parsed;
    parsed.ArmsModel = String(root, "armsModel");
    parsed.WeaponModel = String(root, "weaponModel");
    parsed.DefaultState = String(root, "defaultState");
    parsed.ViewRotation = Vec3(root, "viewRotation", glm::vec3(0.0f));
    parsed.WeaponSocket = String(root, "weaponSocket");
    parsed.WeaponRoot = String(root, "weaponRoot");
    parsed.WeaponMountRotation = Vec3(root, "weaponMountRotation", glm::vec3(0.0f));
    parsed.WeaponMountOffset = Vec3(root, "weaponMountOffset", glm::vec3(0.0f));
    if (parsed.ArmsModel.empty()) return Fail(error, "missing required string 'armsModel'");
    if (parsed.WeaponModel.empty()) return Fail(error, "missing required string 'weaponModel'");
    if (!std::isfinite(parsed.ViewRotation.x) || !std::isfinite(parsed.ViewRotation.y) ||
        !std::isfinite(parsed.ViewRotation.z))
        return Fail(error, "'viewRotation' must be three finite numbers (Y-X-Z degrees)");
    if (!std::isfinite(parsed.WeaponMountRotation.x) || !std::isfinite(parsed.WeaponMountRotation.y) ||
        !std::isfinite(parsed.WeaponMountRotation.z))
        return Fail(error, "'weaponMountRotation' must be three finite numbers (Y-X-Z degrees)");
    if (!std::isfinite(parsed.WeaponMountOffset.x) || !std::isfinite(parsed.WeaponMountOffset.y) ||
        !std::isfinite(parsed.WeaponMountOffset.z))
        return Fail(error, "'weaponMountOffset' must be three finite numbers (metres, socket frame)");
    if (parsed.WeaponSocket.empty() != parsed.WeaponRoot.empty())
        return Fail(error, "'weaponSocket' and 'weaponRoot' must be given together (or both omitted)");

    const auto clipsIt = root.find("clips");
    if (clipsIt == root.end() || !clipsIt->is_array() || clipsIt->empty())
        return Fail(error, "'clips' must be a non-empty array");

    std::unordered_set<std::string> names;
    for (const json& item : *clipsIt) {
        if (!item.is_object()) return Fail(error, "every item in 'clips' must be an object");
        FirstPersonAnimationClip clip;
        clip.Name = String(item, "name");
        clip.ArmsClip = String(item, "arms");
        clip.ArmsBindPose = Bool(item, "armsBindPose", false);
        clip.WeaponClip = String(item, "weapon");
        clip.Loop = Bool(item, "loop", false);
        clip.Fade = Number(item, "fade", 0.08f);
        if (clip.Name.empty()) return Fail(error, "every clip needs a non-empty 'name'");
        if (clip.ArmsClip.empty() && !clip.ArmsBindPose)
            return Fail(error, "clip '" + clip.Name + "' needs 'arms' or armsBindPose=true");
        if (!std::isfinite(clip.Fade) || clip.Fade < 0.0f || clip.Fade > 5.0f)
            return Fail(error, "clip '" + clip.Name + "' has an invalid 'fade' (expected 0..5)");
        if (!names.insert(clip.Name).second)
            return Fail(error, "duplicate clip name '" + clip.Name + "'");
        parsed.Clips.push_back(std::move(clip));
    }

    if (parsed.DefaultState.empty()) parsed.DefaultState = parsed.Clips.front().Name;
    if (!parsed.Find(parsed.DefaultState))
        return Fail(error, "defaultState '" + parsed.DefaultState + "' does not name a clip");

    out = std::move(parsed);
    return true;
}

bool FirstPersonAnimationSet::LoadFile(const std::string& path, FirstPersonAnimationSet& out,
                                        std::string* error) {
    std::ifstream in(path);
    if (!in.is_open()) return Fail(error, "could not open '" + path + "'");
    return FromJsonString(std::string(std::istreambuf_iterator<char>(in), {}), out, error);
}

FirstPersonActionTier FirstPersonTierOf(const std::string& state) {
    if (state == "IdleToSprint" || state == "SprintToIdle") return FirstPersonActionTier::Transition;
    if (state == "Fire" || state == "Inspect" || state == "MagCheck") return FirstPersonActionTier::Action;
    if (state == "TacReload" || state == "EmptyReload" || state == "Melee") return FirstPersonActionTier::Committed;
    if (state == "Draw" || state == "Holster") return FirstPersonActionTier::Equip;
    return FirstPersonActionTier::None;
}

bool FirstPersonCanInterrupt(const std::string& state, FirstPersonActionTier requestedTier,
                             const std::string& current, FirstPersonActionTier currentTier) {
    if (current.empty() || currentTier == FirstPersonActionTier::None) return true;
    if (state == current) return true; // restart in place (Fire spam, re-tapping Melee, ...)
    return requestedTier > currentTier;
}

std::string FirstPersonRestingState(float planarSpeed, bool sprinting, bool aiming) {
    const bool moving = planarSpeed > 0.05f;
    if (sprinting && moving) return "Sprint";
    if (moving) return "Walk";
    if (aiming) return "Aim";
    return "Idle";
}

std::string FirstPersonTransitionVia(const std::string& from, const std::string& to) {
    if (from == "Idle" && to == "Sprint") return "IdleToSprint";
    if (from == "Sprint" && to != "Sprint") return "SprintToIdle";
    return "";
}
