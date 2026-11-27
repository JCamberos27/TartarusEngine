#include "MotionBake.h"

#include "MotionClipStream.h"
#include "MotionDatabase.h"
#include "MotionMath.h"

#include "Core/Env.h"
#include "Core/Paths.h"
#include "Model.h"

#include <glm/gtc/matrix_transform.hpp>
#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;

// ---------------------------------------------------------------------------------------------------------------------
// Config and takes

namespace {

std::string ResolveProjectPath(const std::string& p) {
    if (p.empty()) return p;
    const fs::path path(p);
    if (path.is_absolute()) return path.lexically_normal().string();
    return fs::path(ProjectPaths::Resolve(p)).lexically_normal().string();
}

RootSynthParams ReadParams(const json& j, RootSynthParams p) {
    if (!j.is_object()) return p;
    p.PosSigma = j.value("posSigma", p.PosSigma);
    p.YawSigma = j.value("yawSigma", p.YawSigma);
    p.HipsWeight = j.value("hipsWeight", p.HipsWeight);
    return p;
}

std::vector<std::string> SplitTags(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ';'))
        if (!t.empty()) out.push_back(t);
    return out;
}

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

glm::quat RotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) r[c] = glm::normalize(r[c]);
    return glm::normalize(glm::quat_cast(r));
}

} // namespace

bool MotionBakeConfig::Load(const std::string& path, std::string& error) {
    Path = path;
    std::ifstream in(path);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    const json j = json::parse(in, nullptr, false);
    if (!j.is_object()) {
        error = path + ": not a JSON object";
        return false;
    }
    if (j.contains("source") && j["source"].is_object()) {
        SourceAnims = j["source"].value("anims", SourceAnims);
        SourceRig = j["source"].value("rig", SourceRig);
    }
    Takes = ResolveProjectPath(j.value("takes", std::string("../tools/mm/takes.csv")));
    TargetRig = ResolveProjectPath(j.value("targetRig", std::string()));
    Output = ResolveProjectPath(j.value("output", std::string()));
    Fps = j.value("fps", Fps);
    Workers = std::max(1, j.value("workers", Workers));
    if (j.contains("bones") && j["bones"].is_array())
        for (const auto& b : j["bones"])
            if (b.is_string()) Bones.push_back(b.get<std::string>());
    if (j.contains("staticBones") && j["staticBones"].is_array())
        for (const auto& b : j["staticBones"])
            if (b.is_string()) StaticBonePrefixes.push_back(b.get<std::string>());
    StaticPoseFrom = j.value("staticPoseFrom", std::string());
    if (j.contains("rootSynth") && j["rootSynth"].is_object()) {
        const json& r = j["rootSynth"];
        if (r.contains("default")) RootDefault = ReadParams(r["default"], RootDefault);
        if (r.contains("byTag") && r["byTag"].is_object())
            for (auto it = r["byTag"].begin(); it != r["byTag"].end(); ++it) RootByTag[it.key()] = ReadParams(it.value(), RootDefault);
    }
    if (j.contains("labels") && j["labels"].is_object()) {
        const json& l = j["labels"];
        if (l.contains("trajFrames") && l["trajFrames"].is_array() && l["trajFrames"].size() == 3)
            for (int k = 0; k < 3; ++k) Labels.TrajFrames[k] = l["trajFrames"][(size_t)k].get<int>();
        if (l.contains("gaitSpeeds") && l["gaitSpeeds"].is_array() && l["gaitSpeeds"].size() == 3)
            for (int k = 0; k < 3; ++k) Labels.GaitSpeeds[k] = l["gaitSpeeds"][(size_t)k].get<float>();
        Labels.CrouchRatio = l.value("crouchRatio", Labels.CrouchRatio);
        Labels.ProneHeight = l.value("proneHeight", Labels.ProneHeight);
        Labels.StrafeAngleDeg = l.value("strafeAngleDeg", Labels.StrafeAngleDeg);
        Labels.StrafeMinSpeed = l.value("strafeMinSpeed", Labels.StrafeMinSpeed);
        Labels.AirFootHeight = l.value("airFootHeight", Labels.AirFootHeight);
        Labels.AirMinFrames = l.value("airMinFrames", Labels.AirMinFrames);
        Labels.AirPelvisRise = l.value("airPelvisRise", Labels.AirPelvisRise);
        Labels.ContactHeight = l.value("contactHeight", Labels.ContactHeight);
        Labels.ContactSpeed = l.value("contactSpeed", Labels.ContactSpeed);
        if (l.contains("styleSpeeds") && l["styleSpeeds"].is_object())
            for (auto it = l["styleSpeeds"].begin(); it != l["styleSpeeds"].end(); ++it)
                if (const int st = MotionLabel::StyleFromName(it.key()); st >= 0 && it.value().is_number())
                    Labels.StyleSpeeds[st] = it.value().get<float>();
        Labels.StyleWindowSeconds = l.value("styleWindowSeconds", Labels.StyleWindowSeconds);
    }
    if (j.contains("budgets") && j["budgets"].is_object()) {
        BudgetPoseMB = j["budgets"].value("poseMB", BudgetPoseMB);
        BudgetFeatureMB = j["budgets"].value("featureMB", BudgetFeatureMB);
        BudgetLoadSeconds = j["budgets"].value("loadSeconds", BudgetLoadSeconds);
    }
    if (j.contains("validation") && j["validation"].is_object()) {
        const json& v = j["validation"];
        FacingP95Deg = v.value("facingP95Deg", FacingP95Deg);
        YawJitterMax = v.value("yawJitter", YawJitterMax);
        RigLengthPct = v.value("rigLengthPct", RigLengthPct);
        ReconstructCm = v.value("reconstructCm", ReconstructCm);
    }
    if (j.contains("featureNorm")) {
        // All or nothing: a half-read normalization would quietly unfreeze the costs.
        const json& n = j["featureNorm"];
        const int dims = MotionFeatureSet::kDims;
        bool ok = n.is_object() && n.contains("mean") && n["mean"].is_array() && n["mean"].size() == (size_t)dims &&
                  n.contains("groupScale") && n["groupScale"].is_object();
        std::vector<float> mean(dims, 0.0f), scale(dims, 1.0f);
        for (int d = 0; ok && d < dims; ++d) {
            ok = n["mean"][(size_t)d].is_number();
            if (ok) mean[(size_t)d] = n["mean"][(size_t)d].get<float>();
            ok = ok && std::isfinite(mean[(size_t)d]);
        }
        for (int g = 0; ok && g < MotionFeature::GroupCount; ++g) {
            const json& gs = n["groupScale"];
            const char* name = MotionFeature::GroupName(g);
            ok = gs.contains(name) && gs[name].is_number();
            const float s = ok ? gs[name].get<float>() : 0.0f;
            ok = ok && std::isfinite(s) && s > 0.0f;
            for (int d = MotionFeature::kGroupFirst[g]; ok && d < MotionFeature::kGroupFirst[g + 1]; ++d) scale[(size_t)d] = s;
        }
        if (!ok) {
            error = path + ": featureNorm needs \"mean\" (" + std::to_string(dims) +
                    " numbers) and \"groupScale\" (a positive number per group: footPos, footVel, pelvisVel, trajPos, trajDir)";
            return false;
        }
        FeatureNormMean = std::move(mean);
        FeatureNormScale = std::move(scale);
    }
    if (Bones.empty() || Output.empty() || TargetRig.empty()) {
        error = path + ": needs bones, output and targetRig";
        return false;
    }
    if (std::find(Bones.begin(), Bones.end(), "pelvis") == Bones.end()) {
        error = path + ": the bones must include pelvis";
        return false;
    }
    return true;
}

RootSynthParams MotionBakeConfig::ParamsFor(const std::vector<std::string>& tags) const {
    for (const std::string& t : tags)
        if (auto it = RootByTag.find(t); it != RootByTag.end()) return it->second;
    return RootDefault;
}

bool MotionTake::HasTag(const std::string& t) const { return std::find(Tags.begin(), Tags.end(), t) != Tags.end(); }

std::string MotionTake::TagString() const {
    std::string s;
    for (const std::string& t : Tags) s += (s.empty() ? "" : ";") + t;
    return s;
}

namespace MotionBake {

std::string SourceDir() {
    const std::string dir = Env::Str(Env::Var::MmSource);
    std::error_code ec;
    return !dir.empty() && fs::is_directory(dir, ec) ? dir : std::string();
}

std::string FeatureNormJson(const std::vector<float>& mean, const std::vector<float>& scale) {
    // %.9g: a float's 9 significant digits read back (as a double, then cast) to the same float.
    const auto num = [](float v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.9g", (double)v);
        return std::string(b);
    };
    std::string s = "\"featureNorm\": {\n    \"groupScale\": {";
    for (int g = 0; g < MotionFeature::GroupCount; ++g) {
        const size_t d = (size_t)MotionFeature::kGroupFirst[g];
        s += std::string(g ? ", " : " ") + "\"" + MotionFeature::GroupName(g) + "\": " + (d < scale.size() ? num(scale[d]) : "1");
    }
    s += " },\n    \"mean\": [";
    for (size_t d = 0; d < mean.size(); ++d) s += (d ? (d % 6 ? ", " : ",\n      ") : "") + num(mean[d]);
    s += "]\n  }";
    return s;
}

bool ReadTakes(const std::string& csvPath, std::vector<MotionTake>& out, std::string& error) {
    std::ifstream in(csvPath);
    if (!in) {
        error = "cannot read " + csvPath;
        return false;
    }
    std::string line;
    std::vector<std::string> header;
    const auto split = [](const std::string& s) {
        std::vector<std::string> f;
        std::string cur;
        for (char c : s) {
            if (c == ',') { f.push_back(cur); cur.clear(); }
            else if (c != '\r') cur += c;
        }
        f.push_back(cur);
        return f;
    };
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = split(line);
        if (header.empty()) {
            header = f;
            continue;
        }
        const auto col = [&](const char* name) -> std::string {
            for (size_t i = 0; i < header.size() && i < f.size(); ++i)
                if (header[i] == name) return f[i];
            return {};
        };
        MotionTake t;
        t.Name = col("name");
        if (t.Name.empty()) continue;
        t.Source = col("source");
        t.Loop = std::atoi(col("loop").c_str());
        if (t.Source == t.Name) t.Source.clear();
        t.Include = col("include") == "1";
        t.TrimStart = (float)std::atof(col("trim_start").c_str());
        t.TrimEnd = (float)std::atof(col("trim_end").c_str());
        t.Tags = SplitTags(col("tags"));
        out.push_back(std::move(t));
    }
    if (header.empty() || std::find(header.begin(), header.end(), "name") == header.end()) {
        error = csvPath + ": no header with a name column";
        return false;
    }
    return true;
}

