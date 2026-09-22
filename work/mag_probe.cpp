// DECISIVE PROBE for "the spare magazine doesn't show up in the reload animation".
//
// Both magazines live inside the ONE weapon mesh (`aks74u` in the .blend), as two vertex
// groups of exactly 3190 verts each: `magazine` (the one in the magwell) and `mag2` (the
// spare the left hand swaps in). There is no separate magazine object to be missing, so the
// only ways it can fail to appear are:
//   a) `mag2` never gets a palette slot / never gets written by EvaluatePose  -> stays parked
//   b) `mag2` is animated, but the skinned geometry never moves out of its rest position
//   c) it moves correctly  -> the engine path is exonerated and the fault is placement/culling
//
// So: forward-skin the real weapon mesh exactly as ModelVertex.glsl does, split the result by
// DOMINANT bone, and report where the `mag2` subset sits versus the `magazine` subset and the
// whole-gun centroid, at several ticks. Ground truth from Blender (A_W_Tac_Reload, world cm):
//   tick 0    mag2 is ~60 cm from root (parked at the hip pouch), off-screen
//   tick 85   mag2 is ~13 cm from root, ~5 cm from the installed magazine (in the hand)
//   tick 170  back to the pouch
//
//   mag_probe [clip.fbx] [tick ...]
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

constexpr int MAX_BONE_INFLUENCE = 4;
constexpr int MAX_BONES = 512;
static bool g_NoJoin = false;

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
    // The engine's exact flag set (Model.cpp: Triangulate/FlipUVs/GlobalScale always,
    // Normals+Tangents, LimitBoneWeights, and OptimizeGraph = Join+OptimizeMeshes).
    // --nojoin drops JoinIdenticalVertices|OptimizeMeshes to prove it is the culprit.
    unsigned int flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale |
                         aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
                         aiProcess_LimitBoneWeights;
    if (!g_NoJoin) flags |= aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes;
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

static void CollectNodeGlobals(const aiNode* n, const glm::mat4& parent,
                               std::unordered_map<std::string, glm::mat4>& out) {
    glm::mat4 g = parent * AiToGlm(n->mTransformation);
    out.emplace(n->mName.C_Str(), g);
    for (unsigned i = 0; i < n->mNumChildren; ++i) CollectNodeGlobals(n->mChildren[i], g, out);
}

static void ProcessNodeOrder(const aiNode* node, const aiScene* scene, std::vector<aiMesh*>& out) {
    for (unsigned i = 0; i < node->mNumMeshes; ++i) out.push_back(scene->mMeshes[node->mMeshes[i]]);
    for (unsigned i = 0; i < node->mNumChildren; ++i) ProcessNodeOrder(node->mChildren[i], scene, out);
}

// Model::ExtractBoneWeights, transcribed including largest-weight eviction + renormalization.
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

// Centroid + bbox extent of the subset whose DOMINANT (largest) influence is `want`.
// Passing want = -1 selects every vertex.
struct Sub { glm::vec3 cen, ext; int n; };
static Sub Subset(const std::vector<glm::vec3>& p, const std::vector<Vtx>& verts, int want) {
    Sub s{glm::vec3(0), glm::vec3(0), 0};
    glm::vec3 lo(1e30f), hi(-1e30f), sum(0);
    for (size_t i = 0; i < p.size(); ++i) {
        int best = -1; float bw = 0.0f;
        for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
            if (verts[i].BoneIDs[k] < 0) continue;
            if (verts[i].Weights[k] > bw) { bw = verts[i].Weights[k]; best = verts[i].BoneIDs[k]; }
        }
        if (want >= 0 && best != want) continue;
        sum += p[i]; lo = glm::min(lo, p[i]); hi = glm::max(hi, p[i]); ++s.n;
    }
    if (!s.n) return s;
    s.cen = sum / (float)s.n; s.ext = hi - lo;
    return s;
}

