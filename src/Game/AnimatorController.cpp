#include "AnimatorController.h"
#include "AnimationSystem.h"
#include "AssetLibrary.h"
#include "AssetDatabase.h" // #132 - clip GUIDs
#include "AtomicFile.h"
#include "Components.h"
#include "IK.h"
#include "Log.h"
#include "Model.h"
#include "ProjectPaths.h"
#include "World.h"

#include <json.hpp>
#include <glm/gtc/quaternion.hpp>

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
const char* kBlendingNames[] = {"override", "additive"};

// A crossfade stack deeper than this only happens under trigger spam; the oldest entries are
// already all but invisible by then.
constexpr size_t kMaxStack = 8;

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
std::vector<std::string> Strings(const json& j, const char* key) {
    std::vector<std::string> out;
    if (const auto f = j.find(key); f != j.end() && f->is_array())
        for (const auto& s : *f) if (s.is_string() && !s.get<std::string>().empty()) out.push_back(s.get<std::string>());
    return out;
}
glm::vec2 Vec2(const json& j, const char* key, glm::vec2 fallback) {
    const auto f = j.find(key);
    if (f == j.end() || !f->is_array() || f->size() != 2 || !(*f)[0].is_number() || !(*f)[1].is_number()) return fallback;
    return {(*f)[0].get<float>(), (*f)[1].get<float>()};
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

bool ConditionsHold(const std::vector<AnimatorController::Condition>& cs, const std::vector<AnimatorParam>& params) {
    for (const auto& c : cs) if (!ConditionHolds(c, params)) return false;
    return true;
}

// A trigger is consumed by the transition it fires.
void ConsumeTriggers(const std::vector<AnimatorController::Condition>& cs, std::vector<AnimatorParam>& params) {
    for (const auto& c : cs)
        for (auto& p : params)
            if (p.Name == c.Param && p.Type == (int)AnimatorController::ParamType::Trigger) p.Value = 0.0f;
}

// Floats go through float->double on the way out, which prints 0.1f as 0.10000000149011612. Round to
// 6 decimals so saved files stay readable and hand-editable.
void RoundFloats(json& j) {
    if (j.is_number_float()) { j = std::round(j.get<double>() * 1e6) / 1e6; return; }
    if (j.is_object() || j.is_array()) for (auto& v : j) RoundFloats(v);
}

float ParamValue(const std::vector<AnimatorParam>& params, const std::string& name, float fallback) {
    for (const auto& p : params) if (p.Name == name) return p.Value;
    return fallback;
}

AnimatorController::Motion MotionFromJson(const json& m) {
    AnimatorController::Motion out;
    if (!m.is_object()) return out;
    out.Clip = AssetDatabase::FollowRef(Str(m, "clip"), Str(m, "clipGuid")); // #132
    out.BlendParam = Str(m, "blendParam");
    if (const auto cs = m.find("children"); cs != m.end() && cs->is_array())
        for (const auto& c : *cs) {
            if (!c.is_object()) continue;
            AnimatorController::BlendChild ch;
            ch.Clip = AssetDatabase::FollowRef(Str(c, "clip"), Str(c, "clipGuid"));
            ch.Threshold = Num(c, "threshold", 0.0f);
            ch.Speed = Num(c, "speed", 1.0f);
            out.Children.push_back(std::move(ch));
        }
    return out;
}

json ClipJson(const std::string& clip) {
    json j = {{"clip", clip}};
    if (const std::string g = AssetDatabase::RefGuid(clip); !g.empty()) j["clipGuid"] = g; // #132
    return j;
}

json MotionToJson(const AnimatorController::Motion& m) {
    if (!m.IsBlendTree()) return ClipJson(m.Clip);
    json cs = json::array();
    for (const auto& c : m.Children) {
        json cj = ClipJson(c.Clip);
        cj["threshold"] = c.Threshold;
        cj["speed"] = c.Speed;
        cs.push_back(std::move(cj));
    }
    return {{"blendParam", m.BlendParam}, {"children", cs}};
}

std::vector<AnimatorController::Condition> ConditionsFromJson(const json& t) {
    std::vector<AnimatorController::Condition> out;
    if (const auto cs = t.find("conditions"); cs != t.end() && cs->is_array())
        for (const auto& cj : *cs) {
            if (!cj.is_object()) continue;
            AnimatorController::Condition cond;
            cond.Param = Str(cj, "param");
            cond.Mode = (AnimatorController::Op)IndexOf(kOpNames, Str(cj, "mode"), 0);
            cond.Threshold = Num(cj, "threshold", 0.0f);
            out.push_back(std::move(cond));
        }
    return out;
}

} // namespace