void RootSpaceGlobals(const Model& model, const std::vector<LocalTRS>& pose, int rootNode, std::vector<glm::mat4>& out) {
    const int n = model.NodeCount();
    out.resize((size_t)n);
    for (int i = 0; i < n; ++i) {
        const int p = model.NodeParent(i);
        const glm::mat4 local = pose[(size_t)i].ToMatrix();
        out[(size_t)i] = p >= 0 ? AffineMul(out[(size_t)p], local) : local;
    }
    if (rootNode < 0) return;
    const glm::mat4 inv = glm::inverse(out[(size_t)rootNode]);
    for (int i = 0; i < n; ++i) out[(size_t)i] = AffineMul(inv, out[(size_t)i]);
}

std::vector<BoneDiff> RigDiff(const Model& source, const Model& target, const std::vector<std::string>& bones) {
    std::vector<LocalTRS> sp, tp;
    source.BindLocalPose(sp);
    target.BindLocalPose(tp);
    std::vector<glm::mat4> sg, tg;
    RootSpaceGlobals(source, sp, source.NodeIndex("root"), sg);
    RootSpaceGlobals(target, tp, target.NodeIndex("root"), tg);
    std::vector<BoneDiff> out;
    for (const std::string& b : bones) {
        BoneDiff d;
        d.Name = b;
        const int si = source.NodeIndex(b), ti = target.NodeIndex(b);
        d.InSource = si >= 0;
        d.InTarget = ti >= 0;
        if (si >= 0) d.LengthSource = glm::length(sp[(size_t)si].T);
        if (ti >= 0) d.LengthTarget = glm::length(tp[(size_t)ti].T);
        if (si >= 0 && ti >= 0) {
            d.LengthPct = d.LengthSource > 1e-4f ? std::fabs(d.LengthTarget - d.LengthSource) / d.LengthSource * 100.0f
                                                 : (d.LengthTarget > 1e-4f ? 100.0f : 0.0f);
            d.RestAngleDeg = MotionMath::QuatAngle(sp[(size_t)si].R, tp[(size_t)ti].R) * 180.0f / MotionMath::kPi;
            d.PositionCm = glm::length(glm::vec3(sg[(size_t)si][3]) - glm::vec3(tg[(size_t)ti][3])) * 100.0f;
        }
        out.push_back(d);
    }
    return out;
}

namespace {
// Nodes ordered parents first.
std::vector<int> TopoOrder(const std::vector<int>& parents) {
    const int n = (int)parents.size();
    std::vector<int> order, state((size_t)n, 0);
    order.reserve((size_t)n);
    std::vector<int> stack;
    for (int i = 0; i < n; ++i) {
        for (int k = i; k >= 0 && state[(size_t)k] == 0; k = parents[(size_t)k]) {
            stack.push_back(k);
            state[(size_t)k] = 1;
        }
        while (!stack.empty()) {
            order.push_back(stack.back());
            stack.pop_back();
        }
    }
    return order;
}

std::string MirrorName(const std::string& n) {
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "_l") == 0) return n.substr(0, n.size() - 2) + "_r";
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "_r") == 0) return n.substr(0, n.size() - 2) + "_l";
    return n;
}
} // namespace

void Globals(const std::vector<int>& parents, const std::vector<LocalTRS>& pose, std::vector<glm::mat4>& out) {
    out.assign(parents.size(), glm::mat4(1.0f));
    for (int i : TopoOrder(parents)) {
        const int p = parents[(size_t)i];
        out[(size_t)i] = p >= 0 ? AffineMul(out[(size_t)p], pose[(size_t)i].ToMatrix()) : pose[(size_t)i].ToMatrix();
    }
}

MirrorRig BuildMirror(const std::vector<std::string>& names, const std::vector<int>& parents, const std::vector<LocalTRS>& bind,
                      int root) {
    MirrorRig r;
    const size_t n = names.size();
    r.Parent = parents;
    r.Root = root;
    r.Mirror.resize(n);
    r.Below.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        r.Mirror[i] = (int)i;
        const std::string m = MirrorName(names[i]);
        if (m != names[i])
            for (size_t k = 0; k < n; ++k)
                if (names[k] == m) r.Mirror[i] = (int)k;
        for (int p = parents[i]; p >= 0; p = parents[(size_t)p])
            if (p == root) r.Below[i] = 1;
    }
    std::vector<glm::mat4> g;
    Globals(parents, bind, g);
    const glm::mat4 inv = root >= 0 ? glm::inverse(g[(size_t)root]) : glm::mat4(1.0f);
    for (glm::mat4& m : g) m = inv * m;
    const auto find = [&](const char* s) {
        for (size_t k = 0; k < n; ++k)
            if (names[k] == s) return (int)k;
        return -1;
    };
    const int tl = find("thigh_l"), tr = find("thigh_r");
    if (tl >= 0 && tr >= 0) {
        glm::vec3 d = glm::vec3(g[(size_t)tr][3]) - glm::vec3(g[(size_t)tl][3]);
        if (glm::length(d) > 1e-6f) {
            d = glm::normalize(d);
            // On an axis (the rig is built square to its root): exactly that axis.
            for (int a = 0; a < 3; ++a)
                if (std::fabs(d[a]) > 0.9998f) d = glm::vec3(a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f) * std::copysign(1.0f, d[a]);
            r.Normal = d;
        }
    }
    const glm::mat3 m3 = glm::mat3(1.0f) - 2.0f * glm::outerProduct(r.Normal, r.Normal);
    r.M = glm::mat4(m3);
    r.Correction.assign(n, glm::mat4(1.0f));
    for (size_t i = 0; i < n; ++i)
        if (r.Below[i] || (int)i == root) r.Correction[i] = glm::inverse(r.M * g[(size_t)r.Mirror[i]] * r.M) * g[i];
    return r;
}

void MirrorPose(const MirrorRig& rig, std::vector<LocalTRS>& pose) {
    const size_t n = pose.size();
    if (rig.Root < 0 || rig.Parent.size() != n) return;
    std::vector<glm::mat4> g;
    Globals(rig.Parent, pose, g);
    const glm::mat4 inv = glm::inverse(g[(size_t)rig.Root]);
    for (glm::mat4& m : g) m = inv * m; // the root node's frame
    std::vector<glm::mat4> out(n, glm::mat4(1.0f));
    for (size_t i = 0; i < n; ++i)
        if (rig.Below[i]) out[i] = rig.M * g[(size_t)rig.Mirror[i]] * rig.M * rig.Correction[i];
    for (size_t i = 0; i < n; ++i) {
        if (!rig.Below[i]) continue;
        const int p = rig.Parent[i];
        const glm::mat4 parent = p == rig.Root ? glm::mat4(1.0f) : out[(size_t)p];
        const glm::mat4 local = glm::inverse(parent) * out[i];
        glm::mat3 r(local);
        for (int c = 0; c < 3; ++c) r[c] = glm::normalize(r[c]);
        pose[i].R = glm::normalize(glm::quat_cast(r));
        pose[i].T = glm::vec3(local[3]);
    }
}

