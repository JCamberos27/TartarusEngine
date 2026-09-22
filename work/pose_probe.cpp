// Print the mesh-node world matrix, bind-pose skinned bbox, and (if present) the
// skinned bbox at a given animation time, so we can tell where each rig actually
// lives in its own exported root space.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static glm::mat4 AiToGlm(const aiMatrix4x4& m) {
    glm::mat4 t;
    t[0][0] = m.a1; t[1][0] = m.a2; t[2][0] = m.a3; t[3][0] = m.a4;
    t[0][1] = m.b1; t[1][1] = m.b2; t[2][1] = m.b3; t[3][1] = m.b4;
    t[0][2] = m.c1; t[1][2] = m.c2; t[2][2] = m.c3; t[3][2] = m.c4;
    t[0][3] = m.d1; t[1][3] = m.d2; t[2][3] = m.d3; t[3][3] = m.d4;
    return t;
}

static void Collect(const aiNode* n, const glm::mat4& parent,
                    std::map<std::string, glm::mat4>& out, int depth) {
    glm::mat4 g = parent * AiToGlm(n->mTransformation);
    if (depth <= 2) {
        printf("  node d%d '%-22s' t=(%8.4f %8.4f %8.4f)\n", depth, n->mName.C_Str(),
               g[3][0], g[3][1], g[3][2]);
    }
    out.emplace(n->mName.C_Str(), g);
    for (unsigned i = 0; i < n->mNumChildren; ++i) Collect(n->mChildren[i], g, out, depth + 1);
}

static void PrintM(const char* label, const glm::mat4& m) {
    printf("%s\n", label);
    for (int r = 0; r < 4; ++r)
        printf("    [%8.4f %8.4f %8.4f %8.4f]\n", m[0][r], m[1][r], m[2][r], m[3][r]);
}

