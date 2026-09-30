// DECISIVE PROBE: forward-skin the real arms mesh on the CPU, exactly the way the
// vertex shader does, and measure whether the result is torn or clean.
//
// Why this is the fork the investigation doc asked for:
//   * At bind pose every skin matrix collapses to the same constant (GlobalInverse),
//     so a clean bind pose only proves weights sum to 1 - it proves nothing about
//     which bone each vertex is bound to. See clip_coverage_probe output:
//     GlobalInverse T == (0,0,0).
//   * full_skin_probe proved each of the 52 matrices is individually clean.
//   * bone_match_probe proved channel NAMES match.
//   * Nothing has ever measured the COMBINATION: matrix x weight x vertex.
//
// So:
//   If forward skinning comes out CLEAN  -> CPU path fully exonerated, the bug is
//       downstream (SSBO upload bytes / GPU), i.e. doc candidate #1.
//   If forward skinning comes out TORN   -> found it, and the worst-edge dump below
//       names the exact vertices/bones responsible, i.e. doc candidate #2.
//
// Ground truth from Blender (Test 1 in FPS_ANIMATION_INVESTIGATION.md, action
// A_FP_Idle at frame 64): bbox 55.6 x 81.5 x 33.4 cm, max edge 2.3 cm,
// p99 edge 1.4 cm, mean edge 0.5 cm. Edge lengths are rotation/translation
// invariant, so they compare directly across Blender vs engine spaces.
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
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

constexpr int MAX_BONE_INFLUENCE = 4;
constexpr int MAX_BONES = 512;

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

struct Vtx {
    glm::vec3 Position{0.0f};
    int BoneIDs[MAX_BONE_INFLUENCE] = {-1, -1, -1, -1};
    float Weights[MAX_BONE_INFLUENCE] = {0, 0, 0, 0};
};

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

// Model::CollectNodeGlobals - emplace keeps the FIRST occurrence of a duplicate name.
static void CollectNodeGlobals(const aiNode* n, const glm::mat4& parent,
                               std::unordered_map<std::string, glm::mat4>& out) {
    glm::mat4 g = parent * AiToGlm(n->mTransformation);
    out.emplace(n->mName.C_Str(), g);
    for (unsigned i = 0; i < n->mNumChildren; ++i) CollectNodeGlobals(n->mChildren[i], g, out);
}

// Model::ProcessNode - drives ProcessMesh in tree order, not in mMeshes index order.
static void ProcessNodeOrder(const aiNode* node, const aiScene* scene, std::vector<aiMesh*>& out) {
    for (unsigned i = 0; i < node->mNumMeshes; ++i) out.push_back(scene->mMeshes[node->mMeshes[i]]);
    for (unsigned i = 0; i < node->mNumChildren; ++i) ProcessNodeOrder(node->mChildren[i], scene, out);
}

