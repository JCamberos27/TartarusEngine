// Does the left hand animate sanely, and is "strange" an artifact of the data or of the engine?
//
// Replicates Model::EvaluatePose exactly (base node tree + clip channels matched by name,
// local = channel sample falling back per-component to bind, globals composed parents-first)
// and, for each clip, reports per-bone MOTION through the whole clip:
//   * which skinned bones the clip leaves un-driven (they freeze at the base's authored pose)
//   * left-vs-right path length / per-frame rotation / per-frame translation
//   * the loop seam (pose at end vs pose at 0 - a pop every loop if it is large)
//   * channel key counts (a channel with 0 or 1 rotation key cannot animate)
// A left hand that is frozen, that pops at the loop seam, or that swings far harder than the
// right will show up here as a number rather than a guess.
//
//   hand_probe <armsBase> <armsClip.fbx> [more clips...]
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/config.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

static glm::mat4 AiToGlm(const aiMatrix4x4& m) {
    glm::mat4 t;
    t[0][0] = m.a1; t[1][0] = m.a2; t[2][0] = m.a3; t[3][0] = m.a4;
    t[0][1] = m.b1; t[1][1] = m.b2; t[2][1] = m.b3; t[3][1] = m.b4;
    t[0][2] = m.c1; t[1][2] = m.c2; t[2][2] = m.c3; t[3][2] = m.c4;
    t[0][3] = m.d1; t[1][3] = m.d2; t[2][3] = m.d3; t[3][3] = m.d4;
    return t;
}

struct Node { std::string name; int parent = -1; glm::mat4 bind{1.0f}; };

static void ReadNodes(const aiNode* n, int parent, std::vector<Node>& out) {
    Node nd;
    nd.name = n->mName.C_Str();
    nd.parent = parent;
    nd.bind = AiToGlm(n->mTransformation);
    const int self = (int)out.size();
    out.push_back(nd);
    for (unsigned i = 0; i < n->mNumChildren; ++i) ReadNodes(n->mChildren[i], self, out);
}

struct KeyF { float t; float v; };
struct KeyQ { float t; glm::quat v; };
struct Ch { std::vector<KeyF> px, py, pz; std::vector<KeyQ> rq; };

static float SampleF(const std::vector<KeyF>& k, float tick, float fallback) {
    if (k.empty()) return fallback;
    if (tick <= k.front().t) return k.front().v;
    if (tick >= k.back().t) return k.back().v;
    for (size_t i = 1; i < k.size(); ++i) {
        if (tick <= k[i].t) {
            const float a = k[i - 1].t, b = k[i].t;
            const float w = (b > a) ? (tick - a) / (b - a) : 0.0f;
            return k[i - 1].v + (k[i].v - k[i - 1].v) * w;
        }
    }
    return k.back().v;
}

static glm::quat SampleQ(const std::vector<KeyQ>& k, float tick, const glm::quat& fallback) {
    if (k.empty()) return fallback;
    if (tick <= k.front().t) return k.front().v;
    if (tick >= k.back().t) return k.back().v;
    for (size_t i = 1; i < k.size(); ++i) {
        if (tick <= k[i].t) {
            const float a = k[i - 1].t, b = k[i].t;
            const float w = (b > a) ? (tick - a) / (b - a) : 0.0f;
            return glm::normalize(glm::slerp(k[i - 1].v, k[i].v, w));
        }
    }
    return k.back().v;
}

struct Loaded {
    std::vector<Node> nodes;
    std::map<std::string, Ch> channels;
    std::set<std::string> skinned;
    float duration = 0.0f;
    float tps = 60.0f;
    bool ok = false;
};

static bool LoadRig(const char* path, Loaded& out) {
    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* sc = imp.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs |
                                               aiProcess_GlobalScale | aiProcess_GenSmoothNormals |
                                               aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights |
                                               aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes);
    if (!sc || !sc->mRootNode) { printf("  load failed %s: %s\n", path, imp.GetErrorString()); return false; }
    ReadNodes(sc->mRootNode, -1, out.nodes);
    for (unsigned m = 0; m < sc->mNumMeshes; ++m)
        for (unsigned b = 0; b < sc->mMeshes[m]->mNumBones; ++b)
            out.skinned.insert(sc->mMeshes[m]->mBones[b]->mName.C_Str());
    for (unsigned a = 0; a < sc->mNumAnimations; ++a) {
        const aiAnimation* an = sc->mAnimations[a];
        if (an->mTicksPerSecond > 0.0f) out.tps = (float)an->mTicksPerSecond;
        out.duration = std::max(out.duration, (float)an->mDuration);
        for (unsigned c = 0; c < an->mNumChannels; ++c) {
            const aiNodeAnim* ch = an->mChannels[c];
            Ch& e = out.channels[ch->mNodeName.C_Str()];
            for (unsigned k = 0; k < ch->mNumPositionKeys; ++k) {
                const float t = (float)ch->mPositionKeys[k].mTime;
                const aiVector3D& v = ch->mPositionKeys[k].mValue;
                e.px.push_back({t, v.x}); e.py.push_back({t, v.y}); e.pz.push_back({t, v.z});
            }
            for (unsigned k = 0; k < ch->mNumRotationKeys; ++k) {
                const aiQuaternion& q = ch->mRotationKeys[k].mValue;
                e.rq.push_back({(float)ch->mRotationKeys[k].mTime, glm::quat(q.w, q.x, q.y, q.z)});
            }
        }
    }
    out.ok = true;
    return true;
}