// --- data ------------------------------------------------------------------------------------

const AnimatorController::Motion& AnimatorController::State::MotionFor(int track) const {
    static const Motion kEmpty;
    return track >= 0 && track < (int)Motions.size() ? Motions[track] : kEmpty;
}

bool AnimatorController::State::HasTag(const std::string& tag) const {
    return std::find(Tags.begin(), Tags.end(), tag) != Tags.end();
}

int AnimatorController::Layer::FindState(const std::string& name) const {
    for (int i = 0; i < (int)States.size(); ++i) if (States[i].Name == name) return i;
    return -1;
}

int AnimatorController::Layer::DefaultStateIndex() const {
    if (States.empty()) return -1;
    const int i = FindState(DefaultState);
    return i >= 0 ? i : 0;
}

bool AnimatorController::IsReservedName(const std::string& name) {
    return name == kAnyState || name == kEntryState || name == kExitState || name == "Any State";
}

int AnimatorController::TrackIndex(const std::string& name) const {
    for (int i = 0; i < (int)Tracks.size(); ++i) if (Tracks[i] == name) return i;
    return 0;
}

const AnimatorController::Parameter* AnimatorController::FindParameter(const std::string& name) const {
    for (const auto& p : Parameters) if (p.Name == name) return &p;
    return nullptr;
}

int AnimatorController::PickTransition(int layer, int state, float normalizedTime,
                                       std::vector<AnimatorParam>& params) const {
    if (layer < 0 || layer >= (int)Layers.size()) return -1;
    const Layer& L = Layers[layer];
    if (state < 0 || state >= (int)L.States.size()) return -1;
    const std::string& current = L.States[state].Name;
    for (int pass = 0; pass < 2; ++pass) {
        const bool anyPass = pass == 0;
        for (int i = 0; i < (int)L.Transitions.size(); ++i) {
            const Transition& t = L.Transitions[i];
            if (anyPass ? t.FromKind != Source::Any : (t.FromKind != Source::State || t.From != current)) continue;
            const bool toExit = t.To == kExitState;
            const int to = toExit ? -1 : L.FindState(t.To);
            if (!toExit && to < 0) continue;
            if (anyPass && to == state && !t.CanTransitionToSelf) continue;
            if (anyPass && t.RespectPriority && to >= 0 && to != state &&
                L.States[to].Priority <= L.States[state].Priority)
                continue;
            // Neither conditions nor exit time would fire every frame - Unity refuses those too.
            if (t.Conditions.empty() && !t.HasExitTime) continue;
            if (t.HasExitTime && normalizedTime < t.ExitTime) continue;
            if (!ConditionsHold(t.Conditions, params)) continue;
            ConsumeTriggers(t.Conditions, params);
            return i;
        }
    }
    return -1;
}

