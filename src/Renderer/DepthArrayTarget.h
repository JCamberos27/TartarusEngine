#pragma once
#include <string>

// The depth texture array behind the three shadow maps (SpotShadowMap, PointShadowMap, CascadedShadowMap): a
// DEPTH_COMPONENT32F array with the hardware-compare sampler state, an optional depth-only FBO pair, an optional
// lazily made static-cache copy, and its GpuMemTrack entries. The owning map keeps its public API, its layer
// bookkeeping and its Begin* functions; this holds the GL objects.
class DepthArrayTarget {
public:
    struct Desc {
        unsigned int target = 0;     // GL_TEXTURE_2D_ARRAY or GL_TEXTURE_CUBE_MAP_ARRAY
        int resolution = 0;
        int layers = 0;              // texture depth (a cube array: cubes * 6)
        bool borderClamp = true;     // true: CLAMP_TO_BORDER with a white border (outside = lit); false: CLAMP_TO_EDGE (+R)
        bool fbos = true;            // also make the live FBO and the static FBO (a depth-only FBO each)
        const char* liveName = "";   // GpuMemTrack key names, keyed per owner
        const char* staticName = nullptr; // null: no static cache
    };

    DepthArrayTarget() = default;
    ~DepthArrayTarget() { Release(); }
    DepthArrayTarget(const DepthArrayTarget&) = delete;
    DepthArrayTarget& operator=(const DepthArrayTarget&) = delete;

    // (Re)creates everything; releases first. `owner` makes the GpuMemTrack keys per instance.
    void Create(const Desc& desc, const void* owner);
    void Release();

    // The static-cache texture (a plain 2D array of the same format and layer count), made on first use.
    void EnsureStatic() const;

    // A depth-only framebuffer (no colour draw buffers).
    static unsigned int CreateDepthFbo();

    unsigned int Texture() const { return m_Texture; }
    unsigned int Fbo() const { return m_Fbo; }
    unsigned int StaticTexture() const { return m_Static; }
    unsigned int StaticFbo() const { return m_StaticFbo; }
    // Points the live / static FBO's depth attachment at `layer`.
    void AttachLayer(int layer) const;
    void AttachStaticLayer(int layer) const;

private:
    unsigned int m_Texture = 0;
    unsigned int m_Fbo = 0;
    mutable unsigned int m_Static = 0;
    unsigned int m_StaticFbo = 0;
    int m_Resolution = 0;
    int m_Layers = 0;
    const void* m_Owner = nullptr;
    std::string m_LiveName, m_StaticName;
};
