// List node names in an FBX hierarchy matching a substring (case-insensitive),
// so we can tell whether the rig exposes a head/neck/eye bone at runtime.
//   nodes <fbx>                  list every node, with its parent
//   nodes <fbx> <substring>      list nodes whose name contains <substring>
//   nodes <fbx> --chain <name>   print the root-><name> ancestor chain
// The chain mode answers the question that matters for an external clip: is any
// ancestor of a skinned bone absent from the clip's channel list (absent = the
// engine leaves it at bind, so everything under it lands in the wrong place)?
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

static std::string Lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return o;
}

static int g_total = 0;
static std::string g_filter;  // case-insensitive substring ("" = match everything)
static std::string g_chain;   // exact, case-insensitive name to print a chain for

static void Walk(const aiNode* n, int depth, const std::string& parent) {
    ++g_total;
    std::string name = n->mName.C_Str();
    if (g_filter.empty() || Lower(name).find(g_filter) != std::string::npos)
        printf("  depth=%2d  parent='%s'  '%s'\n", depth, parent.c_str(), name.c_str());
    for (unsigned i = 0; i < n->mNumChildren; ++i)
        Walk(n->mChildren[i], depth + 1, name);
}

// Depth-first search that retains the path taken, so the caller can print the
// full ancestor chain of the node it asked for.
static bool FindChain(const aiNode* n, const std::string& target,
                      std::vector<const aiNode*>& out) {
    out.push_back(n);
    if (Lower(n->mName.C_Str()) == target) return true;
    for (unsigned i = 0; i < n->mNumChildren; ++i)
        if (FindChain(n->mChildren[i], target, out)) return true;
    out.pop_back();
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: nodes <fbx> [substring | --chain <node>]\n");
        return 1;
    }
    bool chainMode = argc >= 4 && std::string(argv[2]) == "--chain";
    if (chainMode)
        g_chain = Lower(argv[3]);
    else if (argc >= 3)
        g_filter = Lower(argv[2]);

    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* sc = imp.ReadFile(argv[1],
        aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale | aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes);
    if (!sc || !sc->mRootNode) { printf("load failed: %s\n", imp.GetErrorString()); return 1; }
    printf("=== %s ===\n", argv[1]);

    if (chainMode) {
        std::vector<const aiNode*> path;
        if (!FindChain(sc->mRootNode, g_chain, path)) {
            printf("  no node named '%s'\n", argv[3]);
            return 1;
        }
        printf("  ancestor chain (%d levels):\n", (int)path.size());
        for (size_t i = 0; i < path.size(); ++i)
            printf("   [%2d] %s'%s'\n", (int)i,
                   std::string(i * 2, ' ').c_str(), path[i]->mName.C_Str());
        return 0;
    }

    g_total = 0;
    Walk(sc->mRootNode, 0, "");
    printf("total nodes = %d\n", g_total);
    printf("meshes = %u\n", sc->mNumMeshes);
    for (unsigned m = 0; m < sc->mNumMeshes; ++m)
        printf("  mesh[%u] '%s' verts=%u bones=%u\n", m, sc->mMeshes[m]->mName.C_Str(),
               sc->mMeshes[m]->mNumVertices, sc->mMeshes[m]->mNumBones);
    return 0;
}