// Model::ExtractBoneWeights, transcribed line-for-line including largest-weight
// eviction and the renormalization pass.
static void ExtractBoneWeights(std::vector<Vtx>& vertices, aiMesh* mesh,
                               std::unordered_map<std::string, int>& boneInfoMap,
                               int& boneCounter) {
    for (unsigned int boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
        aiBone* bone = mesh->mBones[boneIdx];
        std::string boneName = bone->mName.C_Str();
        int boneID;
        auto it = boneInfoMap.find(boneName);
        if (it == boneInfoMap.end()) {
            if (boneCounter >= MAX_BONES) continue;
            boneInfoMap[boneName] = boneCounter;
            boneID = boneCounter++;
        } else {
            boneID = it->second;
        }
        for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
            unsigned int vertexId = bone->mWeights[w].mVertexId;
            float weight = bone->mWeights[w].mWeight;
            if (vertexId >= vertices.size()) continue;
            Vtx& v = vertices[vertexId];
            int slot = 0;
            for (; slot < MAX_BONE_INFLUENCE; ++slot)
                if (v.BoneIDs[slot] < 0) break;
            if (slot == MAX_BONE_INFLUENCE) {
                int smallest = 0;
                for (int s = 1; s < MAX_BONE_INFLUENCE; ++s)
                    if (v.Weights[s] < v.Weights[smallest]) smallest = s;
                if (weight <= v.Weights[smallest]) continue;
                slot = smallest;
            }
            v.BoneIDs[slot] = boneID;
            v.Weights[slot] = weight;
        }
    }
    for (Vtx& v : vertices) {
        float sum = 0.0f;
        for (int s = 0; s < MAX_BONE_INFLUENCE; ++s)
            if (v.BoneIDs[s] >= 0) sum += v.Weights[s];
        if (sum > 0.0001f && std::fabs(sum - 1.0f) > 0.0001f)
            for (int s = 0; s < MAX_BONE_INFLUENCE; ++s)
                if (v.BoneIDs[s] >= 0) v.Weights[s] /= sum;
    }
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

    glm::mat4 globalInverse = glm::inverse(AiToGlm(sceneBase->mRootNode->mTransformation));
    std::unordered_map<std::string, glm::mat4> importGlobals;
    CollectNodeGlobals(sceneBase->mRootNode, glm::mat4(1.0f), importGlobals);

    // --- build vertices + weights in the engine's exact processing order ---
    std::unordered_map<std::string, int> boneInfoMap;   // name -> BoneId
    std::unordered_map<std::string, glm::mat4> boneOffset; // name -> offset matrix
    int boneCounter = 0;

    std::vector<aiMesh*> meshOrder;
    ProcessNodeOrder(sceneBase->mRootNode, sceneBase, meshOrder);

    std::vector<Vtx> vertices;
    std::vector<unsigned int> indices;
    size_t meshVertexBase = 0;
    for (aiMesh* mesh : meshOrder) {
        std::vector<Vtx> local(mesh->mNumVertices);
        for (unsigned i = 0; i < mesh->mNumVertices; ++i)
            local[i].Position = glm::vec3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);

        std::unordered_map<std::string, int> localMap = boneInfoMap;
        int counterBefore = boneCounter;
        ExtractBoneWeights(local, mesh, boneInfoMap, boneCounter);

        // record offsets for newly seen bones (first registration wins)
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            std::string nm = mesh->mBones[b]->mName.C_Str();
            if (!boneOffset.count(nm)) boneOffset[nm] = AiToGlm(mesh->mBones[b]->mOffsetMatrix);
        }
        (void)localMap; (void)counterBefore;

        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            for (unsigned j = 0; j < face.mNumIndices; ++j)
                indices.push_back(meshVertexBase + face.mIndices[j]);
        }
        meshVertexBase += mesh->mNumVertices;
        vertices.insert(vertices.end(), local.begin(), local.end());
    }

    // --- hierarchy + node BoneId assignment (Model::ReadHierarchy) ---
    std::vector<Node> nodes;
    ReadHierarchy(sceneBase->mRootNode, -1, nodes);
    for (Node& n : nodes) {
        auto it = boneInfoMap.find(n.Name);
        if (it != boneInfoMap.end()) { n.BoneId = it->second; n.BoneOffset = boneOffset[n.Name]; }
    }

    // --- every BoneId must be claimed by exactly one node, else its palette slot
    //     is never written by EvaluatePose and stays at the identity forever ---
    std::vector<int> claimedBy(boneCounter, 0);
    for (const Node& n : nodes)
        if (n.BoneId >= 0 && n.BoneId < boneCounter) claimedBy[n.BoneId]++;
    int orphans = 0;
    for (int id = 0; id < boneCounter; ++id)
        if (claimedBy[id] == 0) { printf("!! BoneId %d claimed by NO node - palette slot stuck at identity\n", id); ++orphans; }
        else if (claimedBy[id] > 1) printf("!! BoneId %d claimed by %d nodes - last one wins in EvaluatePose\n", id, claimedBy[id]);

    // --- posed palette (Model::EvaluatePose) ---
    aiAnimation* anim = sceneClip->mAnimations[0];
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

    std::vector<glm::mat4> posedG(nodes.size(), glm::mat4(1.0f));
    std::vector<glm::mat4> bindG(nodes.size(), glm::mat4(1.0f));
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
                size_t a = FindKeyIndex(ch.pos, tick), b = std::min(a + 1, ch.pos.size() - 1);
                T = glm::mix(ch.pos[a].v, ch.pos[b].v, Factor(ch.pos[a].t, ch.pos[b].t, tick));
            }
            if (!ch.rot.empty()) {
                size_t a = FindKeyIndex(ch.rot, tick), b = std::min(a + 1, ch.rot.size() - 1);
                R = glm::normalize(glm::slerp(ch.rot[a].v, ch.rot[b].v, Factor(ch.rot[a].t, ch.rot[b].t, tick)));
            }
            if (!ch.scl.empty()) {
                size_t a = FindKeyIndex(ch.scl, tick), b = std::min(a + 1, ch.scl.size() - 1);
                S = glm::mix(ch.scl[a].v, ch.scl[b].v, Factor(ch.scl[a].t, ch.scl[b].t, tick));
            }
            local = glm::translate(glm::mat4(1.0f), T) * glm::mat4_cast(R) * glm::scale(glm::mat4(1.0f), S);
        }
        posedG[i] = n.Parent >= 0 ? posedG[n.Parent] * local : local;
    }

    std::vector<glm::mat4> posed(boneCounter, glm::mat4(1.0f));
    std::vector<glm::mat4> bindPalette(boneCounter, glm::mat4(1.0f));
    std::vector<bool> posedWritten(boneCounter, false);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const Node& n = nodes[i];
        if (n.BoneId < 0 || n.BoneId >= boneCounter) continue;
        posed[n.BoneId] = globalInverse * posedG[i] * n.BoneOffset; // Model::EvaluatePose
        posedWritten[n.BoneId] = true;
        auto g = importGlobals.find(n.Name);
        if (g != importGlobals.end())
            bindPalette[n.BoneId] = globalInverse * g->second * n.BoneOffset;
    }

    // --- per-vertex influence statistics ---
    int hist[5] = {0, 0, 0, 0, 0};
    int zeroInf = 0, badRange = 0;
    float sumMin = 1e30f, sumMax = -1e30f;
    int badSum = 0;
    std::unordered_set<int> usedIds;
    for (const Vtx& v : vertices) {
        int n = 0; float sum = 0.0f;
        for (int s = 0; s < MAX_BONE_INFLUENCE; ++s) {
            if (v.BoneIDs[s] < 0) continue;
            ++n; sum += v.Weights[s];
            if (v.BoneIDs[s] >= boneCounter) ++badRange;
            usedIds.insert(v.BoneIDs[s]);
        }
        hist[std::min(n, 4)]++;
        if (n == 0) ++zeroInf;
        if (n > 0) { sumMin = std::min(sumMin, sum); sumMax = std::max(sumMax, sum);
                     if (sum < 0.99f || sum > 1.01f) ++badSum; }
    }

    printf("clip / tick        : %s @ %.1f\n", clipPath.c_str(), tick);
    printf("meshes / verts / tris: %d / %d / %d\n", (int)meshOrder.size(), (int)vertices.size(),
           (int)(indices.size() / 3));
    printf("boneCounter        : %d\n", boneCounter);
    printf("orphan palette slots: %d\n", orphans);
    printf("influences per vert : 0:%d 1:%d 2:%d 3:%d 4+:%d\n", hist[0], hist[1], hist[2], hist[3], hist[4]);
    printf("verts w/ 0 weights : %d\n", zeroInf);
    printf("weight sum range   : %.5f .. %.5f   (outside [0.99,1.01]: %d)\n", sumMin, sumMax, badSum);
    printf("BoneIDs out of range: %d\n", badRange);
    printf("distinct BoneIDs used: %d / %d%s\n", (int)usedIds.size(), boneCounter,
           usedIds.size() == (size_t)boneCounter ? "" : "   << NOT ALL PALETTE SLOTS REFERENCED");
    printf("GlobalInverse is identity: %s\n",
           glm::length(glm::vec3(globalInverse[0]) - glm::vec3(1, 0, 0)) < 1e-5f &&
           glm::length(globalInverse[3]) < 1e-5f ? "YES" : "no");

    // --- forward skin, exactly as ModelVertex.glsl does ---
    auto skinAll = [&](const std::vector<glm::mat4>& palette, std::vector<glm::vec3>& out) {
        out.resize(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            const Vtx& v = vertices[i];
            glm::mat4 skin(0.0f); float total = 0.0f;
            for (int s = 0; s < MAX_BONE_INFLUENCE; ++s) {
                if (v.BoneIDs[s] < 0) continue;
                int id = std::clamp(v.BoneIDs[s], 0, boneCounter - 1);
                skin += palette[id] * v.Weights[s];
                total += v.Weights[s];
            }
            if (total <= 0.0001f) skin = glm::mat4(1.0f);
            out[i] = glm::vec3(skin * glm::vec4(v.Position, 1.0f));
        }
    };

    auto report = [&](const char* label, const std::vector<glm::vec3>& p) {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const glm::vec3& q : p) { lo = glm::min(lo, q); hi = glm::max(hi, q); }
        glm::vec3 ext = hi - lo;
        std::vector<float> extS = {ext.x, ext.y, ext.z};
        std::sort(extS.begin(), extS.end(), std::greater<float>());
        std::vector<float> edges;
        edges.reserve(indices.size() / 2);
        float sum = 0.0f;
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const glm::vec3& a = p[indices[i]];
            const glm::vec3& b = p[indices[i + 1]];
            const glm::vec3& c = p[indices[i + 2]];
            float e[] = {glm::length(b - a), glm::length(c - b), glm::length(a - c)};
            for (float v : e) { edges.push_back(v); sum += v; }
        }
        std::sort(edges.begin(), edges.end());
        float mx = edges.empty() ? 0.0f : edges.back();
        float p99 = edges.empty() ? 0.0f : edges[(size_t)(edges.size() * 0.99)];
        float mean = edges.empty() ? 0.0f : sum / edges.size();
        printf("\n[%s]\n", label);
        printf("  bbox extents (sorted) : %.4f  %.4f  %.4f\n", extS[0], extS[1], extS[2]);
        printf("  max / p99 / mean edge : %.4f / %.4f / %.4f\n", mx, p99, mean);
        return edges.empty() ? 0.0f : mx;
    };

    std::vector<glm::vec3> raw(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) raw[i] = vertices[i].Position;
    std::vector<glm::vec3> bindSkinned, posedSkinned;
    skinAll(bindPalette, bindSkinned);
    skinAll(posed, posedSkinned);

    float rawMax = report("RAW mesh positions", raw);
    float bindMax = report("BIND pose forward-skin (expect == RAW, rigid)", bindSkinned);
    float posedMax = report("POSED forward-skin @tick 64", posedSkinned);

    printf("\n  Blender ground truth (A_FP_Idle @ frame 64):\n");
    printf("    bbox 0.5560 0.8150 0.3340   max 0.0230  p99 0.0140  mean 0.0050\n");

    // A correct bind-pose palette is a single shared constant, so bind-skinned edge
    // lengths must be identical to the raw mesh's (a rigid transform changes none).
    printf("\n  bind == raw (rigid): %s (max %.6f vs %.6f)\n",
           std::fabs(bindMax - rawMax) < 1e-5f ? "YES" : "NO", bindMax, rawMax);

    // Verdict.
    const float REF_MAX = 0.0230f;
    printf("\n== verdict ==\n");
    if (posedMax > REF_MAX * 4.0f) {
        printf("POSED forward-skin is TORN (%.4f vs reference %.4f).\n", posedMax, REF_MAX);
        printf(">> BUG IS ON THE CPU / VERTEX-DATA SIDE (doc candidate #2) <<\n\n");

        // Name the worst offenders: which vertices stretch, and what are they bound to.
        struct EdgeRec { float len; unsigned a, b; };
        std::vector<EdgeRec> recs;
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            unsigned tri[3] = {indices[i], indices[i + 1], indices[i + 2]};
            for (int e = 0; e < 3; ++e) {
                unsigned u = tri[e], v = tri[(e + 1) % 3];
                recs.push_back({glm::length(posedSkinned[u] - posedSkinned[v]), u, v});
            }
        }
        std::sort(recs.begin(), recs.end(), [](const EdgeRec& x, const EdgeRec& y) { return x.len > y.len; });
        printf("worst 12 edges (len, rest-length, vtxA binds, vtxB binds):\n");
        for (int i = 0; i < 12 && i < (int)recs.size(); ++i) {
            const EdgeRec& r = recs[i];
            auto binds = [&](unsigned idx) {
                std::string s;
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                    if (vertices[idx].BoneIDs[k] < 0) continue;
                    if (!s.empty()) s += "+";
                    char buf[48];
                    snprintf(buf, sizeof(buf), "%d(%.2f)", vertices[idx].BoneIDs[k], vertices[idx].Weights[k]);
                    s += buf;
                }
                return s.empty() ? std::string("NONE") : s;
            };
            printf("  %8.4f (rest %7.4f)  v%u[%s]  v%u[%s]\n",
                   r.len, glm::length(raw[r.a] - raw[r.b]), r.a, binds(r.a).c_str(),
                   r.b, binds(r.b).c_str());
        }
    } else {
        printf("POSED forward-skin is CLEAN (%.4f vs reference %.4f).\n", posedMax, REF_MAX);
        printf(">> CPU + VERTEX DATA FULLY EXONERATED - bug is downstream:\n");
        printf("   SSBO upload bytes / GPU read (doc candidate #1) <<\n");
    }
    return 0;
}
