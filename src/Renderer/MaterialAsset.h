#pragma once
#include "Material.h"
#include <memory>
#include <string>

class AssetLibrary;
class ShaderAsset;

// A named, file-backed material (.mat JSON, version 1). Wraps a Material struct with a path so
// it can be browsed in the Asset Browser, drag-dropped onto renderers, and referenced from scene
// files as a first-class asset. PR 4 introduces the asset layer only — the scene draw loop
// does not route through MaterialAsset yet (that wiring comes in PR 5).
struct MaterialAsset {
    std::string Path;  // absolute path to the .mat file on disk
    std::string Name;  // display name, defaults to the filename stem

    // Optional shader asset link. When set, the inspector and BindMaterial use the data-driven
    // property path from ShaderAsset::Properties() instead of the hardcoded PBR field layout.
    std::string ShaderPath;
    std::shared_ptr<ShaderAsset> Shader;

    // Render queue — controls when this material is drawn relative to others.
    // Opaque: depth-tested, drawn front-to-back by material key (default).
    // AlphaTest: like Opaque but uses clip() for cutout foliage / fences.
    // Transparent: no depth write, back-to-front sorted, blended via uOpacity.
    enum class Queue { Opaque = 0, AlphaTest = 1, Transparent = 2 };
    Queue RenderQueue = Queue::Opaque;
    int   QueueIndex  = 2000;  // sort order within the queue (lower = drawn first)
    float Opacity     = 1.0f;  // surface alpha, used only when RenderQueue == Transparent

    // PBR properties. Texture shared_ptr slots are populated by Load() when `lib` is non-null;
    // they remain null when loaded without a library (e.g. Save checks texture paths only).
    Material Mat;

    // Serialized texture paths (parallel to Mat's shared_ptr slots). Written to the .mat file
    // and used by Load() to resolve textures via AssetLibrary. The shared_ptrs in Mat are only
    // filled when Load() is given a non-null lib.
    std::string AlbedoMapPath;
    std::string NormalMapPath;
    std::string MetallicRoughnessMapPath;
    std::string MetallicMapPath;
    std::string RoughnessMapPath;
    std::string AOMapPath;
    std::string EmissiveMapPath;

    // Loads a MaterialAsset from a .mat JSON file. Resolves and loads textures via `lib` when
    // non-null (they remain null otherwise). Returns nullptr on I/O or parse error.
    static std::shared_ptr<MaterialAsset> Load(const std::string& path,
                                               AssetLibrary* lib = nullptr);

    // Creates a new MaterialAsset at `path` with default (white, 0.5 roughness) settings and
    // writes the .mat file. Returns nullptr on I/O failure. Does NOT register with AssetLibrary.
    static std::shared_ptr<MaterialAsset> CreateDefault(const std::string& path);

    // Saves Mat and the texture path strings to the .mat file. Returns false on I/O error.
    bool Save() const;

    // Property access by shader property name (e.g. "_BaseColor"). Used by the data-driven
    // inspector and BindMaterial to map ShaderProperty names to Material field values.
    static const std::shared_ptr<Texture>& GetTexture(const Material& m, const std::string& name);
    static glm::vec3 GetColor(const Material& m, const std::string& name);
    static float     GetFloat(const Material& m, const std::string& name);
    static bool      GetBool (const Material& m, const std::string& name);
    static void SetTexture(Material& m, const std::string& name, const std::shared_ptr<Texture>& tex);
    static void SetColor  (Material& m, const std::string& name, const glm::vec3& v);
    static void SetFloat  (Material& m, const std::string& name, float v);
    static void SetBool   (Material& m, const std::string& name, bool v);
};