int AnimatorController::PickEntry(int layer, std::vector<AnimatorParam>& params) const {
    if (layer < 0 || layer >= (int)Layers.size()) return -1;
    const Layer& L = Layers[layer];
    for (const Transition& t : L.Transitions) {
        if (t.FromKind != Source::Entry) continue;
        const int to = L.FindState(t.To);
        if (to < 0 || !ConditionsHold(t.Conditions, params)) continue;
        ConsumeTriggers(t.Conditions, params);
        return to;
    }
    return L.DefaultStateIndex();
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
    if (auto tracks = Strings(j, "tracks"); !tracks.empty()) c.Tracks = std::move(tracks);

    auto readState = [&](const json& s, State& st) {
        st.Name = Str(s, "name");
        st.Speed = Num(s, "speed", 1.0f);
        st.SpeedParam = Str(s, "speedParam");
        st.Loop = Flag(s, "loop", true);
        st.Priority = (int)Num(s, "priority", 0.0f);
        st.Tags = Strings(s, "tags");
        st.Position = Vec2(s, "position", glm::vec2(0.0f));
        st.Motions.assign(c.Tracks.size(), Motion{});
        if (const auto ms = s.find("motions"); ms != s.end() && ms->is_object()) {
            for (int t = 0; t < (int)c.Tracks.size(); ++t)
                if (const auto m = ms->find(c.Tracks[t]); m != ms->end()) st.Motions[t] = MotionFromJson(*m);
        } else {
            // v1, or the one-track shorthand: the state itself carries the first track's motion.
            st.Motions[0] = MotionFromJson(s);
        }
        if (const auto es = s.find("events"); es != s.end() && es->is_array())
            for (const auto& e : *es) {
                if (!e.is_object() || Str(e, "name").empty()) continue;
                st.Events.push_back({Str(e, "name"), std::clamp(Num(e, "time", 1.0f), 0.0f, 1.0f)});
            }
    };
    auto readTransition = [&](const json& t, Transition& tr) {
        tr.From = Str(t, "from");
        if (Flag(t, "fromAny", false) || tr.From == kAnyState) tr.FromKind = Source::Any;
        else if (Flag(t, "fromEntry", false) || tr.From == kEntryState) tr.FromKind = Source::Entry;
        if (tr.FromKind != Source::State) tr.From.clear();
        tr.To = Str(t, "to");
        tr.HasExitTime = Flag(t, "hasExitTime", false);
        tr.ExitTime = std::max(0.0f, Num(t, "exitTime", 0.9f));
        tr.Duration = std::max(0.0f, Num(t, "duration", 0.25f));
        tr.Offset = std::clamp(Num(t, "offset", 0.0f), 0.0f, 1.0f);
        tr.Interruptible = Flag(t, "interruptible", true);
        tr.RespectPriority = Flag(t, "respectPriority", false);
        tr.CanTransitionToSelf = Flag(t, "canTransitionToSelf", false);
        tr.Conditions = ConditionsFromJson(t);
    };
    auto readLayerBody = [&](const json& lj, Layer& L) {
        if (const auto ss = lj.find("states"); ss != lj.end() && ss->is_array())
            for (const auto& s : *ss) {
                if (!s.is_object() || Str(s, "name").empty()) continue;
                State st;
                readState(s, st);
                L.States.push_back(std::move(st));
            }
        if (const auto ts = lj.find("transitions"); ts != lj.end() && ts->is_array())
            for (const auto& t : *ts) {
                if (!t.is_object()) continue;
                Transition tr;
                readTransition(t, tr);
                L.Transitions.push_back(std::move(tr));
            }
        L.DefaultState = Str(lj, "defaultState");
    };

    c.Layers.clear();
    if (const auto ls = j.find("layers"); ls != j.end() && ls->is_array()) {
        for (const auto& lj : *ls) {
            if (!lj.is_object()) continue;
            Layer L;
            if (const std::string n = Str(lj, "name"); !n.empty()) L.Name = n;
            L.Weight = std::clamp(Num(lj, "weight", 1.0f), 0.0f, 1.0f);
            L.Mode = (Blending)IndexOf(kBlendingNames, Str(lj, "blending"), 0);
            L.MaskInclude = Strings(lj, "maskInclude");
            L.MaskExclude = Strings(lj, "maskExclude");
            L.EntryPosition = Vec2(lj, "entryPosition", L.EntryPosition);
            L.AnyPosition = Vec2(lj, "anyPosition", L.AnyPosition);
            L.ExitPosition = Vec2(lj, "exitPosition", L.ExitPosition);
            readLayerBody(lj, L);
            c.Layers.push_back(std::move(L));
        }
    } else {
        Layer L; // v1: the whole file is the base layer
        readLayerBody(j, L);
        // v1 files have no graph positions: lay the states out on a grid so the window opens tidy.
        for (int i = 0; i < (int)L.States.size(); ++i)
            L.States[i].Position = {(float)(i % 4) * 220.0f, (float)(i / 4) * 110.0f};
        c.Layers.push_back(std::move(L));
    }
    if (c.Layers.empty()) c.Layers.emplace_back();
    out = std::move(c);
    return true;
}