struct Box { glm::vec3 mn{1e30f}, mx{-1e30f}; bool any = false; };
static void Grow(Box& b, const glm::vec3& p) {
    b.mn = glm::min(b.mn, p); b.mx = glm::max(b.mx, p); b.any = true;
}
static void PrintBox(const char* label, const Box& b) {
    if (!b.any) { printf("  %-12s (none)\n", label); return; }
    // Blender world is Z-up; FBX/engine is Y-up. Report BOTH so we can compare
    // directly against .blend ground truth.
    glm::vec3 yup = b.mx - b.mn;
    glm::vec3 zup(yup.x, yup.z, yup.y);
    printf("  %-12s Y-up lo=(%7.3f %7.3f %7.3f) hi=(%7.3f %7.3f %7.3f) size=(%5.3f %5.3f %5.3f) | as-Zup-size=(%5.3f %5.3f %5.3f)\n",
           label, b.mn.x, b.mn.y, b.mn.z, b.mx.x, b.mx.y, b.mx.z,
           yup.x, yup.y, yup.z, zup.x, zup.y, zup.z);
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: pose_probe <fbx> [time]\n"); return 1; }
    double t = (argc >= 3) ? atof(argv[2]) : -1.0;

    Assimp::Importer imp;
    imp.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* sc = imp.ReadFile(argv[1],
        aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GlobalScale | aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes);
    if (!sc || !sc->mRootNode) { printf("load failed: %s\n", imp.GetErrorString()); return 1; }

    printf("=== %s ===\n", fs::path(argv[1]).filename().string().c_str());
    std::map<std::string, glm::mat4> globals;
    Collect(sc->mRootNode, glm::mat4(1.0f), globals, 0);
    glm::mat4 ginv = glm::inverse(AiToGlm(sc->mRootNode->mTransformation));
    PrintM("GlobalInverse:", ginv);

    printf("animations: %u\n", sc->mNumAnimations);
    for (unsigned a = 0; a < sc->mNumAnimations; ++a) {
        const aiAnimation* an = sc->mAnimations[a];
        printf("  anim %u '%s' dur=%.4f ticksPerSec=%.1f channels=%u\n", a,
               an->mName.C_Str(), an->mDuration, an->mTicksPerSecond, an->mNumChannels);
    }

    for (unsigned mi = 0; mi < sc->mNumMeshes; ++mi) {
        aiMesh* mesh = sc->mMeshes[mi];
        if (!mesh->mNumBones) continue;

        // owner node
        glm::mat4 M(1.0f); std::string owner = "?";
        std::function<bool(const aiNode*, const glm::mat4&)> find =
            [&](const aiNode* n, const glm::mat4& p) -> bool {
            glm::mat4 g = p * AiToGlm(n->mTransformation);
            for (unsigned i = 0; i < n->mNumMeshes; ++i)
                if (n->mMeshes[i] == mi) { M = g; owner = n->mName.C_Str(); return true; }
            for (unsigned i = 0; i < n->mNumChildren; ++i) if (find(n->mChildren[i], g)) return true;
            return false;
        };
        find(sc->mRootNode, glm::mat4(1.0f));
        printf("mesh[%u] '%s' verts=%u owner='%s'\n", mi, mesh->mName.C_Str(), mesh->mNumVertices, owner.c_str());
        PrintM("  mesh node world M:", M);

        // palette helper: build node transforms at time tt (or bind when tt<0)
        std::map<std::string, glm::mat4> posed;
        auto buildPosed = [&](double tt) {
            std::function<void(const aiNode*, const glm::mat4&)> walk =
                [&](const aiNode* n, const glm::mat4& p) {
                glm::mat4 local = AiToGlm(n->mTransformation);
                if (tt >= 0 && sc->mNumAnimations) {
                    const aiAnimation* an = sc->mAnimations[0];
                    for (unsigned c = 0; c < an->mNumChannels; ++c) {
                        const aiNodeAnim* ch = an->mChannels[c];
                        if (std::string(ch->mNodeName.C_Str()) != n->mName.C_Str()) continue;
                        glm::mat4 rot(1.0f); glm::vec3 sc3(1.0f), tr(0.0f);
                        if (ch->mNumPositionKeys) {
                            const aiVector3D& v = ch->mPositionKeys[0].mValue;
                            tr = glm::vec3(v.x, v.y, v.z);
                            for (unsigned k = 0; k + 1 < ch->mNumPositionKeys; ++k) {
                                if (tt >= ch->mPositionKeys[k].mTime && tt <= ch->mPositionKeys[k + 1].mTime) {
                                    double a0 = ch->mPositionKeys[k].mTime, a1 = ch->mPositionKeys[k + 1].mTime;
                                    double f = (a1 > a0) ? (tt - a0) / (a1 - a0) : 0.0;
                                    const aiVector3D& v0 = ch->mPositionKeys[k].mValue;
                                    const aiVector3D& v1 = ch->mPositionKeys[k + 1].mValue;
                                    tr = glm::mix(glm::vec3(v0.x, v0.y, v0.z), glm::vec3(v1.x, v1.y, v1.z), (float)f);
                                }
                            }
                        }
                        if (ch->mNumScalingKeys) {
                            const aiVector3D& v = ch->mScalingKeys[0].mValue;
                            sc3 = glm::vec3(v.x, v.y, v.z);
                        }
                        if (ch->mNumRotationKeys) {
                            const aiQuatKey& k0 = ch->mRotationKeys[0];
                            aiQuaternion q = k0.mValue;
                            for (unsigned k = 0; k + 1 < ch->mNumRotationKeys; ++k) {
                                if (tt >= ch->mRotationKeys[k].mTime && tt <= ch->mRotationKeys[k + 1].mTime) {
                                    double a0 = ch->mRotationKeys[k].mTime, a1 = ch->mRotationKeys[k + 1].mTime;
                                    double f = (a1 > a0) ? (tt - a0) / (a1 - a0) : 0.0;
                                    aiQuaternion::Interpolate(q, ch->mRotationKeys[k].mValue,
                                                              ch->mRotationKeys[k + 1].mValue, (float)f);
                                }
                            }
                            rot = glm::mat4_cast(glm::quat(q.w, q.x, q.y, q.z));
                        }
                        local = glm::translate(glm::mat4(1.0f), tr) * rot * glm::scale(glm::mat4(1.0f), sc3);
                    }
                }
                glm::mat4 g = p * local;
                posed[n->mName.C_Str()] = g;
                for (unsigned i = 0; i < n->mNumChildren; ++i) walk(n->mChildren[i], g);
            };
            posed.clear();
            walk(sc->mRootNode, glm::mat4(1.0f));
        };

        auto skinBox = [&](const std::map<std::string, glm::mat4>& nodes, const glm::mat4& bake) {
            Box b;
            std::vector<glm::mat4> pal(mesh->mNumBones, glm::mat4(0.0f));
            for (unsigned bi = 0; bi < mesh->mNumBones; ++bi) {
                auto it = nodes.find(mesh->mBones[bi]->mName.C_Str());
                if (it == nodes.end()) continue;
                pal[bi] = ginv * it->second * AiToGlm(mesh->mBones[bi]->mOffsetMatrix);
            }
            for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
                glm::vec4 p(mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z, 1.0f);
                glm::vec4 sk(0.0f);
                for (unsigned bi = 0; bi < mesh->mNumBones; ++bi) {
                    for (unsigned w = 0; w < mesh->mBones[bi]->mNumWeights; ++w) {
                        const aiVertexWeight& vw = mesh->mBones[bi]->mWeights[w];
                        if (vw.mVertexId == v) sk += pal[bi] * vw.mWeight * (bake * p);
                    }
                }
                if (sk.w == 0.0f) sk = bake * p;
                Grow(b, glm::vec3(sk));
            }
            return b;
        };

        // C from first bone + bake
        glm::mat4 C(1.0f); bool have = false;
        for (unsigned bi = 0; bi < mesh->mNumBones && !have; ++bi) {
            auto it = globals.find(mesh->mBones[bi]->mName.C_Str());
            if (it == globals.end()) continue;
            C = it->second * AiToGlm(mesh->mBones[bi]->mOffsetMatrix); have = true;
        }
        glm::mat4 bake = M; // engine behaviour: bake the mesh node's own world transform
        if (have) PrintM("  C (first bone):", C);
        PrintM("  bake (= mesh node world):", bake);

        buildPosed(-1.0);
        PrintBox("BIND(no bake)", skinBox(posed, glm::mat4(1.0f)));
        PrintBox("BIND(bake)", skinBox(posed, bake));
        if (t >= 0) {
            buildPosed(t);
            PrintBox("POSE t", skinBox(posed, bake));
        }
        printf("\n");
    }
    return 0;
}
