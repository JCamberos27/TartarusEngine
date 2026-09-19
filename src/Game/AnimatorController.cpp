#include "AnimatorController.h"
#include "AnimationSystem.h"
#include "AssetLibrary.h"
#include "AtomicFile.h"
#include "Components.h"
#include "Log.h"
#include "Model.h"
#include "ProjectPaths.h"
#include "World.h"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const char* kTypeNames[] = {"float", "int", "bool", "trigger"};
const char* kOpNames[] = {"greater", "less", "equals", "notEqual", "if", "ifNot"};

template <size_t N>
int IndexOf(const char* (&names)[N], const std::string& s, int fallback) {
    for (size_t i = 0; i < N; ++i) if (s == names[i]) return (int)i;
    return fallback;
}

float Num(const json& j, const char* key, float fallback) {
    const auto f = j.find(key);
    return (f != j.end() && f->is_number()) ? f->get<float>() : fallback;
}
bool Flag(const json& j, const char* key, bool fallback) {
    const auto f = j.find(key);
    return (f != j.end() && f->is_boolean()) ? f->get<bool>() : fallback;
}
std::string Str(const json& j, const char* key) {
    const auto f = j.find(key);
    return (f != j.end() && f->is_string()) ? f->get<std::string>() : std::string();
}

bool ConditionHolds(const AnimatorController::Condition& c, const std::vector<AnimatorParam>& params) {
    const AnimatorParam* p = nullptr;
    for (const auto& q : params) if (q.Name == c.Param) { p = &q; break; }
    if (!p) return false;
    using Op = AnimatorController::Op;
    switch (c.Mode) {
        case Op::Greater:  return p->Value > c.Threshold;
        case Op::Less:     return p->Value < c.Threshold;
        case Op::Equals:   return std::abs(p->Value - c.Threshold) < 1e-4f;
        case Op::NotEqual: return std::abs(p->Value - c.Threshold) >= 1e-4f;
        case Op::If:       return p->Value > 0.5f;
        case Op::IfNot:    return p->Value <= 0.5f;
    }
    return false;
}

} // namespace

int AnimatorController::FindState(const std::string& name) const {
    for (int i = 0; i < (int)States.size(); ++i) if (States[i].Name == name) return i;
    return -1;
}

int AnimatorController::DefaultStateIndex() const {
    if (States.empty()) return -1;
    const int i = FindState(DefaultState);
    return i >= 0 ? i : 0;
}

int AnimatorController::PickTransition(int state, float normalizedTime, std::vector<AnimatorParam>& params) const {
    if (state < 0 || state >= (int)States.size()) return -1;
    const std::string& current = States[state].Name;
    for (int pass = 0; pass < 2; ++pass) {
        const bool anyPass = pass == 0;
        for (int i = 0; i < (int)Transitions.size(); ++i) {
            const Transition& t = Transitions[i];
            if (anyPass ? t.From != kAnyState : t.From != current) continue;
            const int to = FindState(t.To);
            if (to < 0 || (anyPass && to == state)) continue;
            // Neither conditions nor exit time would fire every frame - Unity refuses those too.
            if (t.Conditions.empty() && !t.HasExitTime) continue;
            if (t.HasExitTime && normalizedTime < t.ExitTime) continue;
            bool ok = true;
            for (const Condition& c : t.Conditions) if (!ConditionHolds(c, params)) { ok = false; break; }
            if (!ok) continue;
            for (const Condition& c : t.Conditions) // a trigger is consumed by the transition it fires
                for (auto& p : params)
                    if (p.Name == c.Param && p.Type == (int)ParamType::Trigger) p.Value = 0.0f;
            return i;
        }
    }
    return -1;
}

