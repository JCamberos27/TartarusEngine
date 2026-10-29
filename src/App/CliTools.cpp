#include "App/CliTools.h"

#include "BloodFxImport.h"
#include "BuildPipeline.h"
#include "CrashHandler.h"
#include "EnginePaths.h"
#include "FirstPersonProcedural.h" // RecoilAsset
#include "Game/Audio/AudioTest.h"
#include "AssetDatabase.h"
#include "KnifeFxImport.h"
#include "LayerRegistry.h"
#include "Log.h"
#include "Model.h"
#include "ModelVertex.h" // MAX_BONE_INFLUENCE
#include "ProjectPaths.h"
#include "ProjectSettings.h"
#include "ShaderLibrary.h"
#include "Tests/UnitTests.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <vector>

namespace {

// `--model-report <folder or model> [more ...]`: imports each .fbx/.obj/.gltf/.glb with its .meta's import settings
// (materials skipped) and prints its size in meters, its bounds and its mesh count, one line per model - checks
// scale and up-axis after an asset import. No GL needed. Exit 1 if any model fails to import.
int ModelReport(int argc, char** argv, int i) {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    const auto isModel = [](const fs::path& p) {
        std::string e = p.extension().string();
        for (char& c : e) c = (char)std::tolower((unsigned char)c);
        return e == ".fbx" || e == ".obj" || e == ".gltf" || e == ".glb";
    };
    for (int j = i + 1; j < argc && argv[j][0] != '-'; ++j) {
        std::error_code ec;
        if (fs::is_directory(argv[j], ec)) {
            for (const auto& f : fs::recursive_directory_iterator(argv[j], ec))
                if (f.is_regular_file() && isModel(f.path())) files.push_back(f.path());
        } else if (isModel(argv[j])) files.push_back(argv[j]);
    }
    std::sort(files.begin(), files.end());
    int failed = 0;
    for (const auto& f : files) {
        ModelImportSettings s;
        s.MaterialImportMode = ModelImportSettings::MaterialMode::None;
        s.ImportAnimations = false;
        if (std::ifstream in(f.string() + ".meta"); in) {
            const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_object() && j.contains("importer") && j["importer"].is_object()) {
                const auto& imp = j["importer"];
                s.GlobalScale = imp.value("globalScale", 1.0f);
                s.ImportSkeleton = imp.value("importSkeleton", true);
                s.SkinIgnoresMeshNode = imp.value("skinIgnoresMeshNode", false);
                s.MergeByMaterial = imp.value("mergeByMaterial", false);
                s.SourceUpAxis = Model::ParseUpAxis(imp.value("upAxis", std::string("Y")));
                if (imp.contains("pivotOffset") && imp["pivotOffset"].is_array() && imp["pivotOffset"].size() == 3)
                    s.PivotOffset = {imp["pivotOffset"][0].get<float>(), imp["pivotOffset"][1].get<float>(), imp["pivotOffset"][2].get<float>()};
                if (imp.contains("excludeNodes") && imp["excludeNodes"].is_array())
                    for (const auto& n : imp["excludeNodes"])
                        if (n.is_string()) s.ExcludeNodes.push_back(n.get<std::string>());
                if (imp.contains("meshNodes") && imp["meshNodes"].is_array())
                    for (const auto& n : imp["meshNodes"])
                        if (n.is_string()) s.MeshNodes.push_back(n.get<std::string>());
                if (imp.contains("rootTransform") && imp["rootTransform"].is_array() && imp["rootTransform"].size() == 16)
                    for (int k = 0; k < 16; ++k) s.RootTransform[k / 4][k % 4] = imp["rootTransform"][k].get<float>();
                if (imp.contains("unrotateNodes") && imp["unrotateNodes"].is_array())
                    for (const auto& n : imp["unrotateNodes"])
                        if (n.is_string()) s.UnrotateNodes.push_back(n.get<std::string>());
                if (imp.contains("moveNodes") && imp["moveNodes"].is_array())
                    for (const auto& n : imp["moveNodes"])
                        if (n.is_object() && n.contains("by") && n["by"].is_array() && n["by"].size() == 3)
                            s.MoveNodes.push_back({n.value("node", std::string()),
                                                   {n["by"][0].get<float>(), n["by"][1].get<float>(), n["by"][2].get<float>()}});
            }
        }
        const auto model = Model::ImportDeferred(f.string(), s);
        if (!model || model->MeshCount() == 0) {
            ++failed;
            std::cout << "[ModelReport] FAILED " << f.generic_string() << "\n";
            continue;
        }
        const glm::vec3 lo = model->BoundsMin(), hi = model->BoundsMax(), size = hi - lo;
        std::string mats;
        for (int m = 0; m < model->MeshCount(); ++m) {
            const std::string& name = model->MeshMaterial(m).Name;
            if (("|" + mats + "|").find("|" + name + "|") == std::string::npos) mats += (mats.empty() ? "" : "|") + name;
        }
        std::printf("[ModelReport] %s size %.3f %.3f %.3f min %.3f %.3f %.3f max %.3f %.3f %.3f meshes %d materials %s\n",
                    f.generic_string().c_str(), size.x, size.y, size.z, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z,
                    model->MeshCount(), mats.c_str());
        int skinned = 0, overflow = 0, most = 0;
        float lost = 0.0f;
        model->SkinStats(skinned, overflow, most, lost);
        if (skinned) {
            std::printf("[ModelReport]   skin: %d vertices, %d over %d influences (most %d), largest weight lost %.1f%%\n", skinned,
                        overflow, MAX_BONE_INFLUENCE, most, lost * 100.0f);
            // Where the rig's landmarks sit in the bind pose: pieces of one body must agree on these.
            std::string bones;
            for (const char* name : {"root", "pelvis", "hand_l", "hand_r"}) {
                glm::mat4 n(1.0f);
                if (!model->NodeTransform(name, n)) continue;
                char buf[96];
                std::snprintf(buf, sizeof buf, " %s %.3f %.3f %.3f", name, n[3][0], n[3][1], n[3][2]);
                bones += buf;
            }
            if (!bones.empty()) std::printf("[ModelReport]   bind:%s\n", bones.c_str());
        }
    }
    std::cout << "[ModelReport] " << files.size() << " models, " << failed << " failed" << std::endl;
    return failed ? 1 : 0;
}

