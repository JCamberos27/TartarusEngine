#include "Curve.h"

#include <algorithm>
#include <cmath>

using json = nlohmann::json;

float Curve::Evaluate(float t) const {
    if (Keys.empty()) return 0.0f;
    if (!(t > Keys.front().Time)) return Keys.front().Value; // also catches NaN
    if (t >= Keys.back().Time) return Keys.back().Value;
    // Keys are few (a handful per curve), so a linear scan beats anything clever.
    size_t i = 1;
    while (i < Keys.size() && Keys[i].Time < t) ++i;
    const CurveKey& a = Keys[i - 1];
    const CurveKey& b = Keys[i];
    const float dt = b.Time - a.Time;
    if (!(dt > 1e-6f)) return b.Value;
    const float s = (t - a.Time) / dt;
    const float s2 = s * s, s3 = s2 * s;
    const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
    const float h10 = s3 - 2.0f * s2 + s;
    const float h01 = -2.0f * s3 + 3.0f * s2;
    const float h11 = s3 - s2;
    return h00 * a.Value + h10 * dt * a.OutTangent + h01 * b.Value + h11 * dt * b.InTangent;
}

void Curve::Sort() {
    std::stable_sort(Keys.begin(), Keys.end(), [](const CurveKey& a, const CurveKey& b) { return a.Time < b.Time; });
}

void Curve::AutoTangents() {
    const size_t n = Keys.size();
    for (size_t i = 0; i < n; ++i) {
        float slope = 0.0f;
        if (i > 0 && i + 1 < n) {
            const float dt = Keys[i + 1].Time - Keys[i - 1].Time;
            slope = dt > 1e-6f ? (Keys[i + 1].Value - Keys[i - 1].Value) / dt : 0.0f;
        }
        Keys[i].InTangent = Keys[i].OutTangent = slope;
    }
}

int Curve::AddKey(float t) {
    CurveKey k;
    k.Time = t;
    k.Value = Evaluate(t);
    const float h = 1e-3f;
    const float slope = Keys.size() >= 2 ? (Evaluate(t + h) - Evaluate(t - h)) / (2.0f * h) : 0.0f;
    k.InTangent = k.OutTangent = slope;
    Keys.push_back(k);
    Sort();
    for (int i = 0; i < (int)Keys.size(); ++i)
        if (Keys[i].Time == t) return i;
    return (int)Keys.size() - 1;
}

json Curve::ToJson() const {
    json arr = json::array();
    for (const CurveKey& k : Keys) {
        auto r = [](float v) { return std::round((double)v * 1e5) / 1e5; };
        arr.push_back({r(k.Time), r(k.Value), r(k.InTangent), r(k.OutTangent)});
    }
    return arr;
}

bool Curve::FromJson(const json& j, Curve& out) {
    if (!j.is_array()) return false;
    Curve parsed;
    for (const json& k : j) {
        if (!k.is_array() || k.size() < 2) return false;
        CurveKey key;
        float* fields[4] = {&key.Time, &key.Value, &key.InTangent, &key.OutTangent};
        for (size_t i = 0; i < 4 && i < k.size(); ++i) {
            if (!k[i].is_number()) return false;
            *fields[i] = k[i].get<float>();
            if (!std::isfinite(*fields[i])) return false;
        }
        parsed.Keys.push_back(key);
    }
    parsed.Sort();
    out = std::move(parsed);
    return true;
}

Curve Curve::Constant(float value) {
    Curve c;
    c.Keys.push_back({0.0f, value, 0.0f, 0.0f});
    return c;
}

Curve Curve::Line(float t0, float v0, float t1, float v1) {
    Curve c;
    const float slope = t1 > t0 ? (v1 - v0) / (t1 - t0) : 0.0f;
    c.Keys.push_back({t0, v0, slope, slope});
    c.Keys.push_back({t1, v1, slope, slope});
    return c;
}

Curve Curve::EaseInOut() {
    Curve c;
    c.Keys.push_back({0.0f, 0.0f, 0.0f, 0.0f});
    c.Keys.push_back({1.0f, 1.0f, 0.0f, 0.0f});
    return c;
}

Curve Curve::Kick(float peak) {
    peak = std::clamp(peak, 0.01f, 0.9f);
    Curve c;
    c.Keys.push_back({0.0f, 0.0f, 0.0f, 2.0f / peak});
    c.Keys.push_back({peak, 1.0f, 0.0f, 0.0f});
    c.Keys.push_back({1.0f, 0.0f, 0.0f, 0.0f});
    return c;
}