bool AnimatorController::FromJsonString(const std::string& text, AnimatorController& out, std::string* error) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    if (!j.is_object()) {
        if (error) *error = "not a JSON object";
        return false;
    }
    AnimatorController c;
    if (const auto ps = j.find("parameters"); ps != j.end() && ps->is_array())
        for (const auto& p : *ps) {
            if (!p.is_object() || Str(p, "name").empty()) continue;
            Parameter q;
            q.Name = Str(p, "name");
            q.Type = (ParamType)IndexOf(kTypeNames, Str(p, "type"), 0);
            q.Default = Num(p, "default", 0.0f);
            c.Parameters.push_back(std::move(q));
        }
    if (const auto ss = j.find("states"); ss != j.end() && ss->is_array())
        for (const auto& s : *ss) {
            if (!s.is_object() || Str(s, "name").empty()) continue;
            State st;
            st.Name = Str(s, "name");
            st.Clip = Str(s, "clip");
            st.Speed = Num(s, "speed", 1.0f);
            st.Loop = Flag(s, "loop", true);
            c.States.push_back(std::move(st));
        }
    if (const auto ts = j.find("transitions"); ts != j.end() && ts->is_array())
        for (const auto& t : *ts) {
            if (!t.is_object()) continue;
            Transition tr;
            tr.From = Str(t, "from");
            tr.To = Str(t, "to");
            tr.HasExitTime = Flag(t, "hasExitTime", false);
            tr.ExitTime = std::max(0.0f, Num(t, "exitTime", 0.9f));
            tr.Duration = std::max(0.0f, Num(t, "duration", 0.25f));
            if (const auto cs = t.find("conditions"); cs != t.end() && cs->is_array())
                for (const auto& cj : *cs) {
                    if (!cj.is_object()) continue;
                    Condition cond;
                    cond.Param = Str(cj, "param");
                    cond.Mode = (Op)IndexOf(kOpNames, Str(cj, "mode"), 0);
                    cond.Threshold = Num(cj, "threshold", 0.0f);
                    tr.Conditions.push_back(std::move(cond));
                }
            c.Transitions.push_back(std::move(tr));
        }
    c.DefaultState = Str(j, "defaultState");
    out = std::move(c);
    return true;
}

std::string AnimatorController::ToJsonString() const {
    json j;
    j["defaultState"] = DefaultState;
    j["parameters"] = json::array();
    for (const auto& p : Parameters)
        j["parameters"].push_back({{"name", p.Name}, {"type", kTypeNames[(int)p.Type]}, {"default", p.Default}});
    j["states"] = json::array();
    for (const auto& s : States)
        j["states"].push_back({{"name", s.Name}, {"clip", s.Clip}, {"speed", s.Speed}, {"loop", s.Loop}});
    j["transitions"] = json::array();
    for (const auto& t : Transitions) {
        json cs = json::array();
        for (const auto& c : t.Conditions)
            cs.push_back({{"param", c.Param}, {"mode", kOpNames[(int)c.Mode]}, {"threshold", c.Threshold}});
        j["transitions"].push_back({{"from", t.From}, {"to", t.To}, {"hasExitTime", t.HasExitTime},
                                    {"exitTime", t.ExitTime}, {"duration", t.Duration}, {"conditions", cs}});
    }
    return j.dump(2);
}

bool AnimatorController::LoadFile(const std::string& path, AnimatorController& out, std::string* error) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in.is_open()) {
        if (error) *error = "can't open the file";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return FromJsonString(ss.str(), out, error);
}

bool AnimatorController::SaveFile(const std::string& path) const {
    return AtomicFile::WriteJson(fs::u8path(path), json::parse(ToJsonString()));
}