// `--model-nodes <folder or model> [more ...]`: each model's node tree as Assimp reads it with the engine's import
// flags (unit scale applied, pivots folded), before any baking: one line per node with its parent, local transform
// (column-major) and the node-local bounds of its meshes. tools/assets/unity_import.py places Unity prefab pieces
// with it: Unity draws a mesh in its own node's space, the engine bakes the node tree into the vertices.
void PrintModelNodes(const aiScene* scene, const aiNode* node, const char* parent) {
    const aiMatrix4x4& t = node->mTransformation;
    std::printf("[ModelNodes]   node %s parent %s local %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g", node->mName.C_Str(),
                parent, t.a1, t.b1, t.c1, t.d1, t.a2, t.b2, t.c2, t.d2, t.a3, t.b3, t.c3, t.d3, t.a4, t.b4, t.c4, t.d4);
    for (unsigned int m = 0; m < node->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[node->mMeshes[m]];
        aiVector3D lo(1e30f), hi(-1e30f);
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            const aiVector3D& p = mesh->mVertices[v];
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
        }
        aiString mat;
        if (mesh->mMaterialIndex < scene->mNumMaterials) mat = scene->mMaterials[mesh->mMaterialIndex]->GetName();
        std::printf(" | mesh %s material %s aabb %g %g %g %g %g %g", mesh->mName.C_Str(), mat.C_Str(), lo.x, lo.y, lo.z, hi.x,
                    hi.y, hi.z);
    }
    std::printf("\n");
    for (unsigned int c = 0; c < node->mNumChildren; ++c) PrintModelNodes(scene, node->mChildren[c], node->mName.C_Str());
}

int ModelNodes(int argc, char** argv, int i) {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    for (int j = i + 1; j < argc && argv[j][0] != '-'; ++j) {
        std::error_code ec;
        const auto isFbx = [](const fs::path& p) {
            std::string e = p.extension().string();
            for (char& c : e) c = (char)std::tolower((unsigned char)c);
            return e == ".fbx";
        };
        if (fs::is_directory(argv[j], ec)) {
            for (const auto& f : fs::recursive_directory_iterator(argv[j], ec))
                if (f.is_regular_file() && isFbx(f.path())) files.push_back(f.path());
        } else if (isFbx(argv[j])) files.push_back(argv[j]);
    }
    std::sort(files.begin(), files.end());
    int failed = 0;
    for (const auto& f : files) {
        Assimp::Importer importer;
        importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
        // TARTARUS_NODES_PIVOTS=1 keeps the FBX pivot chain as "$AssimpFbx$_<part>" helper nodes (what the engine folds).
        char* pv = nullptr;
        size_t pvn = 0;
        _dupenv_s(&pv, &pvn, "TARTARUS_NODES_PIVOTS");
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, pv && *pv == '1');
        std::free(pv);
        const aiScene* scene = importer.ReadFile(f.string(), aiProcess_Triangulate | aiProcess_GlobalScale);
        if (!scene || !scene->mRootNode) {
            ++failed;
            std::cout << "[ModelNodes] FAILED " << f.generic_string() << "\n";
            continue;
        }
        double unit = 1.0;
        if (scene->mMetaData) scene->mMetaData->Get("UnitScaleFactor", unit);
        std::printf("[ModelNodes] %s unitscale %g\n", f.generic_string().c_str(), unit);
        PrintModelNodes(scene, scene->mRootNode, "-");
    }
    std::cout << "[ModelNodes] " << files.size() << " models, " << failed << " failed" << std::endl;
    return failed ? 1 : 0;
}

} // namespace

