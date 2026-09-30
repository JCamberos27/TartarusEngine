// Audit: for every skinned mesh the engine would import, report the transform the new
// Model::ProcessMesh bakes into the vertices. bake == identity means behaviour is
// byte-identical to the old `if (!skinned)` code path (no regression possible);
// bake == nodeTransform means the old path was silently dropping it (the arms bug).
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

// Assimp's aiMatrix4x4 is row-major (a1 is [row0][col0]); glm::make_mat4 reads column-major, so
// using it here silently transposed every transform and made this probe's numbers meaningless.
// Direct element copy matches Model.cpp's AiToGlm exactly.
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

int main(int argc, char** argv) {
    std::vector<fs::path> files;
    fs::path root = argc > 1 ? argv[1] : "project/assets";
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        std::string e = it->path().extension().string();
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        if (e == ".fbx" || e == ".gltf" || e == ".glb" || e == ".obj") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    printf("%-52s %-26s %5s %9s %9s %9s  %s\n", "file", "mesh", "bones", "|M-I|", "|C-I|", "|bake-I|", "verdict");
    int skinnedMeshes = 0, changed = 0, invariants = 0, badBind = 0;

    for (const fs::path& f : files) {
        Assimp::Importer imp;
        imp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
        imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        unsigned flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale |
                         aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
                         aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes;
        const aiScene* sc = imp.ReadFile(f.string(), flags);
        if (!sc || !sc->mRootNode) continue;

        std::map<std::string, glm::mat4> globals;
        Collect(sc->mRootNode, glm::mat4(1.0f), globals);
        glm::mat4 ginv = glm::inverse(AiToGlm(sc->mRootNode->mTransformation));

        for (unsigned mi = 0; mi < sc->mNumMeshes; ++mi) {
            aiMesh* mesh = sc->mMeshes[mi];
            if (!mesh->mNumBones) continue;
            ++skinnedMeshes;

            // node world transform of the node owning this mesh (same walk as ProcessNode)
            glm::mat4 nodeXf(1.0f);
            std::string owner = "?";
            std::function<bool(const aiNode*, const glm::mat4&)> find =
                [&](const aiNode* n, const glm::mat4& parent) -> bool {
                glm::mat4 g = parent * AiToGlm(n->mTransformation);
                for (unsigned i = 0; i < n->mNumMeshes; ++i)
                    if (n->mMeshes[i] == mi) { nodeXf = g; owner = n->mName.C_Str(); return true; }
                for (unsigned i = 0; i < n->mNumChildren; ++i) if (find(n->mChildren[i], g)) return true;
                return false;
            };
            find(sc->mRootNode, glm::mat4(1.0f));

            // C = RestGlobal * BoneOffset, measured on every bone (bind is only consistent
            // when they all agree); bake = C^-1 * nodeXf
            glm::mat4 C = glm::mat4(1.0f);
            float spread = 0.0f;
            bool have = false;
            for (unsigned b = 0; b < mesh->mNumBones; ++b) {
                auto g = globals.find(mesh->mBones[b]->mName.C_Str());
                if (g == globals.end()) { if (!have) have = false; continue; }
                glm::mat4 Ci = g->second * AiToGlm(mesh->mBones[b]->mOffsetMatrix);
                if (!have) { C = Ci; have = true; }
                else spread = std::max(spread, MaxDiff(C, Ci));
            }
            glm::mat4 bake = have ? glm::inverse(C) * nodeXf : nodeXf;
            float dM = MaxDiff(nodeXf, glm::mat4(1.0f));
            float dC = have ? MaxDiff(C, glm::mat4(1.0f)) : -1.0f;
            float dB = MaxDiff(bake, glm::mat4(1.0f));

            const char* verdict;
            if (!have) verdict = "NO BONE NODE - falls back to nodeXf";
            else if (spread > 1e-3f) { verdict = "INCONSISTENT BIND (C varies per bone)"; ++badBind; }
            else if (dB < 1e-6f) { verdict = "bake=I -> UNCHANGED from old code"; ++invariants; }
            else if (MaxDiff(bake, nodeXf) < 1e-6f) { verdict = "bake=nodeXf -> WAS DROPPED (bug)"; ++changed; }
            else { verdict = "bake=partial -> CHANGED"; ++changed; }

            std::string fn = f.filename().string();
            if (fn.size() > 51) fn = "..." + fn.substr(fn.size() - 48);
            std::string mn = mesh->mName.C_Str();
            if (mn.size() > 25) mn = mn.substr(0, 25);
            printf("%-52s %-26s %5u %9.5f %9.5f %9.5f  %s\n",
                   fn.c_str(), mn.c_str(), mesh->mNumBones, dM, dC, dB, verdict);
        }
    }
    printf("\nskinned meshes: %d | unchanged(bake=I): %d | changed: %d | inconsistent binds: %d\n",
           skinnedMeshes, invariants, changed, badBind);
    return 0;
}
