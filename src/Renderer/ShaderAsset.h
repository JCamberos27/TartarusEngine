#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <glm/glm.hpp>

class Shader;

using ShaderVariantKey = uint32_t;

enum class ShaderPropType { Float, Color, Texture2D, Bool, Vec2, Vec3, Vec4, Int };

struct ShaderProperty {
    std::string Name;        // internal name, e.g. "_AlbedoMap"
    std::string DisplayName; // shown in inspector, e.g. "Albedo"
    ShaderPropType Type = ShaderPropType::Float;
    bool Hidden = false;     // in shader GLSL but not shown in inspector

    float        DefaultFloat = 0.0f;
    bool         DefaultBool  = false;
    glm::vec4    DefaultVec   = {0.0f, 0.0f, 0.0f, 1.0f};
    std::string  DefaultTex;  // "white", "black", "normal"
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
    Shader* Variant(ShaderVariantKey key);

    // Per-property draw bindings (texture-unit assignments). Same for all variants.
    const std::vector<PropertyBinding>& Bindings() const { return m_Bindings; }

    const std::vector<ShaderProperty>& Properties() const { return m_Props; }
    const std::vector<std::string>&    Keywords()   const { return m_Keywords; }
    const std::string&                 Path()       const { return m_Path; }

private:
    std::string m_Path;
    std::string m_VertFile;
    std::string m_FragFile;
    std::vector<ShaderProperty>  m_Props;
    std::vector<PropertyBinding> m_Bindings;
    std::vector<std::string>     m_Keywords;

    std::unordered_map<ShaderVariantKey, std::unique_ptr<Shader>> m_Variants;

    void BuildBindings();
    void CompileVariant(ShaderVariantKey key);
};
