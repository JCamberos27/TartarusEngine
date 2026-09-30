// Answers a question none of the existing probes asked: for every bone the ARMS MESH
// actually carries vertex weights on, does the clip really ANIMATE it?
//
// bone_match_probe only proved each clip channel matches *some* node name (89/89).
// full_skin_probe proved every skin matrix is numerically clean at tick 64.
// Neither asks "does this particular skinned bone move at all during the clip".
//
// A skinned bone that stays at its bind transform while its neighbours move is
// exactly the failure that shreds a mesh into blades: its vertices stay put while
// the vertices around them are carried away.
//
// NOTE: at bind pose *every* skin matrix collapses to the same constant
// (GlobalInverse * RestGlobal * BoneOffset == GlobalInverse), so a clean bind pose
// proves nothing about per-bone behaviour. This probe prints bind vs posed per bone
// so a bone that never leaves bind is flagged explicitly.
#define GLM_ENABLE_EXPERIMENTAL
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

static glm::mat4 AiToGlm(const aiMatrix4x4& m) {
    return glm::mat4(m.a1, m.b1, m.c1, m.d1, m.a2, m.b2, m.c2, m.d2,
                      m.a3, m.b3, m.c3, m.d3, m.a4, m.b4, m.c4, m.d4);
}
static glm::quat AiToGlm(const aiQuaternion& q) { return glm::quat(q.w, q.x, q.y, q.z); }

struct Node {
    std::string Name;
    int Parent = -1;
    glm::mat4 BindLocal{1.0f};
    int BoneId = -1;
    glm::mat4 BoneOffset{1.0f};
};
struct PosKey { glm::vec3 v; float t; };
struct RotKey { glm::quat v; float t; };
struct SclKey { glm::vec3 v; float t; };
struct Channel { std::string BoneName; std::vector<PosKey> pos; std::vector<RotKey> rot; std::vector<SclKey> scl; };

template <typename Keys>
size_t FindKeyIndex(const Keys& keys, float t) {
    if (keys.size() <= 1) return 0;
    size_t i = 0;
    for (; i + 1 < keys.size(); ++i) if (keys[i + 1].t > t) break;
    return i;
}
static float Factor(float a, float b, float t) {
    float d = b - a; if (d <= 0.0f) return 0.0f;
    float f = (t - a) / d; if (f < 0) f = 0; if (f > 1) f = 1; return f;
}

static const aiScene* Load(Assimp::Importer& imp, const std::string& path) {
    imp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    unsigned int flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale |
                         aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
                         aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices |
                         aiProcess_OptimizeMeshes;
    const aiScene* s = imp.ReadFile(path, flags);
    if (!s) fprintf(stderr, "FAILED %s: %s\n", path.c_str(), imp.GetErrorString());
    return s;
}

static void ReadHierarchy(const aiNode* n, int parent, std::vector<Node>& nodes) {
    Node node;
    node.Name = n->mName.C_Str();
    node.Parent = parent;
    node.BindLocal = AiToGlm(n->mTransformation);
    int self = (int)nodes.size();
    nodes.push_back(node);
    for (unsigned i = 0; i < n->mNumChildren; ++i) ReadHierarchy(n->mChildren[i], self, nodes);
}

static float MaxAbsDiff(const glm::mat4& a, const glm::mat4& b) {
    float m = 0.0f;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m = std::max(m, std::fabs(a[c][r] - b[c][r]));
    return m;
}

