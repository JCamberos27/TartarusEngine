// Top of each FBX's node tree with local translation / rotation (euler deg), plus the global of
// the first few bones - compares the female Quantum rig with the male one and with a clip.
//   rig_top_probe <depth> <file.fbx> [more.fbx...]
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/config.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static int g_MaxDepth = 5;

static void Walk(const aiNode* n, int depth, aiMatrix4x4 parent) {
    aiMatrix4x4 g = parent * n->mTransformation;
    aiVector3D s, r, t;
    n->mTransformation.Decompose(s, r, t);
    aiVector3D gs, gr, gt;
    g.Decompose(gs, gr, gt);
    const float d = 57.2958f;
    std::printf("%*s%-34s local T(%.3f %.3f %.3f) R(%.1f %.1f %.1f) S(%.2f)  | global T(%.3f %.3f %.3f) R(%.1f %.1f %.1f)%s\n",
                depth * 2, "", n->mName.C_Str(), t.x, t.y, t.z, r.x * d, r.y * d, r.z * d, s.x, gt.x, gt.y, gt.z,
                gr.x * d, gr.y * d, gr.z * d, n->mNumMeshes ? "  [mesh]" : "");
    if (depth >= g_MaxDepth) return;
    for (unsigned i = 0; i < n->mNumChildren; ++i) Walk(n->mChildren[i], depth + 1, g);
}

int main(int argc, char** argv) {
    g_MaxDepth = std::atoi(argv[1]);
    for (int a = 2; a < argc; ++a) {
        Assimp::Importer imp;
        imp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
        imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        const aiScene* s = imp.ReadFile(argv[a], aiProcess_Triangulate | aiProcess_GlobalScale | aiProcess_LimitBoneWeights);
        if (!s) { std::printf("%s: %s\n", argv[a], imp.GetErrorString()); continue; }
        std::printf("== %s  (%u anims)\n", argv[a], s->mNumAnimations);
        Walk(s->mRootNode, 0, aiMatrix4x4());
    }
}
