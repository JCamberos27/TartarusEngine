// Read-only FBX inventory in the engine's Assimp import convention.
// weapon_import_audit <fbx> <output.json>
#define main socket_probe_main
#include "socket_probe.cpp"
#undef main
#include "../../extern/json.hpp"
#include <fstream>
#include <functional>
using Json = nlohmann::json;
static Json Matrix(const glm::mat4& m) {
    Json rows = Json::array();
    for (int r = 0; r < 4; ++r) {
        Json row = Json::array();
        for (int c = 0; c < 4; ++c) row.push_back(m[c][r]);
        rows.push_back(row);
    }
    return rows;
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* sc = imp.ReadFile(argv[1], aiProcess_GlobalScale | aiProcess_Triangulate |
        aiProcess_FlipUVs | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
        aiProcess_LimitBoneWeights | aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes);
    if (!sc || !sc->mRootNode) return 1;
    Loaded rig;
    if (!LoadRig(argv[1], rig)) return 1;
    Json j;
    j["file"] = argv[1]; j["nodes"] = Json::array();
    for (const Node& n : rig.nodes) j["nodes"].push_back({{"name",n.name},{"parent",n.parent},{"bind",Matrix(n.bind)}});
    j["takes"] = Json::array();
    for (unsigned a=0;a<sc->mNumAnimations;++a) {
        const auto* an=sc->mAnimations[a];
        Json take={{"name",an->mName.C_Str()},{"duration",an->mDuration},{"tps",an->mTicksPerSecond},{"channels",Json::array()}};
        for(unsigned c=0;c<an->mNumChannels;++c) {
            const auto* ch=an->mChannels[c];
            Json channel={{"name",ch->mNodeName.C_Str()},{"positions",Json::array()},{"rotations",Json::array()},{"scales",Json::array()}};
            for(unsigned k=0;k<ch->mNumPositionKeys;++k) { auto& v=ch->mPositionKeys[k]; channel["positions"].push_back({v.mTime,v.mValue.x,v.mValue.y,v.mValue.z}); }
            for(unsigned k=0;k<ch->mNumRotationKeys;++k) { auto& v=ch->mRotationKeys[k]; channel["rotations"].push_back({v.mTime,v.mValue.w,v.mValue.x,v.mValue.y,v.mValue.z}); }
            for(unsigned k=0;k<ch->mNumScalingKeys;++k) { auto& v=ch->mScalingKeys[k]; channel["scales"].push_back({v.mTime,v.mValue.x,v.mValue.y,v.mValue.z}); }
            take["channels"].push_back(channel);
        }
        j["takes"].push_back(take);
    }
    const auto globals=Evaluate(rig,0);
    std::vector<glm::mat4> meshBind(sc->mNumMeshes,glm::mat4(1));
    std::function<void(const aiNode*,glm::mat4)> walk=[&](const aiNode* node,glm::mat4 parent) {
        auto g=parent*AiToGlm(node->mTransformation);
        for(unsigned m=0;m<node->mNumMeshes;++m) meshBind[node->mMeshes[m]]=g;
        for(unsigned c=0;c<node->mNumChildren;++c) walk(node->mChildren[c],g);
    };
    walk(sc->mRootNode,glm::mat4(1));
    const int rootIndex=Find(rig.nodes,Find(rig.nodes,"Main")>=0 ? "Main" : "root");
    const glm::mat4 fromRoot=rootIndex>=0 ? glm::inverse(globals[rootIndex]) : glm::mat4(1);
    const glm::mat4 ginv=glm::inverse(AiToGlm(sc->mRootNode->mTransformation));
    j["meshes"]=Json::array();
    for(unsigned m=0;m<sc->mNumMeshes;++m) {
        auto* mesh=sc->mMeshes[m];
        Json mm={{"name",mesh->mName.C_Str()},{"vertices",mesh->mNumVertices},{"faces",mesh->mNumFaces},{"material",sc->mMaterials[mesh->mMaterialIndex]->GetName().C_Str()},{"bones",Json::array()}};
        for(unsigned b=0;b<mesh->mNumBones;++b) { auto* bone=mesh->mBones[b]; mm["bones"].push_back({{"name",bone->mName.C_Str()},{"weights",bone->mNumWeights},{"offset",Matrix(AiToGlm(bone->mOffsetMatrix))}}); }
        std::vector<glm::vec4> posed(mesh->mNumVertices,glm::vec4(0));
        std::vector<float> weights(mesh->mNumVertices,0);
        for(unsigned b=0;b<mesh->mNumBones;++b) {
            auto* bone=mesh->mBones[b]; int bi=Find(rig.nodes,bone->mName.C_Str());
            if(bi<0) return 3;
            auto skin=fromRoot*ginv*globals[bi]*AiToGlm(bone->mOffsetMatrix)*meshBind[m];
            glm::vec3 centroid(0); float total=0;
            for(unsigned k=0;k<bone->mNumWeights;++k) {
                const auto& w=bone->mWeights[k]; const auto& v=mesh->mVertices[w.mVertexId];
                auto p=skin*glm::vec4(v.x,v.y,v.z,1);
                posed[w.mVertexId]+=p*w.mWeight; weights[w.mVertexId]+=w.mWeight;
                centroid+=glm::vec3(p)*w.mWeight; total+=w.mWeight;
            }
            if(total>0) centroid/=total;
            mm["bones"][b]["rootSpaceCentroid"]={centroid.x,centroid.y,centroid.z};
        }
        glm::vec3 lo(1e30f),hi(-1e30f),mean(0);
        for(unsigned v=0;v<mesh->mNumVertices;++v) {
            glm::vec3 p;
            if(weights[v]>1e-4f) p=glm::vec3(posed[v])/weights[v];
            else { const auto& raw=mesh->mVertices[v]; p=glm::vec3(fromRoot*ginv*meshBind[m]*glm::vec4(raw.x,raw.y,raw.z,1)); }
            lo=glm::min(lo,p); hi=glm::max(hi,p); mean+=p;
        }
        mean/=std::max(mesh->mNumVertices,1u);
        mm["rootSpaceBounds"]={{lo.x,lo.y,lo.z},{hi.x,hi.y,hi.z}};
        mm["rootSpaceCentroid"]={mean.x,mean.y,mean.z};
        j["meshes"].push_back(mm);
    }
    std::ofstream out(argv[2]); out<<j.dump();
    return out ? 0 : 1;
}
