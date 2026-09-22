// Per-bone dump: C_i = RestGlobal_i * BoneOffset_i must be ONE constant for the skin
// palette to be well defined. Print each bone so we can see who disagrees.
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

namespace fs = std::filesystem;

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
static void Collect(const aiNode* n, const glm::mat4& p, std::map<std::string, glm::mat4>& out, int depth) {
    glm::mat4 g = p * AiToGlm(n->mTransformation);
    if (depth <= 3) printf("    node%-2d '%s'\n", depth, n->mName.C_Str());
    out.emplace(n->mName.C_Str(), g);
    for (unsigned i = 0; i < n->mNumChildren; ++i) Collect(n->mChildren[i], g, out, depth + 1);
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    Assimp::Importer imp;
    imp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* sc = imp.ReadFile(argv[1],
        aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale | aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes);
    if (!sc || !sc->mRootNode) { printf("load failed: %s\n", imp.GetErrorString()); return 1; }

    printf("hierarchy (depth<=3):\n");
    std::map<std::string, glm::mat4> globals;
    Collect(sc->mRootNode, glm::mat4(1.0f), globals, 0);

    glm::mat4 ginv = glm::inverse(AiToGlm(sc->mRootNode->mTransformation));
    printf("GlobalInverse |ginv-I| = %.6f\n\n", MaxDiff(ginv, glm::mat4(1.0f)));

    for (unsigned mi = 0; mi < sc->mNumMeshes; ++mi) {
        aiMesh* mesh = sc->mMeshes[mi];
        if (!mesh->mNumBones) continue;
        printf("mesh '%s' (%u bones, %u verts)\n", mesh->mName.C_Str(), mesh->mNumBones, mesh->mNumVertices);
        printf("  %-34s %-7s %9s %9s   %s\n", "bone", "found", "|A-I|", "|C-I|", "C translation");
        glm::mat4 first(1.0f); bool have = false; float spread = 0.0f;
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const char* nm = mesh->mBones[b]->mName.C_Str();
            auto it = globals.find(nm);
            if (it == globals.end()) { printf("  %-34s %-7s\n", nm, "NO"); continue; }
            glm::mat4 A = it->second;
            glm::mat4 Off = AiToGlm(mesh->mBones[b]->mOffsetMatrix);
            glm::mat4 C = A * Off;
            if (!have) { first = C; have = true; } else spread = std::max(spread, MaxDiff(first, C));
            printf("  %-34s %-7s %9.5f %9.5f   (%7.3f %7.3f %7.3f)\n", nm, "yes",
                   MaxDiff(A, glm::mat4(1.0f)), MaxDiff(C, glm::mat4(1.0f)), C[3][0], C[3][1], C[3][2]);
        }
        printf("  spread across bones = %.6f  -> %s\n\n", spread, spread < 1e-4f ? "CONSTANT (consistent bind)" : "NOT CONSTANT");
    }
    return 0;
}