// Model::EvaluatePose: local = channel sample (falling back per-component to bind) or bind-local.
static std::vector<glm::mat4> Evaluate(const Loaded& rig, float tick) {
    std::vector<glm::mat4> g(rig.nodes.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < rig.nodes.size(); ++i) {
        const Node& n = rig.nodes[i];
        glm::mat4 local = n.bind;
        const auto it = rig.channels.find(n.name);
        if (it != rig.channels.end()) {
            const Ch& c = it->second;
            glm::vec3 bs(1.0f); glm::quat br(1.0f, 0, 0, 0); glm::vec3 bp(0.0f);
            glm::vec3 skw; glm::vec4 prp;
            glm::decompose(n.bind, bs, br, bp, skw, prp);
            const glm::vec3 p(SampleF(c.px, tick, bp.x), SampleF(c.py, tick, bp.y), SampleF(c.pz, tick, bp.z));
            const glm::quat r = SampleQ(c.rq, tick, br);
            local = glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), bs);
        }
        g[i] = (n.parent >= 0) ? g[n.parent] * local : local;
    }
    return g;
}

static int Find(const std::vector<Node>& nodes, const std::string& name) {
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i].name == name) return (int)i;
    return -1;
}

static float RotDeg(const glm::mat4& a, const glm::mat4& b) {
    const glm::mat3 Ra(a), Rb(b);
    float tr = 0.0f;
    for (int c = 0; c < 3; ++c) tr += glm::dot(Ra[c], Rb[c]);
    const float c2 = std::max(-1.0f, std::min(1.0f, (tr - 1.0f) * 0.5f));
    return (float)(std::acos(c2) * 180.0 / 3.14159265358979323846);
}

static std::string Side(const std::string& n) {
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "_l") == 0) return "L";
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "_r") == 0) return "R";
    return "-";
}