std::string AnimatorController::ToJsonString() const {
    json j;
    j["version"] = 2;
    j["parameters"] = json::array();
    for (const auto& p : Parameters)
        j["parameters"].push_back({{"name", p.Name}, {"type", kTypeNames[(int)p.Type]}, {"default", p.Default}});
    j["tracks"] = Tracks;
    j["layers"] = json::array();
    for (const Layer& L : Layers) {
        json lj = {{"name", L.Name}, {"weight", L.Weight}, {"blending", kBlendingNames[(int)L.Mode]},
                   {"defaultState", L.DefaultState},
                   {"entryPosition", {L.EntryPosition.x, L.EntryPosition.y}},
                   {"anyPosition", {L.AnyPosition.x, L.AnyPosition.y}},
                   {"exitPosition", {L.ExitPosition.x, L.ExitPosition.y}}};
        if (!L.MaskInclude.empty()) lj["maskInclude"] = L.MaskInclude;
        if (!L.MaskExclude.empty()) lj["maskExclude"] = L.MaskExclude;
        lj["states"] = json::array();
        for (const State& s : L.States) {
            json st = {{"name", s.Name}, {"speed", s.Speed}, {"loop", s.Loop},
                       {"position", {s.Position.x, s.Position.y}}};
            if (!s.SpeedParam.empty()) st["speedParam"] = s.SpeedParam;
            if (s.Priority != 0) st["priority"] = s.Priority;
            if (!s.Tags.empty()) st["tags"] = s.Tags;
            json ms = json::object();
            for (int t = 0; t < (int)Tracks.size(); ++t)
                if (!s.MotionFor(t).Empty()) ms[Tracks[t]] = MotionToJson(s.MotionFor(t));
            st["motions"] = std::move(ms);
            if (!s.Events.empty()) {
                json es = json::array();
                for (const auto& e : s.Events) es.push_back({{"name", e.Name}, {"time", e.Time}});
                st["events"] = std::move(es);
            }
            lj["states"].push_back(std::move(st));
        }
        lj["transitions"] = json::array();
        for (const Transition& t : L.Transitions) {
            json cs = json::array();
            for (const auto& c : t.Conditions)
                cs.push_back({{"param", c.Param}, {"mode", kOpNames[(int)c.Mode]}, {"threshold", c.Threshold}});
            json tj;
            if (t.FromKind == Source::Any) tj["fromAny"] = true;
            else if (t.FromKind == Source::Entry) tj["fromEntry"] = true;
            else tj["from"] = t.From;
            tj["to"] = t.To;
            tj["hasExitTime"] = t.HasExitTime;
            tj["exitTime"] = t.ExitTime;
            tj["duration"] = t.Duration;
            if (t.Offset != 0.0f) tj["offset"] = t.Offset;
            if (!t.Interruptible) tj["interruptible"] = false;
            if (t.RespectPriority) tj["respectPriority"] = true;
            if (t.CanTransitionToSelf) tj["canTransitionToSelf"] = true;
            tj["conditions"] = std::move(cs);
            lj["transitions"].push_back(std::move(tj));
        }
        j["layers"].push_back(std::move(lj));
    }
    RoundFloats(j);
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

std::vector<float> AnimatorBlendWeights(const std::vector<AnimatorController::BlendChild>& children, float value) {
    std::vector<float> w(children.size(), 0.0f);
    if (children.empty()) return w;
    std::vector<int> order(children.size());
    for (int i = 0; i < (int)order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return children[a].Threshold < children[b].Threshold; });
    if (value <= children[order.front()].Threshold) { w[order.front()] = 1.0f; return w; }
    if (value >= children[order.back()].Threshold) { w[order.back()] = 1.0f; return w; }
    for (size_t k = 0; k + 1 < order.size(); ++k) {
        const float a = children[order[k]].Threshold, b = children[order[k + 1]].Threshold;
        if (value >= a && value <= b) {
            const float t = b > a ? (value - a) / (b - a) : 0.0f;
            w[order[k]] = 1.0f - t;
            w[order[k + 1]] += t;
            break;
        }
    }
    return w;
}

std::vector<float> AnimatorMaskWeights(const AnimatorController::Layer& layer,
                                       const std::vector<std::string>& names, const std::vector<int>& parents) {
    const size_t n = names.size();
    std::vector<float> w(n, layer.MaskInclude.empty() ? 1.0f : 0.0f);
    auto listed = [](const std::vector<std::string>& list, const std::string& name) {
        return std::find(list.begin(), list.end(), name) != list.end();
    };
    // Parents come first, so each node inherits its parent's answer unless it is listed itself.
    for (size_t i = 0; i < n; ++i) {
        const int p = i < parents.size() ? parents[i] : -1;
        float v = p >= 0 && p < (int)i ? w[p] : (layer.MaskInclude.empty() ? 1.0f : 0.0f);
        if (listed(layer.MaskInclude, names[i])) v = 1.0f;
        if (listed(layer.MaskExclude, names[i])) v = 0.0f;
        w[i] = v;
    }
    return w;
}

