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
//   mag_track_probe <armsBase> <armsClip> <weaponBase> <weaponClip>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/config.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include "../src/Game/RotationMath.h"
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


// Magazine islands vs the left hand, in the weapon root's space, over a reload.
int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: mag_track_probe armsBase armsClip weaponBase weaponClip\n"); return 2; }
    Loaded armsBase, armsClip, wBase, wClip;
    if (!LoadRig(argv[1], armsBase) || !LoadRig(argv[2], armsClip) || !LoadRig(argv[3], wBase) || !LoadRig(argv[4], wClip)) return 1;
    armsBase.channels = armsClip.channels; armsBase.duration = armsClip.duration; armsBase.tps = armsClip.tps;
    wBase.channels = wClip.channels; wBase.duration = wClip.duration; wBase.tps = wClip.tps;
    const int sock = Find(armsBase.nodes, "ik_hand_gun"), lh = Find(armsBase.nodes, "hand_l");
    const int root = Find(wBase.nodes, "root"), mag = Find(wBase.nodes, "magazine"), mag2 = Find(wBase.nodes, "mag2");
    printf("sock %d lh %d root %d mag %d mag2 %d  dur %.1f/%.1f tps %.1f\n", sock, lh, root, mag, mag2, armsBase.duration, wBase.duration, wBase.tps);
    if (sock < 0 || lh < 0 || root < 0 || mag < 0 || mag2 < 0) return 1;
    const glm::mat4 mountR = glm::mat4_cast(QuaternionFromEulerYXZ(glm::vec3(0, 90, 90)));
    const std::vector<glm::mat4> wb = Evaluate(wBase, -1e9f); // first key (rest)
    printf("scales: sock %.4f root %.4f mag2 %.4f lh %.4f\n", glm::length(glm::vec3(Evaluate(armsBase,0)[sock][0])), glm::length(glm::vec3(wb[root][0])), glm::length(glm::vec3(wb[mag2][0])), glm::length(glm::vec3(Evaluate(armsBase,0)[lh][0])));
    const glm::vec3 magSeat = glm::vec3(glm::inverse(wb[root]) * wb[mag][3]);
    for (float t = 0; t <= wBase.duration + 0.01f; t += 1.0f) {
        const auto a = Evaluate(armsBase, t), w = Evaluate(wBase, t);
        const glm::mat4 toRoot = glm::inverse(a[sock] * mountR);
        const glm::vec3 hand = glm::vec3(toRoot * a[lh][3]);
        const glm::vec3 m1 = glm::vec3(glm::inverse(w[root]) * w[mag][3]);
        const glm::vec3 m2 = glm::vec3(glm::inverse(w[root]) * w[mag2][3]);
        printf("t %5.1f  mag: seat %.3f hand %.3f | mag2: seat %.3f hand %.3f | mag-mag2 %.3f\n", t,
               glm::length(m1 - magSeat), glm::length(m1 - hand), glm::length(m2 - magSeat), glm::length(m2 - hand), glm::length(m1 - m2));
    }
}
