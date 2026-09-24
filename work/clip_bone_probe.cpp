// Prints each clip's first-key local rotation (and key count) for the named bones, as the
// engine's importer reads them - for comparing two clips' poses bone by bone.
//   clip_bone_probe <bone,bone,...> <clip.fbx>...
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 3) return 1;
    std::vector<std::string> bones;
    std::stringstream ss(argv[1]);
    for (std::string b; std::getline(ss, b, ',');) bones.push_back(b);
    for (int f = 2; f < argc; ++f) {
        Assimp::Importer imp;
        const aiScene* s = imp.ReadFile(argv[f], aiProcess_GlobalScale);
        if (!s || !s->mNumAnimations) { printf("%s: no clip\n", argv[f]); continue; }
        printf("%s\n", argv[f]);
        const aiAnimation* an = s->mAnimations[0];
        for (const auto& want : bones) {
            bool found = false;
            for (unsigned c = 0; c < an->mNumChannels; ++c) {
                const aiNodeAnim* ch = an->mChannels[c];
                if (want != ch->mNodeName.C_Str()) continue;
                found = true;
                const aiQuaternion q = ch->mRotationKeys[0].mValue;
                printf("  %-22s %3u rot keys  q0 (%.4f %.4f %.4f %.4f)\n", want.c_str(), ch->mNumRotationKeys, q.w, q.x, q.y, q.z);
            }
            if (!found) printf("  %-22s NOT KEYED\n", want.c_str());
        }
    }
}