RepairStats RepairGlitches(const std::vector<int>& parents, const std::vector<LocalTRS>& bind, const std::vector<char>& repair, float fps,
                           std::vector<std::vector<LocalTRS>>& poses) {
    RepairStats st;
    const size_t n = parents.size(), frames = poses.size();
    if (frames < 2 || bind.size() != n || repair.size() != n) return st;
    std::vector<std::vector<int>> children(n);
    for (size_t i = 0; i < n; ++i)
        if (parents[i] >= 0) children[(size_t)parents[i]].push_back((int)i);
    // The long axis per repaired bone (local frame), where it has one.
    std::vector<glm::vec3> axis(n, glm::vec3(0.0f));
    std::vector<char> hasAxis(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (!repair[i]) continue;
        if (!children[i].empty()) {
            const glm::vec3 first = bind[(size_t)children[i][0]].T;
            if (glm::length(first) < 1e-5f) continue;
            const glm::vec3 a = glm::normalize(first);
            bool line = true;
            for (int c : children[i]) {
                const glm::vec3 t = bind[(size_t)c].T;
                line = line && (glm::length(t) < 1e-5f || std::fabs(glm::dot(glm::normalize(t), a)) > 0.95f);
            }
            if (!line) continue;
            axis[i] = a;
            hasAxis[i] = 1;
        } else if (glm::length(bind[i].T) > 1e-5f) {
            axis[i] = glm::normalize(glm::inverse(bind[i].R) * bind[i].T);
            hasAxis[i] = 1;
        }
    }
    const float rolled = kRolledDeg * MotionMath::kPi / 180.0f;
    const auto countRolled = [&](std::vector<int>& out) {
        out.assign(n, 0);
        for (size_t i = 0; i < n; ++i)
            if (hasAxis[i])
                for (size_t f = 0; f < frames; ++f)
                    out[i] += std::fabs(MotionMath::TwistAngle(glm::inverse(bind[i].R) * poses[f][i].R, axis[i])) > rolled ? 1 : 0;
    };
    countRolled(st.RolledBefore);
    // 1. Twist flips, bone by bone (parents first), each judged on its world rotation as repaired so far. A flip is a run of
    // frames each turning the bone over kFlipStepDeg about its axis (no limb twists that fast), together over kFlipDeg - one
    // frame (the solver's 170-180 deg flips) or a few (60-90 deg each: one frame's threshold missed them, so a flip and its
    // flip back were unrolled one way only and the roll stayed to the clip's end). The run's twist is taken out frame by
    // frame - no jump left in its place - one of two ways, by what the capture did to the bone's children over it:
    //   - they counter-rotated (their local rotations turned with it: their world rotations, and every joint, unchanged - a
    //     pure twist of the bone): the bone is turned back about its axis and the children take the inverse;
    //   - they rode along (the chain below flipped with it): the bone is turned back and the children ride back with it.
    //     Counter-rotating them here too (before 2026-11-18) left the roll on the child, to be caught - or, its axis turned
    //     off the parent's by a bent knee, missed - at the flip back.
    // Measured on the pack (`rolled`: bone-frames whose twist is over kRolledDeg off the bind): 3,357 as captured; 43,181
    // after the old repair (`Transitions_Movement_Take001`'s calf ~130 deg off for the rest of the clip).
    const std::vector<int> order = TopoOrder(parents);
    const float flip = kFlipDeg * MotionMath::kPi / 180.0f;
    const float fast = kFlipStepDeg * MotionMath::kPi / 180.0f * 60.0f / std::max(1.0f, fps);
    std::vector<std::vector<glm::quat>> world(n);           // per bone, per frame: the repaired world rotation
    std::vector<float> compAt(frames * n, 0.0f), turnAt(frames * n, 0.0f); // per frame and bone: the children's share, the bone's turn
    std::vector<float> d(frames, 0.0f);
    for (int ii : order) {
        const size_t i = (size_t)ii;
        const int p = parents[i];
        std::vector<glm::quat>& G = world[i];
        G.resize(frames);
        for (size_t f = 0; f < frames; ++f) { // without its own turn
            glm::quat r = poses[f][i].R;
            if (p >= 0 && compAt[f * n + (size_t)p] != 0.0f) r = glm::inverse(glm::angleAxis(compAt[f * n + (size_t)p], axis[(size_t)p])) * r;
            G[f] = p >= 0 ? world[(size_t)p][f] * r : r;
        }
        if (!hasAxis[i]) continue;
        // (A twist about the bone's own axis on the right doesn't change the twist between frames: the steps are its own turn's.)
        for (size_t f = 1; f < frames; ++f) d[f] = MotionMath::TwistAngle(glm::inverse(G[f - 1]) * G[f], axis[i]);
        const auto wrap = [](float a) { return MotionMath::WrapPi(a); };
        // The flips: runs of fast frames, together over kFlipDeg.
        struct Flip { size_t A, E; float Sum; bool Rode; };
        std::vector<Flip> flips;
        for (size_t f = 1; f < frames;) {
            if (std::fabs(d[f]) <= fast) { ++f; continue; }
            size_t e = f;
            float sum = 0.0f;
            while (e < frames && std::fabs(d[e]) > fast && (e == f || (d[e] > 0.0f) == (d[f] > 0.0f))) sum += d[e++];
            if (std::fabs(sum) > flip) {
                bool rode = true; // the children's local rotations over the run
                for (int ch : children[i])
                    if (MotionMath::QuatAngle(poses[f - 1][(size_t)ch].R, poses[e - 1][(size_t)ch].R) > 0.5f * std::fabs(sum)) rode = false;
                flips.push_back({f, e, sum, rode});
            }
            f = e;
        }
        st.Flips += (int)flips.size(); // (less any dropped below: FlipsDropped)
        // Each flip paired with the next one back: both runs' twist taken out, and what they don't cancel (a flip of 154 deg
        // undone by one of 110) spread over the frames between - after the pair the bone is its capture's again, so no
        // offset is left to run on to the clip's end. A flip with none back (the take starts or ends flipped): unrolled
        // toward the side that ends nearer the bind (the children as the flip had them: riding along, or counter-rotating).
        std::vector<float> turn(frames, 0.0f), compv(frames, 0.0f);
        const auto twistOffBind = [&](size_t from, size_t to) { // median, of the bone's local as it stands (its parent's share applied)
            std::vector<float> t;
            for (size_t f = from; f < to; f += 2) {
                glm::quat r = poses[f][i].R;
                if (p >= 0 && compAt[f * n + (size_t)p] != 0.0f) r = glm::inverse(glm::angleAxis(compAt[f * n + (size_t)p], axis[(size_t)p])) * r;
                t.push_back(MotionMath::TwistAngle(glm::inverse(bind[i].R) * r, axis[i]));
            }
            if (t.empty()) return 0.0f;
            std::nth_element(t.begin(), t.begin() + (std::ptrdiff_t)(t.size() / 2), t.end());
            return t[t.size() / 2];
        };
        for (size_t k = 0; k < flips.size(); ++k) {
            const Flip& A = flips[k];
            std::vector<float> removed(frames, 0.0f);
            size_t last = frames; // the pair's end (exclusive), or the clip's
            // (Paired when together they make no flip - modulo a whole turn: near 180 deg the sign is a toss-up, and a -175 deg
            // flip is often undone by a -170.)
            if (k + 1 < flips.size() && std::fabs(wrap(A.Sum + flips[k + 1].Sum)) < flip) {
                const Flip& B = flips[k + 1];
                for (size_t f = A.A; f < A.E; ++f) removed[f] = d[f];
                for (size_t f = B.A; f < B.E; ++f) removed[f] = d[f];
                const float left = wrap(A.Sum + B.Sum); // put back between them (the whole turns left over are no rotation)
                if (B.A > A.E)
                    for (size_t f = A.E; f < B.A; ++f) removed[f] = -left / (float)(B.A - A.E);
                else
                    for (size_t f = B.A; f < B.E; ++f) removed[f] -= left / (float)(B.E - B.A);
                last = B.E;
                ++k;
            } else {
                for (size_t f = A.A; f < A.E; ++f) removed[f] = d[f];
            }
            float acc = 0.0f;
            for (size_t f = A.A; f < frames; ++f) {
                if (f < last) acc -= removed[f];
                else if (last < frames) break; // (a pair: nothing after it)
                turn[f] += acc;
                if (!A.Rode) compv[f] += acc;
            }
            if (last == frames) { // unpaired: forward (the rest turned back), or backward (what came before turned to it)?
                const float before = twistOffBind(0, A.A), after = twistOffBind(A.E, frames);
                if (std::fabs(wrap(before + A.Sum)) < std::fabs(wrap(after - A.Sum)))
                    for (size_t f = 0; f < frames; ++f) { // the whole take turned by the flip: what came before it turned to it
                        turn[f] += A.Sum;
                        if (!A.Rode) compv[f] += A.Sum;
                    }
            }
        }
        // Kept only if it leaves the bone no more rolled (over kRolledDeg off the bind's twist) than it was without it - the
        // flips the capture shows can't always be read (near 180 deg the twist's sign is a toss-up; a toe's flip and its flip
        // back read -112 and -105), and a correction read wrong runs on, rolled, to the next one.
        int with = 0, without = 0;
        for (size_t f = 0; f < frames; ++f) {
            glm::quat r = poses[f][i].R;
            if (p >= 0 && compAt[f * n + (size_t)p] != 0.0f) r = glm::inverse(glm::angleAxis(compAt[f * n + (size_t)p], axis[(size_t)p])) * r;
            const glm::quat rb = glm::inverse(bind[i].R) * r;
            without += std::fabs(MotionMath::TwistAngle(rb, axis[i])) > rolled ? 1 : 0;
            with += std::fabs(MotionMath::TwistAngle(rb * glm::angleAxis(turn[f], axis[i]), axis[i])) > rolled ? 1 : 0;
        }
        if (with > without) {
            st.FlipsDropped += (int)flips.size();
            std::fill(turn.begin(), turn.end(), 0.0f);
            std::fill(compv.begin(), compv.end(), 0.0f);
        }
        for (size_t f = 0; f < frames; ++f) {
            compAt[f * n + i] = compv[f];
            turnAt[f * n + i] = turn[f];
        }
        for (size_t f = 0; f < frames; ++f)
            if (turnAt[f * n + i] != 0.0f) G[f] = G[f] * glm::angleAxis(turnAt[f * n + i], axis[i]);
    }
    for (size_t f = 0; f < frames; ++f) {
        const std::vector<LocalTRS> was = poses[f];
        for (size_t i = 0; i < n; ++i) {
            glm::quat r = was[i].R;
            const int p = parents[i];
            const bool byParent = p >= 0 && compAt[f * n + (size_t)p] != 0.0f, own = turnAt[f * n + i] != 0.0f;
            if (!byParent && !own) continue; // (untouched: bit for bit)
            if (byParent) r = glm::inverse(glm::angleAxis(compAt[f * n + (size_t)p], axis[(size_t)p])) * r;
            if (own) r = r * glm::angleAxis(turnAt[f * n + i], axis[i]);
            poses[f][i].R = glm::normalize(r);
        }
    }
    // 2. Spikes, on the (unrolled) local rotations.
    const float limit = kSpikeDeg * MotionMath::kPi / 180.0f * 60.0f / std::max(1.0f, fps);
    const float back = kSpikeReturnDeg * MotionMath::kPi / 180.0f;
    for (size_t i = 0; i < n; ++i) {
        if (!repair[i]) continue;
        for (size_t f = 1; f < frames; ++f) {
            const glm::quat a = poses[f - 1][i].R;
            if (MotionMath::QuatAngle(a, poses[f][i].R) <= limit) continue;
            for (size_t k = f + 1; k < frames && k <= f + (size_t)kSpikeMaxFrames; ++k) {
                if (MotionMath::QuatAngle(a, poses[k][i].R) >= back) continue;
                glm::quat b = poses[k][i].R;
                if (glm::dot(a, b) < 0.0f) b = -b;
                for (size_t j = f; j < k; ++j) poses[j][i].R = glm::slerp(a, b, (float)(j - (f - 1)) / (float)(k - (f - 1)));
                ++st.Spikes;
                st.SpikeFrames += (int)(k - f);
                f = k;
                break;
            }
        }
    }
    countRolled(st.RolledAfter);
    return st;
}