int main(int argc, char** argv) {
    std::string dir =
        "C:\\Users\\jacob\\OneDrive\\Documents\\Default Project\\TartarusEngine\\project\\assets\\fps\\AKS74U\\FirstPerson\\";
    std::string base = dir + "AKS-74U_A_FP_ADS.fbx";
    std::string clipPath = argc > 1 ? argv[1] : dir + "AKS-74U_A_FP_Idle.fbx";
    float tick = argc > 2 ? (float)atof(argv[2]) : 64.0f;

    Assimp::Importer impBase, impClip;
    const aiScene* sceneBase = Load(impBase, base);
    const aiScene* sceneClip = Load(impClip, clipPath);
    if (!sceneBase || !sceneClip || !sceneClip->mNumAnimations) return 1;

    std::vector<Node> nodes;
    ReadHierarchy(sceneBase->mRootNode, -1, nodes);
    std::unordered_map<std::string, int> nameToIdx;
    for (int i = 0; i < (int)nodes.size(); ++i) nameToIdx[nodes[i].Name] = i;

    glm::mat4 globalInverse = glm::inverse(AiToGlm(sceneBase->mRootNode->mTransformation));

    // Same first-registration-wins BoneId assignment as Model::ExtractBoneWeights.
    int boneCounter = 0;
    for (unsigned m = 0; m < sceneBase->mNumMeshes; ++m) {
        aiMesh* mesh = sceneBase->mMeshes[m];
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            aiBone* bone = mesh->mBones[b];
            auto it = nameToIdx.find(bone->mName.C_Str());
            if (it == nameToIdx.end()) {
                printf("!! mesh bone '%s' has NO matching node\n", bone->mName.C_Str());
                continue;
            }
            Node& n = nodes[it->second];
            if (n.BoneId < 0) { n.BoneId = boneCounter++; n.BoneOffset = AiToGlm(bone->mOffsetMatrix); }
        }
    }
    printf("clip            : %s\n", clipPath.c_str());
    printf("sample tick     : %.1f\n", tick);
    printf("nodes           : %d\n", (int)nodes.size());
    printf("skinned bones   : %d\n", boneCounter);
    printf("GlobalInverse T : (%.4f, %.4f, %.4f)\n",
           globalInverse[3].x, globalInverse[3].y, globalInverse[3].z);

    aiAnimation* anim = sceneClip->mAnimations[0];
    printf("clip channels   : %u\n", anim->mNumChannels);
    std::unordered_map<std::string, Channel> channels;
    for (unsigned c = 0; c < anim->mNumChannels; ++c) {
        aiNodeAnim* ch = anim->mChannels[c];
        Channel out; out.BoneName = ch->mNodeName.C_Str();
        for (unsigned k = 0; k < ch->mNumPositionKeys; ++k)
            out.pos.push_back({{ch->mPositionKeys[k].mValue.x, ch->mPositionKeys[k].mValue.y,
                                ch->mPositionKeys[k].mValue.z}, (float)ch->mPositionKeys[k].mTime});
        for (unsigned k = 0; k < ch->mNumRotationKeys; ++k)
            out.rot.push_back({AiToGlm(ch->mRotationKeys[k].mValue), (float)ch->mRotationKeys[k].mTime});
        for (unsigned k = 0; k < ch->mNumScalingKeys; ++k)
            out.scl.push_back({{ch->mScalingKeys[k].mValue.x, ch->mScalingKeys[k].mValue.y,
                                ch->mScalingKeys[k].mValue.z}, (float)ch->mScalingKeys[k].mTime});
        channels[out.BoneName] = std::move(out);
    }

    // Compose the hierarchy twice: once with pure bind locals, once with sampled channels.
    std::vector<glm::mat4> bindG(nodes.size(), glm::mat4(1.0f));
    std::vector<glm::mat4> posedG(nodes.size(), glm::mat4(1.0f));
    for (int i = 0; i < (int)nodes.size(); ++i) {
        const Node& n = nodes[i];
        bindG[i] = n.Parent >= 0 ? bindG[n.Parent] * n.BindLocal : n.BindLocal;

        glm::mat4 local = n.BindLocal;
        auto it = channels.find(n.Name);
        if (it != channels.end()) {
            const Channel& ch = it->second;
            glm::vec3 T, S, skew; glm::quat R; glm::vec4 persp;
            glm::decompose(n.BindLocal, S, R, T, skew, persp);
            R = glm::normalize(R);
            if (!ch.pos.empty()) {
                size_t i0 = FindKeyIndex(ch.pos, tick), i1 = std::min(i0 + 1, ch.pos.size() - 1);
                T = glm::mix(ch.pos[i0].v, ch.pos[i1].v, Factor(ch.pos[i0].t, ch.pos[i1].t, tick));
            }
            if (!ch.rot.empty()) {
                size_t i0 = FindKeyIndex(ch.rot, tick), i1 = std::min(i0 + 1, ch.rot.size() - 1);
                R = glm::normalize(glm::slerp(ch.rot[i0].v, ch.rot[i1].v, Factor(ch.rot[i0].t, ch.rot[i1].t, tick)));
            }
            if (!ch.scl.empty()) {
                size_t i0 = FindKeyIndex(ch.scl, tick), i1 = std::min(i0 + 1, ch.scl.size() - 1);
                S = glm::mix(ch.scl[i0].v, ch.scl[i1].v, Factor(ch.scl[i0].t, ch.scl[i1].t, tick));
            }
            local = glm::translate(glm::mat4(1.0f), T) * glm::mat4_cast(R) * glm::scale(glm::mat4(1.0f), S);
        }
        posedG[i] = n.Parent >= 0 ? posedG[n.Parent] * local : local;
    }

    std::vector<int> byId(boneCounter, -1);
    for (int i = 0; i < (int)nodes.size(); ++i) if (nodes[i].BoneId >= 0) byId[nodes[i].BoneId] = i;

    printf("\n%-3s %-26s %-6s %-5s %-5s %-10s %s\n",
           "ID", "bone", "chan", "npos", "nrot", "delta", "note");
    printf("%s\n", std::string(96, '-').c_str());
    int notAnimated = 0, noChan = 0;
    for (int id = 0; id < boneCounter; ++id) {
        int i = byId[id];
        if (i < 0) { printf("%-3d <no node>\n", id); continue; }
        const Node& n = nodes[i];
        glm::mat4 bindSkin = globalInverse * bindG[i] * n.BoneOffset;
        glm::mat4 posedSkin = globalInverse * posedG[i] * n.BoneOffset;
        float d = MaxAbsDiff(bindSkin, posedSkin);

        auto it = channels.find(n.Name);
        bool has = it != channels.end();
        int np = has ? (int)it->second.pos.size() : 0;
        int nr = has ? (int)it->second.rot.size() : 0;

        const char* note = "";
        if (!has) { note = "NO CHANNEL - stays at bind"; ++noChan; }
        else if (d < 1e-4f) { note = "NEVER MOVES (chan inert)"; ++notAnimated; }

        printf("%-3d %-26s %-6s %-5d %-5d %-10.6f %s\n",
               id, n.Name.c_str(), has ? "yes" : "NO", np, nr, d, note);
    }

    printf("\n== summary ==\n");
    printf("skinned bones with NO channel in this clip : %d / %d\n", noChan, boneCounter);
    printf("skinned bones whose skin matrix never moves: %d / %d\n", notAnimated, boneCounter);
    if (noChan || notAnimated)
        printf(">> THOSE BONES HOLD THEIR VERTICES AT BIND WHILE NEIGHBOURS MOVE <<\n");
    return 0;
}
