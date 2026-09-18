#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <glm/glm.hpp>

class Shader;

using ShaderVariantKey = uint32_t;

#include "ShaderPropType.h"

struct ShaderProperty {
    std::string Name;        // internal name, e.g. "_AlbedoMap"
    std::string DisplayName; // shown in inspector, e.g. "Albedo"
    ShaderPropType Type = ShaderPropType::Float;
    bool Hidden = false;     // in shader GLSL but not shown in inspector
    // #104 — Unity ShaderLab property attributes, written before the name: [HDR] (Color: an
    // intensity above 1 is allowed, shown as colour x 2^EV), [Toggle] (Float/Int shown as a
    // checkbox, 0/1), [Normal] (Texture2D expects a normal map), [NoScaleOffset] (no tiling UI;
    // informational until per-texture tiling exists), [Header(text)] (a section title above the
    // property), [Tooltip(text)] (inspector hover text).
    bool HDR = false;
    bool Toggle = false;
    bool NormalMap = false;
    bool NoScaleOffset = false;
    std::string Header;
    std::string Tooltip;

    float        DefaultFloat = 0.0f;
    bool         DefaultBool  = false;
    glm::vec4    DefaultVec   = {0.0f, 0.0f, 0.0f, 1.0f};
    std::string  DefaultTex;  // "white", "black", "normal"
    // #106 — Range(min, max) Float properties: slider limits for the material editor. Plain
    // Float properties have no range (the editor uses an unbounded drag field).
    bool         HasRange = false;
    float        RangeMin = 0.0f;
    float        RangeMax = 1.0f;
    int          PropIndex = 0;
};

// Pre-computed draw-time binding for one property. TextureUnit is -1 for non-texture properties.
struct PropertyBinding {
    ShaderPropType Type;
    int PropIndex   = 0;
    int TextureUnit = -1; // material-side unit (1-based upward); -1 for scalars/colors
};

class ShaderAsset {
public:
    // Parse a .shader file, returning nullptr on error.
    static std::shared_ptr<ShaderAsset> ParseFile(const std::string& path);

    // Lazily compile and return the shader variant for the given keyword bitmask.
    // Bit i of `key` = keyword i (from Keywords()) is active. Throws on compile failure.
    Shader* Variant(ShaderVariantKey key); // nullptr if that variant failed to compile (#100)
    // #100 — the compile/link log of the most recent failed variant, and a way to retry them.
    const std::string& LastCompileError() const { return m_LastCompileError; }
    void ForgetFailedVariants();

    // Per-property draw bindings (texture-unit assignments). Same for all variants.
    const std::vector<PropertyBinding>& Bindings() const { return m_Bindings; }

    const std::vector<ShaderProperty>& Properties() const { return m_Props; }
    const std::vector<std::string>&    Keywords()   const { return m_Keywords; }
    // #104 — whether a keyword is driven by the engine from the material's own settings (the
    // Standard lobes: _CLEARCOAT, _ANISO, ...) rather than toggled by the user. Anything else a
    // shader declares is a custom keyword, switched per material (MaterialAsset::Keywords).
    static bool IsBuiltinKeyword(const std::string& keyword);
    const std::string&                 Path()       const { return m_Path; }

private:
    std::string m_Path;
    // Stage references as written in the descriptor, resolved at compile time through
    // ShaderLibrary::ResolveRef against the descriptor's own folder (#208).
    std::string m_VertFile;
    std::string m_LastCompileError;
    std::string m_FragFile;
    std::vector<ShaderProperty>  m_Props;
    std::vector<PropertyBinding> m_Bindings;
    std::vector<std::string>     m_Keywords;

    std::unordered_map<ShaderVariantKey, std::unique_ptr<Shader>> m_Variants;

    void BuildBindings();
    void CompileVariant(ShaderVariantKey key);
};
