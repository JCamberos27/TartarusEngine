// Female Quantum parts import lying on their back / head floating off (outfit session, 2026-09-27).
//
// For each skinned mesh: the mesh node's global transform N, the first bone's bind residual
// C = GlobalInverse * BoneGlobal * Offset, and the bind-pose skinned bounds of the mesh with the
// engine's two bake choices (bake = N, bake = I). The right choice puts the body upright (tallest
// along +Y) at the same place as the male parts.
//
//   female_bake_probe <file.fbx> [more.fbx...]
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/config.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

static glm::mat4 AiToGlm(const aiMatrix4x4& m) {
    glm::mat4 t;
    t[0][0] = m.a1; t[1][0] = m.a2; t[2][0] = m.a3; t[3][0] = m.a4;
    t[0][1] = m.b1; t[1][1] = m.b2; t[2][1] = m.b3; t[3][1] = m.b4;
    t[0][2] = m.c1; t[1][2] = m.c2; t[2][2] = m.c3; t[3][2] = m.c4;
    t[0][3] = m.d1; t[1][3] = m.d2; t[2][3] = m.d3; t[3][3] = m.d4;
    return t;
}

static bool NearlyEqual(const glm::mat4& a, const glm::mat4& b) {
    float scale = 1.0f;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) scale = std::max(scale, std::max(std::abs(a[c][r]), std::abs(b[c][r])));
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > 1e-3f * scale) return false;
    return true;
}

static std::map<std::string, glm::mat4> g_Globals;
static void Collect(const aiNode* n, const glm::mat4& p) {
    glm::mat4 g = p * AiToGlm(n->mTransformation);
    g_Globals.emplace(n->mName.C_Str(), g);
    for (unsigned i = 0; i < n->mNumChildren; ++i) Collect(n->mChildren[i], g);
}

static void PrintM(const char* label, const glm::mat4& m) {
    std::printf("    %s\n", label);
    for (int r = 0; r < 4; ++r)
        std::printf("      % 9.4f % 9.4f % 9.4f % 9.4f\n", m[0][r], m[1][r], m[2][r], m[3][r]);
}

