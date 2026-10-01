#pragma once
#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

class Shader;

// A small immediate-mode 2D batcher for the Play HUD: text from a bitmap atlas baked at first use with
// stb_truetype (JetBrains Mono, OFL, from assets/fonts), plus solid rectangles, triangles, lines and
// chevrons that share the same atlas (a white texel), so a whole frame of HUD is one draw call.
//
// Coordinates are pixels from the top-left of the target. Sizes passed in are "1080p pixels": the
// batch scales them by max(0.75, height / 1080), like the other HUD overlays.
//
//   hud.Begin(w, h);  hud.Text(...); hud.Rect(...); hud.Flush(fbo);
class HudText {
public:
    enum class Align { Left, Center, Right };

    HudText();
    ~HudText();
    HudText(const HudText&) = delete;
    HudText& operator=(const HudText&) = delete;

    void Begin(int width, int height);
    float Scale() const { return m_Scale; }           // pixels per 1080p pixel
    int Width() const { return m_W; }
    int Height() const { return m_H; }

    // `y` is the top of the line. Returns the width drawn (in target pixels).
    float Text(float x, float y, const std::string& text, float size, const glm::vec4& color, Align align = Align::Left,
               bool shadow = true);
    float Measure(const std::string& text, float size) const;
    float LineHeight(float size) const;

    void Rect(float x, float y, float w, float h, const glm::vec4& color);               // target pixels
    void Tri(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec4& color);
    void Line(const glm::vec2& a, const glm::vec2& b, float thickness, const glm::vec4& color);
    // A chevron (">" shape) pointing along `angle` (radians, 0 = up the screen, clockwise).
    void Chevron(const glm::vec2& center, float angle, float size, float thickness, const glm::vec4& color);
    // The text-sized helpers above take 1080p units; these take target pixels.

    // Draws everything queued into `dstFbo` (alpha-blended) and clears the queue.
    void Flush(unsigned int dstFbo);
    bool Ready() const { return !m_Failed; }
    size_t Vertices() const { return m_Verts.size(); }

private:
    struct Vertex { float X, Y, U, V, R, G, B, A; };
    bool EnsureResources();
    void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const glm::vec4& c);
    void Glyphs(float x, float y, const std::string& text, float size, const glm::vec4& color);

    std::vector<Vertex> m_Verts;
    std::unique_ptr<Shader> m_Shader;
    unsigned int m_VAO = 0, m_VBO = 0, m_Atlas = 0;
    int m_W = 0, m_H = 0;
    float m_Scale = 1.0f;
    bool m_Failed = false, m_Tried = false;
    // Baked glyphs (ASCII 32..126) in an opaque blob (stbtt_bakedchar[95]), the bake size and metrics.
    std::vector<unsigned char> m_Glyphs;
    float m_BakePx = 40.0f, m_Ascent = 0.0f, m_Descent = 0.0f;
    float m_WhiteU = 0.0f, m_WhiteV = 0.0f;
    static constexpr int kAtlas = 512;
};
