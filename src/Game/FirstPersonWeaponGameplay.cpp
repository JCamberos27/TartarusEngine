#include "FirstPersonWeaponGameplay.h"
#include "WeaponPrefab.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
using json = nlohmann::json;
float Num(const json& j, const char* name, float fallback) {
    const auto it = j.find(name);
    return it != j.end() && it->is_number() ? it->get<float>() : fallback;
}
bool Bool(const json& j, const char* name, bool fallback) {
    const auto it = j.find(name);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
glm::vec3 Vec(const json& j, const char* name) {
    const auto it = j.find(name);
    if (it == j.end() || !it->is_array() || it->size() != 3) return glm::vec3(0.0f);
    for (const auto& c : *it) if (!c.is_number()) return glm::vec3(0.0f);
    return {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
}
float Bound(float v, float fallback, float low, float high) { return std::clamp(std::isfinite(v) ? v : fallback, low, high); }
bool Fail(std::string& error, const char* why) { error = why; return false; }
}

bool NormalizeWeaponGameplay(FirstPersonWeaponGameplay& w, std::string& error) {
    if (!(w.RoundsPerMinute > 0.0f) || !std::isfinite(w.RoundsPerMinute)) return Fail(error, "'gameplay.rpm' must be a positive number");
    if (!(w.ReloadHoldSeconds > 0.0f)) return Fail(error, "'gameplay.reloadHoldSeconds' must be positive");
    if (w.BurstRounds < 0 || w.BurstRounds > 32) return Fail(error, "gameplay.burstRounds must be an integer from 0 to 32");
    w.Magazine = std::max(1, w.Magazine);
    w.Pellets = std::clamp(w.Pellets, 1, 64);
    w.BulletHoleRadius = Bound(w.BulletHoleRadius, 0.004f, 0.0f, 0.1f);
    w.CycleDelay = Bound(w.CycleDelay, 0.1f, 0.0f, 2.0f);
    w.SpreadHip = Bound(w.SpreadHip, 0.0f, 0.0f, 45.0f);
    w.SpreadAds = Bound(w.SpreadAds, 0.0f, 0.0f, 45.0f);
    w.Damage = Bound(w.Damage, 34.0f, 0.0f, 10000.0f);
    w.HeadMultiplier = Bound(w.HeadMultiplier, 3.0f, 0.0f, 100.0f);
    w.LimbMultiplier = Bound(w.LimbMultiplier, 0.75f, 0.0f, 100.0f);
    w.FalloffStart = Bound(w.FalloffStart, 25.0f, 0.0f, 10000.0f);
    w.FalloffEnd = std::max(w.FalloffStart, Bound(w.FalloffEnd, 60.0f, 0.0f, 10000.0f));
    w.FalloffMin = Bound(w.FalloffMin, 0.6f, 0.0f, 1.0f);
    if (w.RegripMax < w.RegripMin) std::swap(w.RegripMin, w.RegripMax);
    error.clear();
    return true;
}

namespace Gameplay {
bool ImportLegacyWeaponGameplay(const json& g, FirstPersonWeaponGameplay& w, std::string& error) {
    w = FirstPersonWeaponGameplay{};
    if (!g.is_object()) return NormalizeWeaponGameplay(w, error);
    w.Magazine = std::max(1, (int)Num(g, "magazine", (float)w.Magazine));
    w.RoundsPerMinute = Num(g, "rpm", w.RoundsPerMinute);
    w.AllowFullAuto = Bool(g, "allowFullAuto", w.AllowFullAuto);
    const float burst = Num(g, "burstRounds", 0.0f);
    if (!std::isfinite(burst) || burst < 0.0f || burst > 32.0f || burst != std::floor(burst))
        return Fail(error, "gameplay.burstRounds must be an integer from 0 to 32");
    w.BurstRounds = (int)burst;
    w.ReloadHoldSeconds = Num(g, "reloadHoldSeconds", w.ReloadHoldSeconds);
    w.RegripMin = Num(g, "regripMin", w.RegripMin);
    w.RegripMax = Num(g, "regripMax", w.RegripMax);
    w.ImpactImpulse = std::max(0.0f, Num(g, "impactImpulse", w.ImpactImpulse));
    w.ImpactMaxSpeed = std::max(0.0f, Num(g, "impactMaxSpeed", w.ImpactMaxSpeed));
    w.ZeroDistance = std::max(0.0f, Num(g, "zeroDistance", w.ZeroDistance));
    w.BulletHoleRadius = Num(g, "bulletHoleRadius", w.BulletHoleRadius);
    w.Pellets = std::clamp((int)Num(g, "pellets", (float)w.Pellets), 1, 64);
    if (const auto sight = g.find("sightLine"); sight != g.end() && sight->is_object()) {
        const glm::vec3 origin = Vec(*sight, "origin"), direction = Vec(*sight, "direction");
        const float length = glm::length(direction);
        if (std::isfinite(glm::dot(origin, origin)) && std::isfinite(length) && length > 1e-6f) {
            w.HasSightLine = true;
            w.SightOrigin = origin;
            w.SightDirection = direction / length;
        }
    }
    if (const auto damage = g.find("damage"); damage != g.end() && damage->is_object()) {
        w.Damage = Num(*damage, "base", w.Damage);
        w.HeadMultiplier = Num(*damage, "head", w.HeadMultiplier);
        w.LimbMultiplier = Num(*damage, "limb", w.LimbMultiplier);
        w.FalloffStart = Num(*damage, "falloffStart", w.FalloffStart);
        w.FalloffEnd = Num(*damage, "falloffEnd", w.FalloffEnd);
        w.FalloffMin = Num(*damage, "falloffMin", w.FalloffMin);
    }
    if (const auto spread = g.find("spread"); spread != g.end() && spread->is_object()) {
        w.SpreadHip = Num(*spread, "hip", 0.0f);
        w.SpreadAds = Num(*spread, "ads", 0.0f);
    }
    if (const auto reload = g.find("reload"); reload != g.end() && reload->is_string() && !reload->get<std::string>().empty()) {
        const auto mode = reload->get<std::string>();
        if (mode == "magazine") w.Reload = FirstPersonWeaponGameplay::ReloadMode::Magazine;
        else if (mode == "perRound") w.Reload = FirstPersonWeaponGameplay::ReloadMode::PerRound;
        else return Fail(error, "'gameplay.reload' must be \"magazine\" or \"perRound\"");
    }
    if (const auto cycle = g.find("cycle"); cycle != g.end() && cycle->is_object()) {
        w.CycleAfterShot = Bool(*cycle, "enabled", false);
        w.CycleDelay = Num(*cycle, "delay", 0.1f);
    }
    return NormalizeWeaponGameplay(w, error);
}
}