// ---------------------------------------------------------------------------------------------------------------------
// The bake

namespace {

ModelImportSettings ClipOnlySettings() {
    ModelImportSettings s;
    s.AnimationOnly = true;
    s.ImportNormals = false;
    s.MaterialImportMode = ModelImportSettings::MaterialMode::None;
    return s;
}

// What every take shares (read-only while the workers run).
struct BakeContext {
    const MotionBakeConfig* Cfg = nullptr;
    std::string AnimsDir;
    const Model* Target = nullptr;
    glm::mat4 C{1.0f}, CInv{1.0f};   // the target's `root` node in its model space (the Z-up -> Y-up turn), and inverse
    float RestYaw = 0.0f;
    float Side = 1.0f;                // -1 when the rig's _r bones sit on its left (never seen; checked anyway)
    std::vector<std::string> StaticBones;
    // The target's node per streamed bone, its pelvis, its feet, its bind pose.
    std::vector<int> TargetNodes;
    int TargetPelvis = -1, TargetFoot[2] = {-1, -1};
    std::vector<LocalTRS> TargetBind;
    float PelvisScale = 1.0f;
    // The pack rig's rest pose by node name: the mirror's rest correction and the repair's bone axes (a take's own nodes'
    // defaults are whatever its export left there).
    std::map<std::string, LocalTRS> SourceBind;
};

struct TakeResult {
    bool Ok = false;
    std::string Error;
    MotionTake Take;
    float SourceStart = 0.0f, TakeSeconds = 0.0f;
    std::vector<MotionClipStream::RootSample> Root;
    RootTrack Track;
    std::vector<glm::vec3> PelvisT;
    std::vector<glm::quat> Rot; // frames x bones
    MotionClipFrames Frames;
    RootValidation Raw;          // the generated root, the source's own feet
    RootValidation Rebuilt;      // the feet rebuilt from the quantized stream on the target rig, placed by the root
    float RebuildMaxCm = 0.0f;   // worst distance between a source foot and its rebuild
    float SpeedP50 = 0.0f, PelvisP50 = 0.0f, AnkleP05 = 0.0f;
    std::vector<glm::quat> StaticRot;
    std::vector<glm::quat> FingerRot; // frames x StaticBones, when the take animates its fingers (HasFingers)
    bool HasFingers = false;
    RepairStats Repair;
    std::vector<std::string> NodeNames; // (the take's nodes: RepairStats' per-node counts)
    int GlitchFrames = 0;     // frames still over kGlitchDegrees on a streamed bone after the repair (excluded at load)
    glm::vec3 MirrorNormal{0.0f};
    // The finger bones (the config's staticBones, not streamed): how far each turns over the trimmed take (deg, off its
    // first sampled rotation, every 6th frame) - whether the take animates them (111 of the pack's 139 have finger data).
    int FingerBones = 0, FingerBonesMoving = 0; // ... and how many turn over kFingerMovingDeg
    float FingerRangeDeg = 0.0f;                // the most any turns
    double ImportSeconds = 0.0, BakeSeconds = 0.0;
};
constexpr float kFingerMovingDeg = 5.0f;

float Percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * (double)v.size()))];
}

