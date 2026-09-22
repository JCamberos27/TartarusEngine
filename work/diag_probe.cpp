// Localizes WHY the posed forward-skin tears (forward_skin_probe proved it does).
//
// bind skin_i = G^-1 * R_i * BoneOffset_i must equal one shared constant C, because
// bind forward-skin was bit-identical to the raw mesh. That only constrains
//     BoneOffset_i = inverse(R_i) * K     for ONE constant K shared by all bones.
// Bind pose CANNOT tell us whether K is the right constant. If K is wrong, bind stays
// perfectly clean while every posed frame tears - exactly the reported symptom.
//
// So this probe measures K directly, plus the two ways K (or R) could be wrong:
//   A) the mesh's own node transform was never folded in (Model::ProcessMesh skips
//      baking nodeTransform for skinned meshes - see `if (!skinned)`),
//   B) the base file's rest transforms disagree with the clip file's rest transforms,
//      so composing clip keys through the base tree mixes two reference frames.
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
#include <vector>

constexpr int MAX_BONE_INFLUENCE = 4;
constexpr int MAX_BONES = 512;

static glm::mat4 AiToGlm(const aiMatrix4x4& m) {
    return glm::mat4(m.a1, m.b1, m.c1, m.d1, m.a2, m.b2, m.c2, m.d2,
                      m.a3, m.b3, m.c3, m.d3, m.a4, m.b4, m.c4, m.d4);
}
static glm::quat AiToGlm(const aiQuaternion& q) { return glm::quat(q.w, q.x, q.y, q.z); }

static void PrintMat(const char* label, const glm::mat4& m) {
    printf("%-22s", label);
    for (int r = 0; r < 4; ++r) {
        printf(" [%7.3f %7.3f %7.3f %7.3f]", m[0][r], m[1][r], m[2][r], m[3][r]);
    }
    printf("\n");
}
static float MaxDiff(const glm::mat4& a, const glm::mat4& b) {
    float m = 0.0f;
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) m = std::max(m, std::fabs(a[c][r] - b[c][r]));
    return m;
}
static float TransLen(const glm::mat4& m) { return glm::length(glm::vec3(m[3])); }

struct Node { std::string Name; int Parent = -1; glm::mat4 BindLocal{1.0f}; int BoneId = -1; glm::mat4 BoneOffset{1.0f}; };
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
    Node node; node.Name = n->mName.C_Str(); node.Parent = parent;
    node.BindLocal = AiToGlm(n->mTransformation);
    int self = (int)nodes.size();
    nodes.push_back(node);
    for (unsigned i = 0; i < n->mNumChildren; ++i) ReadHierarchy(n->mChildren[i], self, nodes);
}

static void CollectGlobals(const aiNode* n, const glm::mat4& parent,
                           std::unordered_map<std::string, glm::mat4>& out) {
    glm::mat4 g = parent * AiToGlm(n->mTransformation);
    out.emplace(n->mName.C_Str(), g);
    for (unsigned i = 0; i < n->mNumChildren; ++i) CollectGlobals(n->mChildren[i], g, out);
}

static void ProcessNodeOrder(const aiNode* node, const aiScene* scene,
                             std::vector<aiMesh*>& meshes, std::vector<glm::mat4>& xforms) {
    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        meshes.push_back(scene->mMeshes[node->mMeshes[i]]);
        xforms.push_back(AiToGlm(node->mTransformation) * glm::mat4(1.0f));
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i) ProcessNodeOrder(node->mChildren[i], scene, meshes, xforms);
}

// Model::ProcessNode accumulates the transform down the tree; reproduce that properly.
static void ProcessNodeAccum(const aiNode* node, const aiScene* scene, const glm::mat4& parent,
                             std::vector<aiMesh*>& meshes, std::vector<glm::mat4>& xforms,
                             std::vector<std::string>& owners) {
    glm::mat4 local = parent * AiToGlm(node->mTransformation);
    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        meshes.push_back(scene->mMeshes[node->mMeshes[i]]);
        xforms.push_back(local);
        owners.push_back(node->mName.C_Str());
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        ProcessNodeAccum(node->mChildren[i], scene, local, meshes, xforms, owners);
}