// --- cache / discovery -----------------------------------------------------------------------

namespace {
std::unordered_map<std::string, std::shared_ptr<const AnimatorController>>& MemoryControllers() {
    static std::unordered_map<std::string, std::shared_ptr<const AnimatorController>> m;
    return m;
}
} // namespace

void RegisterAnimatorController(const std::string& key, std::shared_ptr<const AnimatorController> ctrl) {
    MemoryControllers()[key] = std::move(ctrl);
}

std::shared_ptr<const AnimatorController> GetAnimatorController(const std::string& path) {
    if (path.empty()) return nullptr;
    if (const auto m = MemoryControllers().find(path); m != MemoryControllers().end()) return m->second;
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

// --- state machine ---------------------------------------------------------------------------

namespace {

// Pushes the events of `state` whose time lies in (from, to] - per pass for a looping state, so
// an event fires once every loop - onto `out`.
void CrossEvents(const AnimatorController::State& s, float from, float to, std::vector<std::string>& out) {
    if (s.Events.empty() || to <= from) return;
    if (!s.Loop) {
        for (const auto& e : s.Events) if (e.Time > from && e.Time <= to) out.push_back(e.Name);
        return;
    }
    const int first = (int)std::floor(std::max(from, 0.0f));
    const int last = (int)std::floor(to);
    for (int k = first; k <= last; ++k)
        for (const auto& e : s.Events) {
            const float t = e.Time + (float)k;
            if (t > from && t <= to) out.push_back(e.Name);
        }
}

void EnterState(const AnimatorController::Layer& L, AnimatorLayerRuntime& rt, int state, float fade,
                float offset, int transition, bool interruptible, std::vector<std::string>& events) {
    if (state < 0 || state >= (int)L.States.size()) return;
    AnimatorLayerRuntime::Item item;
    item.State = state;
    item.Phase = offset;
    if (fade > 0.0f && !rt.Stack.empty()) {
        item.Fade = 0.0f;
        item.FadeDuration = fade;
    } else {
        rt.Stack.clear();
    }
    rt.Stack.push_back(item);
    if (rt.Stack.size() > kMaxStack) rt.Stack.erase(rt.Stack.begin(), rt.Stack.end() - kMaxStack);
    rt.Transition = rt.Stack.size() > 1 ? transition : -1;
    rt.Interruptible = rt.Stack.size() > 1 ? interruptible : true;
    CrossEvents(L.States[state], offset - 1e-6f, offset, events); // entry events (time 0)
}

void SyncBaseLayerFields(const AnimatorController& ctrl, AnimatorControllerComponent& ac) {
    if (ac.Layers.empty() || ac.Layers[0].Stack.empty() || ctrl.Layers.empty()) return;
    const auto& top = ac.Layers[0].Stack.back();
    const auto& L = ctrl.Layers[0];
    if (top.State < 0 || top.State >= (int)L.States.size()) return;
    ac.State = top.State;
    ac.StateName = L.States[top.State].Name;
    ac.StateTags = L.States[top.State].Tags;
    ac.StateTime = top.Phase;
    ac.InTransition = ac.Layers[0].Stack.size() > 1;
}

} // namespace

void AdvanceAnimator(const AnimatorController& ctrl, AnimatorControllerComponent& ac, float dt,
                     const std::function<float(int, int)>& stateLength) {
    ac.FiredEvents.clear();
    if (!ac.Started) {
        ac.Started = true;
        // Declared parameters get their defaults unless game code already set them.
        for (const auto& p : ctrl.Parameters) {
            bool found = false;
            for (auto& q : ac.Params)
                if (q.Name == p.Name) { q.Type = (int)p.Type; found = true; }
            if (!found) ac.Params.push_back({p.Name, (int)p.Type, p.Default});
        }
        ac.Layers.clear();
    }
    if (ac.Layers.size() != ctrl.Layers.size()) ac.Layers.resize(ctrl.Layers.size());

    for (int li = 0; li < (int)ctrl.Layers.size(); ++li) {
        const auto& L = ctrl.Layers[li];
        auto& rt = ac.Layers[li];
        if (L.States.empty()) { rt = {}; continue; }
        // The controller was edited while playing: drop anything pointing past the state list,
        // and re-find the base layer's state by name when it moved.
        if (li == 0 && !rt.Stack.empty() && !ac.StateName.empty()) {
            auto& top = rt.Stack.back();
            if (top.State >= (int)L.States.size() || L.States[top.State].Name != ac.StateName) {
                const int same = L.FindState(ac.StateName);
                if (same >= 0) { rt.Stack.clear(); rt.Stack.push_back({same, top.Phase, 1.0f, 0.0f}); }
            }
        }
        rt.Stack.erase(std::remove_if(rt.Stack.begin(), rt.Stack.end(),
                                      [&](const AnimatorLayerRuntime::Item& it) {
                                          return it.State < 0 || it.State >= (int)L.States.size();
                                      }),
                       rt.Stack.end());
        if (rt.Transition >= (int)L.Transitions.size()) rt.Transition = -1;
        if (rt.Stack.empty()) {
            EnterState(L, rt, ctrl.PickEntry(li, ac.Params), 0.0f, 0.0f, -1, true, ac.FiredEvents);
            if (rt.Stack.empty()) continue;
        }

        // Advance every entry's clock and fade; only the current state fires events.
        for (size_t k = 0; k < rt.Stack.size(); ++k) {
            auto& it = rt.Stack[k];
            const auto& s = L.States[it.State];
            float speed = s.Speed * ac.Speed;
            if (!s.SpeedParam.empty()) speed *= ParamValue(ac.Params, s.SpeedParam, 1.0f);
            float len = stateLength ? stateLength(li, it.State) : 1.0f;
            if (!(len > 1e-4f)) len = 1.0f;
            const float before = it.Phase;
            it.Phase += dt * std::abs(speed) / len;
            if (k + 1 == rt.Stack.size()) CrossEvents(s, before, it.Phase, ac.FiredEvents);
            if (it.Fade < 1.0f)
                it.Fade = it.FadeDuration > 0.0f ? std::min(1.0f, it.Fade + dt / it.FadeDuration) : 1.0f;
        }
        // An entry at full weight hides everything beneath it.
        for (int k = (int)rt.Stack.size() - 1; k > 0; --k)
            if (rt.Stack[k].Fade >= 1.0f) { rt.Stack.erase(rt.Stack.begin(), rt.Stack.begin() + k); break; }
        if (rt.Stack.size() == 1) { rt.Transition = -1; rt.Interruptible = true; }

        if (!rt.Interruptible) continue;
        const auto& top = rt.Stack.back();
        const int t = ctrl.PickTransition(li, top.State, top.Phase, ac.Params);
        if (t < 0) continue;
        const auto& tr = L.Transitions[t];
        const int to = tr.To == AnimatorController::kExitState ? ctrl.PickEntry(li, ac.Params) : L.FindState(tr.To);
        EnterState(L, rt, to, tr.Duration, tr.Offset, t, tr.Interruptible, ac.FiredEvents);
    }
    SyncBaseLayerFields(ctrl, ac);
}

// --- sampling --------------------------------------------------------------------------------

namespace {

using Pose = std::vector<LocalTRS>;

void BlendInto(Pose& a, const Pose& b, float w) {
    if (w <= 0.0f) return;
    if (w >= 1.0f) { a = b; return; }
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) a[i] = LocalTRS::Blend(a[i], b[i], w);
}

struct Sampler {
    Model& M;
    AssetLibrary& Assets;
    int Track;
    Pose Tmp, Tmp2;

    int Clip(const std::string& ref) { return ref.empty() ? -1 : ResolveAnimationClip(M, ref, Assets); }

    float MotionLength(const AnimatorController::Motion& m, const std::vector<AnimatorParam>& params) {
        if (m.Empty()) return 0.0f;
        if (!m.IsBlendTree()) {
            const int c = Clip(m.Clip);
            return c >= 0 ? M.AnimationLength(c) : 0.0f;
        }
        const auto w = AnimatorBlendWeights(m.Children, ParamValue(params, m.BlendParam, 0.0f));
        float len = 0.0f;
        for (size_t i = 0; i < m.Children.size(); ++i) {
            if (w[i] <= 0.0f) continue;
            const int c = Clip(m.Children[i].Clip);
            const float sp = std::max(std::abs(m.Children[i].Speed), 1e-3f);
            if (c >= 0) len += w[i] * M.AnimationLength(c) / sp;
        }
        return len;
    }

    // `base` is what an empty motion leaves in place. Returns false when the motion is empty.
    bool Sample(const AnimatorController::State& s, float phase, float stateLen,
                const std::vector<AnimatorParam>& params, Pose& out) {
        const auto& m = s.MotionFor(Track);
        if (m.Empty()) return false;
        const AnimationWrapMode wrap = s.Loop ? AnimationWrapMode::Loop : AnimationWrapMode::ClampForever;
        if (!m.IsBlendTree()) {
            const int c = Clip(m.Clip);
            if (c < 0) return false;
            // Real time, not stretched: tracks of different lengths each play at their own rate
            // and a shorter one holds (or loops) - how the paired arms/weapon clips were authored.
            M.SampleLocalPose(c, phase * stateLen, wrap, out);
            return true;
        }
        // Blend tree: children are phase-synced, so a walk and a run keep their feet in step.
        const auto w = AnimatorBlendWeights(m.Children, ParamValue(params, m.BlendParam, 0.0f));
        bool any = false;
        float acc = 0.0f;
        for (size_t i = 0; i < m.Children.size(); ++i) {
            if (w[i] <= 0.0f) continue;
            const int c = Clip(m.Children[i].Clip);
            if (c < 0) continue;
            M.SampleLocalPose(c, phase * M.AnimationLength(c), wrap, any ? Tmp2 : out);
            if (any) BlendInto(out, Tmp2, w[i] / (acc + w[i]));
            acc += w[i];
            any = true;
        }
        return any;
    }
};

// The pose one layer's crossfade stack produces. `base` fills in for empty motions.
void SampleLayer(const AnimatorController::Layer& L, const AnimatorLayerRuntime& rt, Sampler& smp,
                 const std::vector<AnimatorParam>& params, const std::function<float(int, int)>& stateLength,
                 int layerIndex, const Pose& base, bool atPhaseZero, Pose& out) {
    out = base;
    Pose item;
    bool first = true;
    for (const auto& it : rt.Stack) {
        if (it.State < 0 || it.State >= (int)L.States.size()) continue;
        const auto& s = L.States[it.State];
        float len = stateLength(layerIndex, it.State);
        if (!(len > 1e-4f)) len = 1.0f;
        if (!smp.Sample(s, atPhaseZero ? 0.0f : it.Phase, len, params, item)) item = base;
        if (first) { out = item; first = false; }
        else BlendInto(out, item, it.Fade);
    }
}

void ApplyAdditive(Pose& pose, const Pose& layer, const Pose& ref, const std::vector<float>& mask, float weight) {
    for (size_t i = 0; i < pose.size() && i < layer.size() && i < ref.size(); ++i) {
        const float w = weight * (i < mask.size() ? mask[i] : 1.0f);
        if (w <= 0.0f) continue;
        const glm::quat dR = layer[i].R * glm::inverse(ref[i].R);
        pose[i].T += (layer[i].T - ref[i].T) * w;
        pose[i].R = glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), dR, w) * pose[i].R);
        const glm::vec3 dS = layer[i].S / glm::max(ref[i].S, glm::vec3(1e-6f));
        pose[i].S *= glm::mix(glm::vec3(1.0f), dS, w);
    }
}

