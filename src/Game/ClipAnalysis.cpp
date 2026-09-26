#include "ClipAnalysis.h"

#include "Model.h"

#include <algorithm>
#include <cmath>

namespace ClipAnalysis {

// A cyclic on/off signal with its flicker taken out: gaps of `minRun` samples or fewer between two "on" runs are
// filled, then "on" runs shorter than `minRun` dropped (a foot near the height threshold chatters).
void Debounce(std::vector<char>& v, int minRun) {
    const int n = (int)v.size();
    if (n == 0 || minRun < 1) return;
    auto flipRuns = [&](char value) {
        // Find a start that is not inside a `value` run, so every run is seen whole.
        int start = -1;
        for (int i = 0; i < n; ++i)
            if (v[i] != value) { start = i; break; }
        if (start < 0) return; // all `value`
        for (int k = 0; k < n;) {
            const int i = (start + k) % n;
            if (v[i] != value) { ++k; continue; }
            int len = 0;
            while (k + len < n && v[(start + k + len) % n] == value) ++len;
            // A gap (value == off) is filled only between two "on" neighbours, which a cyclic run always has.
            if (len <= minRun) for (int j = 0; j < len; ++j) v[(start + k + j) % n] = !value;
            k += len;
        }
    };
    flipRuns(0); // fill short gaps
    flipRuns(1); // drop short contacts
}

namespace {

glm::vec3 Pos(const glm::mat4& m) { return glm::vec3(m[3]); }

} // namespace

Result Analyze(const Model& model, int clip, int rootNode, const RootMotionSettings& rm,
               const std::string footBones[2], const std::vector<std::string>& seamBones) {
    Result r;
    const float len = model.AnimationLength(clip);
    if (clip < 0 || rootNode < 0 || !(len > 0.0f)) return r;
    r.Valid = true;
    r.Length = len;

    const RootMotionDelta d = model.ClipRootMotion(clip, 0.0f, len, AnimationWrapMode::ClampForever, rootNode, rm);
    r.Distance = glm::length(glm::vec2(d.Translation.x, d.Translation.z));
    r.Speed = r.Distance / len;
    r.YawDegrees = glm::degrees(d.Yaw);
    {
        RootMotionSettings v = rm;
        v.Vertical = true;
        r.VerticalDelta = model.ClipRootMotion(clip, 0.0f, len, AnimationWrapMode::ClampForever, rootNode, v).Translation.y;
    }

    // Ground speed over the clip, for the plot.
    const int n = kSamples;
    const float dt = len / n;
    r.SpeedProfile.resize(n);
    for (int i = 0; i < n; ++i) {
        const RootMotionDelta s = model.ClipRootMotion(clip, i * dt, (i + 1) * dt, AnimationWrapMode::ClampForever, rootNode, rm);
        r.SpeedProfile[i] = glm::length(glm::vec2(s.Translation.x, s.Translation.z)) / dt;
    }

    // Loop seam: each bone relative to the root, last frame against the first.
    auto rel = [&](int node, float t) {
        const glm::mat4 root = model.SampleNodeModelSpace(clip, t, AnimationWrapMode::ClampForever, rootNode);
        return Pos(glm::inverse(root) * model.SampleNodeModelSpace(clip, t, AnimationWrapMode::ClampForever, node));
    };
    for (const std::string& b : seamBones) {
        const int node = model.NodeIndex(b);
        if (node < 0) continue;
        const float gap = glm::length(rel(node, len) - rel(node, 0.0f));
        if (gap > r.LoopSeam) { r.LoopSeam = gap; r.LoopSeamBone = b; }
    }

    // Foot contacts: a foot is planted while it is in the lowest fifth of its height range.
    for (int f = 0; f < 2; ++f) {
        Foot& foot = r.Feet[f];
        foot.Bone = footBones[f];
        const int node = model.NodeIndex(foot.Bone);
        if (node < 0) continue;
        foot.Found = true;
        std::vector<glm::vec3> p(n);
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < n; ++i) {
            p[i] = Pos(model.SampleNodeModelSpace(clip, i * dt, AnimationWrapMode::Loop, node));
            lo = std::min(lo, p[i].y);
            hi = std::max(hi, p[i].y);
        }
        const float limit = lo + 0.2f * (hi - lo);
        std::vector<char> down(n);
        for (int i = 0; i < n; ++i) down[i] = hi - lo > 0.02f && p[i].y <= limit;
        Debounce(down, n / 16);
        int planted = 0;
        for (int i = 0; i < n; ++i) planted += down[i];
        foot.ContactFraction = (float)planted / n;
        for (int i = 0; i < n; ++i)
            if (down[i] && !down[(i + n - 1) % n]) foot.ContactStarts.push_back((float)i / n);
        const int plants = (int)foot.ContactStarts.size();
        if (plants > 0 && r.Distance > 0.005f) foot.Stride = r.Distance / plants;
        else if (plants > 0) {
            // In place: the planted foot slides back by exactly what the body would travel.
            float slide = 0.0f;
            int runs = 0;
            for (int i = 0; i < n; ++i)
                if (down[i] && !down[(i + n - 1) % n]) {
                    int j = i;
                    while (down[(j + 1) % n] && (j + 1) % n != i) j = (j + 1) % n;
                    slide += glm::length(glm::vec2(p[j].x - p[i].x, p[j].z - p[i].z));
                    ++runs;
                }
            if (runs > 0) foot.Stride = slide / runs;
        }
    }
    return r;
}

} // namespace ClipAnalysis
