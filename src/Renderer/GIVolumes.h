#pragma once

#include "GIData.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Shader;
class World;

// The scene's GI Volumes (GIVolumeComponent) on the GPU, for the model shader's baked bounce light (GIVolumes.glsl).
//
// Every baked volume x layer is a "brick" in one RGBA16F 3D atlas on texture unit 28: nx x ny x 3nz texels, the R, G
// and B convolved-L1 coefficients (GIData.h) one slab after another along z. A std140 UBO on binding 4 holds, per
// volume, its box (world -> box rows, half extents, fade), grid, intensity and, per layer, the brick origin and the
// layer's weight this frame: 1 while its circuit's lights are on, 0 off, the Outside always 1. The atlas is rebuilt
// only when the set of baked volumes changes (a load, a re-bake, a moved or resized volume); a switch flip only
// rewrites the UBO's weights.
//
// Volumes with no bake (or a bake for a different grid) are left out; no volumes at all = uGIEnabled 0 and the shader
// skips the lookup, bit-identical to before.
class GIVolumes {
public:
    static constexpr int kMaxVolumes = 32;
    static constexpr int kMaxLayers = 4;
    static constexpr int kUnit = 28;      // TextureUnits::GIAtlas
    static constexpr int kUboBinding = 4; // uniform block GIBlock

    GIVolumes();
    ~GIVolumes();
    GIVolumes(const GIVolumes&) = delete;
    GIVolumes& operator=(const GIVolumes&) = delete;

    // Once per frame, before any scene draw: Gather, then the atlas (when the bakes changed) and the UBO.
    void Update(const World& world);
    // The CPU half of Update (no GL): the baked volumes and this frame's layer weights.
    void Gather(const World& world);
    // uGIEnabled, the atlas on unit 28 and the UBO on binding 4. Every program that shades (ApplyFrameState).
    void Bind(Shader& program) const;
    int Count() const { return (int)m_Volumes.size(); }
    void Release();

    // --- The baker (--bake-gi) ---
    // Bakes held in memory instead of the volumes' Bake Files (bounce two samples bounce one's result); null = off.
    void SetOverride(std::shared_ptr<const GI::BakeFile> bakes) { m_Override = std::move(bakes); }
    // Off: uGIEnabled 0 whatever is baked (bounce one). On by default.
    void SetEnabled(bool on) { m_Enabled = on; }
    // Only the layers of circuit `circuit` ("" = the Outside) shine, at weight 1, whatever the switches say;
    // nullptr = normal (circuit state).
    void SetSolo(const std::string* circuit) { m_SoloOn = circuit != nullptr; m_Solo = circuit ? *circuit : std::string(); }

    // --- Pure helpers (unit tested) ---
    // Each circuit's weight from its lights: 1 if any of its Lights is active (not Deactivated or Inactive), 0 if it has
    // lights and none is; a circuit with no Light at all follows its switch (LightSwitch), else counts as on.
    static std::unordered_map<std::string, float> CircuitWeights(const World& world);
    // Shelf-packs bricks of the given texel sizes into one 3D atlas: rows along x up to `maxWidth`, rows stacked along
    // z. Returns each brick's origin; `atlas` = the atlas size.
    static std::vector<glm::ivec3> Pack(const std::vector<glm::ivec3>& sizes, int maxWidth, glm::ivec3& atlas);

    // One gathered volume (the editor's probe gizmo reads these).
    struct Volume {
        std::string Name;
        GI::Box Bounds;
        glm::ivec3 Dims{0};
        float Fade = 0.05f, Intensity = 1.0f, Spacing = 0.5f;
        std::shared_ptr<const GI::BakeFile> File;
        const GI::VolumeBake* Bake = nullptr;
        std::vector<int> Layers;      // indices into Bake->Layers, at most kMaxLayers
        std::vector<float> Weights;   // this frame, per entry of Layers
        bool ReplacesSky = false;     // has an Outside layer
    };
    const std::vector<Volume>& Volumes() const { return m_Volumes; }
    // The irradiance the shader would add at p for normal n (all volumes, weighted and faded), and the total fade.
    glm::vec3 SampleCPU(const glm::vec3& p, const glm::vec3& n, float* fade = nullptr) const;

private:
    void Rebuild();
    void WriteUbo();

    std::vector<Volume> m_Volumes;
    std::vector<glm::ivec3> m_BrickOrigin; // per volume x layer, flattened in m_Volumes order
    std::string m_AtlasKey;
    unsigned int m_Atlas = 0;
    unsigned int m_Ubo = 0;
    glm::ivec3 m_AtlasSize{0};
    std::vector<float> m_LastUbo;
    std::shared_ptr<const GI::BakeFile> m_Override;
    bool m_Enabled = true;
    bool m_SoloOn = false;
    std::string m_Solo;
};
