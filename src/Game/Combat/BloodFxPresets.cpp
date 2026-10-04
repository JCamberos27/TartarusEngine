#include "BloodFxPresets.h"

#include <cstring>

float BloodCurve::Eval(float t) const {
    if (Count <= 0) return 0.0f;
    if (t <= Keys[0][0]) return Keys[0][1];
    if (t >= Keys[Count - 1][0]) return Keys[Count - 1][1];
    int i = 0;
    while (i + 1 < Count && t > Keys[i + 1][0]) ++i;
    const float* a = Keys[i];
    const float* b = Keys[i + 1];
    const float dt = b[0] - a[0];
    if (dt <= 1e-9f) return b[1];
    // Unity marks a stepped segment with an infinite tangent.
    if (a[3] > 1e29f || a[3] < -1e29f || b[2] > 1e29f || b[2] < -1e29f) return a[1];
    const float s = (t - a[0]) / dt;
    const float s2 = s * s, s3 = s2 * s;
    const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f, h10 = s3 - 2.0f * s2 + s;
    const float h01 = -2.0f * s3 + 3.0f * s2, h11 = s3 - s2;
    return h00 * a[1] + h10 * dt * a[3] + h01 * b[1] + h11 * dt * b[2];
}

const std::vector<BloodPresetDef>& BloodPresets() {
    static const std::vector<BloodPresetDef> presets = {
#include "BloodFxPresets.inc"
    };
    return presets;
}

const BloodPresetDef* FindBloodPreset(const char* name) {
    for (const BloodPresetDef& p : BloodPresets())
        if (std::strcmp(p.Name, name) == 0) return &p;
    return nullptr;
}