int RunEarlyCliTool(int argc, char** argv) {
    // Explicit, headless upgrade through the same parser/serializer as the Inspector.
    for (int i=1;i+1<argc;++i) if (std::string(argv[i])=="--migrate-recoil") {
        RecoilAsset asset; std::string error;
        if (!RecoilAsset::LoadFile(argv[i+1],asset,&error) || !asset.SaveFile(argv[i+1])) {
            std::cerr << "[RecoilMigration] " << error << '\n'; return 1;
        }
        std::cout << "[RecoilMigration] " << argv[i+1] << " -> RecoilAnimData v2\n"; return 0;
    }
    // #173 - `--unit-tests`: pure C++ tests, run before any window / GL / audio / PhysX exists
    // so they work on a GPU-less CI runner. Exit code = failed checks (0 = pass).
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--unit-tests") continue;
        CrashHandler::SetInteractive(false);
        const char* filter=i+1<argc && argv[i+1][0]!='-'?argv[i+1]:nullptr;
        return RunUnitTests(filter) == 0 ? 0 : 1;
    }
    // `--import-blood-fx <VolumetricBloodFX package dir>`: converts the volumetric blood sims and
    // decals into project/assets/Effects/Blood (git-ignored). No GL needed.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--import-blood-fx") continue;
        CrashHandler::SetInteractive(false);
        return BloodFxImport::ImportPackage(argv[i + 1], ProjectPaths::Resolve("assets/Effects/Blood")) ? 0 : 1;
    }
    // `--import-knife-fx <folder holding the extracted Knife packs>`: the Real Blood and PRO Effects textures
    // into project/assets/Effects/Knife (git-ignored). No GL needed.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--import-knife-fx") continue;
        CrashHandler::SetInteractive(false);
        return KnifeFxImport::ImportPacks(argv[i + 1], ProjectPaths::Resolve("assets/Effects/Knife")) ? 0 : 1;
    }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--model-report") continue;
        CrashHandler::SetInteractive(false);
        return ModelReport(argc, argv, i);
    }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--model-nodes") continue;
        CrashHandler::SetInteractive(false);
        return ModelNodes(argc, argv, i);
    }
    // `--audio-test [scene]`: the audio engine's own test, offline (no window, no audio device): a scripted tour of a scene's reverb zones.
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--audio-test") return RunAudioTest(argc, argv);
    return -1;
}

int RunBuildTool(const std::string& outDir, int cancelAt) {
    CrashHandler::SetInteractive(false);
    LayerRegistry::Load();
    AssetDatabase::ScanProject(); // #132 - GUIDs, so moved build scenes / shaders resolve
    ProjectSettings::Load();
    ShaderLibrary::Init(EnginePaths::Resolve("assets/shaders")); // #208 - shader validation resolves engine:// refs
    ProjectSettings::BuildSettings bs = ProjectSettings::Build();
    if (!outDir.empty()) bs.OutputDir = std::filesystem::absolute(outDir).string();
    if (bs.Scenes.empty() && !ProjectSettings::StartupScene().empty()) bs.Scenes.push_back(ProjectSettings::StartupScene());
    // On a worker thread with progress, the same way the editor's Build button runs it.
    BuildPipeline::Progress progress;
    auto future = std::async(std::launch::async, [&] { return BuildPipeline::Build(bs, &progress); });
    int lastShown = -1;
    while (future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
        const int done = progress.Done.load(), total = progress.Total.load();
        if (cancelAt >= 0 && done >= cancelAt) progress.Cancel.store(true);
        if (total > 0 && done * 10 / total != lastShown) {
            lastShown = done * 10 / total;
            std::cout << "[Build] " << BuildPipeline::StageLabel((BuildPipeline::Stage)progress.StageNow.load())
                      << " " << done << "/" << total << "\n";
        }
    }
    const BuildPipeline::Report r = future.get();
    std::cout << "[Build] " << r.Message << "\n";
    if (r.Ok) {
        std::cout << "[Build] " << r.ExePath << "\n";
        for (const auto& [group, bytes] : r.BytesByGroup)
            std::cout << "[Build]   " << group << ": " << bytes / 1024 << " KB\n";
    }
    return r.Ok ? 0 : 1;
}
