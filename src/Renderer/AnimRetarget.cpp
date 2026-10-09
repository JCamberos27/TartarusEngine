#include "AnimRetarget.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace {

int FindNode(const std::vector<AnimNode>& nodes, const std::string& name) {
    for (int i = 0; i < (int)nodes.size(); ++i)
        if (nodes[i].Name == name) return i;
    return -1;
}

bool IsIkBone(const std::string& name) { return name.rfind("ik_", 0) == 0; }

std::vector<glm::mat4> RestGlobals(const std::vector<AnimNode>& nodes) {
    std::vector<glm::mat4> g(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        g[i] = nodes[i].Parent >= 0 ? AffineMul(g[nodes[i].Parent], nodes[i].BindLocal) : nodes[i].BindLocal;
    return g;
}

glm::quat RotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) {
        const float len = glm::length(r[c]);
        if (len > 1e-8f) r[c] /= len;
    }
    return glm::normalize(glm::quat_cast(r));
}

glm::vec3 ScaleOf(const glm::mat4& m) { return {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))}; }

glm::vec3 PositionOf(const glm::mat4& m) { return glm::vec3(m[3]); }

// The turn taking unit `from` onto unit `to` (shortest arc).
glm::quat Arc(const glm::vec3& from, const glm::vec3& to) {
    const float d = glm::dot(from, to);
    if (d > 0.999999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.999999f) {
        glm::vec3 axis = glm::cross(glm::vec3(1, 0, 0), from);
        if (glm::length(axis) < 1e-4f) axis = glm::cross(glm::vec3(0, 1, 0), from);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    return glm::normalize(glm::rotation(from, to));
}

// right / up / front from a skeleton's rest: up = pelvis to head, right = left thigh to right thigh.
bool BodyBasis(const std::vector<glm::mat4>& g, int pelvis, int head, int thighL, int thighR, glm::mat3& out) {
    if (pelvis < 0 || head < 0 || thighL < 0 || thighR < 0) return false;
    glm::vec3 up = PositionOf(g[head]) - PositionOf(g[pelvis]);
    glm::vec3 right = PositionOf(g[thighR]) - PositionOf(g[thighL]);
    if (glm::length(up) < 1e-6f || glm::length(right) < 1e-6f) return false;
    up = glm::normalize(up);
    right = right - up * glm::dot(right, up);
    if (glm::length(right) < 1e-6f) return false;
    right = glm::normalize(right);
    out = glm::mat3(right, up, glm::cross(right, up));
    return true;
}

float LegLength(const std::vector<glm::mat4>& g, int thigh, int calf, int foot) {
    if (thigh < 0 || calf < 0 || foot < 0) return 0.0f;
    return glm::length(PositionOf(g[calf]) - PositionOf(g[thigh])) + glm::length(PositionOf(g[foot]) - PositionOf(g[calf]));
}

int ChildPriority(const std::string& name) {
    if (name.find("twist") != std::string::npos) return -1;
    if (name.rfind("spine", 0) == 0 || name.rfind("neck", 0) == 0 || name == "head") return 3;
    if (name.rfind("middle_01", 0) == 0) return 2;
    return 0;
}

} // namespace

bool RetargetIsUe4ToUe5(const std::vector<AnimNode>& source, const std::vector<AnimNode>& target) {
    return FindNode(source, "spine_03") >= 0 && FindNode(source, "spine_04") < 0 && FindNode(target, "spine_05") >= 0;
}

std::vector<int> RetargetMatchNodes(const std::vector<AnimNode>& source, const std::vector<AnimNode>& target) {
    std::unordered_map<std::string, int> byName;
    for (int i = (int)source.size() - 1; i >= 0; --i) byName[source[i].Name] = i; // the first of a name wins
    const bool ue4to5 = RetargetIsUe4ToUe5(source, target);
    std::vector<int> match(target.size(), -1);
    for (int t = 0; t < (int)target.size(); ++t) {
        std::string name = target[t].Name;
        if (IsIkBone(name)) continue;
        if (ue4to5) {
            // UE4 spine_01..03 spread over UE5 spine_01..05: the ends and the middle match, 02 / 04 are filled in.
            if (name == "spine_02" || name == "spine_04") continue;
            if (name == "spine_03") name = "spine_02";
            else if (name == "spine_05") name = "spine_03";
        }
        if (const auto it = byName.find(name); it != byName.end()) match[t] = it->second;
    }
    return match;
}