TakeResult BakeTake(const BakeContext& ctx, const MotionTake& take, bool wantStatics) {
    TakeResult r;
    r.Take = take;
    const MotionBakeConfig& cfg = *ctx.Cfg;
    const std::string path = take.File().find('/') != std::string::npos ? ResolveProjectPath(take.File() + ".fbx")
                                                                           : (fs::path(ctx.AnimsDir) / (take.File() + ".fbx")).string();
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        r.Error = "missing " + path;
        return r;
    }
    const double t0 = Now();
    Model m(path, ClipOnlySettings());
    r.ImportSeconds = Now() - t0;
    if (m.AnimationCount() < 1) {
        r.Error = "no clip in " + path;
        return r;
    }
    const float fps = cfg.Fps;
    const float clipLen = m.AnimationLength(0);
    const int loops = std::max(1, take.Loop);
    const float len = clipLen * (float)loops; // a loop's cycles back to back
    const int total = (int)std::floor(len * fps + 1e-3f) + 1;
    const int first = std::clamp((int)std::lround(take.TrimStart * fps), 0, total - 1);
    const int last = take.TrimEnd > 0.0f ? std::clamp((int)std::lround(take.TrimEnd * fps), first, total - 1) : total - 1;
    const int n = last - first + 1;
    if (n < (int)fps) {
        r.Error = "under a second left after the trim";
        return r;
    }
    r.SourceStart = (float)first / fps;
    r.TakeSeconds = len;
    // Where in the clip, and which cycle, a time of the (looped) take is.
    const auto clipTime = [&](float t, int& cycle) {
        cycle = loops > 1 ? std::min(loops - 1, (int)std::floor(t / clipLen)) : 0;
        return std::clamp(t - (float)cycle * clipLen, 0.0f, clipLen);
    };

    const int root = m.NodeIndex("root"), pelvis = m.NodeIndex("pelvis");
    const int thigh[2] = {m.NodeIndex("thigh_l"), m.NodeIndex("thigh_r")};
    const int clav[2] = {m.NodeIndex("clavicle_l"), m.NodeIndex("clavicle_r")};
    const int foot[2] = {m.NodeIndex("foot_l"), m.NodeIndex("foot_r")};
    if (root < 0 || pelvis < 0 || thigh[0] < 0 || thigh[1] < 0 || clav[0] < 0 || clav[1] < 0 || foot[0] < 0 || foot[1] < 0) {
        r.Error = "missing root / pelvis / thighs / clavicles / feet";
        return r;
    }
    if (m.NodeParent(pelvis) != root) {
        r.Error = "pelvis is not a child of root";
        return r;
    }
    const size_t nb = cfg.Bones.size();
    std::vector<int> src(nb);
    int pelvisBone = -1;
    for (size_t b = 0; b < nb; ++b) {
        src[b] = m.NodeIndex(cfg.Bones[b]);
        if (src[b] < 0) {
            r.Error = "missing bone " + cfg.Bones[b];
            return r;
        }
        if (cfg.Bones[b] == "pelvis") pelvisBone = (int)b;
    }
    // The finger bones in the stream's order (its Statics), -1 where the take lacks one.
    std::vector<int> statics;
    bool allFingers = !ctx.StaticBones.empty();
    for (const std::string& s : ctx.StaticBones) {
        statics.push_back(m.NodeIndex(s));
        allFingers = allFingers && statics.back() >= 0;
    }
    std::vector<int> fingers;
    for (const std::string& s : ctx.StaticBones)
        if (const int i = m.NodeIndex(s); i >= 0) fingers.push_back(i);
    std::vector<glm::quat> fingerFirst(fingers.size());
    std::vector<float> fingerRange(fingers.size(), 0.0f);
    bool haveFingerFirst = false;

    // 1. Sample every frame: the bones' local rotations and the model-space points the root is made from. The root is
    // generated over up to kMargin more of the take each side of the trim, then cut to it: a trim that ends mid-run
    // (the pattern budgets cut takes anywhere) is then smoothed with the frames that really follow it, and the edge
    // padding (MotionMath::MirrorPad) only ever stands in at the take's own ends - inside the trims, where the actor
    // settles. (Padding at a cut through a turnaround ran the root a metre off the pelvis.)
    const int margin = (int)std::lround(2.0f * fps);
    const int extFirst = std::max(0, first - margin), extLast = std::min(total - 1, last + margin);
    const int ne = extLast - extFirst + 1, head = first - extFirst;
    RootSynthInput in;
    in.Fps = fps;
    in.Pelvis.resize((size_t)ne);
    in.HipsRight.resize((size_t)ne);
    in.ShoulderRight.resize((size_t)ne);
    r.Frames.Fps = fps;
    r.Frames.RestYaw = ctx.RestYaw;
    r.Frames.Pelvis.resize((size_t)ne);
    for (int k = 0; k < 2; ++k) r.Frames.Foot[k].resize((size_t)ne);
    r.Rot.resize((size_t)ne * nb);
    std::vector<glm::mat4> pelvisRootSpace((size_t)ne);
    std::vector<std::vector<LocalTRS>> poses((size_t)ne);
    std::vector<int> cycleOf((size_t)ne, 0);
    for (int f = 0; f < ne; ++f)
        m.SampleLocalPose(0, clipTime(std::min(len, (float)(extFirst + f) / fps), cycleOf[(size_t)f]), AnimationWrapMode::ClampForever,
                          poses[(size_t)f]);
    // Root motion: the root bone's place against its place at the clip's start (model space, from each frame's own pose), and its
    // travel over one cycle - a loop's later cycles carry on from where the earlier ones left it.
    std::vector<glm::mat4> rootFromStart((size_t)ne, glm::mat4(1.0f));
    if (take.RootMotion()) {
        std::vector<LocalTRS> p0, p1;
        std::vector<glm::mat4> gm;
        m.SampleLocalPose(0, 0.0f, AnimationWrapMode::ClampForever, p0);
        RootSpaceGlobals(m, p0, -1, gm);
        const glm::mat4 start = gm[(size_t)root], startInv = glm::inverse(start);
        m.SampleLocalPose(0, clipLen, AnimationWrapMode::ClampForever, p1);
        RootSpaceGlobals(m, p1, -1, gm);
        const glm::mat4 cycle = startInv * gm[(size_t)root];
        for (int f = 0; f < ne; ++f) {
            RootSpaceGlobals(m, poses[(size_t)f], -1, gm);
            glm::mat4 carried(1.0f);
            for (int k = 0; k < cycleOf[(size_t)f]; ++k) carried = carried * cycle;
            rootFromStart[(size_t)f] = carried * startInv * gm[(size_t)root];
        }
    }
    {
        // The take's skeleton, with the pack rig's rest pose where it has the node.
        const int nn = m.NodeCount();
        std::vector<int> parents((size_t)nn);
        std::vector<std::string> names((size_t)nn);
        std::vector<LocalTRS> bind;
        m.BindLocalPose(bind);
        for (int i = 0; i < nn; ++i) {
            parents[(size_t)i] = m.NodeParent(i);
            names[(size_t)i] = m.NodeName(i);
            if (auto it = ctx.SourceBind.find(names[(size_t)i]); it != ctx.SourceBind.end()) bind[(size_t)i] = it->second;
        }
        // Repair first (the capture as shot), then mirror.
        std::vector<char> repair((size_t)nn, 0);
        for (int s : src) repair[(size_t)s] = 1;
        for (int s : fingers) repair[(size_t)s] = 1;
        r.Repair = RepairGlitches(parents, bind, repair, fps, poses);
        r.NodeNames = names;
        if (take.Mirrored()) {
            const MirrorRig mr = BuildMirror(names, parents, bind, root);
            r.MirrorNormal = mr.Normal;
            for (auto& p : poses) MirrorPose(mr, p);
            // ... and a root motion clip's travel with it: reflected through the same plane (the start's root frame is the frame
            // the plane is in), so world = (M R M)(M g M K) - the whole take's mirror image.
            if (take.RootMotion())
                for (glm::mat4& x : rootFromStart) x = mr.M * x * mr.M;
        }
    }
    std::vector<glm::mat4> g;
    for (int f = 0; f < ne; ++f) {
        const std::vector<LocalTRS>& pose = poses[(size_t)f];
        RootSpaceGlobals(m, pose, root, g);
        if (take.RootMotion())
            for (glm::mat4& x : g) x = rootFromStart[(size_t)f] * x;
        const auto P = [&](int node) { return glm::vec3(ctx.C * g[(size_t)node][3]); };
        in.Pelvis[(size_t)f] = r.Frames.Pelvis[(size_t)f] = P(pelvis);
        in.HipsRight[(size_t)f] = (P(thigh[1]) - P(thigh[0])) * ctx.Side;
        in.ShoulderRight[(size_t)f] = (P(clav[1]) - P(clav[0])) * ctx.Side;
        for (int k = 0; k < 2; ++k) r.Frames.Foot[k][(size_t)f] = P(foot[k]);
        for (size_t b = 0; b < nb; ++b) r.Rot[(size_t)f * nb + b] = glm::normalize(pose[(size_t)src[b]].R);
        pelvisRootSpace[(size_t)f] = g[(size_t)pelvis];
        if (allFingers && f >= head && f < head + n)
            for (int s : statics) r.FingerRot.push_back(glm::normalize(pose[(size_t)s].R));
        if (f >= head && f < head + n && (f - head) % 6 == 0) {
            for (size_t k = 0; k < fingers.size(); ++k) {
                const glm::quat q = glm::normalize(pose[(size_t)fingers[k]].R);
                if (!haveFingerFirst) fingerFirst[k] = q;
                else fingerRange[k] = std::max(fingerRange[k], MotionMath::QuatAngle(fingerFirst[k], q) * 180.0f / MotionMath::kPi);
            }
            haveFingerFirst = true;
        }
        if (wantStatics && f == head + n / 2) {
            r.StaticRot.resize(statics.size());
            for (size_t s = 0; s < statics.size(); ++s)
                r.StaticRot[s] = statics[s] >= 0 ? glm::normalize(pose[(size_t)statics[s]].R) : glm::quat(1, 0, 0, 0);
        }
    }

    // 2. The root, then the pelvis re-expressed under it (in the `root` node's frame, which is its parent).
    r.Track = RootSynth::Generate(in, cfg.ParamsFor(take.Tags));
    // A root motion clip carries its own root - the path and facing an animator authored for the capsule - so that is the root,
    // not one generated from the hips. Measured on the old jog set (2026-11-17): generated, the loops, starts, stops and pivots
    // wobbled 9-173 deg/s^2 (authored: 0), and the side clips faced the hips' twist toward the travel - the strafe loops 44 and
    // 57 deg off ahead, the diagonals 21 - so a strafe matched nothing well. The path: the root bone on the ground. The facing:
    // the root bone's own ahead (its turn since the clip's start: it starts facing the rig's rest ahead, ctx.RestYaw - the
    // straight clips' hips agree within 2 deg, the turning starts' within 7).
    if (take.RootMotion()) {
        const glm::mat3 C3(ctx.C), CInv3(ctx.CInv);
        for (int f = 0; f < ne; ++f) {
            const glm::vec3 p = glm::vec3(ctx.C * rootFromStart[(size_t)f][3]);
            r.Track.Position[(size_t)f] = glm::vec3(p.x, 0.0f, p.z);
            const glm::vec3 ahead = C3 * glm::mat3(rootFromStart[(size_t)f]) * CInv3 * MotionMath::YawDir(ctx.RestYaw);
            float t = MotionMath::YawOf(glm::vec3(ahead.x, 0.0f, ahead.z));
            if (f > 0) t = r.Track.Yaw[(size_t)f - 1] + std::remainder(t - r.Track.Yaw[(size_t)f - 1], 2.0f * MotionMath::kPi); // unwrapped
            r.Track.Yaw[(size_t)f] = t;
        }
    }
    // Cut everything to the trim.
    const auto cut = [&](auto& v, size_t per) {
        v.erase(v.begin() + (long)((size_t)(head + n) * per), v.end());
        v.erase(v.begin(), v.begin() + (long)((size_t)head * per));
    };
    cut(in.Pelvis, 1);
    cut(in.HipsRight, 1);
    cut(in.ShoulderRight, 1);
    cut(r.Frames.Pelvis, 1);
    cut(r.Frames.Foot[0], 1);
    cut(r.Frames.Foot[1], 1);
    cut(r.Rot, nb);
    cut(pelvisRootSpace, 1);
    cut(r.Track.Position, 1);
    cut(r.Track.Yaw, 1);
    r.Root.resize((size_t)n);
    r.PelvisT.resize((size_t)n);
    for (int f = 0; f < n; ++f) {
        r.Root[(size_t)f].Position = r.Track.Position[(size_t)f];
        r.Root[(size_t)f].Yaw = r.Track.Yaw[(size_t)f];
        const glm::mat4 R = RootSynth::RootMatrix(r.Track.Position[(size_t)f], r.Track.Yaw[(size_t)f], ctx.RestYaw);
        const glm::mat4 local = ctx.CInv * glm::inverse(R) * ctx.C * pelvisRootSpace[(size_t)f];
        r.PelvisT[(size_t)f] = glm::vec3(local[3]);
        if (pelvisBone >= 0) r.Rot[(size_t)f * nb + (size_t)pelvisBone] = RotationOf(local);
    }

    // 3. Validation: the root against the source's hips and feet...
    std::vector<char> contacts[2];
    for (int k = 0; k < 2; ++k)
        contacts[k] = RootSynth::Contacts(r.Frames.Foot[k], fps, cfg.Labels.ContactHeight, cfg.Labels.ContactSpeed);
    r.Raw = RootSynth::Validate(r.Track, fps, in.HipsRight, r.Frames.Foot, contacts);
    // ...and the feet rebuilt the way the stream will play: quantized rotations on the target rig, the pelvis scaled,
    // placed by the root. They must land on the source's feet (and so plant where it planted).
    std::vector<glm::vec3> rebuilt[2];
    for (int k = 0; k < 2; ++k) rebuilt[k].resize((size_t)n);
    std::vector<LocalTRS> tpose = ctx.TargetBind;
    std::vector<glm::mat4> tg;
    for (int f = 0; f < n; ++f) {
        for (size_t b = 0; b < nb; ++b)
            if (ctx.TargetNodes[b] >= 0)
                tpose[(size_t)ctx.TargetNodes[b]].R = MotionMath::UnpackQuat(MotionMath::PackQuat(r.Rot[(size_t)f * nb + b]));
        if (ctx.TargetPelvis >= 0) tpose[(size_t)ctx.TargetPelvis].T = r.PelvisT[(size_t)f] * ctx.PelvisScale;
        RootSpaceGlobals(*ctx.Target, tpose, -1, tg); // the target's own model space
        const glm::mat4 R = RootSynth::RootMatrix(r.Track.Position[(size_t)f], r.Track.Yaw[(size_t)f], ctx.RestYaw);
        for (int k = 0; k < 2; ++k) {
            if (ctx.TargetFoot[k] < 0) continue;
            rebuilt[k][(size_t)f] = glm::vec3(R * tg[(size_t)ctx.TargetFoot[k]][3]);
            r.RebuildMaxCm = std::max(r.RebuildMaxCm, glm::length(rebuilt[k][(size_t)f] - r.Frames.Foot[k][(size_t)f]) * 100.0f);
        }
    }
    r.Rebuilt = RootSynth::Validate(r.Track, fps, in.HipsRight, rebuilt, contacts);

    // Numbers the label thresholds are tuned from.
    std::vector<float> speed((size_t)n), pelvisH((size_t)n), ankle((size_t)n);
    for (int f = 0; f < n; ++f) {
        const int a = std::max(0, f - 1), b = std::min(n - 1, f + 1);
        speed[(size_t)f] = glm::length(r.Track.Position[(size_t)b] - r.Track.Position[(size_t)a]) * fps / (float)std::max(1, b - a);
        pelvisH[(size_t)f] = r.Frames.Pelvis[(size_t)f].y;
        ankle[(size_t)f] = std::min(r.Frames.Foot[0][(size_t)f].y, r.Frames.Foot[1][(size_t)f].y);
    }
    r.FingerBones = (int)fingers.size();
    for (float a : fingerRange) {
        r.FingerRangeDeg = std::max(r.FingerRangeDeg, a);
        r.FingerBonesMoving += a > kFingerMovingDeg ? 1 : 0;
    }
    // A take whose fingers never move holds the export's default there, not a captured hand: it plays the relaxed pose.
    r.HasFingers = allFingers && r.FingerBonesMoving > 0;
    if (!r.HasFingers) std::vector<glm::quat>().swap(r.FingerRot);
    // What the repair left: frames with a streamed bone over the load-time glitch limit (MotionDatabase::kGlitchDegrees).
    {
        const float limit = MotionDatabase::kGlitchDegrees * MotionMath::kPi / 180.0f * 60.0f / std::max(1.0f, fps);
        for (int f = 1; f < n; ++f) {
            bool bad = false;
            for (size_t b = 0; b < nb && !bad; ++b)
                bad = MotionMath::QuatAngle(r.Rot[(size_t)(f - 1) * nb + b], r.Rot[(size_t)f * nb + b]) > limit;
            r.GlitchFrames += bad ? 1 : 0;
        }
    }
    r.SpeedP50 = Percentile(speed, 0.5f);
    r.PelvisP50 = Percentile(pelvisH, 0.5f);
    r.AnkleP05 = Percentile(ankle, 0.05f);
    r.BakeSeconds = Now() - t0 - r.ImportSeconds;
    r.Ok = true;
    return r;
}

