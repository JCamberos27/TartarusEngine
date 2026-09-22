// Standalone Assimp probe - replicates Model.cpp's import flags and AttachClip/EvaluatePose math
// for a handful of bones, completely independent of the engine/editor/UI, to compare directly
// against Blender's live evaluation of the same rig+action.
#define GLM_ENABLE_EXPERIMENTAL
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>

static glm::mat4 AiToGlm(const aiMatrix4x4& m) {
    return glm::mat4(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4);
}
static glm::vec3 AiToGlm(const aiVector3D& v) { return glm::vec3(v.x, v.y, v.z); }
static glm::quat AiToGlm(const aiQuaternion& q) { return glm::quat(q.w, q.x, q.y, q.z); }

struct Node {
    std::string Name;
    int Parent = -1;
    glm::mat4 BindLocal{1.0f};
};

struct PosKey { glm::vec3 v; float t; };
struct RotKey { glm::quat v; float t; };
struct SclKey { glm::vec3 v; float t; };

struct Channel {
    std::string BoneName;
    std::vector<PosKey> pos;
    std::vector<RotKey> rot;
    std::vector<SclKey> scl;
};

template <typename Keys>
size_t FindKeyIndex(const Keys& keys, float t) {
    if (keys.size() <= 1) return 0;
    size_t i = 0;
    for (; i + 1 < keys.size(); ++i) if (keys[i + 1].t > t) break;
    return i;
}
static float Factor(float a, float b, float t) {
    float d = b - a;
    if (d <= 0.0f) return 0.0f;
    float f = (t - a) / d;
    if (f < 0) f = 0; if (f > 1) f = 1;
    return f;
}

const aiScene* Load(Assimp::Importer& imp, const std::string& path) {
    imp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    unsigned int flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale |
                          aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
                          aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices |
                          aiProcess_OptimizeMeshes;
    const aiScene* scene = imp.ReadFile(path, flags);
    if (!scene) {
        std::cerr << "FAILED to load " << path << ": " << imp.GetErrorString() << "\n";
    }
    return scene;
}

void ReadHierarchy(const aiNode* n, int parent, std::vector<Node>& nodes) {
    Node node;
    node.Name = n->mName.C_Str();
    node.Parent = parent;
    node.BindLocal = AiToGlm(n->mTransformation);
    int self = (int)nodes.size();
    nodes.push_back(node);
    for (unsigned i = 0; i < n->mNumChildren; ++i) ReadHierarchy(n->mChildren[i], self, nodes);
}

