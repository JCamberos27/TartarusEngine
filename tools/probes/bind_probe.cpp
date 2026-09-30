// What does the engine render when there is no live clip?
//
// Model::UpdateAnimation drops a Once clip the moment it reaches its end (Clip = -1), and from
// that frame Model::UploadBoneMatrices stops using the posed palette and paints the model with
// m_D->BindPoseBones - the BASE FBX's own node tree. The next PlayAnimation then crossfades
// *from that bind pose*, because PlayAnimation stores m_Anim (Clip = -1) as the fade source and
// Model::EvaluatePose reads an absent source clip as "bind".
//
// So whenever a one-shot state (Fire, reloads, IdleToSprint/SprintToIdle, Draw, Holster,
// Melee, Inspect, MagCheck) finishes and the state machine restarts a looping state, there is
// at least one frame painted in BIND POSE plus a fade that travels through it. Whether the
// player SEES that as "the arms disappear" depends entirely on how far the base FBX's bind
// pose sits from the first-person pose.
//
// This measures exactly that: world-space positions of the anchors that placement depends on
// (head = the camera bone, ik_hand_gun = the weapon socket) and of the hands, under
//   (a) the base model's bind pose
//   (b) each supplied clip at t = 0 and at its last frame
// plus the largest per-bone displacement between (a) and (b).
//
//   bind_probe <armsBase.fbx> <clip.fbx> [more clips...]
//
// Reuses hand_probe's assimp setup and its faithful copy of Model::EvaluatePose.
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

// Model::EvaluatePose: no channels at all -> pure bind (this is the "Clip = -1" frame).
static std::vector<glm::mat4> Evaluate(const std::vector<Node>& nodes,
                                       const std::map<std::string, Ch>* channels, float tick) {
    std::vector<glm::mat4> g(nodes.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < nodes.size(); ++i) {
        const Node& n = nodes[i];
        glm::mat4 local = n.bind;
        if (channels) {
            const auto it = channels->find(n.name);
            if (it != channels->end()) {
                const Ch& c = it->second;
                glm::vec3 bs(1.0f); glm::quat br(1.0f, 0, 0, 0); glm::vec3 bp(0.0f);
                glm::vec3 skw; glm::vec4 prp;
                glm::decompose(n.bind, bs, br, bp, skw, prp);
                const glm::vec3 p(SampleF(c.px, tick, bp.x), SampleF(c.py, tick, bp.y),
                                  SampleF(c.pz, tick, bp.z));
                const glm::quat r = SampleQ(c.rq, tick, br);
                local = glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(r) *
                        glm::scale(glm::mat4(1.0f), bs);
            }
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

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: bind_probe <base.fbx> <clip.fbx> [more...]\n"); return 1; }
    Loaded base;
    if (!LoadRig(argv[1], base)) return 1;

    const std::vector<glm::mat4> bind = Evaluate(base.nodes, nullptr, 0.0f);
    printf("=== base %s\n    nodes=%zu\n", argv[1], base.nodes.size());

    const char* focus[] = {"root", "pelvis", "spine_01", "spine_05", "neck_01", "head",
                           "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l",
                           "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r",
                           "ik_hand_l", "ik_hand_r", "ik_hand_gun"};

    printf("\n  --- BIND POSE (what the engine paints once a Once clip ends) ---\n");
    printf("  %-20s %12s %12s %12s\n", "node", "bind.x", "bind.y", "bind.z");
    for (const char* n : focus) {
        const int i = Find(base.nodes, n);
        if (i < 0) continue;
        const glm::vec3 p(bind[i][3]);
        printf("  %-20s %12.4f %12.4f %12.4f\n", n, p.x, p.y, p.z);
    }

    for (int arg = 2; arg < argc; ++arg) {
        Loaded clip;
        if (!LoadRig(argv[arg], clip)) continue;
        const std::vector<glm::mat4> first = Evaluate(base.nodes, &clip.channels, 0.0f);
        const std::vector<glm::mat4> last = Evaluate(base.nodes, &clip.channels, clip.duration);

        printf("\n---------------- clip %s (%.0f ticks, %.2f s, %zu channels)\n",
               argv[arg], clip.duration, clip.tps > 0 ? clip.duration / clip.tps : 0.0f,
               clip.channels.size());

        // The placement anchors: if these move, FirstPersonPresentation::Update() moves the
        // whole arms entity (head) or the whole weapon entity (ik_hand_gun) with them.
        printf("  %-20s %14s %14s %14s\n", "node", "bind->t0(mm)", "bind->tEnd(mm)", "rot(deg)");
        for (const char* n : focus) {
            const int i = Find(base.nodes, n);
            if (i < 0) continue;
            const float d0 = glm::length(glm::vec3(first[i][3]) - glm::vec3(bind[i][3])) * 1000.0f;
            const float de = glm::length(glm::vec3(last[i][3]) - glm::vec3(bind[i][3])) * 1000.0f;
            const float dr = RotDeg(bind[i], first[i]);
            if (d0 < 0.5f && de < 0.5f && dr < 0.05f) continue; // genuinely identical
            printf("  %-20s %14.1f %14.1f %14.2f\n", n, d0, de, dr);
        }

        // Worst case across every node: how far does the whole rig jump when it reverts?
        float worstD = 0.0f, worstR = 0.0f;
        int worstI = -1;
        for (size_t i = 0; i < base.nodes.size(); ++i) {
            const float d = glm::length(glm::vec3(first[i][3]) - glm::vec3(bind[i][3])) * 1000.0f;
            if (d > worstD) { worstD = d; worstI = (int)i; }
            worstR = std::max(worstR, RotDeg(bind[i], first[i]));
        }
        printf("  worst bind-vs-clip displacement: %.1f mm at '%s';  worst rotation %.1f deg\n",
               worstD, worstI >= 0 ? base.nodes[worstI].name.c_str() : "?", worstR);

        // The end pose matters as much as the start: a state whose weapon clip ends and then
        // hands over to a state with no weapon clip calls StopAnimation(), which cuts the weapon
        // rig straight back to bind. Anything still off bind AT THE END is a visible snap.
        float worstDe = 0.0f;
        int worstEi = -1;
        for (size_t i = 0; i < base.nodes.size(); ++i) {
            const float d = glm::length(glm::vec3(last[i][3]) - glm::vec3(bind[i][3])) * 1000.0f;
            if (d > worstDe) { worstDe = d; worstEi = (int)i; }
        }
        printf("  worst bind-vs-END-OF-CLIP:            %.1f mm at '%s'\n",
               worstDe, worstEi >= 0 ? base.nodes[worstEi].name.c_str() : "?");
    }
    return 0;
}
