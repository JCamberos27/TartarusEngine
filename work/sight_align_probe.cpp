// Where is the ADS iron-sight line relative to the camera?
//
// Replicates Model::EvaluatePose (same code as work/socket_probe.cpp), the weapon attachment
// from FirstPersonPresentation::Update():
//
//     weaponEntity = armsEntity * (socket * mount * weaponRoot^-1),  mount = socket * R(YXZ) * wroot^-1
//
// and, importantly, the ENGINE's own vertex path, because a skinned mesh only reaches world
// space through the palette (investigation UPDATE: "vertex placement was never measured"):
//     ProcessMesh   : v' = nodeTransform(owning node, BIND) * v            (Model.cpp:254/321)
//     ExtractBone   : 4 slots, smallest-replaced, weights renormalized       (Model.cpp:716-779)
//     vertex shader : skinMat = sum(Palette[bone] * w); no division; I if total <= 1e-4
//     Palette       = GlobalInverse * global(bone) * bone->mOffsetMatrix    (Model.cpp:737/1022)
//
// then measures the sight line against the eye. In ENGINE space the camera sits on the posed
// `head` node and looks along +Z with -X to the right, because
//     rotation = cameraRot * Ry(viewRotation = 180)   ->  fwd=+Z, up=+Y, right=-X
// (cross-checked: hand_l x=+0.048 / hand_r x=-0.009 and fire_selector -15 mm off-plane, i.e.
//  the AK's right-side controls toward -X = screen right.)
//
// Both FBXs come from the same .blend with the same exporter, so their file coordinates share
// one space: "authored" = the gun exactly where the artist put it (E = I), "engine" = after
// the attachment matrix E is applied. Comparing the two says whether a misalignment is
// authored in the pose or introduced by weaponMountRotation.
//
//   ads_sight_probe <armsBase> <armsClip|-> <weaponBase> <weaponClip|-> [mountX mountY mountZ]
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
#include <functional>
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
static unsigned EngineFlags() {
    return aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale | aiProcess_GenSmoothNormals |
           aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices |
           aiProcess_OptimizeMeshes;
}
static bool LoadRig(const char* path, Loaded& out) {
    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* sc = imp.ReadFile(path, EngineFlags());
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

// Model::EvaluatePose: local = channel sample (per-component fallback to bind) or bind-local.
static std::vector<glm::mat4> Evaluate(const Loaded& rig, float tick) {
    std::vector<glm::mat4> g(rig.nodes.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < rig.nodes.size(); ++i) {
        const Node& n = rig.nodes[i];
        glm::mat4 local = n.bind;
        const auto it = rig.channels.find(n.name);
        if (it != rig.channels.end()) {
            const Ch& c = it->second;
            glm::vec3 s(1.0f); glm::quat br(1.0f, 0, 0, 0); glm::vec3 bp(0.0f);
            glm::vec3 skw; glm::vec4 prp;
            glm::decompose(n.bind, s, br, bp, skw, prp);
            const glm::vec3 p(SampleF(c.px, tick, bp.x), SampleF(c.py, tick, bp.y), SampleF(c.pz, tick, bp.z));
            const glm::quat r = SampleQ(c.rq, tick, br);
            local = glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
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
static glm::vec3 P(const glm::mat4& m) { return glm::vec3(m[3]); }

// ---------------------------------------------------------------------------
// Weapon vertices in ROOT space, through the engine's own pipeline.
// `evaluated` = the weapon model's node globals (bind when no clip plays, posed otherwise),
// indexed the same as `nodes` (which comes from the identical import, so names line up).
// ---------------------------------------------------------------------------
static bool LoadSkinnedRootVerts(const char* path,
                                 const std::vector<Node>& nodes,
                                 const std::vector<glm::mat4>& evaluated,
                                 std::vector<glm::vec3>& out, glm::vec3& bmin, glm::vec3& bmax) {
    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* sc = imp.ReadFile(path, EngineFlags());
    if (!sc || !sc->mRootNode) { printf("  mesh load failed %s: %s\n", path, imp.GetErrorString()); return false; }

    // Node globals from THIS import (bind tree) - ProcessMesh bakes the owning node's bind
    // world into the vertex, so these must be bind, never posed.
    std::map<std::string, glm::mat4> bindG;
    std::function<void(const aiNode*, const glm::mat4&)> walk = [&](const aiNode* n, const glm::mat4& parent) {
        const glm::mat4 g = parent * AiToGlm(n->mTransformation);
        bindG.emplace(n->mName.C_Str(), g);
        for (unsigned i = 0; i < n->mNumChildren; ++i) walk(n->mChildren[i], g);
    };
    walk(sc->mRootNode, glm::mat4(1.0f));

    // Model.cpp:113
    const glm::mat4 GlobalInverse = glm::inverse(AiToGlm(sc->mRootNode->mTransformation));

    // Name -> index into `nodes`/`evaluated` for the palette (Model.cpp:737 / :1022).
    std::map<std::string, int> idx;
    for (size_t i = 0; i < nodes.size(); ++i) idx.emplace(nodes[i].name, (int)i);

    std::vector<std::string> owner(sc->mNumMeshes);
    for (unsigned mi = 0; mi < sc->mNumMeshes; ++mi) {
        const aiMesh* m = sc->mMeshes[mi];
        (void)m;
    }
    {
        std::function<void(const aiNode*)> findOwner = [&](const aiNode* n) {
            for (unsigned i = 0; i < n->mNumMeshes; ++i) owner[n->mMeshes[i]] = n->mName.C_Str();
            for (unsigned i = 0; i < n->mNumChildren; ++i) findOwner(n->mChildren[i]);
        };
        findOwner(sc->mRootNode);
    }

    bmin = glm::vec3(1e30f); bmax = glm::vec3(-1e30f);

    for (unsigned mi = 0; mi < sc->mNumMeshes; ++mi) {
        const aiMesh* m = sc->mMeshes[mi];

        // Palette for every bone of this mesh: first registration wins (BoneInfoMap, Model.cpp:721).
        std::vector<glm::mat4> pals(m->mNumBones, glm::mat4(1.0f));
        for (unsigned bi = 0; bi < m->mNumBones; ++bi) {
            const std::string nm = m->mBones[bi]->mName.C_Str();
            auto e = idx.find(nm);
            // Not found in the node tree -> BindPoseBones keeps its identity fill (Model.cpp:114/736).
            if (e != idx.end())
                pals[bi] = GlobalInverse * evaluated[e->second] * AiToGlm(m->mBones[bi]->mOffsetMatrix);
        }

        // ExtractBoneWeights replica: 4 slots, a later bigger weight may evict the smallest.
        std::vector<int> ids((size_t)m->mNumVertices * 4, -1);
        std::vector<float> ws((size_t)m->mNumVertices * 4, 0.0f);
        for (unsigned bi = 0; bi < m->mNumBones; ++bi) {
            for (unsigned w = 0; w < m->mBones[bi]->mNumWeights; ++w) {
                const unsigned vid = m->mBones[bi]->mWeights[w].mVertexId;
                const float wt = m->mBones[bi]->mWeights[w].mWeight;
                if (vid >= m->mNumVertices) continue;
                int* id4 = &ids[(size_t)vid * 4];
                float* w4 = &ws[(size_t)vid * 4];
                int slot = 0;
                for (; slot < 4; ++slot) if (id4[slot] < 0) break;
                if (slot == 4) {
                    int smallest = 0;
                    for (int s = 1; s < 4; ++s) if (w4[s] < w4[smallest]) smallest = s;
                    if (wt <= w4[smallest]) continue;
                    slot = smallest;
                }
                id4[slot] = (int)bi;
                w4[slot] = wt;
            }
        }
        for (size_t v = 0; v < (size_t)m->mNumVertices; ++v) {   // Model.cpp:771-779
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) if (ids[v * 4 + k] >= 0) sum += ws[v * 4 + k];
            if (sum > 0.0001f && std::fabs(sum - 1.0f) > 0.0001f)
                for (int k = 0; k < 4; ++k) if (ids[v * 4 + k] >= 0) ws[v * 4 + k] /= sum;
        }

        glm::mat4 ownerG(1.0f);
        auto og = bindG.find(owner[mi]);
        if (og != bindG.end()) ownerG = og->second;

        for (size_t v = 0; v < (size_t)m->mNumVertices; ++v) {
            const glm::vec3 raw(m->mVertices[v].x, m->mVertices[v].y, m->mVertices[v].z);
            const glm::vec3 baked = glm::vec3(ownerG * glm::vec4(raw, 1.0f));   // Model.cpp:321
            glm::mat4 skin(0.0f);
            float total = 0.0f;
            for (int k = 0; k < 4; ++k) {                                       // ModelVertex.glsl:31-46
                if (ids[v * 4 + k] < 0) continue;
                skin += pals[ids[v * 4 + k]] * ws[v * 4 + k];
                total += ws[v * 4 + k];
            }
            if (total <= 0.0001f) skin = glm::mat4(1.0f);
            const glm::vec3 p = glm::vec3(skin * glm::vec4(baked, 1.0f));
            out.push_back(p);
            bmin = glm::min(bmin, p); bmax = glm::max(bmax, p);
        }
    }
    return true;
}

// Least-squares plane through points, parametrised as n = (1, a, b) - the gun's mid-plane is
// X = const by construction, and a naive average of cross products cancels sign-wise.
static void FitPlane(const std::vector<glm::vec3>& pts, glm::vec3& point, glm::vec3& normal) {
    point = glm::vec3(0.0f);
    for (const glm::vec3& p : pts) point += p;
    point /= (float)pts.size();
    float sYY = 0, sYZ = 0, sZZ = 0, sXY = 0, sXZ = 0;
    for (const glm::vec3& p : pts) {
        const glm::vec3 d = p - point;
        sYY += d.y * d.y; sYZ += d.y * d.z; sZZ += d.z * d.z;
        sXY += d.x * d.y; sXZ += d.x * d.z;
    }
    const float det = sYY * sZZ - sYZ * sYZ;
    float a = 0.0f, b = 0.0f;
    if (std::fabs(det) > 1e-12f) {
        a = (-sXY * sZZ + sXZ * sYZ) / det;
        b = (-sXZ * sYY + sXY * sYZ) / det;
    }
    normal = glm::normalize(glm::vec3(1.0f, a, b));
    if (normal.x > 0.0f) normal = -normal;     // orient toward -X = the camera's right
}

static float NearestDist(const glm::vec3& q, const std::vector<glm::vec3>& pts) {
    float best = 1e30f;
    for (const glm::vec3& p : pts) best = std::min(best, glm::length(q - p));
    return best;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        printf("usage: ads_sight_probe <armsBase> <armsClip|-> <weaponBase> <weaponClip|-> [mountX mountY mountZ]\n");
        return 1;
    }
    const glm::vec3 mountEuler = (argc >= 8)
        ? glm::vec3((float)atof(argv[5]), (float)atof(argv[6]), (float)atof(argv[7]))
        : glm::vec3(0.0f, 90.0f, 90.0f);

    Loaded armsBase, armsClip, wBase, wClip;
    if (!LoadRig(argv[1], armsBase)) return 1;
    if (std::string(argv[2]) != "-" && !LoadRig(argv[2], armsClip)) return 1;
    if (!LoadRig(argv[3], wBase)) return 1;
    if (std::string(argv[4]) != "-" && !LoadRig(argv[4], wClip)) return 1;

    printf("=== ADS sight alignment (engine space: +Z forward, +Y up, -X right) ===\n");
    printf("arms  : %s + %s\n", argv[1], argv[2]);
    printf("weapon: %s + %s\n", argv[3], argv[4]);
    printf("weaponMountRotation eulerYXZ = (%.1f, %.1f, %.1f)\n", mountEuler.x, mountEuler.y, mountEuler.z);

    Loaded armsRun = armsBase;
    if (armsClip.ok) { armsRun.channels = armsClip.channels; armsRun.duration = armsClip.duration; }
    Loaded wRun = wBase;
    if (wClip.ok) { wRun.channels = wClip.channels; wRun.duration = wClip.duration; }
    printf("arms clip duration = %.0f ticks\n", armsRun.duration);

    const std::vector<glm::mat4> ga = Evaluate(armsRun, 0.0f);
    const std::vector<glm::mat4> gw = Evaluate(wRun, 0.0f);
    {
        const std::vector<glm::mat4> ga2 = Evaluate(armsRun, armsRun.duration);
        const int h = Find(armsBase.nodes, "head");
        if (h >= 0)
            printf("eye drift over clip (tick 0 -> %.0f) = %.4f mm\n", armsRun.duration,
                   glm::length(P(ga2[h]) - P(ga[h])) * 1000.0f);
    }

    const int iHead = Find(armsBase.nodes, "head");
    const int iSock = Find(armsBase.nodes, "ik_hand_gun");
    const int iHl = Find(armsBase.nodes, "hand_l");
    const int iHr = Find(armsBase.nodes, "hand_r");
    const int iRoot = Find(wBase.nodes, "root");
    if (iHead < 0 || iSock < 0 || iRoot < 0) { printf("missing head/socket/root node\n"); return 1; }

    const glm::mat4 eye = ga[iHead];
    const glm::mat4 socket = ga[iSock];
    const glm::mat4 wroot = gw[iRoot];
    printf("\neye (posed head)      = (%8.4f, %8.4f, %8.4f)\n", P(eye).x, P(eye).y, P(eye).z);
    printf("socket ik_hand_gun    = (%8.4f, %8.4f, %8.4f)\n", P(socket).x, P(socket).y, P(socket).z);
    if (iHl >= 0) printf("hand_l                = (%8.4f, %8.4f, %8.4f)\n", P(ga[iHl]).x, P(ga[iHl]).y, P(ga[iHl]).z);
    if (iHr >= 0) printf("hand_r                = (%8.4f, %8.4f, %8.4f)\n", P(ga[iHr]).x, P(ga[iHr]).y, P(ga[iHr]).z);
    printf("weapon root           = (%8.4f, %8.4f, %8.4f)\n", P(wroot).x, P(wroot).y, P(wroot).z);

    // The mount the AUTHORING implies: weaponRoot == socket * mount.
    const glm::mat4 R = glm::inverse(socket) * wroot;
    const glm::vec3 eR = EulerYXZFromQuaternion(QuaternionFromMatrix(R));
    printf("\nauthored mount (socket^-1 * wroot):\n");
    printf("  t        = (%8.4f, %8.4f, %8.4f) m  -> %s\n", P(R).x, P(R).y, P(R).z,
           glm::length(P(R)) < 1e-4f
               ? "root sits ON the socket (pure rotation, rotation-only mount is exact)"
               : "root is OFF the socket -> a rotation-only mount CANNOT reproduce it");
    printf("  eulerYXZ = (%8.3f, %8.3f, %8.3f) deg   (asset says %.1f, %.1f, %.1f)\n",
           eR.x, eR.y, eR.z, mountEuler.x, mountEuler.y, mountEuler.z);

    // E: what FirstPersonPresentation actually multiplies the weapon content by.
    const glm::mat4 MOUNT = glm::mat4_cast(QuaternionFromEulerYXZ(mountEuler));
    const glm::mat4 E = socket * MOUNT * glm::inverse(wroot);
    printf("\nattachment E = socket * mount * wroot^-1 (engine vs authored):\n");
    printf("  translation = (%8.4f, %8.4f, %8.4f) m\n", P(E).x, P(E).y, P(E).z);
    printf("  right/up/fwd = %+.1f / %+.1f / %+.1f mm        (camera axes)\n",
           -P(E).x * 1000.0f, P(E).y * 1000.0f, P(E).z * 1000.0f);
    printf("  rotation    = %.3f deg   (%s)\n", RotDeg(E, glm::mat4(1.0f)),
           RotDeg(E, glm::mat4(1.0f)) < 0.1f ? "pure translation: the rotation part of the mount is RIGHT,"
                                                   " only the translation is missing"
                                             : "has a rotation error too");

    // The exact translation a fixed engine would need, in two useful frames:
    //   socket frame  : mount = socket * T(t_R) * R ...   (i.e. mount sits where the artist put it)
    //   mount  frame  : mount = socket * R * T(off) ...
    {
        const glm::mat3 Mr(MOUNT);
        const glm::vec3 offMount = glm::transpose(Mr) * P(R);
        printf("  needed mount translation:\n");
        printf("    in socket frame (mount = socket * T * R * wroot^-1): (%.4f, %.4f, %.4f)\n",
               P(R).x, P(R).y, P(R).z);
        printf("    in mount  frame (mount = socket * R * T * wroot^-1): (%.4f, %.4f, %.4f)\n",
               offMount.x, offMount.y, offMount.z);
    }

    // --- weapon geometry, both placements -------------------------------------
    std::vector<glm::vec3> verts;
    glm::vec3 bmin, bmax;
    if (!LoadSkinnedRootVerts(argv[3], wBase.nodes, gw, verts, bmin, bmax)) return 1;
    printf("\nweapon mesh (engine-exact skinning): %zu verts, root-space box\n"
           "   x[%.3f %.3f] y[%.3f %.3f] z[%.3f %.3f]\n",
           verts.size(), bmin.x, bmax.x, bmin.y, bmax.y, bmin.z, bmax.z);

    auto place = [&](const glm::vec3& p, bool engine) {
        return engine ? glm::vec3(E * glm::vec4(p, 1.0f)) : p;
    };

    // hands vs gun in both placements (sanity: is the gun in the hands at all?)
    if (iHl >= 0 && iHr >= 0) {
        for (int eng = 1; eng >= 0; --eng) {
            const glm::vec3 hl = place(P(ga[iHl]), eng), hr = place(P(ga[iHr]), eng);
            printf("  %-8s nearest gun vertex to hand_l = %5.1f mm, hand_r = %5.1f mm\n",
                   eng ? "engine" : "authored", NearestDist(hl, verts) * 1000.0f,
                   NearestDist(hr, verts) * 1000.0f);
        }
    }

    // --- the gun's mid-plane from its bones (raw = authored coordinates) -------
    // E is a pure translation with a 0.03 deg rotation, so fitting on raw and translating
    // for "engine" is exact - and it keeps every pick in the artist's own frame.
    const char* midBones[] = { "root", "stock", "rear_sling_loop", "magazine", "mag2", "trigger" };
    const char* sideBones[] = { "bolt", "fire_selector", "mag_release" };

    glm::vec3 cRaw, nRaw;
    {
        std::vector<glm::vec3> mid;
        printf("\nweapon bones, X = lateral coordinate (raw / engine in brackets):\n");
        for (const char* n : midBones) {
            const int i = Find(wBase.nodes, n);
            if (i < 0) { printf("  %-16s MISSING\n", n); continue; }
            const glm::vec3 p = P(gw[i]);
            printf("  %-16s (%8.4f, %8.4f, %8.4f)  X=%+.4f  [engine X=%+.4f]\n",
                   n, p.x, p.y, p.z, p.x, place(p, true).x);
            mid.push_back(p);
        }
        for (const char* n : sideBones) {
            const int i = Find(wBase.nodes, n);
            if (i < 0) { printf("  %-16s MISSING\n", n); continue; }
            const glm::vec3 p = P(gw[i]);
            printf("  %-16s (%8.4f, %8.4f, %8.4f)  X=%+.4f   <- should sit right of plane\n",
                   n, p.x, p.y, p.z, p.x);
        }
        if (mid.size() < 3) { printf("not enough mid-plane bones\n"); return 1; }
        FitPlane(mid, cRaw, nRaw);           // nRaw points toward -X = screen right
        printf("\nmid-plane (raw): point=(%.4f, %.4f, %.4f) normal->right=(%.4f, %.4f, %.4f)\n",
               cRaw.x, cRaw.y, cRaw.z, nRaw.x, nRaw.y, nRaw.z);
        printf("  mid-bone distance from plane (mm): ");
        for (const glm::vec3& p : mid) printf("%.1f ", glm::dot(p - cRaw, nRaw) * 1000.0f);
        printf("| right-side parts: ");
        for (const char* n : sideBones) {
            const int i = Find(wBase.nodes, n);
            if (i >= 0) printf("%.1f ", glm::dot(P(gw[i]) - cRaw, nRaw) * 1000.0f);
        }
        printf("mm (positive = right of centre plane)\n");
    }

    // --- top profile of the gun along its length (identifies the sights) -------
    std::vector<glm::vec3> strip;             // raw mid-plane vertices
    std::vector<std::pair<float, float>> peaks;   // (y, z) of local maxima
    {
        const float binSize = 0.02f;
        const int nbins = (int)((bmax.z - bmin.z) / binSize) + 1;
        std::vector<float> topy(nbins, -1e30f);
        std::vector<int> cnt(nbins, 0);
        for (const glm::vec3& v : verts) {
            if (std::fabs(glm::dot(v - cRaw, nRaw)) > 0.004f) continue;
            strip.push_back(v);
            int b = (int)((v.z - bmin.z) / binSize);
            if (b < 0) b = 0;
            if (b >= nbins) b = nbins - 1;
            topy[b] = std::max(topy[b], v.y);
            cnt[b]++;
        }
        printf("\ntop profile along +Z (raw mid-plane verts, max Y per 20 mm bin), %zu verts:\n", strip.size());
        for (int b = 0; b < nbins; ++b) {
            if (cnt[b] == 0) continue;
            printf("   z=%+.3f  y=%+.4f  n=%d\n", bmin.z + (b + 0.5f) * binSize, topy[b], cnt[b]);
        }
        // local maxima (peaks), highest first - the rear sight, the front sight and anything
        // that competes with them, so the pick below can be checked by eye against the table.
        for (int b = 1; b + 1 < nbins; ++b) {
            if (cnt[b] == 0) continue;
            if (topy[b] > topy[b - 1] && topy[b] >= topy[b + 1]) peaks.push_back({topy[b], bmin.z + (b + 0.5f) * binSize});
        }
        std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        printf("  top local maxima (y, z): ");
        for (size_t i = 0; i < peaks.size() && i < 6; ++i) printf("y=%.4f@z=%+.3f  ", peaks[i].first, peaks[i].second);
        printf("\n");
    }

    // Sight candidates, picked in the artist's frame from BANDS rather than a blind half split
    // (the real rear sight sits at ~50-58% of the gun's length, ahead of the top cover):
    //   rear  = tallest mid-plane vertex in [40%, 68%] of z-range
    //   front = tallest mid-plane vertex in [70%, 100%] (the hooded post, gun's tallest point)
    glm::vec3 rear(0.0f), front(0.0f);
    float rearY = -1e30f, frontY = -1e30f;
    {
        const float zlen = bmax.z - bmin.z;
        const float zRear0 = bmin.z + 0.40f * zlen, zRear1 = bmin.z + 0.68f * zlen;
        const float zFront0 = bmin.z + 0.70f * zlen;
        for (const glm::vec3& p : strip) {
            if (p.z >= zRear0 && p.z <= zRear1 && p.y > rearY) { rearY = p.y; rear = p; }
            if (p.z >= zFront0 && p.y > frontY) { frontY = p.y; front = p; }
        }
        printf("\nsight candidates (raw, bands z[%.3f %.3f] rear / z>%.3f front):\n",
               zRear0, zRear1, zFront0);
        printf("  rear  = (%8.4f, %8.4f, %8.4f)\n", rear.x, rear.y, rear.z);
        printf("  front = (%8.4f, %8.4f, %8.4f)\n", front.x, front.y, front.z);
        if (glm::length(front - rear) < 1e-4f) printf("  (degenerate - no strip verts)\n");
    }

    // Sight-line direction is a property of the raw pose; E barely rotates (0.03 deg), so
    // yaw/pitch are reported once from the raw line.
    glm::vec3 d = front - rear;
    const float dlen = glm::length(d);
    if (dlen > 1e-5f) d /= dlen;
    const float yawDeg = (float)(std::atan2(-d.x, d.z) * 180.0 / 3.14159265358979323846);
    const float pitchDeg = (float)(std::asin(std::max(-1.0f, std::min(1.0f, d.y))) * 180.0 / 3.14159265358979323846);
    printf("  sight line dir = (%+.4f, %+.4f, %+.4f)  yaw=%+.2f deg (+ = muzzle right of aim)  pitch=%+.2f deg\n",
           d.x, d.y, d.z, yawDeg, pitchDeg);

    // --- the actual question: sight vs eye ------------------------------------
    float engLat = 0.0f, engVert = 0.0f, authLat = 0.0f, authVert = 0.0f, engDepth = 0.0f, authDepth = 0.0f;
    for (int eng = 1; eng >= 0; --eng) {
        const glm::vec3 eyeP = P(eye);                 // the camera never moves
        const glm::vec3 rp = place(rear, eng), fp = place(front, eng);   // ONE application of E
        const glm::vec3 vr = rp - eyeP, vf = fp - eyeP;
        const float latR = -vr.x, vertR = vr.y;         // right = -X, up = +Y
        const float latF = -vf.x, vertF = vf.y;
        const glm::vec3 c = place(cRaw, eng);
        const float latPlane = glm::dot(c - eyeP, nRaw);   // + = gun centre plane RIGHT of camera
        printf("\n[%s] sight vs eye\n", eng ? "engine" : "authored");
        printf("  at REAR sight : lateral %+.1f mm (+=sight RIGHT of eye), vertical %+.1f mm (+=sight ABOVE eye), depth %+.1f mm\n",
               latR * 1000.0f, vertR * 1000.0f, vr.z * 1000.0f);
        printf("  at FRONT sight: lateral %+.1f mm, vertical %+.1f mm, depth %+.1f mm\n",
               latF * 1000.0f, vertF * 1000.0f, vf.z * 1000.0f);
        printf("  gun centre plane vs camera (bone fit): %+.1f mm (%s of camera)\n",
               latPlane * 1000.0f, latPlane < 0 ? "LEFT" : "RIGHT");
        if (eng) { engLat = latR; engVert = vertR; engDepth = vr.z; }
        else     { authLat = latR; authVert = vertR; authDepth = vr.z; }
    }

    printf("\n--- summary -------------------------------------------------------------\n");
    printf("authored (what the artist placed): sight %+.1f mm right, %+.1f mm up, %.0f mm deep\n",
           authLat * 1000.0f, authVert * 1000.0f, authDepth * 1000.0f);
    printf("engine  (what the player sees)   : sight %+.1f mm right, %+.1f mm up, %.0f mm deep\n",
           engLat * 1000.0f, engVert * 1000.0f, engDepth * 1000.0f);
    printf("  -> the mount's missing translation is responsible for %+.1f mm lateral, %+.1f mm vertical\n",
           (engLat - authLat) * 1000.0f, (engVert - authVert) * 1000.0f);
    // Camera-local axes: +X right, +Y up, +Z BACK (camera looks -Z), so a needed forward
    // displacement (authDepth - engDepth) enters as a NEGATIVE z:
    //   offset.x = -lat   (right = -X in engine space, +X in camera frame -> both flip once)
    //   offset.y = -vert  (up matches)
    //   offset.z =  engDepth - authDepth
    printf("\nfix A (scene, no code):  \"View Model Offset\" = (%.3f, %.3f, %.3f)\n",
           -engLat, -engVert, engDepth - authDepth);
    printf("   centres the sight now and restores authored depth; applies to every state in that scene.\n");
    printf("fix B (engine):          weaponMountOffset = (%.4f, %.4f, %.4f)  [socket frame]\n",
           P(R).x, P(R).y, P(R).z);
    printf("   makes E identity -> the gun returns to the authored pose in EVERY state;\n");
    printf("   a residual %+.1f mm lateral (author aimed with the right eye) remains, fixable with\n", authLat * 1000.0f);
    printf("   a small View Model Offset x=%+.3f if you want it dead centre.\n", -authLat);
    printf("\ncamera axes: right = -X, up = +Y, forward = +Z  (rotation = cameraRot * Ry(180)).\n");
    printf("A lateral error that differs between the rear and front sight is YAW (%+.2f deg here),\n", yawDeg);
    printf("which an offset cannot fix - that needs View Model Rotation or an authored-pose change.\n");

    // =====================================================================================
    // sight_align: map the real sight geometry (heightmaps below), then put chosen points
    // through FirstPersonPresentation::Update's camera-frame math.
    // =====================================================================================
    auto heightmap = [&](float z0, float z1, float xc, float dz = 0.005f, float dx = 0.001f, int nx = 41) {
        const float halfW = dx * nx * 0.5f;
        const int nz = (int)((z1 - z0) / dz + 0.5f);
        printf("\nheightmap z[%.3f %.3f], lateral centre raw x=%.4f, cols = 1 mm, screen-left -> screen-right;"
               " cell = max y in mm above 1.500 ('.' = no verts)\n", z0, z1, xc);
        for (int iz = 0; iz < nz; ++iz) {
            std::vector<float> top(nx, -1e30f);
            const float za = z0 + iz * dz, zb = za + dz;
            for (const glm::vec3& v : verts) {
                if (v.z < za || v.z >= zb) continue;
                const int ix = (int)std::floor((xc + halfW - v.x) / dx);   // col 0 = most +X = screen left
                if (ix < 0 || ix >= nx) continue;
                top[ix] = std::max(top[ix], v.y);
            }
            printf("  z=%.3f ", za);
            for (int ix = 0; ix < nx; ++ix) {
                if (top[ix] < -1e29f) printf("  .");
                else printf("%3d", (int)std::lround((top[ix] - 1.5f) * 1000.0f));
            }
            printf("\n");
        }
    };
    // Fine maps, both centred on the same raw x so columns line up between them:
    // col c <-> raw x = xc + halfW - (c + 0.5) * dx.
    const float fineXc = -0.0690f;
    printf("\n[fine] column c <-> raw x = %.4f - (c + 0.5) * 0.0005\n", fineXc + 0.0005f * 41 * 0.5f);
    heightmap(0.478f, 0.498f, fineXc, 0.001f, 0.0005f, 41);
    heightmap(0.710f, 0.730f, fineXc, 0.001f, 0.0005f, 41);

    // Sight picture points read off the fine maps (raw frame):
    //   rear  = centre of the rear notch at its SHOULDER height (post tip flush with notch top)
    //   front = centre of the front post at its TIP
    const glm::vec3 R0(-0.0682f, 1.559f, 0.4865f);
    const glm::vec3 F0(-0.06983f, 1.558f, 0.7235f);

    const glm::vec3 viewRot(0.0f, 180.0f, 0.0f);   // AKS74U.fpsanim viewRotation
    // FirstPersonPresentation::Update, in the camera frame (+x right, +y up, +z back):
    //   q = Qv * Qs * (E * p - head) + offset
    auto toCam = [&](const glm::vec3& raw, const glm::vec3& rotDeg, const glm::vec3& off) {
        const glm::vec3 pr = glm::vec3(E * glm::vec4(raw, 1.0f));
        const glm::quat r = QuaternionFromEulerYXZ(viewRot) * QuaternionFromEulerYXZ(rotDeg);
        return r * (pr - P(eye)) + off;
    };
    const double kDeg = 180.0 / 3.14159265358979323846;
    // Angular position on screen: + = right / up of screen centre.
    auto ang = [&](const glm::vec3& q) {
        return glm::dvec2(std::atan2((double)q.x, (double)-q.z) * kDeg, std::atan2((double)q.y, (double)-q.z) * kDeg);
    };
    // px per degree at screen centre for a view-model FOV of 60 deg (vertical), per 1000 px of view height.
    const double pxPerDeg1000 = 500.0 / std::tan(30.0 / kDeg) / kDeg;

    auto report = [&](const char* label, const glm::vec3& rot, const glm::vec3& off) {
        const glm::vec3 qr = toCam(R0, rot, off), qf = toCam(F0, rot, off);
        const glm::dvec2 ar = ang(qr), af = ang(qf);
        printf("\n%s  offset=(%.4f, %.4f, %.4f) rotation=(%.3f, %.3f, %.3f)\n", label, off.x, off.y, off.z, rot.x, rot.y, rot.z);
        printf("  rear notch : %+6.2f mm right, %+6.2f mm up at %5.1f mm  -> %+.3f / %+.3f deg\n",
               qr.x * 1000.0f, qr.y * 1000.0f, -qr.z * 1000.0f, ar.x, ar.y);
        printf("  front post : %+6.2f mm right, %+6.2f mm up at %5.1f mm  -> %+.3f / %+.3f deg\n",
               qf.x * 1000.0f, qf.y * 1000.0f, -qf.z * 1000.0f, af.x, af.y);
        printf("  post vs notch (sight picture error): %+.3f deg right, %+.3f deg up\n", af.x - ar.x, af.y - ar.y);
        printf("  in px, 1080p full-height view / 659 px Game tab: post %+.1f,%+.1f / %+.1f,%+.1f   notch %+.1f,%+.1f / %+.1f,%+.1f\n",
               af.x * pxPerDeg1000 * 1.080, af.y * pxPerDeg1000 * 1.080, af.x * pxPerDeg1000 * 0.659, af.y * pxPerDeg1000 * 0.659,
               ar.x * pxPerDeg1000 * 1.080, ar.y * pxPerDeg1000 * 1.080, ar.x * pxPerDeg1000 * 0.659, ar.y * pxPerDeg1000 * 0.659);
    };

    glm::vec3 curOff(0.054f, -0.031f, 0.0f), curRot(0.0f);
    if (argc >= 11) curOff = glm::vec3((float)atof(argv[8]), (float)atof(argv[9]), (float)atof(argv[10]));
    if (argc >= 14) curRot = glm::vec3((float)atof(argv[11]), (float)atof(argv[12]), (float)atof(argv[13]));
    report("[current]", curRot, curOff);

    // Solve: unknowns (rot.x, rot.y, off.x, off.y); residuals = both sights' screen angles -> 0.
    auto solve = [&](bool withRot) {
        double u[4] = { 0.0, 0.0, curOff.x, curOff.y };
        auto resid = [&](const double* v, double* out) {
            const glm::vec3 rot((float)v[0], (float)v[1], 0.0f), off((float)v[2], (float)v[3], 0.0f);
            const glm::dvec2 ar = ang(toCam(R0, rot, off)), af = ang(toCam(F0, rot, off));
            if (withRot) { out[0] = ar.x; out[1] = ar.y; out[2] = af.x; out[3] = af.y; }
            else {         // offset only: put the eye ON the sight line (post inside notch)
                out[0] = af.x - ar.x; out[1] = af.y - ar.y; out[2] = 0.0; out[3] = 0.0;
            }
        };
        const int n = withRot ? 4 : 2;
        const int o = withRot ? 0 : 2;        // offset-only solves only u[2], u[3]
        for (int it = 0; it < 30; ++it) {
            double r[4]; resid(u, r);
            double J[4][4] = {};
            for (int j = 0; j < n; ++j) {
                double up[4] = { u[0], u[1], u[2], u[3] };
                const double h = (o + j) < 2 ? 1e-3 : 1e-5;
                up[o + j] += h;
                double rp[4]; resid(up, rp);
                for (int i = 0; i < n; ++i) J[i][j] = (rp[i] - r[i]) / h;
            }
            // Gaussian elimination on J * du = -r
            double A[4][5];
            for (int i = 0; i < n; ++i) { for (int j = 0; j < n; ++j) A[i][j] = J[i][j]; A[i][n] = -r[i]; }
            for (int c = 0; c < n; ++c) {
                int p = c; for (int i = c + 1; i < n; ++i) if (std::fabs(A[i][c]) > std::fabs(A[p][c])) p = i;
                for (int j = 0; j <= n; ++j) std::swap(A[c][j], A[p][j]);
                for (int i = 0; i < n; ++i) if (i != c) { const double f = A[i][c] / A[c][c]; for (int j = c; j <= n; ++j) A[i][j] -= f * A[c][j]; }
            }
            for (int j = 0; j < n; ++j) u[o + j] += A[j][n] / A[j][j];
        }
        return std::make_pair(glm::vec3((float)u[0], (float)u[1], 0.0f), glm::vec3((float)u[2], (float)u[3], 0.0f));
    };
    const auto onLine = solve(false);
    report("[offset only: eye on the sight line]", onLine.first, onLine.second);
    const auto full = solve(true);
    report("[rotation + offset: sight line on screen centre]", full.first, full.second);
    const auto r3 = [](float v) { return std::round(v * 1000.0f) / 1000.0f; };
    const auto r2 = [](float v) { return std::round(v * 100.0f) / 100.0f; };
    report("[rounded for the scene file]", glm::vec3(r2(full.first.x), r2(full.first.y), 0.0f),
           glm::vec3(r3(full.second.x), r3(full.second.y), 0.0f));
    return 0;
}
