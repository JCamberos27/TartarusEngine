// Compare where two skinned FBX meshes actually sit in ROOT space, under the engine's
// old behaviour (nothing baked) and the new one (bake = C^-1 * nodeTransform), so we can
// tell whether the weapon still lines up with the hands.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Must match Model.cpp's AiToGlm exactly (direct element copy, NO transpose).
static glm::mat4 AiToGlm(const aiMatrix4x4& m) {
    glm::mat4 t;
    t[0][0] = m.a1; t[1][0] = m.a2; t[2][0] = m.a3; t[3][0] = m.a4;
    t[0][1] = m.b1; t[1][1] = m.b2; t[2][1] = m.b3; t[3][1] = m.b4;
    t[0][2] = m.c1; t[1][2] = m.c2; t[2][2] = m.c3; t[3][2] = m.c4;
    t[0][3] = m.d1; t[1][3] = m.d2; t[2][3] = m.d3; t[3][3] = m.d4;
    return t;
}
static float MaxDiff(const glm::mat4& a, const glm::mat4& b) {
    float d = 0.0f;
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) d = std::max(d, std::abs(a[c][r] - b[c][r]));
    return d;
}
static void Collect(const aiNode* n, const glm::mat4& parent, std::map<std::string, glm::mat4>& out) {
    glm::mat4 g = parent * AiToGlm(n->mTransformation);
    out.emplace(n->mName.C_Str(), g);
    for (unsigned i = 0; i < n->mNumChildren; ++i) Collect(n->mChildren[i], g, out);
}

struct Info {
    bool ok = false;
    std::string owner;
    glm::mat4 M{1.0f}, C{1.0f}, bake{1.0f};
    float spread = 0.0f;
    bool haveC = false;
    glm::vec3 mn{1e30f}, mx{-1e30f};      // old behaviour (nothing baked)
    glm::vec3 mnN{1e30f}, mxN{-1e30f};    // new behaviour (bake applied)
};

static Info Analyze(const fs::path& f) {
    Info r;
    Assimp::Importer imp;
    imp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    unsigned flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale |
                     aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
                     aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes;
    const aiScene* sc = imp.ReadFile(f.string(), flags);
    if (!sc || !sc->mRootNode || !sc->mNumMeshes) return r;
    r.ok = true;

    std::map<std::string, glm::mat4> globals;
    Collect(sc->mRootNode, glm::mat4(1.0f), globals);

    aiMesh* mesh = nullptr;
    unsigned meshIndex = 0;
    for (unsigned i = 0; i < sc->mNumMeshes; ++i) if (sc->mMeshes[i]->mNumBones) { mesh = sc->mMeshes[i]; meshIndex = i; break; }
    if (!mesh) return r;

    std::function<bool(const aiNode*, const glm::mat4&)> find =
        [&](const aiNode* n, const glm::mat4& p) -> bool {
        glm::mat4 g = p * AiToGlm(n->mTransformation);
        for (unsigned i = 0; i < n->mNumMeshes; ++i)
            if (n->mMeshes[i] == meshIndex) { r.M = g; r.owner = n->mName.C_Str(); return true; }
        for (unsigned i = 0; i < n->mNumChildren; ++i) if (find(n->mChildren[i], g)) return true;
        return false;
    };
    find(sc->mRootNode, glm::mat4(1.0f));

    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        auto g = globals.find(mesh->mBones[b]->mName.C_Str());
        if (g == globals.end()) continue;
        glm::mat4 Ci = g->second * AiToGlm(mesh->mBones[b]->mOffsetMatrix);
        if (!r.haveC) { r.C = Ci; r.haveC = true; }
        else r.spread = std::max(r.spread, MaxDiff(r.C, Ci));
    }
    r.bake = r.haveC ? glm::inverse(r.C) * r.M : r.M;

    for (unsigned i = 0; i < mesh->mNumVertices; ++i) {
        glm::vec4 v(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z, 1.0f);
        glm::vec3 o = glm::vec3(v), n = glm::vec3(r.bake * v);
        r.mn = glm::min(r.mn, o); r.mx = glm::max(r.mx, o);
        r.mnN = glm::min(r.mnN, n); r.mxN = glm::max(r.mxN, n);
    }
    return r;
}

static void PrintBox(const char* label, const glm::vec3& mn, const glm::vec3& mx) {
    printf("    %-14s (%7.3f %7.3f %7.3f) .. (%7.3f %7.3f %7.3f)   size %5.3f x %5.3f x %5.3f\n",
           label, mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: align_probe <fileA> <fileB>\n"); return 1; }
    Info a = Analyze(argv[1]), b = Analyze(argv[2]);
    for (Info* p : {&a, &b}) {
        if (!p->ok) { printf("failed to load\n"); continue; }
        printf("%-34s owner='%s'\n", fs::path(p == &a ? argv[1] : argv[2]).filename().string().c_str(), p->owner.c_str());
        printf("    |M-I|=%.5f  C=%s  |C-I|=%.5f spread=%.6f\n", MaxDiff(p->M, glm::mat4(1.0f)),
               p->haveC ? "found" : "MISSING", p->haveC ? MaxDiff(p->C, glm::mat4(1.0f)) : -1.0f, p->spread);
        printf("    |bake-I|=%.5f  |bake-M|=%.5f  -> %s\n", MaxDiff(p->bake, glm::mat4(1.0f)),
               MaxDiff(p->bake, p->M),
               MaxDiff(p->bake, glm::mat4(1.0f)) < 1e-6f ? "bake=I (unchanged)" : "bake applied");
        PrintBox("OLD (no bake)", p->mn, p->mx);
        PrintBox("NEW (bake)", p->mnN, p->mxN);
        printf("\n");
    }
    if (a.ok && b.ok) {
        auto gap = [](const glm::vec3& a0, const glm::vec3& a1, const glm::vec3& b0, const glm::vec3& b1) {
            float g = 0.0f;
            for (int i = 0; i < 3; ++i) {
                if (a1[i] < b0[i]) g = std::max(g, b0[i] - a1[i]);
                else if (b1[i] < a0[i]) g = std::max(g, a0[i] - b1[i]);
            }
            return g;
        };
        printf("AABB separation  OLD: %.3f   NEW: %.3f   (0 = boxes touch/overlap)\n",
               gap(a.mn, a.mx, b.mn, b.mx), gap(a.mnN, a.mxN, b.mnN, b.mxN));
    }
    return 0;
}
