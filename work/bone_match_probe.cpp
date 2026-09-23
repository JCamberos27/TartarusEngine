// Lists every base-skeleton node name and every clip-channel bone name, and reports any
// clip channel with no exact-name match in the base hierarchy (AttachClip's matching rule).
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_set>

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

void CollectNames(const aiNode* n, std::unordered_set<std::string>& out) {
    out.insert(n->mName.C_Str());
    for (unsigned i = 0; i < n->mNumChildren; ++i) CollectNames(n->mChildren[i], out);
}

void CheckClip(const std::string& basePath, const std::string& clipPath, const std::unordered_set<std::string>& baseNames) {
    Assimp::Importer impClip;
    const aiScene* sceneClip = Load(impClip, clipPath);
    if (!sceneClip || sceneClip->mNumAnimations == 0) { std::cout << clipPath << ": no animations\n"; return; }
    aiAnimation* anim = sceneClip->mAnimations[0];
    int matched = 0, total = (int)anim->mNumChannels;
    std::vector<std::string> unmatched;
    for (unsigned c = 0; c < anim->mNumChannels; ++c) {
        std::string name = anim->mChannels[c]->mNodeName.C_Str();
        if (baseNames.count(name)) ++matched;
        else unmatched.push_back(name);
    }
    printf("\n=== %s ===\n", clipPath.c_str());
    printf("channels=%d matched=%d unmatched=%d\n", total, matched, (int)unmatched.size());
    for (auto& n : unmatched) std::cout << "  UNMATCHED: " << n << "\n";
}

int main() {
    std::string dir = "C:\\Users\\jacob\\Desktop\\TartarusEngine\\.claude\\worktrees\\fps-first-person-animation\\project\\assets\\fps\\AKS74U\\FirstPerson\\";
    std::string base = dir + "AKS-74U_A_FP_ADS.fbx";

    Assimp::Importer impBase;
    const aiScene* sceneBase = Load(impBase, base);
    if (!sceneBase) return 1;
    std::unordered_set<std::string> baseNames;
    CollectNames(sceneBase->mRootNode, baseNames);
    printf("Base '%s' node count: %zu\n", base.c_str(), baseNames.size());

    const char* clips[] = { "AKS-74U_A_FP_Idle.fbx", "AKS-74U_A_FP_Walk.fbx", "AKS-74U_A_FP_Sprint.fbx",
                             "AKS-74U_A_FP_Fire.fbx", "AKS-74U_A_FP_Draw.fbx" };
    for (const char* c : clips) CheckClip(base, dir + c, baseNames);
    return 0;
}