int main(int argc, char** argv) {
    std::string dir =
        "C:\\Users\\jacob\\OneDrive\\Documents\\Default Project\\TartarusEngine\\project\\assets\\fps\\AKS74U\\Weapon\\";
    std::string base = dir + "AKS-74U_A_W_ADS.fbx";

    bool nojoin = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--nojoin") nojoin = true;
        else args.push_back(argv[i]);
    }
    g_NoJoin = nojoin;

    // --scan: is the mag2 corruption unique to the weapon, or does JoinIdenticalVertices break
    // skin weights elsewhere too? Check every shipped FBX under FirstPerson/ and Weapon/.
    if (!args.empty() && args[0] == "--scan") {
        const std::string root =
            "C:\\Users\\jacob\\OneDrive\\Documents\\Default Project\\TartarusEngine\\project\\assets\\fps\\AKS74U\\";
        std::vector<std::string> files;
        for (const char* sub : {"FirstPerson\\", "Weapon\\"}) {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(root + sub, ec))
                if (e.path().extension() == ".fbx") files.push_back(e.path().string());
        }
        std::sort(files.begin(), files.end());
        printf("=== out-of-range skin weights under the engine's exact flag set ===\n");
        long grandOor = 0;
        for (const std::string& f : files) {
            Assimp::Importer im;
            im.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
            im.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
            unsigned int flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale |
                                 aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
                                 aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices |
                                 aiProcess_OptimizeMeshes;
            const aiScene* s = im.ReadFile(f, flags);
            std::string leaf = f.substr(f.find_last_of("\\/") + 1);
            if (!s) { printf("  %-40s FAILED\n", leaf.c_str()); continue; }
            long verts = 0, oor = 0;
            std::map<std::string, long> bad;
            for (unsigned m = 0; m < s->mNumMeshes; ++m) {
                const aiMesh* mm = s->mMeshes[m];
                verts += (long)mm->mNumVertices;
                for (unsigned b = 0; b < mm->mNumBones; ++b) {
                    const aiBone* bb = mm->mBones[b];
                    long o = 0;
                    for (unsigned k = 0; k < bb->mNumWeights; ++k)
                        if (bb->mWeights[k].mVertexId >= mm->mNumVertices) ++o;
                    if (o) { bad[bb->mName.C_Str()] += o; oor += o; }
                }
            }
            grandOor += oor;
            if (!oor) continue;
            std::string names;
            for (auto& kv : bad) { if (!names.empty()) names += ","; names += kv.first; }
            printf("  %-40s verts=%-7ld DROPPED=%-7ld bones=%zu [%s]\n",
                   leaf.c_str(), verts, oor, bad.size(), names.c_str());
        }
        printf("  -- files checked: %zu, total dropped weights: %ld\n", files.size(), grandOor);
        return 0;
    }

    std::string clipPath = args.empty() ? dir + "AKS-74U_A_W_Tac_Reload.fbx" : args[0];
    std::vector<float> ticks;
    for (size_t i = 1; i < args.size(); ++i) ticks.push_back((float)atof(args[i].c_str()));
    if (ticks.empty()) ticks = {0.0f, 85.0f, 170.0f};
    printf("import flags        : %s\n",
           g_NoJoin ? "engine WITHOUT JoinIdenticalVertices/OptimizeMeshes (--nojoin)"
                    : "engine EXACT (JoinIdenticalVertices + OptimizeMeshes)");

    Assimp::Importer impBase, impClip;
    const aiScene* sceneBase = Load(impBase, base);
    const aiScene* sceneClip = Load(impClip, clipPath);
    if (!sceneBase || !sceneClip || !sceneClip->mNumAnimations) return 1;

    glm::mat4 globalInverse = glm::inverse(AiToGlm(sceneBase->mRootNode->mTransformation));
    std::unordered_map<std::string, glm::mat4> importGlobals;
    CollectNodeGlobals(sceneBase->mRootNode, glm::mat4(1.0f), importGlobals);

    std::unordered_map<std::string, int> boneInfoMap;
    std::unordered_map<std::string, glm::mat4> boneOffset;
    int boneCounter = 0;

    std::vector<aiMesh*> meshOrder;
    ProcessNodeOrder(sceneBase->mRootNode, sceneBase, meshOrder);

    // --- RAW aiBone blocks, straight from assimp, before any engine-side extraction ---
    // If a bone's weights exist here but not in the extracted vertices, the loss is in
    // ExtractBoneWeights (slot eviction / out-of-range vertex ids), not in the asset.
    printf("=== raw aiBone blocks (as assimp reports them) ===\n");
    for (size_t m = 0; m < meshOrder.size(); ++m) {
        const aiMesh* mm = meshOrder[m];
        printf(" mesh[%d] verts=%u bones=%u\n", (int)m, mm->mNumVertices, mm->mNumBones);
        unsigned long long totalW = 0;
        for (unsigned b = 0; b < mm->mNumBones; ++b) {
            const aiBone* bb = mm->mBones[b];
            totalW += bb->mNumWeights;
            float wmin = 1e30f, wmax = -1e30f;
            unsigned oor = 0;
            std::unordered_map<int, int> dup;
            for (unsigned k = 0; k < bb->mNumWeights; ++k) {
                float w = bb->mWeights[k].mWeight;
                wmin = std::min(wmin, w); wmax = std::max(wmax, w);
                if (bb->mWeights[k].mVertexId >= mm->mNumVertices) ++oor;
                dup[(int)bb->mWeights[k].mVertexId]++;
            }
            int multi = 0;
            for (auto& kv : dup) if (kv.second > 1) ++multi;
            printf("   bone %-20s weights=%-6u distinct=%-6u repeated=%-6u outOfRange=%-4u w=[%.6f..%.6f]\n",
                   bb->mName.C_Str(), bb->mNumWeights, (unsigned)dup.size(), multi, oor, wmin, wmax);
        }
        printf("   total weights=%llu over %u verts (%.3f per vert)\n",
               totalW, mm->mNumVertices, (double)totalW / (double)mm->mNumVertices);
    }
    printf("\n");

    // Same file, ZERO post-processing: if mag2's ids are in range here but not above, the
    // corruption is manufactured by an aiProcess_* step; if they are out of range too, it is
    // baked into the .fbx itself and only a re-export can fix it.
    {
        Assimp::Importer rawImp;
        rawImp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
        rawImp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        const aiScene* raw = rawImp.ReadFile(base, 0u);
        printf("=== same file with NO post-processing ===\n");
        if (!raw) {
            printf("  FAILED: %s\n", rawImp.GetErrorString());
        } else {
            std::vector<aiMesh*> order;
            ProcessNodeOrder(raw->mRootNode, raw, order);
            for (size_t m = 0; m < order.size(); ++m) {
                const aiMesh* mm = order[m];
                printf(" mesh[%d] verts=%u\n", (int)m, mm->mNumVertices);
                for (unsigned b = 0; b < mm->mNumBones; ++b) {
                    const aiBone* bb = mm->mBones[b];
                    unsigned oor = 0;
                    for (unsigned k = 0; k < bb->mNumWeights; ++k)
                        if (bb->mWeights[k].mVertexId >= mm->mNumVertices) ++oor;
                    if (bb->mName.C_Str() == std::string("mag2") ||
                        bb->mName.C_Str() == std::string("magazine") || oor)
                        printf("   bone %-20s weights=%-6u outOfRange=%u\n",
                               bb->mName.C_Str(), bb->mNumWeights, oor);
                }
            }
        }
        printf("\n");
    }

    // Which post-processing flag corrupts mag2? Bisect: base = everything the engine always
    // passes, then add each of the three optional ones alone and in combination.
    {
        const unsigned baseFlags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale |
                              aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;
        struct Case { const char* name; unsigned flags; };
        std::vector<Case> cases = {
            {"base (engine, no optional flags)", baseFlags},
            {"base + JoinIdenticalVertices",     baseFlags | aiProcess_JoinIdenticalVertices},
            {"base + OptimizeMeshes",            baseFlags | aiProcess_OptimizeMeshes},
            {"base + LimitBoneWeights",          baseFlags | aiProcess_LimitBoneWeights},
            {"base + Join + Optimize (engine!)", baseFlags | aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes},
            {"base + Join + Limit",              baseFlags | aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights},
            {"base + all three (engine exact)",  baseFlags | aiProcess_JoinIdenticalVertices |
                                                     aiProcess_OptimizeMeshes | aiProcess_LimitBoneWeights},
        };
        printf("=== flag bisect: mag2 out-of-range weights ===\n");
        for (const Case& c : cases) {
            Assimp::Importer im;
            im.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
            im.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
            const aiScene* s = im.ReadFile(base, c.flags);
            std::string verdict = "FAILED";
            if (s) {
                std::vector<aiMesh*> order;
                ProcessNodeOrder(s->mRootNode, s, order);
                char buf[192];
                snprintf(buf, sizeof(buf), "meshes=%d verts=%d", (int)order.size(), 0);
                unsigned long long totOor = 0, totW = 0, mag2w = 0;
                int verts = 0;
                for (aiMesh* mm : order) {
                    verts += (int)mm->mNumVertices;
                    for (unsigned b = 0; b < mm->mNumBones; ++b) {
                        const aiBone* bb = mm->mBones[b];
                        for (unsigned k = 0; k < bb->mNumWeights; ++k) {
                            ++totW;
                            if (bb->mWeights[k].mVertexId >= mm->mNumVertices) ++totOor;
                        }
                        if (bb->mName.C_Str() == std::string("mag2"))
                            for (unsigned k = 0; k < bb->mNumWeights; ++k)
                                if (bb->mWeights[k].mVertexId < mm->mNumVertices) ++mag2w;
                    }
                }
                snprintf(buf, sizeof(buf), "meshes=%d verts=%d  mag2 IN-RANGE=%llu  total oor=%llu/%llu",
                         (int)order.size(), verts, mag2w, totOor, totW);
                verdict = buf;
            } else {
                verdict = std::string("FAILED ") + im.GetErrorString();
            }
            printf(" %-36s %s\n", c.name, verdict.c_str());
        }
        printf("\n");
    }

    std::vector<Vtx> vertices;
    size_t meshVertexBase = 0;
    for (aiMesh* mesh : meshOrder) {
        std::vector<Vtx> local(mesh->mNumVertices);
        for (unsigned i = 0; i < mesh->mNumVertices; ++i)
            local[i].Position = glm::vec3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);
        ExtractBoneWeights(local, mesh, boneInfoMap, boneCounter);
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            std::string nm = mesh->mBones[b]->mName.C_Str();
            if (!boneOffset.count(nm)) boneOffset[nm] = AiToGlm(mesh->mBones[b]->mOffsetMatrix);
        }
        meshVertexBase += mesh->mNumVertices;
        vertices.insert(vertices.end(), local.begin(), local.end());
    }

    std::vector<Node> nodes;
    ReadHierarchy(sceneBase->mRootNode, -1, nodes);
    for (Node& n : nodes) {
        auto it = boneInfoMap.find(n.Name);
        if (it != boneInfoMap.end()) { n.BoneId = it->second; n.BoneOffset = boneOffset[n.Name]; }
    }

    // Every palette slot must be claimed by exactly one node, or EvaluatePose never writes it.
    std::vector<int> claimedBy(boneCounter, 0);
    for (const Node& n : nodes)
        if (n.BoneId >= 0 && n.BoneId < boneCounter) claimedBy[n.BoneId]++;

    printf("clip              : %s\n", clipPath.c_str());
    printf("meshes / verts    : %d / %d\n", (int)meshOrder.size(), (int)vertices.size());
    printf("\nbone palette (name -> id, claimed by N nodes):\n");
    std::vector<std::pair<std::string, int>> ids(boneInfoMap.begin(), boneInfoMap.end());
    std::sort(ids.begin(), ids.end(), [](auto& a, auto& b) { return a.second < b.second; });
    for (auto& kv : ids) {
        int id = kv.second;
        printf("  id %-3d %-20s claimed=%d%s\n", id, kv.first.c_str(), claimedBy[id],
               claimedBy[id] != 1 ? "   <--- NOT EXACTLY ONE NODE" : "");
    }
    auto idOf = [&](const char* nm) -> int {
        auto it = boneInfoMap.find(nm);
        return it == boneInfoMap.end() ? -1 : it->second;
    };
    const int idMag2 = idOf("mag2");
    const int idMag = idOf("magazine");
    const int idRoot = idOf("root");
    printf("\n mag2 id=%d  magazine id=%d  root id=%d\n", idMag2, idMag, idRoot);

    // Does the clip carry a channel for mag2 at all?
    aiAnimation* anim = sceneClip->mAnimations[0];
    std::unordered_map<std::string, Channel> channels;
    bool hasMag2 = false;
    for (unsigned c = 0; c < anim->mNumChannels; ++c) {
        aiNodeAnim* ch = anim->mChannels[c];
        Channel out; out.BoneName = ch->mNodeName.C_Str();
        if (out.BoneName == "mag2") hasMag2 = true;
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
    printf("clip channels=%u  mag2 channel present: %s  keys=%zu\n",
           anim->mNumChannels, hasMag2 ? "YES" : "NO",
           hasMag2 ? channels["mag2"].pos.size() : (size_t)0);

    auto paletteAt = [&](float tick, std::vector<glm::mat4>& out) {
        std::vector<glm::mat4> posedG(nodes.size(), glm::mat4(1.0f));
        for (int i = 0; i < (int)nodes.size(); ++i) {
            const Node& n = nodes[i];
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
        out.assign(boneCounter, glm::mat4(1.0f));
        for (size_t i = 0; i < nodes.size(); ++i) {
            const Node& n = nodes[i];
            if (n.BoneId < 0 || n.BoneId >= boneCounter) continue;
            out[n.BoneId] = globalInverse * posedG[i] * n.BoneOffset;
        }
    };

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

    auto describe = [&](const char* label, const std::vector<glm::vec3>& p) {
        Sub whole = Subset(p, vertices, -1);
        Sub m2 = Subset(p, vertices, idMag2);
        Sub mg = Subset(p, vertices, idMag);
        printf("\n[%s]\n", label);
        printf("  ALL        n=%-6d centroid=(%9.4f,%9.4f,%9.4f)\n", whole.n, whole.cen.x, whole.cen.y, whole.cen.z);
        printf("  magazine   n=%-6d centroid=(%9.4f,%9.4f,%9.4f)  ext=(%.4f,%.4f,%.4f)\n",
               mg.n, mg.cen.x, mg.cen.y, mg.cen.z, mg.ext.x, mg.ext.y, mg.ext.z);
        printf("  mag2/SPARE n=%-6d centroid=(%9.4f,%9.4f,%9.4f)  ext=(%.4f,%.4f,%.4f)\n",
               m2.n, m2.cen.x, m2.cen.y, m2.cen.z, m2.ext.x, m2.ext.y, m2.ext.z);
        if (m2.n && mg.n)
            // Vertex-space separation, NOT the engine's node-level displacement. This probe skins
            // raw mesh vertices with globalInverse*world*mOffsetMatrix and never bakes each mesh's
            // node transform the way Model::ProcessMesh does, so a constant offset-matrix mismatch
            // between the `magazine` and `mag2` bones shows up here as extra separation that the
            // engine does not render. For the number that matters - how far the rig JUMPS when it
            // reverts to bind - use work/bind_probe.cpp (node globals vs node globals), which
            // reports 121.8 mm.
            printf("  >> |mag2 - magazine| = %.4f   (vertex space - see comment; for the jump use "
                   "bind_probe)\n", glm::length(m2.cen - mg.cen));
        if (m2.n && whole.n)
            printf("  >> |mag2 - gun centroid| = %.4f\n", glm::length(m2.cen - whole.cen));
    };

    std::vector<glm::vec3> raw(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) raw[i] = vertices[i].Position;
    describe("RAW mesh positions (rest geometry)", raw);

    // --- why does mag2 dominate nothing? ---------------------------------------------
    // Report, per bone of interest, every vertex that carries an influence at all: how many
    // slots that bone occupies on a vertex (duplicates), how large its weight actually is,
    // and which bone ends up dominant instead.
    auto audit = [&](const char* name, int want) {
        if (want < 0) { printf("\n[audit] %s: NOT A BONE IN THIS MODEL\n", name); return; }
        int n = 0, dup = 0, ge99 = 0, ge50 = 0, dominant = 0;
        std::vector<float> ws;
        std::unordered_map<std::string, int> domHist;
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (size_t i = 0; i < vertices.size(); ++i) {
            const Vtx& v = vertices[i];
            int slots = 0; float w = 0.0f; int best = -1; float bw = 0.0f;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                if (v.BoneIDs[k] < 0) continue;
                if (v.BoneIDs[k] == want) { ++slots; w += v.Weights[k]; }
                if (v.Weights[k] > bw) { bw = v.Weights[k]; best = v.BoneIDs[k]; }
            }
            if (!slots) continue;
            ++n;
            if (slots > 1) ++dup;
            ws.push_back(w);
            if (w >= 0.99f) ++ge99;
            if (w >= 0.50f) ++ge50;
            if (best == want) ++dominant;
            std::string dn = "(none)";
            for (auto& kv : ids) if (kv.second == best) dn = kv.first;
            domHist[dn]++;
            lo = glm::min(lo, raw[i]); hi = glm::max(hi, raw[i]);
        }
        std::sort(ws.begin(), ws.end());
        printf("\n[audit] %-12s vertices carrying an influence: %d\n", name, n);
        printf("   dominant bone is '%s' on : %d\n", name, dominant);
        printf("   occupies >1 slot (dup)   : %d\n", dup);
        printf("   combined weight >= 0.99  : %d\n", ge99);
        printf("   combined weight >= 0.50  : %d\n", ge50);
        if (!ws.empty())
            printf("   weight  min=%.4f  p50=%.4f  max=%.4f\n",
                   ws.front(), ws[ws.size() / 2], ws.back());
        printf("   rest-space bbox          : (%.4f,%.4f,%.4f) .. (%.4f,%.4f,%.4f)\n",
               lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
        std::vector<std::pair<std::string, int>> hist(domHist.begin(), domHist.end());
        std::sort(hist.begin(), hist.end(), [](auto& a, auto& b) { return a.second > b.second; });
        printf("   dominant-bone histogram  :");
        int shown = 0;
        for (auto& h : hist) { if (shown++ == 8) break; printf(" %s=%d", h.first.c_str(), h.second); }
        printf("\n");
    };
    audit("mag2", idMag2);
    audit("magazine", idMag);
    audit("root", idRoot);

    // Which bone ends up owning the spare magazine's geometry once JoinIdenticalVertices has
    // thrown its mag2 weights away? Take mag2's vertex positions from a load WITHOUT the
    // corrupting flag, then find those same positions in the engine-flag load and read off the
    // bone that landed on them. Same flags minus Join => identical vertex positions.
    {
        const unsigned refFlags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale |
                                  aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace;
        Assimp::Importer ri;
        ri.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
        ri.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        const aiScene* ref = ri.ReadFile(base, refFlags);
        auto key = [](const glm::vec3& v) -> uint64_t {
            auto q = [](float f) -> int64_t { return (int64_t)llround((double)f * 1e5); };
            uint64_t h = 1469598103934665603ULL;
            int64_t c[3] = {q(v.x), q(v.y), q(v.z)};
            for (int i = 0; i < 3; ++i) h = (h ^ (uint64_t)c[i]) * 1099511628211ULL;
            return h;
        };
        std::unordered_set<uint64_t> sparePos;
        if (ref) {
            for (unsigned m = 0; m < ref->mNumMeshes; ++m) {
                const aiMesh* mm = ref->mMeshes[m];
                for (unsigned b = 0; b < mm->mNumBones; ++b) {
                    if (mm->mBones[b]->mName.C_Str() != std::string("mag2")) continue;
                    for (unsigned k = 0; k < mm->mBones[b]->mNumWeights; ++k) {
                        unsigned vid = mm->mBones[b]->mWeights[k].mVertexId;
                        if (vid >= mm->mNumVertices) continue;
                        sparePos.insert(key(glm::vec3(mm->mVertices[vid].x, mm->mVertices[vid].y,
                                                      mm->mVertices[vid].z)));
                    }
                }
            }
        }
        printf("\n[where did the spare magazine's %zu distinct vertices go?]\n", sparePos.size());
        if (sparePos.empty()) {
            printf("   could not establish a reference (base load failed)\n");
        } else {
            std::map<std::string, long> owner;
            long matched = 0;
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (size_t i = 0; i < vertices.size(); ++i) {
                if (!sparePos.count(key(raw[i]))) continue;
                ++matched;
                lo = glm::min(lo, raw[i]); hi = glm::max(hi, raw[i]);
                int best = -1; float bw = 0.0f;
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
                    if (vertices[i].BoneIDs[k] < 0) continue;
                    if (vertices[i].Weights[k] > bw) { bw = vertices[i].Weights[k]; best = vertices[i].BoneIDs[k]; }
                }
                std::string nm = "(NO INFLUENCE - identity skin, frozen at rest)";
                for (auto& kv : ids) if (kv.second == best) nm = kv.first;
                owner[nm]++;
            }
            printf("   matched post-join vertices : %ld\n", matched);
            printf("   their rest-space bbox      : (%.4f,%.4f,%.4f) .. (%.4f,%.4f,%.4f)\n",
                   lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
            printf("   owning bone (dominant)     :\n");
            for (auto& kv : owner)
                printf("      %-46s %ld\n", kv.first.c_str(), kv.second);
            glm::vec3 c = (lo + hi) * 0.5f;
            const long owned = owner.count("mag2") ? owner["mag2"] : 0;
            printf("   => rest centre (%.4f,%.4f,%.4f), %.3f m from the model origin.\n",
                   c.x, c.y, c.z, glm::length(c));
            if (owned > 0) {
                printf("   => mag2 owns %ld of these: the spare magazine exists and follows its clip.\n",
                       owned);
            } else {
                printf("   => mag2 owns NONE of these: the spare magazine is frozen at rest and will\n"
                       "      never animate.\n");
            }
        }
    }

    for (float t : ticks) {
        std::vector<glm::mat4> pal;
        paletteAt(t, pal);
        std::vector<glm::vec3> p;
        skinAll(pal, p);
        char label[128];
        snprintf(label, sizeof(label), "POSED forward-skin @ tick %.0f", t);
        describe(label, p);
    }
    return 0;
}
