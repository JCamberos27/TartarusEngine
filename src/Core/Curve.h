#pragma once

#include <json.hpp>

#include <vector>

// A 1D animation curve: keys with cubic Hermite tangents, the same model as Unity's
// AnimationCurve. Used for procedural animation (recoil, bob, breathing, blend easing) and
// edited in the Inspector with the curve editor (src/Editor/CurveEditor.h).
//
// Outside the key range the curve holds its first/last value. An empty curve evaluates to 0.
struct CurveKey {
    float Time = 0.0f;
    float Value = 0.0f;
    float InTangent = 0.0f;  // slope (value per unit time) arriving at the key
    float OutTangent = 0.0f; // slope leaving it
};

struct Curve {
    std::vector<CurveKey> Keys;

    float Evaluate(float t) const;
    bool Empty() const { return Keys.empty(); }
    float StartTime() const { return Keys.empty() ? 0.0f : Keys.front().Time; }
    float EndTime() const { return Keys.empty() ? 0.0f : Keys.back().Time; }

    // Keeps keys in time order (the editor calls this after a drag).
    void Sort();
    // Catmull-Rom style tangents for every key: flat at the ends, smooth through the middle.
    void AutoTangents();
    // Adds a key at `t` on the current curve shape (value and slope preserved), returns its index.
    int AddKey(float t);

    // [[time, value, inTangent, outTangent], ...]
    nlohmann::json ToJson() const;
    // Accepts that form. Malformed input leaves `out` unchanged and returns false.
    static bool FromJson(const nlohmann::json& j, Curve& out);

    static Curve Constant(float value);
    // A straight line from (t0, v0) to (t1, v1).
    static Curve Line(float t0, float v0, float t1, float v1);
    // 0 -> 1 over 0..1 with flat ends (smoothstep-like).
    static Curve EaseInOut();
    // A kick: rises to 1 by `peak`, then settles smoothly back to 0 at 1 (recoil-shaped).
    static Curve Kick(float peak = 0.12f);
};
