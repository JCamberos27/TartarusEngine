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
#include <vector>

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

// --scan: is each skinned mesh where its bones are? For every vertex, the distance from its bind-pose
// position (the engine's current rule, rule 2 above) to the bind position of the bone weighting it most.
// A mesh that imports lying down or floating off sits far from its bones; one that's right sits within a
// few centimetres. Prints one line per mesh: median distance for the engine's bake (N) and for no bake (I).
static std::string g_File;
static void Scan(const aiScene* s, const aiNode* n, const glm::mat4& p, std::vector<glm::mat4> frames = {}) {
    glm::mat4 N = p * AiToGlm(n->mTransformation);
    frames.push_back(N); // this node and its ancestors, for the engine rule below
    for (unsigned mi = 0; mi < n->mNumMeshes; ++mi) {
        const aiMesh* m = s->mMeshes[n->mMeshes[mi]];
        if (!m->mNumBones) continue;
        const aiBone* b0 = m->mBones[0];
        const glm::mat4 C = GI * g_Globals[b0->mName.C_Str()] * AiToGlm(b0->mOffsetMatrix);
        const bool cIsN = NearlyEqual(C, N), cIsI = NearlyEqual(C, glm::mat4(1.0f));
        std::vector<int> mainBone(m->mNumVertices, -1);
        std::vector<float> mainW(m->mNumVertices, 0.0f);
        for (unsigned bi = 0; bi < m->mNumBones; ++bi)
            for (unsigned k = 0; k < m->mBones[bi]->mNumWeights; ++k) {
                const auto& wt = m->mBones[bi]->mWeights[k];
                if (wt.mWeight > mainW[wt.mVertexId]) { mainW[wt.mVertexId] = wt.mWeight; mainBone[wt.mVertexId] = (int)bi; }
            }
        auto median = [&](const glm::mat4& bake, const glm::mat4& offsetFix) {
            std::vector<float> d;
            std::vector<glm::vec3> acc(m->mNumVertices, glm::vec3(0));
            std::vector<float> w(m->mNumVertices, 0.0f);
            for (unsigned bi = 0; bi < m->mNumBones; ++bi) {
                const aiBone* b = m->mBones[bi];
                const glm::mat4 pal = GI * g_Globals[b->mName.C_Str()] * AiToGlm(b->mOffsetMatrix) * offsetFix;
                for (unsigned k = 0; k < b->mNumWeights; ++k) {
                    const unsigned vi = b->mWeights[k].mVertexId;
                    const aiVector3D& v = m->mVertices[vi];
                    acc[vi] += b->mWeights[k].mWeight * glm::vec3(pal * bake * glm::vec4(v.x, v.y, v.z, 1));
                    w[vi] += b->mWeights[k].mWeight;
                }
            }
            for (unsigned vi = 0; vi < m->mNumVertices; vi += 7) {
                if (mainBone[vi] < 0 || w[vi] <= 0) continue;
                const glm::vec3 bone = glm::vec3((GI * g_Globals[m->mBones[mainBone[vi]]->mName.C_Str()])[3]);
                d.push_back(glm::length(acc[vi] / w[vi] - bone));
            }
            if (d.empty()) return -1.0f;
            std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
            return d[d.size() / 2];
        };
        // The engine's rule: bake N; when C is the frame of the mesh node or one of its ancestors (and not
        // I), the offsets get inverse(C) folded in.
        bool fix = false;
        if (!cIsI)
            for (const auto& f : frames) fix = fix || NearlyEqual(C, f);
        const float engine = median(N, fix ? glm::inverse(C) : glm::mat4(1.0f));
        const float noBake = median(glm::mat4(1.0f), glm::mat4(1.0f));
        std::printf("%s | %-28s | C==N %d C==I %d | engine %.3f | noBake %.3f%s\n", g_File.c_str(), m->mName.C_Str(), cIsN,
                    cIsI, engine, noBake, engine > 0.25f ? "   <-- OFF" : "");
        if (fix && !cIsN) std::printf("%s | %-28s | ANCESTOR-FRAME FIX\n", g_File.c_str(), m->mName.C_Str());
    }
    for (unsigned i = 0; i < n->mNumChildren; ++i) Scan(s, n->mChildren[i], N, frames);
}

// --bones: per dominant bone, how many vertices it moves most and their mean bind height (Y).
static void Bones(const aiScene* s) {
    for (unsigned mi = 0; mi < s->mNumMeshes; ++mi) {
        const aiMesh* m = s->mMeshes[mi];
        std::vector<int> best(m->mNumVertices, -1);
        std::vector<float> bw(m->mNumVertices, 0.0f);
        for (unsigned bi = 0; bi < m->mNumBones; ++bi)
            for (unsigned k = 0; k < m->mBones[bi]->mNumWeights; ++k) {
                const auto& w = m->mBones[bi]->mWeights[k];
                if (w.mWeight > bw[w.mVertexId]) { bw[w.mVertexId] = w.mWeight; best[w.mVertexId] = (int)bi; }
            }
        std::map<std::string, std::pair<int, float>> stat;
        for (unsigned v = 0; v < m->mNumVertices; ++v) {
            if (best[v] < 0) continue;
            auto& st = stat[m->mBones[best[v]]->mName.C_Str()];
            st.first++;
            st.second += m->mVertices[v].z; // FBX Z-up source space; relative heights are what matter
        }
        std::printf("mesh %s\n", m->mName.C_Str());
        for (auto& [n, st] : stat) std::printf("  %-28s %6d verts  mean %.3f\n", n.c_str(), st.first, st.second / st.first);
    }
}

int main(int argc, char** argv) {
    const bool sim = argc > 1 && std::string(argv[1]) == "--sim";
    const bool scan = argc > 1 && std::string(argv[1]) == "--scan";
    if (argc > 2 && std::string(argv[1]) == "--bones") {
        Assimp::Importer imp;
        const aiScene* s = imp.ReadFile(argv[2], aiProcess_Triangulate);
        if (s) Bones(s);
        return 0;
    }
    for (int a = (sim || scan) ? 2 : 1; a < argc; ++a) {
        Assimp::Importer imp;
        imp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
        imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        const aiScene* s = imp.ReadFile(argv[a], aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale |
                                                     aiProcess_LimitBoneWeights);
        if (!s) { std::printf("%s: %s\n", argv[a], imp.GetErrorString()); continue; }
        if (scan) {
            g_Globals.clear();
            Collect(s->mRootNode, glm::mat4(1));
            GI = glm::inverse(AiToGlm(s->mRootNode->mTransformation));
            g_File = argv[a];
            Scan(s, s->mRootNode, glm::mat4(1));
            continue;
        }
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
