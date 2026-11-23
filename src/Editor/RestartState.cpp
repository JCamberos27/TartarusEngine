#include "RestartState.h"

#include "Components.h"
#include "World.h"
#include "PrefabMemberRef.h"

#include <chrono>
#include <fstream>
#include <unordered_map>

#include <json.hpp>

namespace RestartState {
using json = nlohmann::json;

std::int64_t NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool Save(const std::filesystem::path& file, const State& state) {
    json selection = json::array();
    for (const EntityRef& r : state.Selection) {
        json ref = {{"order", r.Order}, {"name", r.Name}};
        if (r.Local >= 0) ref["local"] = r.Local;
        selection.push_back(std::move(ref));
    }
    const json j = {
        {"version", 1},
        {"scene", state.ScenePath},
        {"restoreRecovery", state.RestoreRecovery},
        {"wasPlaying", state.WasPlaying},
        {"selection", std::move(selection)},
        {"savedAt", state.SavedAtUnix != 0 ? state.SavedAtUnix : NowUnix()},
    };
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << j.dump(2);
    out.flush();
    return static_cast<bool>(out);
}

std::optional<State> Load(const std::filesystem::path& file, std::int64_t nowUnix, std::string* why) {
    auto fail = [&](const std::string& reason) -> std::optional<State> {
        if (why) *why = reason;
        return std::nullopt;
    };
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail("no restart state file");
    const json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.value("version", 0) != 1) return fail("restart state file is malformed");
    State s;
    s.SavedAtUnix = j.value("savedAt", std::int64_t{0});
    if (s.SavedAtUnix <= 0 || nowUnix - s.SavedAtUnix > kMaxAgeSeconds || s.SavedAtUnix - nowUnix > kMaxAgeSeconds)
        return fail("restart state file is stale");
    s.ScenePath = j.value("scene", std::string{});
    s.RestoreRecovery = j.value("restoreRecovery", false);
    s.WasPlaying = j.value("wasPlaying", false);
    if (const auto it = j.find("selection"); it != j.end() && it->is_array()) {
        for (const json& e : *it) {
            if (!e.is_object() || !e.contains("order")) continue;
            s.Selection.push_back({e.value("order", 0), e.value("name", std::string{}), e.value("local", -1)});
        }
    }
    return s;
}

std::optional<std::string> StartupScene(const std::filesystem::path& file, std::int64_t nowUnix) {
    const std::optional<State> s = Load(file, nowUnix);
    if (!s) return std::nullopt;
    if (s->ScenePath.empty()) return std::string{};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(s->ScenePath), ec)) return std::nullopt;
    return s->ScenePath;
}

bool WriteCheckReport(const std::filesystem::path& file, const CheckReport& report) {
    const json j = {{"scene", report.ScenePath},
                    {"sceneLoaded", report.SceneLoaded},
                    {"selection", report.Selection},
                    {"playing", report.Playing}};
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << j.dump(2);
    out.flush();
    return static_cast<bool>(out);
}

std::vector<EntityRef> RefsFor(const World& world, const std::vector<entt::entity>& entities) {
    std::vector<EntityRef> refs;
    for (entt::entity e : entities) {
        if (!world.Registry.valid(e)) continue;
        const auto* name = world.Registry.try_get<NameComponent>(e);
        int rootOrder = -1, local = -1;
        if (PrefabMemberRef(world, e, rootOrder, local)) {
            refs.push_back({rootOrder, name ? name->Name : std::string{}, local});
            continue;
        }
        const auto* order = world.Registry.try_get<OrderComponent>(e);
        if (!order) continue;
        refs.push_back({order->Value, name ? name->Name : std::string{}});
    }
    return refs;
}

std::vector<entt::entity> Resolve(const World& world, const std::vector<EntityRef>& refs) {
    std::vector<entt::entity> out;
    if (refs.empty()) return out;
    std::unordered_map<int, entt::entity> byOrder;
    for (auto [e, order] : world.Registry.view<const OrderComponent>().each()) byOrder.emplace(order.Value, e);
    for (const EntityRef& r : refs) {
        const auto it = byOrder.find(r.Order);
        if (it == byOrder.end()) continue;
        const entt::entity e = r.Local >= 0 ? PrefabMemberAt(world, it->second, r.Local) : it->second;
        if (e == entt::null) continue;
        const auto* name = world.Registry.try_get<NameComponent>(e);
        if ((name ? name->Name : std::string{}) != r.Name) continue;
        out.push_back(e);
    }
    return out;
}

} // namespace RestartState
