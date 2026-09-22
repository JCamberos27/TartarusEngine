// Checks whether the base skeleton's node tree has duplicate names. If it does, any map keyed
// by name (std::unordered_map<name,...>) silently collapses to ONE entry per name, while a
// flat per-node array keeps one slot per node - these two representations can then disagree
// about which node's transform "wins" for a given bone name.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

const aiScene* Load(Assimp::Importer& imp, const std::string& path) {
    imp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    unsigned int flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale |
                          aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
                          aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices |
                          aiProcess_OptimizeMeshes;
    const aiScene* scene = imp.ReadFile(path, flags);
    if (!scene) std::cerr << "FAILED to load " << path << ": " << imp.GetErrorString() << "\n";
    return scene;
}

void Walk(const aiNode* n, std::vector<std::string>& names) {
    names.push_back(n->mName.C_Str());
    for (unsigned i = 0; i < n->mNumChildren; ++i) Walk(n->mChildren[i], names);
}

int main() {
    std::string base = "C:\\Users\\jacob\\Desktop\\TartarusEngine\\.claude\\worktrees\\fps-first-person-animation\\project\\assets\\fps\\AKS74U\\FirstPerson\\AKS-74U_A_FP_ADS.fbx";
    Assimp::Importer imp;
    const aiScene* scene = Load(imp, base);
    if (!scene) return 1;

    std::vector<std::string> names;
    Walk(scene->mRootNode, names);
    printf("Total nodes: %zu\n", names.size());

    std::unordered_map<std::string, int> counts;
    for (auto& n : names) counts[n]++;
    printf("Unique names: %zu\n", counts.size());

    // Which bones actually carry mesh weights (these are the ones that matter for skinning).
    std::vector<std::string> boneNames;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        aiMesh* mesh = scene->mMeshes[m];
        for (unsigned b = 0; b < mesh->mNumBones; ++b)
            boneNames.push_back(mesh->mBones[b]->mName.C_Str());
    }
    printf("\nTotal mesh bone refs (across all meshes): %zu\n", boneNames.size());

    int dupCount = 0;
    for (auto& kv : counts) {
        if (kv.second > 1) {
            ++dupCount;
            bool isSkinned = counts.count(kv.first) && std::find(boneNames.begin(), boneNames.end(), kv.first) != boneNames.end();
            printf("DUPLICATE NAME (x%d)%s: %s\n", kv.second, isSkinned ? " [SKINNED BONE]" : "", kv.first.c_str());
        }
    }
    printf("\nTotal duplicate names: %d\n", dupCount);
    return 0;
}
