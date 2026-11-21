#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <glm/glm.hpp>
#include "Animation.h"
#include "ModelVertex.h"

struct ModelImportSettings;

// The cooked model cache (project/Library/Models, git-ignored): what an import of a model file produces - its meshes
// after merge and Simplify, the skeleton, the node tree, the clips - kept on disk so the next import of the same file
// with the same settings skips Assimp and meshoptimizer entirely. A forest scene imports ~860 FBX files (~410 MB) on
// every open; from the cache that is a read of the cooked data.
//
// An entry is keyed on the file's CONTENT (a hash of every byte), the import settings and the importer: Assimp's
// version and local patches, meshoptimizer's version, the vertex layout and a hash of the import code itself (CMake
// fills TARTARUS_MODEL_COOK_CODE from the source files that decide what an import produces). Any change to one of them
// is a different entry, so a stale one is never read; old entries age out (Prune). Textures are not cooked here: a
// material keeps the texture names the file gives (MaterialSource), and every load resolves them against the disk as
// it is now and loads them through the texture cache, exactly as a fresh import does.
//
// Entries are written to a temporary file and renamed into place, so several engines (worktrees, parallel test
// instances) importing the same file at once each see a whole entry or none. Only FBX and GLB files are cooked (an OBJ's
// .mtl and a glTF's buffers are other files the key would have to cover), and never one with embedded textures.
namespace ModelCache {

// What an import reads from one of the file's materials: the input of Model::ExtractMaterial, so a cooked import builds
// its materials the same way a fresh one does.
struct MaterialSource {
    enum Slot { TexDiffuse, TexBaseColor, TexNormals, TexUnknown, TexRoughness, TexMetalness, TexAmbientOcclusion, TexLightmap, TexEmissive, SlotCount };
    std::string Name;
    std::array<std::string, SlotCount> Textures; // the first texture the file names in each slot ("" = none)
    std::array<bool, SlotCount> Embedded{};      // ... and whether it is one of the file's embedded textures
    bool HasAlphaMode = false;                   // glTF alphaMode (its presence also marks a glTF material)
    std::string AlphaMode;
    bool HasAlphaCutoff = false;
    float AlphaCutoff = 0.5f;
    bool HasDiffuse = false, HasEmissive = false;
    glm::vec3 Diffuse{0.0f}, Emissive{0.0f};
    bool HasTwoSided = false;
    int TwoSided = 0;
    // KHR_texture_transform per slot, in ExtractMaterial's order: base colour, diffuse, normal, emissive, metallic,
    // roughness, occlusion.
    struct UvTransform {
        bool Has = false;
        glm::vec2 Scaling{1.0f}, Translation{0.0f};
        float Rotation = 0.0f;
    };
    std::array<UvTransform, 7> UvTransforms{};
    bool HasMetallic = false, HasRoughness = false;
    float Metallic = 0.0f, Roughness = 0.0f;

    bool AnyEmbedded() const {
        for (bool e : Embedded)
            if (e) return true;
        return false;
    }
};

// One ModelMesh as the import made it, before the mesh's own constructor (triangle order, GPU upload) runs.
struct MeshRecord {
    std::vector<ModelVertex> Vertices;
    std::vector<unsigned int> Indices;
    std::vector<glm::vec3> Posed; // a skinned mesh's bind-pose positions (ModelMesh::SetLocalPositions); empty otherwise
    unsigned int MaterialIndex = 0;
    bool VertexColors = false;
    bool Skinned = false;
};

struct Bone {
    std::string Name;
    int Id = 0;
    glm::mat4 Offset{1.0f};
};

// Everything one import produces apart from GPU objects and textures.
struct Cooked {
    std::vector<MaterialSource> Materials;
    std::vector<MeshRecord> Meshes; // in the model's mesh order
    std::vector<Bone> Bones;
    int BoneCounter = 0;
    int SkinVertices = 0, SkinOverflow = 0, SkinMaxInfluences = 0;
    float SkinMaxDropped = 0.0f;
    glm::mat4 GlobalInverseTransform{1.0f};
    std::vector<AnimNode> Nodes;
    std::vector<AnimationClip> Animations; // channels' TrackInfo is rebuilt (Finalize) on load
    std::vector<glm::mat4> BindPoseBones;
    glm::vec3 BoundsMin{1e30f}, BoundsMax{-1e30f};
    std::vector<glm::vec3> BoneBoxMin, BoneBoxMax;
    glm::vec3 UnweightedMin{1e30f}, UnweightedMax{-1e30f};
};

struct Key {
    std::uint64_t Source = 0;   // the file's content hash
    std::uint64_t Settings = 0; // the import settings and the importer's version
    std::uint64_t SourceBytes = 0;
    bool Valid = false;         // false: this import isn't cached (off, not a cooked format, unreadable file)
};

// Whether imports go through the cache: off with TARTARUS_NO_MODEL_CACHE=1 (A/B runs, the same-output check).
bool Enabled();
// The key of importing `path` with `settings`: reads and hashes the whole file. Invalid when the cache is off, the
// file isn't an FBX / GLB or can't be read, or the settings ask for something a fresh import does differently
// (TARTARUS_SKIN_AUDIT).
Key MakeKey(const std::string& path, const ModelImportSettings& settings);
// Hashes of the settings and the importer that a key mixes in (exposed for the unit tests).
std::uint64_t HashSettings(const ModelImportSettings& settings);
std::uint64_t HashBytes(const void* data, std::size_t size, std::uint64_t seed = 0);

// Reads the entry for `key`; false on a miss or any mismatch (then nothing in `out` is to be used).
bool Load(const Key& key, Cooked& out);
// Writes the entry for `key` (temp file + rename). Never throws; a failed write just leaves no entry.
void Store(const Key& key, const Cooked& cooked);

// The cache folder's size and entry count (Preferences), and removing every entry.
struct Usage {
    std::uint64_t Bytes = 0;
    int Entries = 0;
};
Usage DiskUsage();
int Clear();
// Removes entries unused for 30 days, abandoned temp files, and the least recently used entries past the size cap.
void Prune();
// The folder (tests point it at a scratch folder; "" = project/Library/Models).
void SetDirectoryForTests(const std::string& dir);
// While set, MakeKey answers "not cached": imports go to the file (--model-cache-check's fresh imports).
void SetBypass(bool bypass);

// Since start: imports answered from the cache, imports cooked into it, and the time each kind took (ms).
struct Stats {
    int Hits = 0, Cooked = 0;
    double HitMs = 0.0, CookMs = 0.0;
};
Stats GetStats();
void RecordHit(double ms);
void RecordCook(double ms);

} // namespace ModelCache
