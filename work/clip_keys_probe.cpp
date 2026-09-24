// Prints a clip's duration and the first/last key times of a few channels, as the engine's
// importer sees them: a gap between the last key and the duration is a hold at every loop.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cstdio>
#include <string>
int main(int argc, char** argv) {
    Assimp::Importer imp;
    const aiScene* s = imp.ReadFile(argv[1], aiProcess_Triangulate | aiProcess_GlobalScale);
    if (!s) return 1;
    for (unsigned a = 0; a < s->mNumAnimations; ++a) {
        const aiAnimation* an = s->mAnimations[a];
        printf("clip '%s' duration %.3f ticks @ %.1f tps, %u channels\n", an->mName.C_Str(), an->mDuration, an->mTicksPerSecond, an->mNumChannels);
        for (unsigned c = 0; c < an->mNumChannels; ++c) {
            const aiNodeAnim* ch = an->mChannels[c];
            std::string n = ch->mNodeName.C_Str();
            if (n != "hand_l" && n != "ik_hand_gun" && n != "root" && n != "hand_r" && n != "lowerarm_l" && n != "upperarm_l" && n != "clavicle_l") continue;
            printf("  %-12s pos %u keys [%.3f .. %.3f]  rot %u keys [%.3f .. %.3f]\n", n.c_str(), ch->mNumPositionKeys,
                   ch->mPositionKeys[0].mTime, ch->mPositionKeys[ch->mNumPositionKeys - 1].mTime, ch->mNumRotationKeys,
                   ch->mRotationKeys[0].mTime, ch->mRotationKeys[ch->mNumRotationKeys - 1].mTime);
            if (n == "hand_l" && ch->mNumRotationKeys > 3) {
                const auto& k = ch->mRotationKeys;
                unsigned L = ch->mNumRotationKeys;
                printf("    first keys t: %.3f %.3f %.3f  last keys t: %.3f %.3f %.3f\n", k[0].mTime, k[1].mTime, k[2].mTime, k[L-3].mTime, k[L-2].mTime, k[L-1].mTime);
                auto q = [&](unsigned i) { return k[i].mValue; };
                printf("    first q (%.4f %.4f %.4f %.4f)  last q (%.4f %.4f %.4f %.4f)\n", q(0).w, q(0).x, q(0).y, q(0).z, q(L-1).w, q(L-1).x, q(L-1).y, q(L-1).z);
            }
        }
    }
}