std::string Mb(size_t bytes) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return b;
}

} // namespace

int Run(const std::string& configPath, const std::string& match, const std::string& outOverride) {
    const double start = Now();
    MotionBakeConfig cfg;
    std::string error;
    const std::string cfgPath = ResolveProjectPath(configPath);
    std::error_code ec;
    if (!cfg.Load(fs::exists(configPath, ec) ? configPath : cfgPath, error)) {
        std::cerr << "[MotionBake] ERROR: " << error << std::endl;
        return 2;
    }
    if (!outOverride.empty()) cfg.Output = fs::absolute(outOverride).lexically_normal().string();
    const std::string source = SourceDir();
    if (source.empty()) {
        std::cerr << "[MotionBake] ERROR: TARTARUS_MM_SOURCE is not set to the mocap pack's folder (anims\\, rig\\)" << std::endl;
        return 2;
    }
    std::vector<MotionTake> all, takes;
    if (!ReadTakes(cfg.Takes, all, error)) {
        std::cerr << "[MotionBake] ERROR: " << error << std::endl;
        return 2;
    }
    for (const MotionTake& t : all)
        if (t.Include && (match.empty() || t.Name.find(match) != std::string::npos)) takes.push_back(t);
    std::printf("[MotionBake] %s: %zu of %zu takes, source %s\n", fs::path(cfg.Path).filename().string().c_str(), takes.size(),
                all.size(), source.c_str());
    if (takes.empty()) {
        std::cerr << "[MotionBake] ERROR: no takes to bake" << std::endl;
        return 2;
    }

    // The two rigs: the pack's and the one the stream plays on.
    const std::string sourceRigPath = (fs::path(source) / cfg.SourceRig).string();
    if (!fs::exists(sourceRigPath, ec) || !fs::exists(cfg.TargetRig, ec)) {
        std::cerr << "[MotionBake] ERROR: missing rig " << (fs::exists(sourceRigPath, ec) ? cfg.TargetRig : sourceRigPath) << std::endl;
        return 2;
    }
    Model sourceRig(sourceRigPath, ClipOnlySettings());
    Model targetRig(cfg.TargetRig, ClipOnlySettings());
    const int targetRoot = targetRig.NodeIndex("root");
    if (targetRoot < 0 || sourceRig.NodeIndex("root") < 0) {
        std::cerr << "[MotionBake] ERROR: a rig has no root node" << std::endl;
        return 2;
    }

    // Rig diff (rest pose and bone lengths, in each rig's root frame).
    json report;
    json diffJ = json::array();
    int overLength = 0, missing = 0;
    std::printf("[MotionBake] rig diff: %s vs %s (bone length, rest angle, rest position)\n", cfg.SourceRig.c_str(),
                fs::path(cfg.TargetRig).filename().string().c_str());
    for (const BoneDiff& d : RigDiff(sourceRig, targetRig, cfg.Bones)) {
        const bool over = d.LengthPct > cfg.RigLengthPct;
        overLength += over ? 1 : 0;
        missing += d.InSource && d.InTarget ? 0 : 1;
        if (over || !d.InSource || !d.InTarget || d.RestAngleDeg > 1.0f || d.PositionCm > 1.0f)
            std::printf("[MotionBake]   %-24s %s len %.4f -> %.4f (%.2f%%) rest %.2f deg pos %.2f cm%s\n", d.Name.c_str(),
                        d.InSource && d.InTarget ? "" : "MISSING", d.LengthSource, d.LengthTarget, d.LengthPct, d.RestAngleDeg,
                        d.PositionCm, over ? "  OVER" : "");
        diffJ.push_back({{"bone", d.Name}, {"inSource", d.InSource}, {"inTarget", d.InTarget}, {"lengthSource", d.LengthSource},
                         {"lengthTarget", d.LengthTarget}, {"lengthPct", d.LengthPct}, {"restAngleDeg", d.RestAngleDeg},
                         {"positionCm", d.PositionCm}});
    }
    float worstPct = 0.0f, worstAngle = 0.0f;
    for (const auto& d : diffJ) {
        worstPct = std::max(worstPct, d["lengthPct"].get<float>());
        worstAngle = std::max(worstAngle, d["restAngleDeg"].get<float>());
    }
    std::printf("[MotionBake] rig diff: %zu core bones, %d over %.1f%% length, %d missing, worst length %.3f%%, worst rest angle %.3f deg\n",
                cfg.Bones.size(), overLength, cfg.RigLengthPct, missing, worstPct, worstAngle);
    report["rigDiff"] = diffJ;

    BakeContext ctx;
    ctx.Cfg = &cfg;
    ctx.AnimsDir = (fs::path(source) / cfg.SourceAnims).string();
    ctx.Target = &targetRig;
    targetRig.BindLocalPose(ctx.TargetBind);
    {
        std::vector<glm::mat4> tg;
        RootSpaceGlobals(targetRig, ctx.TargetBind, -1, tg);
        ctx.C = tg[(size_t)targetRoot];
        ctx.CInv = glm::inverse(ctx.C);
        const auto P = [&](const char* n) {
            const int i = targetRig.NodeIndex(n);
            return i >= 0 ? glm::vec3(tg[(size_t)i][3]) : glm::vec3(0.0f);
        };
        // The rest facing, and which side the _r bones are on: up x (thigh_l -> thigh_r) must agree with the feet
        // (ankle -> ball).
        const glm::vec3 right = P("thigh_r") - P("thigh_l");
        const glm::vec3 toes = (P("ball_l") - P("foot_l")) + (P("ball_r") - P("foot_r"));
        glm::vec3 fwd = RootSynth::ForwardFromRight(right);
        if (glm::dot(fwd, glm::vec3(toes.x, 0.0f, toes.z)) < 0.0f) {
            ctx.Side = -1.0f;
            fwd = -fwd;
            std::printf("[MotionBake] note: the rig's _r bones sit on its left; facing taken the other way\n");
        }
        ctx.RestYaw = MotionMath::YawOf(fwd);
        ctx.TargetPelvis = targetRig.NodeIndex("pelvis");
        ctx.TargetFoot[0] = targetRig.NodeIndex("foot_l");
        ctx.TargetFoot[1] = targetRig.NodeIndex("foot_r");
        const float targetPelvisH = ctx.TargetPelvis >= 0 ? tg[(size_t)ctx.TargetPelvis][3].y : 0.0f;
        std::vector<LocalTRS> sb;
        std::vector<glm::mat4> sg;
        sourceRig.BindLocalPose(sb);
        RootSpaceGlobals(sourceRig, sb, sourceRig.NodeIndex("root"), sg);
        const int sp = sourceRig.NodeIndex("pelvis");
        const float sourcePelvisH = sp >= 0 ? (ctx.C * sg[(size_t)sp][3]).y : 0.0f;
        ctx.PelvisScale = sourcePelvisH > 1e-3f && targetPelvisH > 1e-3f ? targetPelvisH / sourcePelvisH : 1.0f;
        cfg.Labels.StandingPelvis = sourcePelvisH;
        std::printf("[MotionBake] rest facing %.1f deg (0 = +Z), pelvis %.4f m (source) / %.4f m (target), scale %.4f\n",
                    ctx.RestYaw * 180.0f / MotionMath::kPi, sourcePelvisH, targetPelvisH, ctx.PelvisScale);
        report["restYawDeg"] = ctx.RestYaw * 180.0f / MotionMath::kPi;
        report["sourcePelvisHeight"] = sourcePelvisH;
        report["targetPelvisHeight"] = targetPelvisH;
    }
    for (const std::string& b : cfg.Bones) ctx.TargetNodes.push_back(targetRig.NodeIndex(b));
    {
        std::vector<LocalTRS> sb;
        sourceRig.BindLocalPose(sb);
        for (int i = 0; i < sourceRig.NodeCount(); ++i) ctx.SourceBind[sourceRig.NodeName(i)] = sb[(size_t)i];
    }
    for (int i = 0; i < sourceRig.NodeCount(); ++i) {
        const std::string& n = sourceRig.NodeName(i);
        if (std::find(cfg.Bones.begin(), cfg.Bones.end(), n) != cfg.Bones.end()) continue;
        for (const std::string& p : cfg.StaticBonePrefixes)
            if (n.rfind(p, 0) == 0) {
                ctx.StaticBones.push_back(n);
                break;
            }
    }

    // The takes, on worker threads (the FBX import is most of the time), kept in takes.csv order.
    std::string staticFrom = cfg.StaticPoseFrom;
    if (std::none_of(takes.begin(), takes.end(), [&](const MotionTake& t) { return t.Name == staticFrom; }))
        staticFrom = takes.front().Name;
    std::vector<TakeResult> results(takes.size());
    std::atomic<size_t> next{0};
    std::atomic<int> done{0};
    std::mutex printLock;
    const int workers = std::min<int>((int)takes.size(), std::max(1, cfg.Workers));
    std::vector<std::thread> pool;
    for (int w = 0; w < workers; ++w)
        pool.emplace_back([&] {
            for (size_t i = next++; i < takes.size(); i = next++) {
                results[i] = BakeTake(ctx, takes[i], takes[i].Name == staticFrom);
                const TakeResult& r = results[i];
                std::lock_guard<std::mutex> lock(printLock);
                const int d = ++done;
                if (!r.Ok) std::printf("[MotionBake] %3d/%zu FAILED %s: %s\n", d, takes.size(), takes[i].Name.c_str(), r.Error.c_str());
                else
                    std::printf("[MotionBake] %3d/%zu %-46s %6zu frames  import %5.1f s  bake %4.1f s  speed %.2f  facing p95 %5.1f  jitter %6.1f  rebuild %.2f cm\n",
                                d, takes.size(), takes[i].Name.c_str(), r.Root.size(), r.ImportSeconds, r.BakeSeconds, r.SpeedP50,
                                r.Raw.FacingP95, r.Raw.YawJitter, r.RebuildMaxCm);
                std::fflush(stdout);
            }
        });
    for (std::thread& t : pool) t.join();

    // Assemble.
    MotionClipStream stream;
    stream.Reset(cfg.Fps, cfg.Bones);
    stream.RestYaw = ctx.RestYaw;
    stream.SourcePelvisHeight = cfg.Labels.StandingPelvis;
    MotionFeatureSet features;
    int failed = 0;
    json takesJ = json::array();
    float worstFacing = 0.0f, worstJitter = 0.0f, worstRebuild = 0.0f;
    std::string worstFacingAt, worstJitterAt, worstRebuildAt;
    json overJ = {{"facingP95", json::array()}, {"yawJitter", json::array()}, {"rebuildCm", json::array()}};
    // The relaxed finger pose first: the finger tracks are stored in its order.
    for (const TakeResult& r : results)
        if (r.Ok && r.Take.Name == staticFrom && !r.StaticRot.empty())
            for (size_t s = 0; s < ctx.StaticBones.size(); ++s) stream.Statics.push_back({ctx.StaticBones[s], r.StaticRot[s]});
    RepairStats repaired;
    int rolledBoneFrames[2] = {0, 0}, rolledClipBones = 0;
    int residualGlitches = 0, fingerClips = 0;
    std::uint32_t fingerFrames = 0;
    for (const TakeResult& r : results) {
        if (!r.Ok) {
            ++failed;
            takesJ.push_back({{"name", r.Take.Name}, {"error", r.Error}});
            continue;
        }
        MotionClipStream::Clip c;
        c.Name = r.Take.Name;
        c.Tags = r.Take.TagString();
        c.SourceStart = r.SourceStart;
        const bool fingers = r.HasFingers && r.FingerRot.size() == r.Root.size() * stream.Statics.size();
        stream.AddClip(c, r.Root, r.PelvisT, r.Rot, fingers ? &r.FingerRot : nullptr);
        fingerClips += fingers ? 1 : 0;
        fingerFrames += fingers ? (std::uint32_t)r.Root.size() : 0u;
        repaired.Flips += r.Repair.Flips;
        repaired.FlipsDropped += r.Repair.FlipsDropped;
        repaired.Spikes += r.Repair.Spikes;
        repaired.SpikeFrames += r.Repair.SpikeFrames;
        residualGlitches += r.GlitchFrames;
        json rolledJ = json::object();
        for (size_t i = 0; i < r.Repair.RolledAfter.size(); ++i) {
            const int before = i < r.Repair.RolledBefore.size() ? r.Repair.RolledBefore[i] : 0, after = r.Repair.RolledAfter[i];
            if (before == 0 && after == 0) continue;
            rolledJ[r.NodeNames[i]] = {before, after};
            rolledBoneFrames[0] += before;
            rolledBoneFrames[1] += after;
            if (after > 0) ++rolledClipBones;
            std::printf("[MotionBake]   %s %s: twist over %.0f deg off the bind on %d frames as captured, %d repaired (of %zu)\n",
                        r.Take.Name.c_str(), r.NodeNames[i].c_str(), kRolledDeg, before, after, r.Root.size());
        }
        const std::uint32_t before = features.FrameCount();
        features.AddClip(r.Take.Name, r.Frames, r.Track, cfg.Labels, MotionClipIntent::FromTags(c.Tags));
        int gaits[4] = {0, 0, 0, 0}, crouch = 0, strafe = 0, air = 0, excluded = 0;
        int styles[MotionLabel::StyleCount] = {};
        for (std::uint32_t f = before; f < features.FrameCount(); ++f) {
            ++styles[std::min<int>(MotionLabel::StyleCount - 1, features.Style(f))];
            ++gaits[std::min<int>(3, features.Gait(f))];
            const std::uint8_t fl = features.Flags(f);
            crouch += fl & MotionLabel::Crouch ? 1 : 0;
            strafe += fl & MotionLabel::Strafe ? 1 : 0;
            air += fl & MotionLabel::Airborne ? 1 : 0;
            excluded += fl & MotionLabel::Excluded ? 1 : 0;
        }
        const bool forward = r.Take.HasTag("forward");
        if (forward && r.Raw.FacingP95 > worstFacing) { worstFacing = r.Raw.FacingP95; worstFacingAt = r.Take.Name; }
        if (r.Raw.YawJitter > worstJitter) { worstJitter = r.Raw.YawJitter; worstJitterAt = r.Take.Name; }
        if (r.RebuildMaxCm > worstRebuild) { worstRebuild = r.RebuildMaxCm; worstRebuildAt = r.Take.Name; }
        // The validation limits (the .mmconfig's "validation"): a take over one is flagged, not failed - the numbers are the
        // root's and the rebuild's quality, for a look at the take, not a broken bake.
        if (forward && r.Raw.FacingP95 > cfg.FacingP95Deg) overJ["facingP95"].push_back(r.Take.Name);
        if (r.Raw.YawJitter > cfg.YawJitterMax) overJ["yawJitter"].push_back(r.Take.Name);
        if (r.RebuildMaxCm > cfg.ReconstructCm) overJ["rebuildCm"].push_back(r.Take.Name);
        takesJ.push_back({{"name", r.Take.Name}, {"tags", r.Take.TagString()}, {"frames", r.Root.size()},
                          {"sourceStart", r.SourceStart}, {"takeSeconds", r.TakeSeconds}, {"importSeconds", r.ImportSeconds},
                          {"speedP50", r.SpeedP50}, {"speedMax", r.Raw.SpeedMax}, {"stepMaxM", r.Raw.StepMax},
                          {"pelvisP50", r.PelvisP50}, {"ankleP05", r.AnkleP05},
                          {"yawJitter", r.Raw.YawJitter}, {"pathWobbleCm", r.Raw.PathWobble}, {"yawRateMax", r.Raw.YawRateMax},
                          {"facingP95", r.Raw.FacingP95}, {"facingMax", r.Raw.FacingMax}, {"forward", forward},
                          {"footSlideCmS", {r.Raw.FootSlide[0], r.Raw.FootSlide[1]}},
                          {"rebuiltFootSlideCmS", {r.Rebuilt.FootSlide[0], r.Rebuilt.FootSlide[1]}},
                          {"contactFrames", {r.Raw.ContactFrames[0], r.Raw.ContactFrames[1]}},
                          {"rebuildMaxCm", r.RebuildMaxCm},
                          {"gaitFrames", {gaits[0], gaits[1], gaits[2], gaits[3]}}, {"crouchFrames", crouch},
                          {"strafeFrames", strafe}, {"airborneFrames", air}, {"excludedFrames", excluded},
                          {"fingerBones", r.FingerBones}, {"fingerBonesMoving", r.FingerBonesMoving}, {"fingerRangeDeg", r.FingerRangeDeg},
                          {"hasFingers", fingers}, {"source", r.Take.File()}, {"mirrored", r.Take.Mirrored()},
                          {"styleFrames", {styles[0], styles[1], styles[2], styles[3], styles[4], styles[5], styles[6]}},
                          {"twistFlipsUnrolled", r.Repair.Flips - r.Repair.FlipsDropped}, {"spikesRepaired", r.Repair.Spikes},
                          {"spikeFramesRepaired", r.Repair.SpikeFrames}, {"glitchFramesLeft", r.GlitchFrames},
                          {"rolledFrames", rolledJ}});
        if (r.Take.Mirrored())
            takesJ.back()["mirrorNormal"] = {r.MirrorNormal.x, r.MirrorNormal.y, r.MirrorNormal.z};
    }
    features.Finish();
    report["takes"] = takesJ;
    // The search's normalization: the config's frozen one (stored in the .mmdb), reported against what these takes would
    // derive - a fresh derivation moves every cost whenever any take changes.
    std::vector<float> normMean, normScale;
    {
        std::vector<float> freshMean, freshScale;
        MotionDatabase::DeriveNormalization(features, freshMean, freshScale);
        const bool frozen = cfg.HasFeatureNorm();
        normMean = frozen ? cfg.FeatureNormMean : freshMean;
        normScale = frozen ? cfg.FeatureNormScale : freshScale;
        json nj = {{"frozen", frozen}};
        if (frozen) std::printf("[MotionBake] feature normalization FROZEN (config featureNorm), per group frozen scale / fresh "
                                "derivation (ratio), worst mean shift in frozen scales:\n");
        for (int gi = 0; gi < MotionFeature::GroupCount; ++gi) {
            const int a = MotionFeature::kGroupFirst[gi], b = MotionFeature::kGroupFirst[gi + 1];
            float shift = 0.0f;
            for (int d = a; d < b; ++d) shift = std::max(shift, std::fabs(freshMean[(size_t)d] - normMean[(size_t)d]) / normScale[(size_t)d]);
            const float fz = normScale[(size_t)a], fr = freshScale[(size_t)a];
            nj["groups"][MotionFeature::GroupName(gi)] = {{"frozenScale", fz}, {"freshScale", fr}, {"meanShift", shift}};
            if (frozen)
                std::printf("[MotionBake]   %-9s %.6g / %.6g (%.4f), mean shift %.4f\n", MotionFeature::GroupName(gi), fz, fr,
                            fr / fz, shift);
        }
        if (!frozen)
            std::printf("[MotionBake] WARNING: no featureNorm in %s: this bake's own normalization is stored, so every cost moves "
                        "with any take. Freeze it by adding to the config:\n  %s\n", cfg.Path.c_str(),
                        FeatureNormJson(freshMean, freshScale).c_str());
        report["featureNorm"] = nj;
        // Stored either way: without a config block this is the bake's own derivation (what an unfrozen load would derive
        // anyway), so "stored" in the .mmdb means frozen only when the report says "frozen": true.
        features.SetNormalization(normMean, normScale);
    }
    // The 16-bit features' worst error per group, in the group's normalization units (MotionDatabase's scale).
    {
        json qj = json::object();
        std::printf("[MotionBake] feature quantization, worst error per group (normalized):");
        for (int gi = 0; gi < MotionFeature::GroupCount; ++gi) {
            const int a = MotionFeature::kGroupFirst[gi], b = MotionFeature::kGroupFirst[gi + 1];
            const float scale = normScale[(size_t)a];
            float worst = 0.0f;
            for (int d = a; d < b; ++d) worst = std::max(worst, features.QError()[(size_t)d] / scale);
            qj[MotionFeature::GroupName(gi)] = worst;
            std::printf(" %s %.2e", MotionFeature::GroupName(gi), worst);
        }
        std::printf("\n");
        report["featureQuantization"] = qj;
    }
    std::printf("[MotionBake] repair: %d twist flips unrolled (%d more left as captured: unrolled, they rolled the bone off the bind), "
                "%d spikes slerped across (%d bone-frames); %d frames still over %.0f deg (excluded at load)\n", repaired.Flips - repaired.FlipsDropped,
                repaired.FlipsDropped, repaired.Spikes, repaired.SpikeFrames, residualGlitches, MotionDatabase::kGlitchDegrees);
    std::printf("[MotionBake] fingers: %d clips animate them (%u frames, %s)\n", fingerClips, fingerFrames, Mb(stream.FingerBytes()).c_str());
    std::printf("[MotionBake] rolled: %d bone-frames twisted over %.0f deg off the bind as captured, %d after the repair (%d clip-bones)\n",
                rolledBoneFrames[0], kRolledDeg, rolledBoneFrames[1], rolledClipBones);
    report["rolledBoneFrames"] = {rolledBoneFrames[0], rolledBoneFrames[1]};
    report["twistFlipsUnrolled"] = repaired.Flips - repaired.FlipsDropped;
    report["twistFlipsLeft"] = repaired.FlipsDropped;
    report["spikesRepaired"] = repaired.Spikes;
    report["glitchFramesLeft"] = residualGlitches;
    report["fingerClips"] = fingerClips;
    report["fingerMB"] = (double)stream.FingerBytes() / (1024.0 * 1024.0);

    // Write, then time a load of both files the way the runtime will.
    fs::create_directories(fs::path(cfg.Output).parent_path(), ec);
    const std::string animPath = cfg.Output + ".mmanim", dbPath = cfg.Output + ".mmdb";
    if (!stream.Save(animPath, &error) || !features.Save(dbPath, &error)) {
        std::cerr << "[MotionBake] ERROR: " << error << std::endl;
        return 1;
    }
    MotionDatabase check; // (the runtime's whole load: both files, then Build - the glitch scan, the boxes)
    const bool loaded = check.Load(cfg.Output, &error);
    const double loadSeconds = check.LoadSeconds();
    if (!loaded) {
        std::cerr << "[MotionBake] ERROR: reading back: " << error << std::endl;
        return 1;
    }

    const double poseMB = (double)stream.PoseBytes() / (1024.0 * 1024.0), featMB = (double)features.Bytes() / (1024.0 * 1024.0);
    const double hours = (double)stream.FrameCount() / cfg.Fps / 3600.0;
    const bool poseOk = poseMB <= cfg.BudgetPoseMB, featOk = featMB <= cfg.BudgetFeatureMB,
               loadOk = loadSeconds <= cfg.BudgetLoadSeconds;
    std::printf("[MotionBake] %zu clips, %u frames (%.2f h), %zu bones streamed, %zu static\n", stream.Clips().size(),
                stream.FrameCount(), hours, cfg.Bones.size(), stream.Statics.size());
    std::printf("[MotionBake] budget: poses %.1f / %.0f MB %s, features %.1f / %.0f MB %s, load %.2f / %.1f s %s\n", poseMB,
                cfg.BudgetPoseMB, poseOk ? "OK" : "OVER", featMB, cfg.BudgetFeatureMB, featOk ? "OK" : "OVER", loadSeconds,
                cfg.BudgetLoadSeconds, loadOk ? "OK" : "OVER");
    std::printf("[MotionBake] worst: forward facing p95 %.1f deg (%s), yaw jitter %.1f (%s), foot rebuild %.2f cm (%s)\n",
                worstFacing, worstFacingAt.c_str(), worstJitter, worstJitterAt.c_str(), worstRebuild, worstRebuildAt.c_str());
    std::printf("[MotionBake] validation (warnings): %zu forward takes over facing p95 %.1f deg, %zu over yaw jitter %.0f, "
                "%zu over foot rebuild %.2f cm (the report's \"overValidation\" names them)\n",
                overJ["facingP95"].size(), cfg.FacingP95Deg, overJ["yawJitter"].size(), cfg.YawJitterMax, overJ["rebuildCm"].size(),
                cfg.ReconstructCm);
    report["overValidation"] = overJ;
    std::printf("[MotionBake] wrote %s (%s) and %s (%s) in %.0f s\n", animPath.c_str(), Mb((size_t)fs::file_size(animPath, ec)).c_str(),
                dbPath.c_str(), Mb((size_t)fs::file_size(dbPath, ec)).c_str(), Now() - start);
    report["config"] = cfg.Path;
    report["clips"] = stream.Clips().size();
    report["frames"] = stream.FrameCount();
    report["hours"] = hours;
    report["poseMB"] = poseMB;
    report["featureMB"] = featMB;
    report["loadSeconds"] = loadSeconds;
    report["failed"] = failed;
    report["seconds"] = Now() - start;
    const fs::path reportDir = fs::path(ProjectPaths::Root()).parent_path() / "work" / "mm_bake";
    fs::create_directories(reportDir, ec);
    const fs::path reportPath = reportDir / (fs::path(cfg.Output).filename().string() + ".report.json");
    std::ofstream(reportPath) << report.dump(1);
    std::printf("[MotionBake] report %s\n", reportPath.string().c_str());
    const bool ok = failed == 0 && poseOk && featOk && loadOk;
    std::printf("[MotionBake] %s\n", ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok ? 0 : 1;
}

} // namespace MotionBake