int main() {
    std::string base = "C:\\Users\\jacob\\Desktop\\TartarusEngine\\.claude\\worktrees\\fps-first-person-animation\\project\\assets\\fps\\AKS74U\\FirstPerson\\AKS-74U_A_FP_ADS.fbx";
    std::string clipPath = "C:\\Users\\jacob\\Desktop\\TartarusEngine\\.claude\\worktrees\\fps-first-person-animation\\project\\assets\\fps\\AKS74U\\FirstPerson\\AKS-74U_A_FP_Idle.fbx";

    Assimp::Importer impBase, impClip;
    const aiScene* sceneBase = Load(impBase, base);
    const aiScene* sceneClip = Load(impClip, clipPath);
    if (!sceneBase || !sceneClip) return 1;

    std::vector<Node> nodes;
    ReadHierarchy(sceneBase->mRootNode, -1, nodes);
    std::unordered_map<std::string, int> nameToIdx;
    for (int i = 0; i < (int)nodes.size(); ++i) nameToIdx[nodes[i].Name] = i;

    std::cout << "Base file node count: " << nodes.size() << "\n";
    if (sceneClip->mNumAnimations == 0) { std::cerr << "No animations in clip file\n"; return 1; }
    aiAnimation* anim = sceneClip->mAnimations[0];
    std::cout << "Clip: '" << anim->mName.C_Str() << "' duration=" << anim->mDuration
              << " ticksPerSecond=" << anim->mTicksPerSecond << " numChannels=" << anim->mNumChannels << "\n";

    std::map<std::string, Channel> channels;
    for (unsigned c = 0; c < anim->mNumChannels; ++c) {
        aiNodeAnim* ch = anim->mChannels[c];
        Channel out;
        out.BoneName = ch->mNodeName.C_Str();
        for (unsigned k = 0; k < ch->mNumPositionKeys; ++k)
            out.pos.push_back({AiToGlm(ch->mPositionKeys[k].mValue), (float)ch->mPositionKeys[k].mTime});
        for (unsigned k = 0; k < ch->mNumRotationKeys; ++k)
            out.rot.push_back({AiToGlm(ch->mRotationKeys[k].mValue), (float)ch->mRotationKeys[k].mTime});
        for (unsigned k = 0; k < ch->mNumScalingKeys; ++k)
            out.scl.push_back({AiToGlm(ch->mScalingKeys[k].mValue), (float)ch->mScalingKeys[k].mTime});
        channels[out.BoneName] = std::move(out);
    }

    // Sample at tick=64 (matching Blender frame 64, assuming ticksPerSecond == scene FPS == 1 tick/frame)
    const float sampleTick = 64.0f;

    std::vector<glm::mat4> globals(nodes.size(), glm::mat4(1.0f));
    for (int i = 0; i < (int)nodes.size(); ++i) {
        const Node& n = nodes[i];
        glm::mat4 local = n.BindLocal;
        auto it = channels.find(n.Name);
        if (it != channels.end()) {
            const Channel& ch = it->second;
            glm::vec3 skew; glm::vec4 persp;
            glm::vec3 bindS, bindT; glm::quat bindR;
            glm::decompose(n.BindLocal, bindS, bindR, bindT, skew, persp);

            glm::vec3 T = bindT, S = bindS; glm::quat R = bindR;
            if (!ch.pos.empty()) {
                size_t ii = FindKeyIndex(ch.pos, sampleTick);
                size_t jj = std::min(ii + 1, ch.pos.size() - 1);
                T = glm::mix(ch.pos[ii].v, ch.pos[jj].v, Factor(ch.pos[ii].t, ch.pos[jj].t, sampleTick));
            }
            if (!ch.rot.empty()) {
                size_t ii = FindKeyIndex(ch.rot, sampleTick);
                size_t jj = std::min(ii + 1, ch.rot.size() - 1);
                R = glm::normalize(glm::slerp(ch.rot[ii].v, ch.rot[jj].v, Factor(ch.rot[ii].t, ch.rot[jj].t, sampleTick)));
            }
            if (!ch.scl.empty()) {
                size_t ii = FindKeyIndex(ch.scl, sampleTick);
                size_t jj = std::min(ii + 1, ch.scl.size() - 1);
                S = glm::mix(ch.scl[ii].v, ch.scl[jj].v, Factor(ch.scl[ii].t, ch.scl[jj].t, sampleTick));
            }
            local = glm::translate(glm::mat4(1.0f), T) * glm::mat4_cast(R) * glm::scale(glm::mat4(1.0f), S);
        }
        globals[i] = n.Parent >= 0 ? globals[n.Parent] * local : local;
    }

    const char* watch[] = {"root", "pelvis", "spine_05", "clavicle_r", "upperarm_r",
                            "upperarm_twist_01_r", "upperarm_twist_02_r", "lowerarm_r",
                            "lowerarm_twist_01_r", "hand_r"};
    for (const char* name : watch) {
        auto it = nameToIdx.find(name);
        if (it == nameToIdx.end()) { std::cout << name << ": NOT FOUND in base hierarchy\n"; continue; }
        glm::vec3 t = glm::vec3(globals[it->second][3]);
        glm::vec3 c0 = glm::vec3(globals[it->second][0]);
        glm::vec3 c1 = glm::vec3(globals[it->second][1]);
        glm::vec3 c2 = glm::vec3(globals[it->second][2]);
        bool hasChan = channels.count(name) > 0;
        printf("%-24s hasChan=%s globalT=(%.4f, %.4f, %.4f) colLens=(%.4f, %.4f, %.4f)\n",
               name, hasChan ? "Y" : "N", t.x, t.y, t.z,
               glm::length(c0), glm::length(c1), glm::length(c2));
    }
    return 0;
}