void PoseModel(const AnimatorController& ctrl, const AnimatorControllerComponent& ac, Model& model,
               AssetLibrary& assets, int track, const std::function<float(int, int)>& stateLength,
               const IKRigComponent* ikRig) {
    if (model.NodeCount() == 0) return;
    Sampler smp{model, assets, track, {}, {}};
    Pose bind, pose, layerPose, refPose;
    model.BindLocalPose(bind);
    pose = bind;
    std::vector<std::string> names;
    std::vector<int> parents;
    for (int li = 0; li < (int)ctrl.Layers.size() && li < (int)ac.Layers.size(); ++li) {
        const auto& L = ctrl.Layers[li];
        const auto& rt = ac.Layers[li];
        if (rt.Stack.empty()) continue;
        if (li == 0) {
            SampleLayer(L, rt, smp, ac.Params, stateLength, li, bind, false, pose);
            continue;
        }
        if (L.Weight <= 0.0f) continue;
        if (names.empty()) {
            names.resize(model.NodeCount());
            parents.resize(model.NodeCount());
            for (int i = 0; i < model.NodeCount(); ++i) { names[i] = model.NodeName(i); parents[i] = model.NodeParent(i); }
        }
        const std::vector<float> mask = AnimatorMaskWeights(L, names, parents);
        if (L.Mode == AnimatorController::Blending::Override) {
            SampleLayer(L, rt, smp, ac.Params, stateLength, li, pose, false, layerPose);
            for (size_t i = 0; i < pose.size(); ++i) {
                const float w = L.Weight * mask[i];
                if (w > 0.0f) pose[i] = LocalTRS::Blend(pose[i], layerPose[i], std::min(w, 1.0f));
            }
        } else {
            // Additive: the layer's motion relative to its own first frame, added on top.
            SampleLayer(L, rt, smp, ac.Params, stateLength, li, bind, false, layerPose);
            SampleLayer(L, rt, smp, ac.Params, stateLength, li, bind, true, refPose);
            ApplyAdditive(pose, layerPose, refPose, mask, L.Weight);
        }
    }
    // Procedural offsets and IK run on the finished blend, so they see what the clips did.
    if (ikRig) IK::ApplyRig(*ikRig, model, pose);
    model.ApplyLocalPose(pose);
}

} // namespace

