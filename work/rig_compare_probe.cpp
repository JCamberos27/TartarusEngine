// Compares two rigs exactly as the engine's importer sees them: every node the reference has,
// its local and global transform in the candidate, and whether each skinned bone's
// global * offset agrees with its mesh node (a well-formed bind).
//   rig_compare_probe <reference.fbx> <candidate.fbx>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

static glm::mat4 G(const aiMatrix4x4& m) {
    glm::mat4 t;
    t[0][0] = m.a1; t[1][0] = m.a2; t[2][0] = m.a3; t[3][0] = m.a4;
    t[0][1] = m.b1; t[1][1] = m.b2; t[2][1] = m.b3; t[3][1] = m.b4;
    t[0][2] = m.c1; t[1][2] = m.c2; t[2][2] = m.c3; t[3][2] = m.c4;
    t[0][3] = m.d1; t[1][3] = m.d2; t[2][3] = m.d3; t[3][3] = m.d4;
    return t;
}
static float D(const glm::mat4& a, const glm::mat4& b) {
    float d = 0; for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) d = std::max(d, std::abs(a[c][r] - b[c][r])); return d;
}
struct Rig { const aiScene* S; std::map<std::string, glm::mat4> L, W; std::map<std::string, std::string> P; };
static void Walk(const aiNode* n, const glm::mat4& p, Rig& r) {
    glm::mat4 l = G(n->mTransformation), w = p * l;
    r.L[n->mName.C_Str()] = l; r.W[n->mName.C_Str()] = w;
    r.P[n->mName.C_Str()] = n->mParent ? n->mParent->mName.C_Str() : "";
    for (unsigned i = 0; i < n->mNumChildren; ++i) Walk(n->mChildren[i], w, r);
}
static const aiScene* Load(Assimp::Importer& imp, const char* path) {
    imp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    return imp.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale | aiProcess_LimitBoneWeights);
}
int main(int argc, char** argv) {
    if (argc < 3) return 1;
    Assimp::Importer ia, ib;
    Rig a{Load(ia, argv[1])}, b{Load(ib, argv[2])};
    if (!a.S || !b.S) { printf("load failed\n"); return 1; }
    Walk(a.S->mRootNode, glm::mat4(1), a); Walk(b.S->mRootNode, glm::mat4(1), b);
    int missing = 0, parentDiff = 0; float worstL = 0, worstW = 0; std::string wl, ww;
    for (auto& [name, l] : a.L) {
        if (!b.L.count(name)) { if (++missing <= 10) printf("missing in candidate: %s\n", name.c_str()); continue; }
        if (a.P[name] != b.P[name]) { if (++parentDiff <= 10) printf("parent differs: %s (%s vs %s)\n", name.c_str(), a.P[name].c_str(), b.P[name].c_str()); }
        float dl = D(l, b.L[name]), dw = D(a.W[name], b.W[name]);
        if (dl > worstL) { worstL = dl; wl = name; }
        if (dw > worstW) { worstW = dw; ww = name; }
    }
    printf("nodes ref %zu cand %zu, missing %d, parent diffs %d\n", a.L.size(), b.L.size(), missing, parentDiff);
    printf("worst local diff %.6f (%s), worst global diff %.6f (%s)\n", worstL, wl.c_str(), worstW, ww.c_str());
    for (const char* n : {"root", "pelvis", "head", "hand_r", "ik_hand_gun", "hand_l", "ik_hand_l"}) {
        if (!a.W.count(n) || !b.W.count(n)) continue;
        glm::vec3 pa(a.W[n][3]), pb(b.W[n][3]);
        printf("  %-12s ref (%.4f %.4f %.4f) cand (%.4f %.4f %.4f) local d %.6f\n", n, pa.x, pa.y, pa.z, pb.x, pb.y, pb.z, D(a.L[n], b.L[n]));
    }
    for (unsigned mi = 0; mi < b.S->mNumMeshes; ++mi) {
        const aiMesh* m = b.S->mMeshes[mi];
        printf("cand mesh '%s': %u verts, %u bones\n", m->mName.C_Str(), m->mNumVertices, m->mNumBones);
        glm::mat4 c0; float worst = 0; std::string wb;
        for (unsigned bi = 0; bi < m->mNumBones; ++bi) {
            const aiBone* bo = m->mBones[bi];
            glm::mat4 c = b.W[bo->mName.C_Str()] * G(bo->mOffsetMatrix);
            if (bi == 0) c0 = c; else { float d = D(c, c0); if (d > worst) { worst = d; wb = bo->mName.C_Str(); } }
        }
        printf("  bind consistency: worst %.6f (%s)\n", worst, wb.c_str());
    }
    return 0;
}
