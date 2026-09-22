// What is actually inside an FBX: nodes, meshes (vertex counts), and every take it carries.
//
// The shipped arm clips are ~10 MB while a re-export of the same action with our script lands
// near 6.4 MB, so before accepting a re-export this reports exactly what differs - most
// usefully the animation (take) count, because Blender's default bake_anim_use_all_actions=True
// packs every action in the file into one FBX as separate takes.
//
//   fbx_info <file.fbx> [more...]
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/config.h>
#include <cstdio>
#include <set>
#include <string>

static int CountNodes(const aiNode* n) {
    int c = 1;
    for (unsigned i = 0; i < n->mNumChildren; ++i) c += CountNodes(n->mChildren[i]);
    return c;
}
static void CollectNames(const aiNode* n, std::set<std::string>& out) {
    out.insert(n->mName.C_Str());
    for (unsigned i = 0; i < n->mNumChildren; ++i) CollectNames(n->mChildren[i], out);
}

int main(int argc, char** argv) {
    for (int a = 1; a < argc; ++a) {
        Assimp::Importer imp;
        imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        const aiScene* sc = imp.ReadFile(argv[a], aiProcess_Triangulate | aiProcess_FlipUVs |
                                                   aiProcess_GlobalScale | aiProcess_GenSmoothNormals |
                                                   aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights |
                                                   aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes);
        printf("\n=== %s\n", argv[a]);
        if (!sc || !sc->mRootNode) { printf("  FAILED: %s\n", imp.GetErrorString()); continue; }
        printf("  nodes=%d  meshes=%u  animations=%u  materials=%u\n",
               CountNodes(sc->mRootNode), sc->mNumMeshes, sc->mNumAnimations,
               sc->mNumMaterials);
        unsigned long long verts = 0, tris = 0, bones = 0;
        for (unsigned m = 0; m < sc->mNumMeshes; ++m) {
            const aiMesh* mm = sc->mMeshes[m];
            verts += mm->mNumVertices;
            tris += mm->mNumFaces;
            bones += mm->mNumBones;
            printf("    mesh %-28s verts=%-8u faces=%-8u bones=%u\n",
                   mm->mName.C_Str(), mm->mNumVertices, mm->mNumFaces, mm->mNumBones);
        }
        printf("    TOTAL verts=%llu faces=%llu bone-instances=%llu\n", verts, tris, bones);
        for (unsigned x = 0; x < sc->mNumAnimations; ++x) {
            const aiAnimation* an = sc->mAnimations[x];
            printf("    take %-30s dur=%-8.1f tps=%-6.1f channels=%u\n",
                   an->mName.C_Str(), an->mDuration, an->mTicksPerSecond, an->mNumChannels);
        }
        std::set<std::string> names;
        CollectNames(sc->mRootNode, names);
        printf("    unique node names=%zu\n", names.size());
    }
    return 0;
}
