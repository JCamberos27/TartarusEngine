// List node names in an FBX hierarchy matching a substring (case-insensitive),
// so we can tell whether the rig exposes a head/neck/eye bone at runtime.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

static std::string Lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return o;
}

static int g_total = 0;

static void Walk(const aiNode* n, int depth) {
    ++g_total;
    std::string name = n->mName.C_Str();
    std::string low = Lower(name);
    bool hit = low.find("head") != std::string::npos ||
               low.find("neck") != std::string::npos ||
               low.find("eye") != std::string::npos ||
               low.find("spine") != std::string::npos ||
               low.find("camera") != std::string::npos;
    if (hit) printf("  depth=%2d  '%s'\n", depth, name.c_str());
    for (unsigned i = 0; i < n->mNumChildren; ++i) Walk(n->mChildren[i], depth + 1);
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: nodes <fbx>\n"); return 1; }
    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* sc = imp.ReadFile(argv[1],
        aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale | aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes);
    if (!sc || !sc->mRootNode) { printf("load failed: %s\n", imp.GetErrorString()); return 1; }
    printf("=== %s ===\n", argv[1]);
    g_total = 0;
    Walk(sc->mRootNode, 0);
    printf("total nodes = %d\n", g_total);
    printf("meshes = %u\n", sc->mNumMeshes);
    for (unsigned m = 0; m < sc->mNumMeshes; ++m)
        printf("  mesh[%u] '%s' verts=%u bones=%u\n", m, sc->mMeshes[m]->mName.C_Str(),
               sc->mMeshes[m]->mNumVertices, sc->mMeshes[m]->mNumBones);
    return 0;
}
