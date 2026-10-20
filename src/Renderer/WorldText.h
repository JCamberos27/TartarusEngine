#pragma once
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Shader;
class World;

// World-space text (WorldTextComponent): a signed-distance atlas of JetBrains Mono baked once with stb_truetype,
// so labels stay sharp from a hand's width to across a range, and every visible label of a view in one
// instanced draw (two when some are On Top). Labels past their Cull Distance or outside the frustum are skipped
// on the CPU; between Fade Start and Cull Distance they fade out. Unlit: the colour is scaled against the scene's
// exposure, so Brightness 1 reads as plain white whatever the scene's EV, and above 1 it blooms.
namespace WorldTextLayout {

// Unicode code points of UTF-8 text; malformed bytes are dropped.
std::vector<std::uint32_t> DecodeUtf8(const std::string& text);

// The glyphs the atlas holds: printable ASCII plus a few measurement signs (degree, times, plus-minus, squared,
// cubed, middle dot, en dash). Anything else draws as '?'.
const std::vector<std::uint32_t>& Charset();

struct Glyph {
    glm::vec4 Uv{0.0f};        // atlas u0 v0 u1 v1 (v0 at the glyph's top)
    glm::vec2 Lo{0.0f}, Hi{0.0f}; // the quad, in line heights from the pen on the baseline, y up
    float Advance = 0.0f;      // line heights
    bool  Visible = false;     // false for a space
};

// The baked font. Metrics are in line heights (ascent + descent = 1), so a label's Size scales them straight.
struct Font {
    std::vector<std::uint32_t> Codepoints; // sorted, parallel to Glyphs
    std::vector<Glyph> Glyphs;
    std::vector<unsigned char> Atlas;      // R8, AtlasSize squared
    int AtlasSize = 0;
    float Ascent = 0.8f;                   // line heights above the baseline
    const Glyph* Find(std::uint32_t cp) const;
    bool Valid() const { return !Glyphs.empty() && AtlasSize > 0; }
};

// Bakes the charset from TrueType bytes. Returns an invalid font on failure.
Font Bake(const std::vector<unsigned char>& ttf, int atlasSize = 1024, float pixelHeight = 48.0f);

struct Quad {
    glm::vec2 Lo{0.0f}, Hi{0.0f}; // label-plane metres for Size 1 (x right, y up), relative to the label's origin
    glm::vec4 Uv{0.0f};
};

// Lays out `text` (lines split on '\n'): each line aligned on the origin (0 left, 1 centre, 2 right), the block
// centred vertically on it. Returns the quads and, through `extent`, the block's half-size (for culling).
std::vector<Quad> Layout(const Font& font, const std::string& text, int align, glm::vec2* extent = nullptr);

// 1 nearer than fadeStart, 0 at and past cullDistance, smooth between.
float Fade(float distance, float fadeStart, float cullDistance);

} // namespace WorldTextLayout

class WorldTextRenderer {
public:
    static WorldTextRenderer& Get(); // deliberately leaked (GL objects outlive static destruction otherwise)

    struct Stats { int Labels = 0, Culled = 0, Glyphs = 0; };

    // Draws `world`'s labels onto the bound HDR target (depth-tested against it, no depth write). `editorView`
    // honours the Scene tab's per-entity hides. Returns draw calls.
    int Draw(World& world, const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye, bool editorView);
    const Stats& LastStats() const { return m_Stats; }

    // Font baked and the program compiled before the first label needs them.
    void WarmUp() { EnsureResources(); }

private:
    WorldTextRenderer() = default;
    bool EnsureResources();

    struct GpuGlyph {
        glm::vec4 Anchor; // xyz world
        glm::vec4 Right;  // xyz world metres per layout unit across; w outline
        glm::vec4 Up;     // xyz world metres per layout unit up
        glm::vec4 Rect;   // layout lo.x lo.y hi.x hi.y
        glm::vec4 Uv;
        glm::vec4 Color;  // rgb linear (exposure-compensated), a fade
    };
    std::vector<GpuGlyph> m_Glyphs;
    WorldTextLayout::Font m_Font;
    std::unique_ptr<Shader> m_Program;
    unsigned m_Buffer = 0, m_Vao = 0, m_Atlas = 0;
    size_t m_Capacity = 0;
    bool m_Tried = false, m_Failed = false;
    Stats m_Stats;
};
