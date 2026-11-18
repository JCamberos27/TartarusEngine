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

std::string SourceDir(const MotionBakeConfig& cfg) {
    (void)cfg;
    const std::string dir = Env::Str(Env::Var::MmSource);
    std::error_code ec;
    return !dir.empty() && fs::is_directory(dir, ec) ? dir : std::string();
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
        if (t.Source == t.Name) t.Source.clear();
        t.Group = col("group");
        t.Pattern = col("pattern");
        t.Seconds = (float)std::atof(col("seconds").c_str());
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
    // 1. Twist flips. Each bone's world rotation is untouched by its ancestors' unrolling (the children take the inverse), so
    // every bone is judged on its captured world rotation and its own correction so far.
    const std::vector<int> order = TopoOrder(parents);
    const float flip = kFlipDeg * MotionMath::kPi / 180.0f;
    std::vector<glm::quat> prevG(n), curG(n);
    std::vector<float> corr(n, 0.0f);
    std::vector<std::vector<std::pair<size_t, float>>> apply(frames); // per frame: (bone, correction) where non-zero
    for (size_t f = 0; f < frames; ++f) {
        for (int i : order) {
            const int p = parents[(size_t)i];
            curG[(size_t)i] = p >= 0 ? curG[(size_t)p] * poses[f][(size_t)i].R : poses[f][(size_t)i].R;
        }
        for (size_t i = 0; i < n && f > 0; ++i) {
            if (!hasAxis[i]) continue;
            const glm::quat c = glm::angleAxis(corr[i], axis[i]);
            const float tw = MotionMath::TwistAngle(glm::inverse(prevG[i] * c) * (curG[i] * c), axis[i]);
            if (std::fabs(tw) > flip) {
                corr[i] -= tw;
                ++st.Flips;
            }
        }
        for (size_t i = 0; i < n; ++i)
            if (hasAxis[i] && std::fabs(corr[i]) > 1e-6f) apply[f].push_back({i, corr[i]});
        std::swap(prevG, curG);
    }
    for (size_t f = 0; f < frames; ++f)
        for (const auto& [i, c] : apply[f]) {
            const glm::quat t = glm::angleAxis(c, axis[i]);
            poses[f][i].R = glm::normalize(poses[f][i].R * t);
            for (int ch : children[i]) poses[f][(size_t)ch].R = glm::normalize(glm::inverse(t) * poses[f][(size_t)ch].R);
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
    std::vector<glm::vec3> HipsRight;
    RootValidation Raw;          // the generated root, the source's own feet
    RootValidation Rebuilt;      // the feet rebuilt from the quantized stream on the target rig, placed by the root
    float RebuildMaxCm = 0.0f;   // worst distance between a source foot and its rebuild
    float SpeedP50 = 0.0f, PelvisP50 = 0.0f, AnkleP05 = 0.0f;
    int Labelled[4] = {0, 0, 0, 0};
    std::vector<glm::quat> StaticRot;
    std::vector<glm::quat> FingerRot; // frames x StaticBones, when the take animates its fingers (HasFingers)
    bool HasFingers = false;
    RepairStats Repair;
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
    const std::string path = (fs::path(ctx.AnimsDir) / (take.File() + ".fbx")).string();
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
    const float len = m.AnimationLength(0);
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
    for (int f = 0; f < ne; ++f) m.SampleLocalPose(0, std::min(len, (float)(extFirst + f) / fps), AnimationWrapMode::ClampForever, poses[(size_t)f]);
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
        if (take.Mirrored()) {
            const MirrorRig mr = BuildMirror(names, parents, bind, root);
            r.MirrorNormal = mr.Normal;
            for (auto& p : poses) MirrorPose(mr, p);
        }
    }
    std::vector<glm::mat4> g;
    for (int f = 0; f < ne; ++f) {
        const std::vector<LocalTRS>& pose = poses[(size_t)f];
        RootSpaceGlobals(m, pose, root, g);
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
    r.HipsRight = in.HipsRight;
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
    const std::string source = SourceDir(cfg);
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
    // The relaxed finger pose first: the finger tracks are stored in its order.
    for (const TakeResult& r : results)
        if (r.Ok && r.Take.Name == staticFrom && !r.StaticRot.empty())
            for (size_t s = 0; s < ctx.StaticBones.size(); ++s) stream.Statics.push_back({ctx.StaticBones[s], r.StaticRot[s]});
    RepairStats repaired;
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
        repaired.Spikes += r.Repair.Spikes;
        repaired.SpikeFrames += r.Repair.SpikeFrames;
        residualGlitches += r.GlitchFrames;
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
                          {"styleFrames", {styles[0], styles[1], styles[2], styles[3], styles[4], styles[5]}},
                          {"twistFlipsUnrolled", r.Repair.Flips}, {"spikesRepaired", r.Repair.Spikes},
                          {"spikeFramesRepaired", r.Repair.SpikeFrames}, {"glitchFramesLeft", r.GlitchFrames}});
        if (r.Take.Mirrored())
            takesJ.back()["mirrorNormal"] = {r.MirrorNormal.x, r.MirrorNormal.y, r.MirrorNormal.z};
    }
    features.Finish();
    report["takes"] = takesJ;
    // The 16-bit features' worst error per group, in the group's normalization units (MotionDatabase's scale).
    {
        json qj = json::object();
        std::printf("[MotionBake] feature quantization, worst error per group (normalized):");
        for (int gi = 0; gi < MotionFeature::GroupCount; ++gi) {
            const int a = MotionFeature::kGroupFirst[gi], b = MotionFeature::kGroupFirst[gi + 1];
            float scale = 0.0f, worst = 0.0f;
            for (int d = a; d < b; ++d) scale += features.Std()[(size_t)d];
            scale = std::max(1e-4f, scale / (float)(b - a));
            for (int d = a; d < b; ++d) worst = std::max(worst, features.QError()[(size_t)d] / scale);
            qj[MotionFeature::GroupName(gi)] = worst;
            std::printf(" %s %.2e", MotionFeature::GroupName(gi), worst);
        }
        std::printf("\n");
        report["featureQuantization"] = qj;
    }
    std::printf("[MotionBake] repair: %d twist flips unrolled, %d spikes slerped across (%d bone-frames); %d frames still over %.0f deg "
                "(excluded at load)\n", repaired.Flips, repaired.Spikes, repaired.SpikeFrames, residualGlitches, MotionDatabase::kGlitchDegrees);
    std::printf("[MotionBake] fingers: %d clips animate them (%u frames, %s)\n", fingerClips, fingerFrames, Mb(stream.FingerBytes()).c_str());
    report["twistFlipsUnrolled"] = repaired.Flips;
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
    const double l0 = Now();
    MotionClipStream check;
    MotionFeatureSet checkDb;
    const bool loaded = check.Load(animPath, &error) && checkDb.Load(dbPath, &error);
    const double loadSeconds = Now() - l0;
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