bool RetargetBakeClip(const std::vector<AnimNode>& source, const AnimationClip& clip, const std::vector<AnimNode>& target,
                      AnimationClip& out, std::vector<int>& nodeChannel) {
    const std::vector<int> match = RetargetMatchNodes(source, target);
    int matched = 0;
    for (int m : match) matched += m >= 0 ? 1 : 0;
    if (matched < 8) return false;

    const std::vector<glm::mat4> srcRest = RestGlobals(source), tgtRest = RestGlobals(target);
    const int n = (int)target.size();
    auto tNode = [&](const char* name) { return FindNode(target, name); };
    auto sNode = [&](const char* name) { const int t = FindNode(target, name); return t >= 0 ? match[t] : -1; };

    // The two files' world frames: F takes a source world direction to the target's.
    glm::mat3 bs, bt;
    if (!BodyBasis(srcRest, sNode("pelvis"), sNode("head"), sNode("thigh_l"), sNode("thigh_r"), bs) ||
        !BodyBasis(tgtRest, tNode("pelvis"), tNode("head"), tNode("thigh_l"), tNode("thigh_r"), bt))
        return false;
    const glm::mat3 fm = bt * glm::transpose(bs);
    const glm::quat F = glm::normalize(glm::quat_cast(fm));
    const float srcLeg = LegLength(srcRest, sNode("thigh_l"), sNode("calf_l"), sNode("foot_l"));
    const float tgtLeg = LegLength(tgtRest, tNode("thigh_l"), tNode("calf_l"), tNode("foot_l"));
    const float k = srcLeg > 1e-6f && tgtLeg > 1e-6f ? tgtLeg / srcLeg : 1.0f;

    // The matched tree: per target node its nearest matched ancestor, and per matched node the matched
    // nodes directly under it (no matched node between), ik bones left out.
    std::vector<int> matchedAncestor(n, -1), depth(n, 0);
    std::vector<std::vector<int>> matchedChildren(n);
    std::vector<int> subtree(n, 0); // matched descendants, for picking a bone's direction
    for (int i = 0; i < n; ++i) {
        const int p = target[i].Parent;
        depth[i] = p >= 0 ? depth[p] + 1 : 0;
        if (p >= 0) matchedAncestor[i] = match[p] >= 0 ? p : matchedAncestor[p];
        if (match[i] >= 0 && matchedAncestor[i] >= 0) matchedChildren[matchedAncestor[i]].push_back(i);
    }
    for (int i = n - 1; i >= 0; --i)
        if (match[i] >= 0 && matchedAncestor[i] >= 0) subtree[matchedAncestor[i]] += subtree[i] + 1;

    // Per matched node, the constant part of R(t) = F * source(t) * Off: Off = inv(F S) * A * T, where A swings the
    // target's rest bone onto the source's rest bone direction (toward the preferred matched child).
    std::vector<glm::quat> off(n, glm::quat(1, 0, 0, 0)), align(n, glm::quat(1, 0, 0, 0)), tgtRot(n);
    std::vector<int> towards(n, -1);
    for (int i = 0; i < n; ++i) tgtRot[i] = RotationOf(tgtRest[i]);
    for (int i = 0; i < n; ++i) {
        if (match[i] < 0) continue;
        int best = -1;
        for (int c : matchedChildren[i]) {
            if (best < 0) { best = c; continue; }
            const int pc = ChildPriority(target[c].Name), pb = ChildPriority(target[best].Name);
            if (pc > pb || (pc == pb && subtree[c] > subtree[best])) best = c;
        }
        if (best >= 0 && ChildPriority(target[best].Name) < 0) best = -1;
        towards[i] = best;
        glm::quat a = matchedAncestor[i] >= 0 ? align[matchedAncestor[i]] : glm::quat(1, 0, 0, 0);
        if (best >= 0) {
            const glm::vec3 td = PositionOf(tgtRest[best]) - PositionOf(tgtRest[i]);
            const glm::vec3 sd = fm * (PositionOf(srcRest[match[best]]) - PositionOf(srcRest[match[i]]));
            if (glm::length(td) > 1e-6f && glm::length(sd) > 1e-6f) a = Arc(glm::normalize(td), glm::normalize(sd));
        }
        align[i] = a;
        off[i] = glm::normalize(glm::inverse(F * RotationOf(srcRest[match[i]])) * a * tgtRot[i]);
    }

    // Unmatched bones between a matched bone and a matched one below it share their turns by depth.
    std::vector<int> lower(n, -1);
    std::vector<float> frac(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        if (match[i] >= 0 || matchedAncestor[i] < 0 || IsIkBone(target[i].Name)) continue;
        int found = -1; // the first matched node below i, breadth first
        std::vector<int> queue{i};
        for (size_t q = 0; q < queue.size() && found < 0; ++q)
            for (int c = 0; c < n; ++c)
                if (target[c].Parent == queue[q] && !IsIkBone(target[c].Name)) {
                    if (match[c] >= 0) { found = c; break; }
                    queue.push_back(c);
                }
        if (found < 0) continue;
        const int a = matchedAncestor[i];
        lower[i] = found;
        frac[i] = float(depth[i] - depth[a]) / float(std::max(1, depth[found] - depth[a]));
    }

    // Which nodes the baked clip drives, and which of them take the source's translation (root, pelvis, and any
    // matched node with no matched ancestor).
    std::vector<bool> driven(n, false), moves(n, false);
    for (int i = 0; i < n; ++i) {
        driven[i] = match[i] >= 0 || lower[i] >= 0;
        moves[i] = match[i] >= 0 && (matchedAncestor[i] < 0 || target[i].Name == "pelvis");
    }

    // The source's own node -> channel map (the import's, else by name).
    std::vector<int> srcChannel = clip.NodeChannel;
    if (srcChannel.size() != source.size()) {
        srcChannel.assign(source.size(), -1);
        for (int c = 0; c < (int)clip.Channels.size(); ++c) {
            const int s = FindNode(source, clip.Channels[c].BoneName);
            if (s >= 0) srcChannel[s] = c;
        }
    }

    // Key times: the source's densest rotation track.
    std::vector<float> times;
    for (const BoneAnimChannel& ch : clip.Channels)
        if (ch.Rotations.size() > times.size()) {
            times.clear();
            for (const RotationKey& key : ch.Rotations) times.push_back(key.TimeTicks);
        }
    if (times.size() < 2) times = {0.0f, std::max(clip.DurationTicks, 1e-3f)};

    out = AnimationClip();
    out.Name = clip.Name;
    out.DurationTicks = clip.DurationTicks;
    out.TicksPerSecond = clip.TicksPerSecond;
    out.StartTicks = clip.StartTicks;
    out.EndTicks = clip.EndTicks;
    nodeChannel.assign(n, -1);
    for (int i = 0; i < n; ++i) {
        if (!driven[i]) continue;
        nodeChannel[i] = (int)out.Channels.size();
        BoneAnimChannel ch;
        ch.BoneName = target[i].Name;
        ch.Rotations.reserve(times.size());
        if (moves[i]) ch.Positions.reserve(times.size());
        out.Channels.push_back(std::move(ch));
    }
    out.NodeChannel = nodeChannel;

    std::vector<glm::mat4> srcNow(source.size()), tgtNow(n);
    std::vector<glm::quat> rot(n), turn(n);
    for (float t : times) {
        for (size_t s = 0; s < source.size(); ++s) {
            const int c = srcChannel[s];
            const glm::mat4 local = c >= 0 ? clip.Channels[c].Sample(t, source[s].BindTRS).ToMatrix() : source[s].BindLocal;
            srcNow[s] = source[s].Parent >= 0 ? AffineMul(srcNow[source[s].Parent], local) : local;
        }
        for (int i = 0; i < n; ++i)
            if (match[i] >= 0) {
                rot[i] = glm::normalize(F * RotationOf(srcNow[match[i]]) * off[i]);
                turn[i] = glm::normalize(rot[i] * glm::inverse(tgtRot[i]));
            }
        for (int i = 0; i < n; ++i)
            if (lower[i] >= 0) {
                turn[i] = glm::normalize(glm::slerp(turn[matchedAncestor[i]], turn[lower[i]], frac[i]));
                rot[i] = glm::normalize(turn[i] * tgtRot[i]);
            }
        for (int i = 0; i < n; ++i) {
            const int p = target[i].Parent;
            if (!driven[i]) {
                tgtNow[i] = p >= 0 ? AffineMul(tgtNow[p], target[i].BindLocal) : target[i].BindLocal;
                continue;
            }
            glm::vec3 pos = p >= 0 ? glm::vec3(tgtNow[p] * glm::vec4(target[i].BindTRS.T, 1.0f)) : target[i].BindTRS.T;
            if (moves[i])
                pos = PositionOf(tgtRest[i]) + fm * (PositionOf(srcNow[match[i]]) - PositionOf(srcRest[match[i]])) * k;
            tgtNow[i] = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot[i]) * glm::scale(glm::mat4(1.0f), ScaleOf(tgtRest[i]));
            const glm::mat4 local = p >= 0 ? glm::inverse(tgtNow[p]) * tgtNow[i] : tgtNow[i];
            BoneAnimChannel& ch = out.Channels[nodeChannel[i]];
            glm::quat r = RotationOf(local);
            if (!ch.Rotations.empty() && glm::dot(ch.Rotations.back().Value, r) < 0.0f) r = -r; // keep keys on one hemisphere
            ch.Rotations.push_back({r, t});
            if (moves[i]) ch.Positions.push_back({PositionOf(local), t});
        }
    }
    return true;
}