static void ExtractBoneWeights(std::vector<Vtx>& vertices, aiMesh* mesh,
                               std::unordered_map<std::string, int>& boneInfoMap, int& boneCounter) {
    for (unsigned int bi = 0; bi < mesh->mNumBones; ++bi) {
        aiBone* bone = mesh->mBones[bi];
        std::string boneName = bone->mName.C_Str();
        int boneID;
        auto it = boneInfoMap.find(boneName);
        if (it == boneInfoMap.end()) {
            if (boneCounter >= MAX_BONES) continue;
            boneInfoMap[boneName] = boneCounter; boneID = boneCounter++;
        } else boneID = it->second;
        for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
            unsigned int vid = bone->mWeights[w].mVertexId;
            float weight = bone->mWeights[w].mWeight;
            if (vid >= vertices.size()) continue;
            Vtx& v = vertices[vid];
            int slot = 0;
            for (; slot < MAX_BONE_INFLUENCE; ++slot) if (v.BoneIDs[slot] < 0) break;
            if (slot == MAX_BONE_INFLUENCE) {
                int sm = 0;
                for (int s = 1; s < MAX_BONE_INFLUENCE; ++s) if (v.Weights[s] < v.Weights[sm]) sm = s;
                if (weight <= v.Weights[sm]) continue;
                slot = sm;
            }
            v.BoneIDs[slot] = boneID; v.Weights[slot] = weight;
        }
    }
    for (Vtx& v : vertices) {
        float sum = 0.0f;
        for (int s = 0; s < MAX_BONE_INFLUENCE; ++s) if (v.BoneIDs[s] >= 0) sum += v.Weights[s];
        if (sum > 0.0001f && std::fabs(sum - 1.0f) > 0.0001f)
            for (int s = 0; s < MAX_BONE_INFLUENCE; ++s) if (v.BoneIDs[s] >= 0) v.Weights[s] /= sum;
    }
}

static float SkinMaxEdge(const std::vector<Vtx>& verts, const std::vector<unsigned>& idx,
                         const std::vector<glm::mat4>& palette, int boneCounter) {
    std::vector<glm::vec3> p(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        const Vtx& v = verts[i];
        glm::mat4 skin(0.0f); float total = 0.0f;
        for (int s = 0; s < MAX_BONE_INFLUENCE; ++s) {
            if (v.BoneIDs[s] < 0) continue;
            int id = std::clamp(v.BoneIDs[s], 0, boneCounter - 1);
            skin += palette[id] * v.Weights[s];
            total += v.Weights[s];
        }
        if (total <= 0.0001f) skin = glm::mat4(1.0f);
        p[i] = glm::vec3(skin * glm::vec4(v.Position, 1.0f));
    }
    float mx = 0.0f;
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const glm::vec3 a = p[idx[i]], b = p[idx[i+1]], c = p[idx[i+2]];
        mx = std::max({mx, glm::length(b - a), glm::length(c - b), glm::length(a - c)});
    }
    return mx;
}