struct Stat {
    std::string name;
    int rotKeys = 0, posKeys = 0;
    float path = 0.0f;          // total world path length over the clip (m)
    float maxStepRot = 0.0f;    // largest world rotation change between adjacent samples (deg)
    float maxStepPos = 0.0f;    // largest world translation change between adjacent samples (mm)
    float seamRot = 0.0f;       // pose at end vs pose at 0 (deg)
    float seamPos = 0.0f;       // ... (mm)
    bool driven = false;
    float frozenParentStep = 0.0f; // parent's own per-step rotation while this one is static
};

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: hand_probe <armsBase> <armsClip.fbx> [more clips...]\n"); return 1; }
    Loaded base;
    if (!LoadRig(argv[1], base)) return 1;
    printf("=== base %s\n    nodes=%zu skinned=%zu\n", argv[1], base.nodes.size(), base.skinned.size());

    const int N = 240; // uniform samples across the clip
    const char* focus[] = {"clavicle_l", "clavicle_r", "upperarm_l", "upperarm_r",
                           "lowerarm_l", "lowerarm_r", "hand_l", "hand_r",
                           "ik_hand_l", "ik_hand_r", "ik_hand_gun",
                           "index_01_l", "index_01_r", "middle_01_l", "pinky_01_l"};

    for (int arg = 2; arg < argc; ++arg) {
        Loaded clip;
        if (!LoadRig(argv[arg], clip)) continue;
        Loaded rig = base;
        rig.channels = clip.channels;

        printf("\n---------------- clip %s\n", argv[arg]);
        printf("  duration=%.0f ticks  tps=%.0f (%.2f s)\n", clip.duration, clip.tps,
               clip.tps > 0 ? clip.duration / clip.tps : 0.0f);
        printf("  channels=%zu  driven skinned bones=%zu\n", clip.channels.size(),
               [&] { size_t n = 0; for (const auto& s : base.skinned) n += clip.channels.count(s) ? 1 : 0; return n; }());

        // Skinned bones the clip does NOT drive: they freeze at the base's authored pose.
        std::vector<std::string> frozen;
        for (const auto& s : base.skinned) if (!clip.channels.count(s)) frozen.push_back(s);
        printf("  skinned but UNDRIVEN (%zu):", frozen.size());
        for (size_t i = 0; i < frozen.size() && i < 40; ++i) printf(" %s", frozen[i].c_str());
        printf("%s\n", frozen.size() > 40 ? " ..." : "");

        std::vector<Stat> stats;
        std::vector<glm::mat4> prev;
        for (size_t i = 0; i < base.nodes.size(); ++i) {
            Stat s;
            s.name = base.nodes[i].name;
            const auto it = clip.channels.find(s.name);
            if (it != clip.channels.end()) {
                s.driven = true;
                s.rotKeys = (int)it->second.rq.size();
                s.posKeys = (int)it->second.px.size();
            }
            stats.push_back(s);
        }

        const std::vector<glm::mat4> firstPose = Evaluate(rig, 0.0f);
        prev = firstPose;
        for (int f = 1; f <= N; ++f) {
            const float tick = clip.duration * (float)f / (float)N;
            const std::vector<glm::mat4> g = Evaluate(rig, tick);
            for (size_t i = 0; i < base.nodes.size(); ++i) {
                Stat& s = stats[i];
                const float dr = RotDeg(prev[i], g[i]);
                const float dp = glm::length(glm::vec3(g[i][3]) - glm::vec3(prev[i][3]));
                s.path += dp;
                s.maxStepRot = std::max(s.maxStepRot, dr);
                s.maxStepPos = std::max(s.maxStepPos, dp * 1000.0f);
            }
            prev = g;
        }
        const std::vector<glm::mat4> lastPose = prev;
        for (size_t i = 0; i < base.nodes.size(); ++i) {
            stats[i].seamRot = RotDeg(firstPose[i], lastPose[i]);
            stats[i].seamPos = glm::length(glm::vec3(lastPose[i][3]) - glm::vec3(firstPose[i][3])) * 1000.0f;
        }

        printf("\n  %-20s %-6s %-5s %-5s %8s %9s %9s %9s %9s\n", "bone", "side", "rotK", "posK",
               "path(mm)", "maxStepR", "maxStepP", "seamRot", "seamPos");
        for (const char* n : focus) {
            const int i = Find(base.nodes, n);
            if (i < 0) continue;
            const Stat& s = stats[i];
            printf("  %-20s %-6s %-5d %-5d %8.1f %9.2f %9.2f %9.2f %9.2f%s\n",
                   s.name.c_str(), Side(s.name).c_str(), s.rotKeys, s.posKeys, s.path * 1000.0f,
                   s.maxStepRot, s.maxStepPos, s.seamRot, s.seamPos, s.driven ? "" : "   <FROZEN");
        }

        printf("\n  motionless skinned bones whose PARENT still moves (frozen child = broken pose):\n");
        int reported = 0;
        for (size_t i = 0; i < base.nodes.size() && reported < 20; ++i) {
            const Stat& s = stats[i];
            if (s.driven || !base.skinned.count(s.name)) continue;
            const int p = base.nodes[i].parent;
            if (p < 0) continue;
            if (stats[p].maxStepRot > 0.5f && s.maxStepRot < 1e-6f) {
                printf("    %-24s (step 0.00)  parent %-24s step %.2f deg\n",
                       s.name.c_str(), stats[p].name.c_str(), stats[p].maxStepRot);
                ++reported;
            }
        }
        if (!reported) printf("    none\n");

        printf("\n  worst movers by max per-sample world rotation:\n");
        std::vector<std::pair<float, int>> rank;
        for (size_t i = 0; i < stats.size(); ++i) rank.push_back({stats[i].maxStepRot, (int)i});
        std::sort(rank.rbegin(), rank.rend());
        for (int i = 0; i < 12 && i < (int)rank.size(); ++i) {
            const Stat& s = stats[rank[i].second];
            printf("    %-26s %8.2f deg/step   path %7.1f mm   seam %6.2f deg\n",
                   s.name.c_str(), s.maxStepRot, s.path * 1000.0f, s.seamRot);
        }

        printf("\n  left-vs-right aggregate over %d samples:\n", N);
        double lp = 0, rp = 0, ls = 0, rs = 0; int ln = 0, rn = 0;
        for (const Stat& s : stats) {
            if (!s.driven) continue;
            if (Side(s.name) == "L") { lp += s.path; ls += s.maxStepRot; ++ln; }
            if (Side(s.name) == "R") { rp += s.path; rs += s.maxStepRot; ++rn; }
        }
        printf("    L: %2d bones  total path %8.1f mm  mean maxStep %6.2f deg\n", ln, lp * 1000.0, ln ? ls / ln : 0.0);
        printf("    R: %2d bones  total path %8.1f mm  mean maxStep %6.2f deg\n", rn, rp * 1000.0, rn ? rs / rn : 0.0);
        printf("    R/L path ratio = %.2f  (1.0 = symmetric)\n", lp > 1e-9 ? rp / lp : 0.0);
    }
    return 0;
}