std::shared_ptr<const AnimatorController> GetAnimatorController(const std::string& path) {
    if (path.empty()) return nullptr;
    struct Entry {
        std::shared_ptr<const AnimatorController> Ctrl;
        fs::file_time_type Stamp{};
        std::chrono::steady_clock::time_point Checked{};
        bool Failed = false;
    };
    static std::unordered_map<std::string, Entry> cache;
    const std::string abs = fs::u8path(path).is_absolute() ? path : ProjectPaths::Resolve(path);
    Entry& e = cache[abs];
    const auto now = std::chrono::steady_clock::now();
    // A stat per lookup is cheap, but lookups happen per entity per frame: check twice a second.
    if (e.Ctrl && now - e.Checked < std::chrono::milliseconds(500)) return e.Ctrl;
    if (e.Failed && now - e.Checked < std::chrono::seconds(2)) return nullptr;
    e.Checked = now;
    std::error_code ec;
    const auto stamp = fs::last_write_time(fs::u8path(abs), ec);
    if (ec) { e.Ctrl.reset(); e.Failed = true; return nullptr; }
    if (e.Ctrl && stamp == e.Stamp) return e.Ctrl;
    auto c = std::make_shared<AnimatorController>();
    std::string err;
    if (!AnimatorController::LoadFile(abs, *c, &err)) {
        if (!e.Failed) Log::Error("Animator Controller '" + ProjectPaths::Relativize(abs) + "' couldn't be read: " + err);
        e.Failed = true;
        return e.Ctrl; // keep the last good version running
    }
    e.Ctrl = std::move(c);
    e.Stamp = stamp;
    e.Failed = false;
    return e.Ctrl;
}

std::vector<std::string> FindAnimatorControllers() {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path root(ProjectPaths::Root());
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() == 0 && it->is_directory() && it->path().filename() == "Library") {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() && it->path().extension() == ".controller")
            out.push_back(fs::relative(it->path(), root, ec).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void UpdateAnimatorControllers(World& world, AssetLibrary& assets, float dt) {
    auto view = world.Registry.view<AnimatorControllerComponent, RenderableComponent>(entt::exclude<InactiveTag>);
    for (auto e : view) {
        auto& ac = view.get<AnimatorControllerComponent>(e);
        Model* model = view.get<RenderableComponent>(e).ModelRef.get();
        const auto ctrl = GetAnimatorController(ac.Controller);
        if (!model || !ctrl || ctrl->States.empty()) continue;

        auto enter = [&](int stateIndex, float fade) {
            const AnimatorController::State& s = ctrl->States[stateIndex];
            int clip = ResolveAnimationClip(*model, s.Clip, assets);
            if (clip < 0) clip = model->OwnAnimationCount() > 0 ? 0 : -1;
            model->PlayAnimation(clip, fade, s.Loop ? AnimationWrapMode::Loop : AnimationWrapMode::ClampForever,
                                 s.Speed * ac.Speed);
            ac.State = stateIndex;
            ac.StateName = s.Name;
            ac.StateTime = 0.0f;
        };

        if (!ac.Started) {
            ac.Started = true;
            // Declared parameters get their defaults unless game code already set them.
            for (const auto& p : ctrl->Parameters) {
                bool found = false;
                for (auto& q : ac.Params)
                    if (q.Name == p.Name) { q.Type = (int)p.Type; found = true; }
                if (!found) ac.Params.push_back({p.Name, (int)p.Type, p.Default});
            }
            enter(ctrl->DefaultStateIndex(), 0.0f);
        } else if (ac.State < 0 || ac.State >= (int)ctrl->States.size() ||
                   ctrl->States[ac.State].Name != ac.StateName) {
            // The controller was edited while playing and this state moved or went away.
            const int same = ctrl->FindState(ac.StateName);
            if (same >= 0) ac.State = same;
            else enter(ctrl->DefaultStateIndex(), 0.0f);
        }

        const AnimatorController::State& s = ctrl->States[ac.State];
        const float speed = s.Speed * ac.Speed;
        ac.StateTime += dt * std::abs(speed);
        const int clip = model->CurrentAnimation();
        const float len = clip >= 0 ? model->AnimationLength(clip) : 0.0f;
        const float normalized = len > 1e-4f ? ac.StateTime / len : 1.0f;

        const int t = ctrl->PickTransition(ac.State, normalized, ac.Params);
        if (t >= 0) {
            enter(ctrl->FindState(ctrl->Transitions[t].To), ctrl->Transitions[t].Duration);
        } else {
            model->SetAnimationSpeed(speed); // live Speed edits
        }
    }
}