void UpdateAnimatorControllers(World& world, AssetLibrary& assets, float dt) {
    struct Rig {
        entt::entity E;
        AnimatorControllerComponent* AC;
        Model* M;
    };
    // Group followers under their drivers: a state's length is the longest of its motions over
    // every rig in the group, so no rig's clip is cut short by a shorter partner's.
    std::unordered_map<entt::entity, std::vector<Rig>> groups;
    std::vector<entt::entity> drivers;
    auto view = world.Registry.view<AnimatorControllerComponent, RenderableComponent>();
    for (auto e : view) {
        auto& ac = view.get<AnimatorControllerComponent>(e);
        if (world.Registry.all_of<InactiveTag>(e) && !ac.UpdateWhenInactive) continue;
        Model* m = view.get<RenderableComponent>(e).ModelRef.get();
        const entt::entity driver =
            ac.Driver != entt::null && world.Registry.valid(ac.Driver) &&
                    world.Registry.all_of<AnimatorControllerComponent>(ac.Driver)
                ? ac.Driver
                : e;
        if (driver == e) drivers.push_back(e);
        groups[driver].push_back({e, &ac, m});
    }

    for (entt::entity d : drivers) {
        auto& group = groups[d];
        auto& dac = world.Registry.get<AnimatorControllerComponent>(d);
        const auto ctrl = GetAnimatorController(dac.Controller);
        if (!ctrl) continue;

        std::vector<Sampler> samplers;
        samplers.reserve(group.size());
        for (const Rig& r : group)
            if (r.M) samplers.push_back({*r.M, assets, ctrl->TrackIndex(r.AC->Track), {}, {}});
        const auto stateLength = [&](int li, int si) {
            if (li < 0 || li >= (int)ctrl->Layers.size() || si < 0 || si >= (int)ctrl->Layers[li].States.size())
                return 1.0f;
            const auto& s = ctrl->Layers[li].States[si];
            float len = 0.0f;
            for (Sampler& smp : samplers) len = std::max(len, smp.MotionLength(s.MotionFor(smp.Track), dac.Params));
            return len > 1e-4f ? len : 1.0f;
        };

        AdvanceAnimator(*ctrl, dac, dt, stateLength);
        for (const Rig& r : group) {
            if (r.AC != &dac) {
                // Followers mirror the driver's playback exactly; their own params are unused.
                r.AC->Controller = dac.Controller;
                r.AC->Params = dac.Params;
                r.AC->Layers = dac.Layers;
                r.AC->Started = true;
                r.AC->State = dac.State;
                r.AC->StateName = dac.StateName;
                r.AC->StateTags = dac.StateTags;
                r.AC->StateTime = dac.StateTime;
                r.AC->InTransition = dac.InTransition;
                r.AC->FiredEvents = dac.FiredEvents;
            }
            if (r.M)
                PoseModel(*ctrl, *r.AC, *r.M, assets, ctrl->TrackIndex(r.AC->Track), stateLength,
                          world.Registry.try_get<IKRigComponent>(r.E));
        }
    }
}