int main(int argc, char** argv) {
    std::string dir =
        "C:\\Users\\jacob\\OneDrive\\Documents\\Default Project\\TartarusEngine\\project\\assets\\fps\\AKS74U\\FirstPerson\\";
    std::string basePath = dir + "AKS-74U_A_FP_ADS.fbx";
    std::string clipPath = argc > 1 ? argv[1] : dir + "AKS-74U_A_FP_Idle.fbx";
    float tick = argc > 2 ? (float)atof(argv[2]) : 64.0f;

    Assimp::Importer ib, ic;
    const aiScene* B = Load(ib, basePath);
    const aiScene* C = Load(ic, clipPath);
    if (!B || !C || !C->mNumAnimations) return 1;

    glm::mat4 globalInverse = glm::inverse(AiToGlm(B->mRootNode->mTransformation));
    printf("base root transform:\n"); PrintMat("  root", AiToGlm(B->mRootNode->mTransformation));
    printf("GlobalInverse:\n");        PrintMat("  Ginv", globalInverse);

    // ---- A) mesh owning node transform (ProcessMesh skips this for skinned meshes) ----
    std::vector<aiMesh*> meshList; std::vector<glm::mat4> meshXf; std::vector<std::string> owners;
    ProcessNodeAccum(B->mRootNode, B, glm::mat4(1.0f), meshList, meshXf, owners);
    printf("\n[A] meshes=%d\n", (int)meshList.size());
    for (size_t i = 0; i < meshList.size(); ++i)
        printf("    mesh %zu owned by node '%s'  transformIsIdentity=%s  transLen=%.4f\n",
               i, owners[i].c_str(),
               MaxDiff(meshXf[i], glm::mat4(1.0f)) < 1e-6f ? "YES" : "NO", TransLen(meshXf[i]));

    // ---- rest globals for both files, by node name ----
    std::unordered_map<std::string, glm::mat4> restBase, restClip;
    CollectGlobals(B->mRootNode, glm::mat4(1.0f), restBase);
    CollectGlobals(C->mRootNode, glm::mat4(1.0f), restClip);

    aiAnimation* anim = C->mAnimations[0];
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

    // ---- B) base vs clip REST transforms, per node name ----
    printf("\n[B] base vs clip REST transforms (same node name)\n");
    int shared = 0, differKeyed = 0, differNonKeyed = 0;
    float worst = 0.0f; std::string worstName; bool worstKeyed = false;
    for (const auto& kv : restBase) {
        auto it = restClip.find(kv.first);
        if (it == restClip.end()) continue;
        ++shared;
        float d = MaxDiff(kv.second, it->second);
        if (d < 1e-5f) continue;
        bool keyed = channels.count(kv.first) > 0;
        if (keyed) ++differKeyed; else ++differNonKeyed;
        if (d > worst) { worst = d; worstName = kv.first; worstKeyed = keyed; }
    }
    printf("    nodes present in both      : %d / %d base nodes\n", shared, (int)restBase.size());
    printf("    REST differs, node KEYED   : %d\n", differKeyed);
    printf("    REST differs, node NOT keyed: %d   <-- these silently take the BASE value\n", differNonKeyed);
    printf("    worst rest mismatch        : '%s' d=%.5f keyed=%s\n",
           worstName.c_str(), worst, worstKeyed ? "yes" : "NO");

    // ---- build base mesh + weights ----
    std::unordered_map<std::string, int> boneInfoMap;
    std::unordered_map<std::string, glm::mat4> offsets;
    int boneCounter = 0;
    std::vector<Vtx> verts; std::vector<unsigned> indices; size_t base0 = 0;
    for (aiMesh* mesh : meshList) {
        std::vector<Vtx> local(mesh->mNumVertices);
        for (unsigned i = 0; i < mesh->mNumVertices; ++i)
            local[i].Position = glm::vec3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);
        ExtractBoneWeights(local, mesh, boneInfoMap, boneCounter);
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            std::string nm = mesh->mBones[b]->mName.C_Str();
            if (!offsets.count(nm)) offsets[nm] = AiToGlm(mesh->mBones[b]->mOffsetMatrix);
        }
        for (unsigned f = 0; f < mesh->mNumFaces; ++f)
            for (unsigned j = 0; j < mesh->mFaces[f].mNumIndices; ++j)
                indices.push_back(base0 + mesh->mFaces[f].mIndices[j]);
        base0 += mesh->mNumVertices;
        verts.insert(verts.end(), local.begin(), local.end());
    }

    std::vector<Node> nodes; ReadHierarchy(B->mRootNode, -1, nodes);
    for (Node& n : nodes) {
        auto it = boneInfoMap.find(n.Name);
        if (it != boneInfoMap.end()) { n.BoneId = it->second; n.BoneOffset = offsets[n.Name]; }
    }
    std::vector<glm::mat4> restG(nodes.size(), glm::mat4(1.0f));
    for (int i = 0; i < (int)nodes.size(); ++i)
        restG[i] = nodes[i].Parent >= 0 ? restG[nodes[i].Parent] * nodes[i].BindLocal : nodes[i].BindLocal;

    // ---- C) K_i = R_i * BoneOffset_i  (must be constant; is it the RIGHT constant?) ----
    printf("\n[C] K_i = RestGlobal_i * BoneOffset_i   (constancy check)\n");
    glm::mat4 K0(0.0f); bool haveK0 = false;
    float kSpread = 0.0f; glm::mat4 Kref;
    int shown = 0;
    for (const Node& n : nodes) {
        if (n.BoneId < 0) continue;
        glm::mat4 K = restG[std::find_if(nodes.begin(), nodes.end(),
                       [&](const Node& x){ return x.Name == n.Name; }) - nodes.begin()] * n.BoneOffset;
        if (!haveK0) { Kref = K; haveK0 = true; }
        kSpread = std::max(kSpread, MaxDiff(K, Kref));
        if (shown++ < 4) { char lbl[64]; snprintf(lbl, sizeof(lbl), "  K[%s]", n.Name.c_str()); PrintMat(lbl, K); }
    }
    printf("    max spread across all bones : %.8f  (0 => K is one shared constant)\n", kSpread);
    PrintMat("    K (shared)", Kref);
    PrintMat("    Ginv * K  (=bind palette)", globalInverse * Kref);
    printf("    NOTE: bind clean only proves K is CONSTANT. It cannot prove K is correct.\n");

    // ---- D) posed globals: composed on BASE tree vs CLIP tree ----
    std::vector<glm::mat4> posedBase(nodes.size(), glm::mat4(1.0f));
    std::vector<glm::mat4> posedClip(nodes.size(), glm::mat4(1.0f));
    std::vector<int> clipNodeIdx(nodes.size(), -1);
    for (int i = 0; i < (int)nodes.size(); ++i) {
        const Node& n = nodes[i];
        glm::mat4 bind = n.BindLocal;
        glm::mat4 local = bind, localClip = bind;
        auto it = channels.find(n.Name);
        if (it != channels.end()) {
            const Channel& ch = it->second;
            glm::vec3 T, S, sk; glm::quat R; glm::vec4 pp;
            glm::decompose(bind, S, R, T, sk, pp); R = glm::normalize(R);
            glm::vec3 Tc = T, Sc = S; glm::quat Rc = R;
            auto rc = restClip.find(n.Name);
            if (rc != restClip.end()) {
                glm::vec3 Ts, Ss, skk; glm::quat Rs; glm::vec4 ppp;
                glm::decompose(rc->second, Ss, Rs, Ts, skk, ppp);
                Tc = Ts; Sc = Ss; Rc = glm::normalize(Rs);
            }
            if (!ch.pos.empty()) { size_t a = FindKeyIndex(ch.pos, tick), b = std::min(a+1, ch.pos.size()-1);
                T = glm::mix(ch.pos[a].v, ch.pos[b].v, Factor(ch.pos[a].t, ch.pos[b].t, tick)); Tc = T; }
            if (!ch.rot.empty()) { size_t a = FindKeyIndex(ch.rot, tick), b = std::min(a+1, ch.rot.size()-1);
                R = glm::normalize(glm::slerp(ch.rot[a].v, ch.rot[b].v, Factor(ch.rot[a].t, ch.rot[b].t, tick))); Rc = R; }
            if (!ch.scl.empty()) { size_t a = FindKeyIndex(ch.scl, tick), b = std::min(a+1, ch.scl.size()-1);
                S = glm::mix(ch.scl[a].v, ch.scl[b].v, Factor(ch.scl[a].t, ch.scl[b].t, tick)); Sc = S; }
            local = glm::translate(glm::mat4(1.0f), T) * glm::mat4_cast(R) * glm::scale(glm::mat4(1.0f), S);
            localClip = glm::translate(glm::mat4(1.0f), Tc) * glm::mat4_cast(Rc) * glm::scale(glm::mat4(1.0f), Sc);
        }
        posedBase[i] = nodes[i].Parent >= 0 ? posedBase[nodes[i].Parent] * local : local;
        posedClip[i] = nodes[i].Parent >= 0 ? posedClip[nodes[i].Parent] * localClip : localClip;
    }
    float posedDiff = 0.0f; std::string pdName;
    for (int i = 0; i < (int)nodes.size(); ++i) {
        float d = MaxDiff(posedBase[i], posedClip[i]);
        if (d > posedDiff) { posedDiff = d; pdName = nodes[i].Name; }
    }
    printf("\n[D] posed globals: BASE-tree vs CLIP-tree rest fallback\n");
    printf("    max difference %.6f on '%s'\n", posedDiff, pdName.c_str());

    // ---- E) forward skin variants ----
    auto buildPalette = [&](const std::vector<glm::mat4>& posedG) {
        std::vector<glm::mat4> pal(boneCounter, glm::mat4(1.0f));
        for (const Node& n : nodes)
            if (n.BoneId >= 0 && n.BoneId < boneCounter) {
                size_t i = 0; for (; i < nodes.size(); ++i) if (nodes[i].Name == n.Name) break;
                pal[n.BoneId] = globalInverse * posedG[i] * n.BoneOffset;
            }
        return pal;
    };

    glm::mat4 M = meshXf.empty() ? glm::mat4(1.0f) : meshXf[0];
    printf("\n[E] owning mesh-node world transform M:\n");
    for (int r = 0; r < 4; ++r)
        printf("    [ %8.4f %8.4f %8.4f %8.4f ]\n",
               M[0][r], M[1][r], M[2][r], M[3][r]);
    {
        float det = glm::determinant(glm::mat3(M));
        float sx = glm::length(glm::vec3(M[0])), sy = glm::length(glm::vec3(M[1])), sz = glm::length(glm::vec3(M[2]));
        printf("    det=%.6f  scale=(%.5f %.5f %.5f)\n", det, sx, sy, sz);
    }

    auto stats = [&](const char* label, const std::vector<Vtx>& vv, const std::vector<glm::mat4>& pal) {
        std::vector<glm::vec3> p(vv.size());
        for (size_t i = 0; i < vv.size(); ++i) {
            const Vtx& v = vv[i];
            glm::mat4 skin(0.0f); float total = 0.0f;
            for (int s = 0; s < MAX_BONE_INFLUENCE; ++s) {
                if (v.BoneIDs[s] < 0) continue;
                int id = v.BoneIDs[s]; if (id >= boneCounter) id = boneCounter - 1;
                skin += pal[id] * v.Weights[s];
                total += v.Weights[s];
            }
            if (total <= 0.0001f) skin = glm::mat4(1.0f);
            p[i] = glm::vec3(skin * glm::vec4(v.Position, 1.0f));
        }
        glm::vec3 mn(1e30f), mx(-1e30f);
        for (auto& q : p) { mn = glm::min(mn, q); mx = glm::max(mx, q); }
        std::vector<float> e; e.reserve(indices.size() / 3);
        double sum = 0.0; float mx2 = 0.0f;
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            glm::vec3 a = p[indices[i]], b = p[indices[i+1]], c = p[indices[i+2]];
            float l = std::max({ glm::length(a-b), glm::length(b-c), glm::length(c-a) });
            e.push_back(l); sum += l; mx2 = std::max(mx2, l);
        }
        std::sort(e.begin(), e.end());
        printf("    %-34s bbox %6.3f x %6.3f x %6.3f | max %7.4f p99 %7.4f mean %7.4f\n",
               label, mx.x-mn.x, mx.y-mn.y, mx.z-mn.z, mx2,
               e[(size_t)(e.size()*0.99)], sum / (double)e.size());
    };

    printf("\n[E] forward-skin stats   (Blender frame-64 ref: bbox 0.556 x 0.815 x 0.334, max 0.0230 p99 0.0140 mean 0.0050)\n");
    std::vector<Vtx> vertsM = verts;
    for (Vtx& v : vertsM) v.Position = glm::vec3(M * glm::vec4(v.Position, 1.0f));
    auto palBind = buildPalette(restG);
    auto palPose = buildPalette(posedBase);
    std::vector<glm::mat4> identPal(boneCounter, glm::mat4(1.0f));
    stats("RAW mesh (identity palette)", verts, identPal);
    stats("BIND  V0  (engine-exact)", verts, palBind);
    stats("BIND  V1  (x mesh-node M)", vertsM, palBind);
    stats("POSED V0  (engine-exact)", verts, palPose);
    stats("POSED V1  (x mesh-node M)", vertsM, palPose);
    stats("POSED V2  (clip-tree rest)", verts, buildPalette(posedClip));

    printf("\n== summary ==\n");
    printf("K constant           : %s\n", kSpread < 1e-5f ? "YES" : "NO");
    printf("mesh node transform  : %s\n",
           meshXf.empty() || MaxDiff(meshXf[0], glm::mat4(1.0f)) < 1e-6f ? "IDENTITY (V1==V0 expected)"
                                                                          : "NON-IDENTITY - candidate");
    printf("base/clip rest differ: %d nodes (%d unkeyed)\n", differKeyed + differNonKeyed, differNonKeyed);
    return 0;
}
