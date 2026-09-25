#include "AnimatorLint.h"

#include <algorithm>
#include <set>

namespace AnimatorLint {

using AC = AnimatorController;

namespace {

const AC::Parameter* Find(const AC& c, const std::string& name) { return c.FindParameter(name); }

bool IsNumeric(AC::ParamType t) { return t == AC::ParamType::Float || t == AC::ParamType::Int; }

} // namespace

std::vector<Issue> Check(const AC& c) {
    std::vector<Issue> out;
    auto add = [&](Level lv, std::string msg, std::string hint, int layer = -1, int state = -1, int transition = -1) {
        out.push_back({lv, std::move(msg), std::move(hint), layer, state, transition});
    };

    // --- parameters
    {
        std::set<std::string> seen;
        for (const auto& p : c.Parameters) {
            if (p.Name.empty()) add(Level::Error, "A parameter has no name.", "Name it or remove it in the Parameters tab.");
            else if (!seen.insert(p.Name).second)
                add(Level::Error, "Two parameters are named '" + p.Name + "'.", "Only the first is found by name: rename or remove one.");
        }
    }
    if (c.Tracks.empty()) add(Level::Error, "The controller has no tracks.", "Add a track in the Layers tab: states play their clips on tracks.");

    for (int li = 0; li < (int)c.Layers.size(); ++li) {
        const AC::Layer& L = c.Layers[li];
        const std::string where = c.Layers.size() > 1 ? " (layer '" + L.Name + "')" : std::string();
        if (L.States.empty()) {
            add(Level::Warning, "The layer has no states." + where, "Right-click the graph to create one.", li);
            continue;
        }
        // Duplicate state names: transitions find the first.
        {
            std::set<std::string> names;
            for (int si = 0; si < (int)L.States.size(); ++si) {
                const AC::State& s = L.States[si];
                if (s.Name.empty()) add(Level::Error, "A state has no name." + where, "Rename it.", li, si);
                else if (!names.insert(s.Name).second)
                    add(Level::Error, "Two states are named '" + s.Name + "'." + where, "Transitions find only the first: rename one.", li, si);
                if (AC::IsReservedName(s.Name)) add(Level::Error, "'" + s.Name + "' is a reserved name." + where, "Rename the state.", li, si);
            }
        }
        const int def = L.DefaultStateIndex();

        // Reachability: from the default state (and Entry transitions) along the transitions; Any State
        // transitions reach their targets from anywhere.
        std::vector<char> reach(L.States.size(), 0);
        std::vector<int> stack;
        auto mark = [&](int s) { if (s >= 0 && !reach[s]) { reach[s] = 1; stack.push_back(s); } };
        mark(def);
        for (const auto& t : L.Transitions)
            if (t.FromKind != AC::Source::State) mark(L.FindState(t.To));
        while (!stack.empty()) {
            const int s = stack.back();
            stack.pop_back();
            for (const auto& t : L.Transitions)
                if (t.FromKind == AC::Source::State && t.From == L.States[s].Name) mark(L.FindState(t.To));
        }
        for (int si = 0; si < (int)L.States.size(); ++si)
            if (!reach[si])
                add(Level::Warning, "State '" + L.States[si].Name + "' can never be reached." + where,
                    "No transition leads to it from the default state or Any State: add one, or delete the state.", li, si);

        // Dead ends: a state nothing leaves (it plays forever) - fine for a looping idle, a trap for a one-shot.
        for (int si = 0; si < (int)L.States.size(); ++si) {
            const AC::State& s = L.States[si];
            bool leaves = false;
            for (const auto& t : L.Transitions)
                if (t.FromKind == AC::Source::State && t.From == s.Name) leaves = true;
            bool anyTargets = false;
            for (const auto& t : L.Transitions)
                if (t.FromKind == AC::Source::Any && t.To == s.Name) anyTargets = true;
            if (!leaves && !s.Loop)
                add(Level::Warning, "State '" + s.Name + "' doesn't loop and nothing leaves it." + where,
                    "It will freeze on its last frame. Add a transition out (e.g. with Has Exit Time), or tick Loop.", li, si);
            else if (!leaves && si != def && !anyTargets)
                add(Level::Info, "State '" + s.Name + "' has no way out." + where, "Fine for a final state; otherwise add a transition.", li, si);
        }

        // States: speed parameter, motions, events, blend trees.
        for (int si = 0; si < (int)L.States.size(); ++si) {
            const AC::State& s = L.States[si];
            if (!s.SpeedParam.empty()) {
                const auto* p = Find(c, s.SpeedParam);
                if (!p) add(Level::Error, "State '" + s.Name + "' has speed parameter '" + s.SpeedParam + "', which doesn't exist." + where,
                            "Add the parameter or clear the field.", li, si);
                else if (!IsNumeric(p->Type))
                    add(Level::Warning, "State '" + s.Name + "' uses '" + s.SpeedParam + "' (" + std::string(p->Type == AC::ParamType::Bool ? "Bool" : "Trigger") + ") as its speed.",
                        "A speed multiplier should be a Float.", li, si);
            }
            if (s.Speed <= 0.0f) add(Level::Warning, "State '" + s.Name + "' has speed " + std::to_string(s.Speed).substr(0, 5) + "." + where, "It never advances (or plays backwards).", li, si);
            bool anyMotion = false;
            for (int t = 0; t < (int)s.Motions.size() && t < (int)c.Tracks.size(); ++t) {
                const AC::Motion& m = s.Motions[t];
                if (m.Empty()) continue;
                anyMotion = true;
                if (m.IsBlendTree()) {
                    const auto* px = Find(c, m.BlendParam);
                    if (m.BlendParam.empty() || !px)
                        add(Level::Error, "State '" + s.Name + "' blends on '" + m.BlendParam + "', which doesn't exist." + where, "Pick the blend parameter.", li, si);
                    else if (!IsNumeric(px->Type))
                        add(Level::Warning, "State '" + s.Name + "' blends on '" + m.BlendParam + "', which is not a Float.", "Blend parameters should be Float.", li, si);
                    if (m.Is2D()) {
                        const auto* py = Find(c, m.BlendParamY);
                        if (!py) add(Level::Error, "State '" + s.Name + "' blends its Y on '" + m.BlendParamY + "', which doesn't exist." + where, "Pick the Y parameter.", li, si);
                    }
                    std::set<std::pair<float, float>> pos;
                    for (const auto& ch : m.Children) {
                        if (ch.Clip.empty()) add(Level::Warning, "A blend child of '" + s.Name + "' has no clip." + where, "Pick a clip or remove the child.", li, si);
                        if (!pos.insert({ch.Threshold, m.Is2D() ? ch.ThresholdY : 0.0f}).second)
                            add(Level::Warning, "Two blend children of '" + s.Name + "' sit at the same position." + where, "One of them never plays alone: move or remove it.", li, si);
                    }
                    if (m.Children.size() == 1) add(Level::Info, "The blend tree of '" + s.Name + "' has one child." + where, "Use a plain clip instead.", li, si);
                }
            }
            if (!anyMotion) add(Level::Info, "State '" + s.Name + "' has no motion." + where, "It plays the bind pose. Pick a clip.", li, si);
            for (const auto& e : s.Events) {
                if (e.Name.empty()) add(Level::Warning, "An event of '" + s.Name + "' has no name." + where, "Name it or remove it.", li, si);
                if (e.Time < 0.0f || e.Time > 1.0f)
                    add(Level::Warning, "Event '" + e.Name + "' of '" + s.Name + "' is at " + std::to_string(e.Time).substr(0, 4) + ", outside 0..1." + where,
                        "Events fire within one pass of the state: it may never fire.", li, si);
            }
        }

        // Transitions.
        for (int ti = 0; ti < (int)L.Transitions.size(); ++ti) {
            const AC::Transition& t = L.Transitions[ti];
            const std::string label = (t.FromKind == AC::Source::Any ? std::string("Any State") : t.FromKind == AC::Source::Entry ? std::string("Entry") : t.From) + " -> " + t.To;
            const bool fromMissing = t.FromKind == AC::Source::State && L.FindState(t.From) < 0;
            const bool toMissing = t.To != AC::kExitState && L.FindState(t.To) < 0;
            if (fromMissing || toMissing) {
                add(Level::Error, "Transition " + label + " points at a state that doesn't exist." + where, "It is not drawn and never fires: remove it (Layers tab).", li, -1, ti);
                continue;
            }
            const int from = t.FromKind == AC::Source::State ? L.FindState(t.From) : -1;
            for (const auto& cond : t.Conditions) {
                const auto* p = Find(c, cond.Param);
                if (!p) { add(Level::Error, "Transition " + label + " tests '" + cond.Param + "', which doesn't exist." + where, "Pick a parameter.", li, from, ti); continue; }
                const bool boolOp = cond.Mode == AC::Op::If || cond.Mode == AC::Op::IfNot;
                if (boolOp && IsNumeric(p->Type))
                    add(Level::Warning, "Transition " + label + " tests the " + (p->Type == AC::ParamType::Float ? "Float" : "Int") + " '" + cond.Param + "' as a Bool.", "Use Greater / Less, or make it a Bool.", li, from, ti);
                if (!boolOp && !IsNumeric(p->Type))
                    add(Level::Warning, "Transition " + label + " compares '" + cond.Param + "' (" + (p->Type == AC::ParamType::Bool ? "Bool" : "Trigger") + ") to a number.", "Use If / If Not.", li, from, ti);
                if (cond.Mode == AC::Op::IfNot && p->Type == AC::ParamType::Trigger)
                    add(Level::Warning, "Transition " + label + " tests the Trigger '" + cond.Param + "' with If Not.", "A trigger can only be tested with If.", li, from, ti);
            }
            if (t.Conditions.empty() && !t.HasExitTime && t.FromKind != AC::Source::Entry)
                add(Level::Warning, "Transition " + label + " never fires." + where, "It has no condition and no exit time: add one.", li, from, ti);
            if (t.FromKind == AC::Source::State && from >= 0 && !L.States[from].Loop && t.HasExitTime && t.ExitTime > 1.0f)
                add(Level::Warning, "Transition " + label + " has an exit time above 1 from a non-looping state." + where, "The state has ended by then: lower it.", li, from, ti);
            if (t.Duration < 0.0f || t.Offset < 0.0f || t.Offset > 1.0f)
                add(Level::Warning, "Transition " + label + " has an odd duration or offset." + where, "Duration is seconds >= 0, offset is 0..1.", li, from, ti);
            if (t.FromKind == AC::Source::State && t.From == t.To)
                add(Level::Info, "Transition " + label + " loops a state into itself." + where, "Fine for a restart on a trigger; otherwise a mistake.", li, from, ti);
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const Issue& a, const Issue& b) { return (int)a.Severity > (int)b.Severity; });
    return out;
}

} // namespace AnimatorLint