static glm::mat4 GI;
static void Walk(const aiScene* s, const aiNode* n, const glm::mat4& p) {
    glm::mat4 N = p * AiToGlm(n->mTransformation);
    for (unsigned mi = 0; mi < n->mNumMeshes; ++mi) {
        const aiMesh* m = s->mMeshes[n->mMeshes[mi]];
        std::printf("  mesh '%s' on node '%s' (%u verts, %u bones)\n", m->mName.C_Str(), n->mName.C_Str(),
                    m->mNumVertices, m->mNumBones);
        if (!m->mNumBones) continue;
        PrintM("N (mesh node global)", N);
        const aiBone* b0 = m->mBones[0];
        glm::mat4 C = GI * g_Globals[b0->mName.C_Str()] * AiToGlm(b0->mOffsetMatrix);
        PrintM("C (first bone residual)", C);
        std::printf("    C==N: %d   C==I: %d   C==GI*N: %d\n", NearlyEqual(C, N), NearlyEqual(C, glm::mat4(1)),
                    NearlyEqual(C, GI * N));
        // Bind-pose skinned bounds for both bakes.
        for (int choice = 0; choice < 2; ++choice) {
            glm::mat4 bake = choice == 0 ? N : glm::mat4(1.0f);
            std::vector<glm::vec3> acc(m->mNumVertices, glm::vec3(0));
            std::vector<float> w(m->mNumVertices, 0.0f);
            for (unsigned bi = 0; bi < m->mNumBones; ++bi) {
                const aiBone* b = m->mBones[bi];
                glm::mat4 pal = GI * g_Globals[b->mName.C_Str()] * AiToGlm(b->mOffsetMatrix);
                for (unsigned k = 0; k < b->mNumWeights; ++k) {
                    unsigned vi = b->mWeights[k].mVertexId;
                    const aiVector3D& v = m->mVertices[vi];
                    acc[vi] += b->mWeights[k].mWeight * glm::vec3(pal * bake * glm::vec4(v.x, v.y, v.z, 1));
                    w[vi] += b->mWeights[k].mWeight;
                }
            }
            glm::vec3 lo(1e9f), hi(-1e9f);
            for (unsigned vi = 0; vi < m->mNumVertices; ++vi) {
                if (w[vi] <= 0) continue;
                glm::vec3 p3 = acc[vi] / w[vi];
                lo = glm::min(lo, p3); hi = glm::max(hi, p3);
            }
            std::printf("    bake=%s  min(%.3f %.3f %.3f) max(%.3f %.3f %.3f)\n", choice == 0 ? "N" : "I",
                        lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
        }
    }
    for (unsigned i = 0; i < n->mNumChildren; ++i) Walk(s, n->mChildren[i], N);
}

// The engine keeps ONE offset per bone name per model (the first mesh to reference it wins).
// Simulate ProcessMesh in node order with that sharing: rule 0 = working-tree fix (own C==N ->
// bake I), rule 1 = that choice corrected by inv(sharedOffset) * ownOffset.
static std::map<std::string, glm::mat4> g_Shared;
static void Simulate(const aiScene* s, const aiNode* n, const glm::mat4& p, int rule) {
    glm::mat4 N = p * AiToGlm(n->mTransformation);
    for (unsigned mi = 0; mi < n->mNumMeshes; ++mi) {
        const aiMesh* m = s->mMeshes[n->mMeshes[mi]];
        if (!m->mNumBones) continue;
        const aiBone* b0 = m->mBones[0];
        const glm::mat4 own = AiToGlm(b0->mOffsetMatrix);
        glm::mat4 bake = N;
        glm::mat4 offsetFix(1.0f);
        if (NearlyEqual(GI * g_Globals[b0->mName.C_Str()] * own, N)) bake = glm::mat4(1.0f);
        if (rule == 2 && bake == glm::mat4(1.0f)) { bake = N; offsetFix = glm::inverse(N); }
        if (rule == 1) {
            for (unsigned bi = 0; bi < m->mNumBones; ++bi) {
                auto it = g_Shared.find(m->mBones[bi]->mName.C_Str());
                if (it == g_Shared.end()) continue;
                bake = glm::inverse(it->second) * AiToGlm(m->mBones[bi]->mOffsetMatrix) * bake;
                break;
            }
        }
        for (unsigned bi = 0; bi < m->mNumBones; ++bi)
            g_Shared.emplace(m->mBones[bi]->mName.C_Str(), AiToGlm(m->mBones[bi]->mOffsetMatrix) * offsetFix);
        glm::vec3 lo(1e9f), hi(-1e9f);
        std::vector<glm::vec3> acc(m->mNumVertices, glm::vec3(0));
        std::vector<float> w(m->mNumVertices, 0.0f);
        for (unsigned bi = 0; bi < m->mNumBones; ++bi) {
            const aiBone* b = m->mBones[bi];
            glm::mat4 pal = GI * g_Globals[b->mName.C_Str()] * g_Shared[b->mName.C_Str()];
            for (unsigned k = 0; k < b->mNumWeights; ++k) {
                unsigned vi = b->mWeights[k].mVertexId;
                const aiVector3D& v = m->mVertices[vi];
                acc[vi] += b->mWeights[k].mWeight * glm::vec3(pal * bake * glm::vec4(v.x, v.y, v.z, 1));
                w[vi] += b->mWeights[k].mWeight;
            }
        }
        for (unsigned vi = 0; vi < m->mNumVertices; ++vi)
            if (w[vi] > 0) { glm::vec3 q = acc[vi] / w[vi]; lo = glm::min(lo, q); hi = glm::max(hi, q); }
        std::printf("    rule%d %-24s min(%.3f %.3f %.3f) max(%.3f %.3f %.3f)\n", rule, m->mName.C_Str(), lo.x, lo.y,
                    lo.z, hi.x, hi.y, hi.z);
    }
    for (unsigned i = 0; i < n->mNumChildren; ++i) Simulate(s, n->mChildren[i], N, rule);
}

int main(int argc, char** argv) {
    const bool sim = argc > 1 && std::string(argv[1]) == "--sim";
    for (int a = sim ? 2 : 1; a < argc; ++a) {
        Assimp::Importer imp;
        imp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
        imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        const aiScene* s = imp.ReadFile(argv[a], aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale |
                                                     aiProcess_LimitBoneWeights);
        if (!s) { std::printf("%s: %s\n", argv[a], imp.GetErrorString()); continue; }
        std::printf("== %s\n", argv[a]);
        g_Globals.clear();
        Collect(s->mRootNode, glm::mat4(1));
        GI = glm::inverse(AiToGlm(s->mRootNode->mTransformation));
        if (sim) {
            for (int rule = 0; rule < 3; ++rule) {
                g_Shared.clear();
                Simulate(s, s->mRootNode, glm::mat4(1), rule);
            }
            continue;
        }
        PrintM("root", AiToGlm(s->mRootNode->mTransformation));
        Walk(s, s->mRootNode, glm::mat4(1));
    }
}
