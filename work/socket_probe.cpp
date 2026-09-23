// Does the weapon track the arms' gun socket?
//
// Replicates Model::EvaluatePose exactly (base node tree + clip channels matched by name,
// local = channel sample when present else bind-local, globals composed parents-first) and
// reports the root-space translation of the arms' `ik_hand_gun` / `hand_r` against the
// weapon's `root`, over a clip. If ik_hand_gun's delta from bind is non-trivial while the
// weapon's root sits still, the gun cannot follow the hands and will visibly float.
//
//   socket_probe <armsBase> <armsClip|-> <weaponBase> <weaponClip|->
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/config.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
// Engine's own rotation helpers, so the mount we measure is expressed in exactly the
// representation FirstPersonPresentation will consume (Y-X-Z Euler degrees).
#include "../src/Game/RotationMath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
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
    float duration = 0.0f;
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
    for (unsigned a = 0; a < sc->mNumAnimations; ++a) {
        const aiAnimation* an = sc->mAnimations[a];
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
            glm::vec3 s(1.0f); glm::quat r(1.0f, 0, 0, 0); glm::vec3 p(0.0f);
            glm::vec3 bs(1.0f); glm::quat br(1.0f, 0, 0, 0); glm::vec3 bp(0.0f);
            glm::vec3 skw; glm::vec4 prp;
            glm::decompose(n.bind, bs, br, bp, skw, prp);
            p = glm::vec3(SampleF(c.px, tick, bp.x), SampleF(c.py, tick, bp.y), SampleF(c.pz, tick, bp.z));
            r = SampleQ(c.rq, tick, br);
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

static void Vec(const char* label, const glm::mat4& m) {
    printf("%-22s (%7.4f, %7.4f, %7.4f)", label, m[3][0], m[3][1], m[3][2]);
}

int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: socket_probe <armsBase> <armsClip|-> <weaponBase> <weaponClip|->\n"); return 1; }
    Loaded armsBase, armsClip, wBase, wClip;
    if (!LoadRig(argv[1], armsBase)) return 1;
    if (std::string(argv[2]) != "-" && !LoadRig(argv[2], armsClip)) return 1;
    if (!LoadRig(argv[3], wBase)) return 1;
    if (std::string(argv[4]) != "-" && !LoadRig(argv[4], wClip)) return 1;

    printf("=== %s  +  %s\n     %s  +  %s\n", argv[1], argv[2], argv[3], argv[4]);
    printf("arms base nodes=%zu  clip channels=%zu (dur %.1f)\n",
           armsBase.nodes.size(), armsClip.channels.size(), armsClip.duration);
    printf("weapon base nodes=%zu  clip channels=%zu (dur %.1f)\n",
           wBase.nodes.size(), wClip.channels.size(), wClip.duration);

    for (const char* n : {"ik_hand_gun", "ik_hand_root", "hand_r", "root", "spine_03"})
        printf("  arms base has %-14s = %s | clip drives it = %s\n", n,
               Find(armsBase.nodes, n) >= 0 ? "Y" : "N",
               armsClip.channels.count(n) ? "Y" : "N");
    for (const char* n : {"root", "bolt", "trigger"})
        printf("  weapon base has %-13s = %s | clip drives it = %s\n", n,
               Find(wBase.nodes, n) >= 0 ? "Y" : "N",
               wClip.channels.count(n) ? "Y" : "N");

    const int igA = Find(armsBase.nodes, "ik_hand_gun");
    const int hrA = Find(armsBase.nodes, "hand_r");
    const int rtW = Find(wBase.nodes, "root");

    const float dur = armsClip.ok ? armsClip.duration : 0.0f;
    const float ticks[] = {0.0f, dur * 0.25f, dur * 0.5f, dur * 0.75f, dur};
    glm::mat4 igBind(1.0f), hrBind(1.0f), rtBind(1.0f);

    // Evaluate: arms = armsBase tree + armsClip channels; weapon = wBase tree + wClip channels.
    auto runArms = [&](float t) {
        Loaded tmp = armsBase;
        if (armsClip.ok) { tmp.channels = armsClip.channels; tmp.duration = armsClip.duration; }
        return Evaluate(tmp, t);
    };
    auto runW = [&](float t) {
        Loaded tmp = wBase;
        if (wClip.ok) { tmp.channels = wClip.channels; tmp.duration = wClip.duration; }
        return Evaluate(tmp, t);
    };

    printf("\n%-6s | %-22s %-22s | %-22s | %s\n", "tick",
           "ik_hand_gun (arms)", "hand_r (arms)", "weapon root",
           "socket-wroot: t/deg | grip(ig,hand_r): t/deg");

    // rotation difference between two globals, in degrees
    auto rotDeg = [](const glm::mat4& a, const glm::mat4& b) {
        const glm::mat3 Ra(a), Rb(b);
        float tr = 0.0f;
        for (int c = 0; c < 3; ++c) tr += glm::dot(Ra[c], Rb[c]);
        const float c2 = std::max(-1.0f, std::min(1.0f, (tr - 1.0f) * 0.5f));
        return (float)(std::acos(c2) * 180.0 / 3.14159265358979323846);
    };

    for (int i = 0; i < 5; ++i) {
        const float t = ticks[i];
        const std::vector<glm::mat4> ga = runArms(t);
        const std::vector<glm::mat4> gw = runW(t);
        const glm::mat4 ig = (igA >= 0) ? ga[igA] : glm::mat4(1.0f);
        const glm::mat4 hr = (hrA >= 0) ? ga[hrA] : glm::mat4(1.0f);
        const glm::mat4 rt = (rtW >= 0) ? gw[rtW] : glm::mat4(1.0f);
        if (i == 0) { igBind = ig; hrBind = hr; rtBind = rt; }
        const glm::vec3 d = glm::vec3(ig[3]) - glm::vec3(rt[3]);
        // Is the socket a faithful stand-in for the right hand? If inverse(ig)*hr is constant
        // across every clip/tick, socket and hand are rigidly bound and the socket is a valid
        // attachment point for the weapon. If it drifts, the clips disagree with each other.
        const glm::mat4 grip = glm::inverse(ig) * hr;
        const glm::vec3 gt(grip[3]);
        const glm::mat4 grel0 = glm::inverse(igBind) * hrBind;
        printf("%6.0f | ", t);
        Vec("ig", ig); printf(" ");
        Vec("hand_r", hr); printf(" ");
        Vec("wroot", rt);
        printf("  d=%6.3f rot=%6.2f | grip in socket frame t=(%6.3f,%6.3f,%6.3f) drift=%6.3f deg\n",
               glm::length(d), rotDeg(ig, rt), gt.x, gt.y, gt.z, rotDeg(grel0, grip));
        if (i == 0) {
            // The transform that rigidly parents the weapon's root to the socket at this pose:
            // this is what FirstPersonPresentation must reproduce (asset field `weaponMountRotation`).
            const glm::mat4 rel = glm::inverse(ig) * rt;
            const glm::vec3 euler = EulerYXZFromQuaternion(QuaternionFromMatrix(rel));
            const float roundTrip = rotDeg(rel, glm::mat4_cast(QuaternionFromEulerYXZ(euler)));
            printf("         mount = socket^-1 * wroot : t=(%7.4f,%7.4f,%7.4f) |eulerYXZ deg|=(%8.4f,%8.4f,%8.4f) round-trip=%.5f deg\n",
                   rel[3][0], rel[3][1], rel[3][2], euler.x, euler.y, euler.z, roundTrip);
        }
    }

    printf("\ndeltas from bind (how far each moves over the clip):\n");
    {
        const std::vector<glm::mat4> ga = runArms(dur);
        const std::vector<glm::mat4> gw = runW(dur);
        auto mv = [&](const glm::mat4& a, const glm::mat4& b) { return glm::length(glm::vec3(a[3]) - glm::vec3(b[3])); };
        printf("  ik_hand_gun  end-vs-start = %.4f m\n", mv(ga[igA >= 0 ? igA : 0], igBind));
        printf("  hand_r       end-vs-start = %.4f m\n", mv(ga[hrA >= 0 ? hrA : 0], hrBind));
        printf("  weapon root  end-vs-start = %.4f m\n", mv(gw[rtW >= 0 ? rtW : 0], rtBind));
        if (igA >= 0 && hrA >= 0)
            printf("  |ik_hand_gun - hand_r| at end = %.4f m (bind: %.4f m)\n",
                   mv(ga[igA], ga[hrA]), mv(igBind, hrBind));
    }
    return 0;
}
