#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include "LightBuffer.h"

class IblProbe;
class Model;
class Shader;
struct MaterialAsset;

// #107 - renders a MaterialAsset onto a preview shape (Unity's material preview sphere), for the
// Inspector and the Asset Browser's material thumbnails.
//
// Draws through the same path the scene uses (Model::DrawSelected + the material's ShaderAsset
// variant program), so custom shaders, keywords, render state, cutout, transparency and
// transmission all show as they will in the scene. Lighting is a fixed studio setup: a key and a
// fill directional light plus image-based lighting baked from a neutral gradient sky, so metals
// have something to reflect. The backdrop turns into a checkerboard for see-through materials.
//
// Owns its GL objects (lazily created, freed in the destructor, like ModelPreviewRenderer). The
// IBL probe is shared between every live instance, since it's the same bake.
class MaterialPreviewRenderer {
public:
    enum class Shape { Sphere = 0, Cube, Cylinder, Torus, Plane, Count };
    static const char* ShapeName(Shape s);

    MaterialPreviewRenderer();
    ~MaterialPreviewRenderer();
    MaterialPreviewRenderer(const MaterialPreviewRenderer&) = delete;
    MaterialPreviewRenderer& operator=(const MaterialPreviewRenderer&) = delete;

    // yaw / pitch in radians around the shape. Returns the RGBA8 colour texture (valid until the
    // next Render with a different size), or 0 if the preview couldn't be drawn.
    unsigned int Render(const std::shared_ptr<MaterialAsset>& material, Shape shape,
                        float yaw, float pitch, int width, int height);

    // Everything a render of `m` depends on: values, flags, keywords, custom shader properties,
    // the linked shader, render queue / opacity and the GL identity of every bound texture (a
    // texture reimport makes a new GL texture). Cheap enough to check every frame; a changed key
    // means a cached preview is stale.
    static std::uint64_t ContentKey(const MaterialAsset& m);

private:
    void EnsureResources(int width, int height);
    void DrawBackdrop(bool checker, bool tonemap, int width, int height);

    static constexpr int kSupersample = 2;
    unsigned int m_FBO = 0, m_ColorTex = 0, m_DepthRBO = 0; // internal, kSupersample x the output
    unsigned int m_OutFBO = 0, m_OutTex = 0;                // the requested size
    unsigned int m_BackdropFBO = 0, m_BackdropTex = 0; // linear HDR backdrop, mipped: refraction source
    unsigned int m_EmptyVAO = 0;
    int m_W = 0, m_H = 0;
    int m_BackdropChecker = -1; // what m_BackdropTex holds (-1 = nothing yet)

    std::unique_ptr<Shader> m_Fallback; // built-in model shader, for materials without a ShaderAsset
    std::unique_ptr<Shader> m_Backdrop;
    std::array<std::shared_ptr<Model>, (size_t)Shape::Count> m_Shapes;
    std::shared_ptr<IblProbe> m_Ibl;
    LightBuffer m_Lights;
};
