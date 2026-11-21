#pragma once
#include "UndoDeltaChain.h"
#include <json.hpp>
#include <algorithm>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Undo / redo of a step that only moved, rotated, scaled or renamed entities, without reloading
// the whole scene (X-33 C).
//
// The history still records whole-scene snapshots (UndoDeltaChain.h); those stay the source of
// truth. What this adds is a check, worked out on a background thread when the step is recorded:
// the JSON Patch from the scene the step will be undone FROM (the state right after it) to the
// state it restores, and whether every op in it only replaces the position / rotation / scale /
// name of an entity that is in both. When it does, Undo writes those fields onto the live
// entities (found by their OrderComponent) instead of clearing the registry and rebuilding every
// entity from the snapshot. Anything else (an entity added or removed, a component field, a
// material, scene settings, the asset library, a prefab override) takes the full reload as before.
namespace UndoInPlace {

// The snapshot arrays that hold entities, and the fields a step may change and still be undone
// in place. "position", "rotation", "scale" and "name" are written for every kind (SceneBuild.cpp)
// and read the same way for every kind (SceneSerializer::ApplyEntityTransformAndName).
inline constexpr const char* kEntityArrays[] = {"boxes", "models", "empties", "prefabInstances"};
inline constexpr const char* kFields[] = {"position", "rotation", "scale", "name"};
// More entities than this in one step: the full reload (a select-all move of a big scene).
constexpr size_t kMaxEntities = 4096;

// One entity of a snapshot: its array (index into kEntityArrays) and its index in that array.
struct EntityRef {
    int Array = 0;
    size_t Index = 0;
    bool operator<(const EntityRef& o) const { return Array != o.Array ? Array < o.Array : Index < o.Index; }
    bool operator==(const EntityRef& o) const { return Array == o.Array && Index == o.Index; }
};

namespace Detail {
// A JSON Pointer token that is a plain array index ("0", "17"; not "-", "07" or "1e2").
inline bool IndexToken(const std::string& t, size_t& out) {
    if (t.empty() || t.size() > 9 || (t.size() > 1 && t[0] == '0')) return false;
    size_t v = 0;
    for (char c : t) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<size_t>(c - '0');
    }
    out = v;
    return true;
}
template <size_t N>
inline int IndexOf(const char* const (&names)[N], const std::string& t) {
    for (size_t i = 0; i < N; ++i)
        if (t == names[i]) return static_cast<int>(i);
    return -1;
}
} // namespace Detail

// The entities `patch` (nlohmann::json::diff of two scene snapshots) changes, sorted and without
// repeats, when every op only replaces a position / rotation / scale / name value of an entity
// present in both snapshots. False otherwise (`out` is then unspecified). An empty patch is true
// with no entities. json::diff writes a changed array element by element when the sizes match, so
// a moved entity is "replace /models/12/position/0" and a renamed one "replace /models/12/name";
// an entity added or removed shows up as add / remove ops, which are refused.
inline bool Classify(const nlohmann::json& patch, std::vector<EntityRef>& out) {
    out.clear();
    if (!patch.is_array()) return false;
    for (const auto& op : patch) {
        if (!op.is_object()) return false;
        const auto kind = op.find("op");
        const auto path = op.find("path");
        if (kind == op.end() || path == op.end() || !kind->is_string() || !path->is_string()) return false;
        if (kind->get_ref<const std::string&>() != "replace") return false;
        const std::string& p = path->get_ref<const std::string&>();
        // Our keys need no escaping, so a "~" means a key that isn't one of them.
        if (p.empty() || p[0] != '/' || p.find('~') != std::string::npos) return false;
        std::vector<std::string> tokens;
        for (size_t start = 1;;) {
            const size_t slash = p.find('/', start);
            tokens.push_back(p.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        if (tokens.size() < 3 || tokens.size() > 4) return false;
        EntityRef ref;
        ref.Array = Detail::IndexOf(kEntityArrays, tokens[0]);
        if (ref.Array < 0 || !Detail::IndexToken(tokens[1], ref.Index)) return false;
        const int field = Detail::IndexOf(kFields, tokens[2]);
        if (field < 0) return false;
        if (tokens.size() == 4) {
            // An element of a vector / quaternion; a name is a whole string.
            size_t element = 0;
            if (std::string(kFields[field]) == "name" || !Detail::IndexToken(tokens[3], element)) return false;
        }
        out.push_back(ref);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out.size() <= kMaxEntities;
}

// The result of StepPatch's background job.
struct StepResult {
    uint64_t FromHash = 0;           // UndoDelta::HashText of the scene the step is undone from
    bool InPlace = false;            // Classify accepted the patch
    std::vector<EntityRef> Entities; // what it changes, when InPlace
    // Per entity (same order): its "order" and the four fields as the target snapshot has them,
    // so an in-place undo never has to parse the whole target (only keys the snapshot has).
    std::vector<nlohmann::json> Targets;
};

// The keys of a snapshot entity an in-place step writes, plus "order" to find it by.
inline nlohmann::json TargetFields(const nlohmann::json& entity) {
    nlohmann::json out = nlohmann::json::object();
    for (const char* key : {"order", "position", "rotation", "scale", "name"})
        if (const auto it = entity.find(key); it != entity.end()) out[key] = *it;
    return out;
}

// The whole verdict for going from `fromJson` to `toJson` (what StepPatch's job works out).
inline StepResult Judge(const std::string& fromJson, const std::string& toJson) {
    StepResult r;
    r.FromHash = UndoDelta::HashText(fromJson);
    try {
        const nlohmann::json to = nlohmann::json::parse(toJson);
        const nlohmann::json patch = fromJson == toJson ? nlohmann::json::array()
                                                        : nlohmann::json::diff(nlohmann::json::parse(fromJson), to);
        r.InPlace = to.is_object() && Classify(patch, r.Entities);
        for (const EntityRef& ref : r.Entities) {
            if (!r.InPlace) break;
            const auto array = to.find(kEntityArrays[ref.Array]);
            if (array == to.end() || !array->is_array() || ref.Index >= array->size() || !(*array)[ref.Index].is_object()) {
                r.InPlace = false;
                break;
            }
            r.Targets.push_back(TargetFields((*array)[ref.Index]));
        }
    } catch (const std::exception&) {
        r.InPlace = false;
    }
    if (!r.InPlace) {
        r.Entities.clear();
        r.Targets.clear();
    }
    return r;
}

// Works out Judge on a background thread. Only the verdict is kept (a few hundred bytes for a
// move), never the patch or the snapshots. Copyable like UndoDelta::Patch; dropping it never
// waits for the job.
class StepPatch {
public:
    void Start(std::string fromJson, std::string toJson) {
        auto promise = std::make_shared<std::promise<StepResult>>();
        m_Job = promise->get_future().share();
        std::thread([promise, from = std::move(fromJson), to = std::move(toJson)] {
            promise->set_value(Judge(from, to));
        }).detach();
    }
    // The verdict when the scene being left hashes to `fromHash` and the step can be done in
    // place (waiting for the job if it's still running); null otherwise.
    const StepResult* ResultFrom(uint64_t fromHash) const {
        if (!m_Job.valid()) return nullptr;
        const StepResult& r = m_Job.get();
        return (r.InPlace && r.FromHash == fromHash) ? &r : nullptr;
    }
    void clear() { m_Job = {}; }
    void Wait() const { if (m_Job.valid()) m_Job.wait(); } // tests: time the step without the job

private:
    std::shared_future<StepResult> m_Job;
};

} // namespace UndoInPlace
