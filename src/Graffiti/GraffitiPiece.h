#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A graffiti piece (.graffiti JSON, graffitiVersion 1): the editable recipe the Graffiti Designer
// works on (docs/GRAFFITI_DESIGNER.md). It bakes to a texture set plus an auto-generated
// DA_Piece_<name>.decal, which is what the decal renderer draws; the recipe stays the source.
// Everything that shapes the bake is in here, quantised the way it is saved (colours as #rrggbb,
// spray samples as integers), so a piece in memory and the same piece re-read from disk bake to
// the same bytes.
//
// {
//   "graffitiVersion": 1,
//   "canvas": { "width": 2048, "height": 1024, "metres": [2, 1] },
//   "seed": 1234,
//   "paint": { "roughness": 0.45, "relief": 0.35, "showThrough": 0.3, "drips": true,
//              "glow": "#ff40c0", "glowStrength": 0, "uv": false },
//   "weathering": { "fade": 0, "bleach": 0, "peel": 0, "buff": 0 },
//   "layers": [
//     { "type": "text", "name": "Letters", "visible": true, "opacity": 1, "blend": "normal",
//       "clip": false, "color": "#f2e640", "gradient": { "on": false, "to": "#e04020", "angle": 90 },
//       "outline": { "on": true, "color": "#101010", "width": 0.012 },
//       "shadow": { "on": false, "color": "#000000", "offset": [0.01, 0.015], "blur": 0.01, "opacity": 0.6 },
//       "metal": 0, "glow": 0,
//       "text": "KING", "alphabet": "Tag", "position": [0.5, 0.5], "size": 0.5, "rotation": 0,
//       "kerning": 0, "jitter": 0.2, "warp": "arc", "warpAmount": 0.3,
//       "letters": [ { "index": 2, "color": "#40c0ff" } ] },
//     { "type": "spray", "name": "Fill-in", ...shared keys...,
//       "strokes": [ { "cap": "fat", "color": "#e02020", "erase": false, "flow": 1, "angle": 45,
//                      "samples": [[x, y, pressure, distanceMm, timeMs], ...] } ] },
//     { "type": "stamp", ...shared keys..., "texture": "assets/...png", "luma": true,
//       "textureColor": false, "position": [0.5, 0.5], "size": 0.4, "aspect": 1, "rotation": 0 },
//     { "type": "fill", ...shared keys... }
//   ]
// }
namespace Graffiti {

enum class LayerType : uint8_t { Fill = 0, Stamp, Text, Spray };
enum class Blend : uint8_t { Normal = 0, Multiply, Screen, Add, Erase };
enum class Cap : uint8_t { Fat = 0, Skinny, Calligraphy };
enum class Warp : uint8_t { None = 0, Arc, Wave };

// The four alphabets the Text tool sets letters from (assets/Decals/Graffiti/Alphabets/<name>/).
inline constexpr const char* kAlphabets[] = {"Tag", "Brush", "Bubble", "Block"};

// One recorded spray sample, in the integer units it is saved in: positions in 1/100000 of the
// canvas (x right, y down; a little past the edges is allowed), pressure in 1/1000, the can's
// distance from the wall in millimetres, time in milliseconds since the stroke started.
struct SpraySample {
    int32_t X = 0, Y = 0;
    uint16_t Pressure = 1000;
    uint16_t DistanceMm = 150;
    int32_t TimeMs = 0;

    static constexpr float kPosUnits = 100000.0f;
    glm::vec2 Pos() const { return glm::vec2((float)X, (float)Y) / kPosUnits; }
    float Pressure01() const { return (float)Pressure / 1000.0f; }
    float Distance() const { return (float)DistanceMm / 1000.0f; } // metres
    float Time() const { return (float)TimeMs / 1000.0f; }         // seconds
    // Quantises a live sample to what the file keeps.
    static SpraySample Make(glm::vec2 uv, float pressure, float distanceMetres, float timeSeconds);
};

struct Stroke {
    Cap Tip = Cap::Fat;
    glm::vec3 Color = glm::vec3(230.0f, 26.0f, 26.0f) / 255.0f; // sRGB, on the #rrggbb grid like every piece colour
    bool Erase = false;                // takes paint off this layer instead of adding it
    float Flow = 1.0f;                 // 0..1 paint per second, on top of the cap's own rate
    float Angle = 45.0f;               // degrees: the calligraphy nib's angle
    std::vector<SpraySample> Samples;
};

// A per-letter override of a Text layer's fill and outline colours.
struct LetterStyle {
    int Index = 0; // into the layer's text, by character
    bool HasFill = false, HasOutline = false;
    glm::vec3 Fill{1.0f}, Outline{0.0f};
};

struct Layer {
    LayerType Type = LayerType::Spray;
    std::string Name;
    bool Visible = true;
    float Opacity = 1.0f;
    Blend Mode = Blend::Normal;
    bool Clip = false; // only where the layers below have paint (a fill inside the letters)

    // Shared by every type. Colours are sRGB; lengths are fractions of the canvas height.
    glm::vec3 Color = glm::vec3(242.0f, 230.0f, 64.0f) / 255.0f; // (defaults on the #rrggbb grid too)
    bool Gradient = false;
    glm::vec3 GradientTo = glm::vec3(230.0f, 64.0f, 26.0f) / 255.0f;
    float GradientAngle = 90.0f; // degrees: 0 left to right, 90 top to bottom
    bool Outline = false;
    glm::vec3 OutlineColor = glm::vec3(13.0f / 255.0f);
    float OutlineWidth = 0.012f;
    bool Shadow = false;
    glm::vec3 ShadowColor{0.0f};
    glm::vec2 ShadowOffset{0.01f, 0.015f};
    float ShadowBlur = 0.01f;
    float ShadowOpacity = 0.6f;
    float Metal = 0.0f; // chrome
    float Glow = 0.0f;  // the piece's glow colour, masked by this layer

    // Stamp and Text placement: the centre in canvas uv, height as a fraction of the canvas.
    glm::vec2 Position{0.5f};
    float Size = 0.4f;
    float Rotation = 0.0f; // degrees

    // Stamp.
    std::string Texture; // project-relative
    bool Luma = true;          // opacity from luminance (white-on-black alpha packs)
    bool TextureColor = false; // the image's own colours instead of the layer's
    float Aspect = 1.0f;       // on top of the image's own

    // Text.
    std::string Text;
    std::string Alphabet = "Tag";
    float Kerning = 0.0f; // fraction of the letter height added between letters
    float Jitter = 0.15f; // 0..1 baseline and spin jitter, from the piece seed
    Warp WarpMode = Warp::None;
    float WarpAmount = 0.3f; // arc: 0..1 of a half circle; wave: 0..1 of the letter height
    std::vector<LetterStyle> Letters;

    // Spray.
    std::vector<Stroke> Strokes;
};

struct Piece {
    std::string Path; // absolute path to the .graffiti file
    std::string Name; // file stem without GP_
    bool Missing = false;

    int Width = 2048, Height = 1024;   // canvas texels: 512 | 1024 | 2048 per side
    glm::vec2 Metres{2.0f, 1.0f};      // physical size on the wall: the decal's default size
    uint32_t Seed = 1;

    float Roughness = 0.45f;  // the paint's roughness (the .decal's paint roughness)
    float Relief = 0.35f;     // paint-thickness bump
    float ShowThrough = 0.3f; // 0..1: thin paint (overspray, dusting) lets the wall show through
    bool Drips = true;
    glm::vec3 GlowColor = glm::vec3(255.0f, 64.0f, 191.0f) / 255.0f;
    float GlowStrength = 0.0f; // emissive; masked per layer by Layer::Glow
    bool UV = false;           // the glow only shows under UV light
    float Fade = 0.0f, Bleach = 0.0f, Peel = 0.0f, Buff = 0.0f; // weathering the .decal starts with

    std::vector<Layer> Layers;

    static constexpr int kMinSide = 512, kMaxSide = 2048;
    static int SnapSide(int s);

    static std::shared_ptr<Piece> Load(const std::string& path);
    // Parses `json` (the file's bytes); `path` names it in warnings.
    static std::shared_ptr<Piece> FromString(const std::string& json, const std::string& path);
    // Keys in schema order, so the same piece always writes the same bytes.
    std::string ToString() const;
    bool Save() const; // atomic, to Path

    // The baked files beside the recipe (absolute): <dir>/<Name>/T_Decal_Piece_<Name>_<Map>.png and
    // <dir>/DA_Piece_<Name>.decal.
    std::string BakeDir() const;
    std::string MapPath(const char* map) const; // "Albedo", "Normal", "Surface", "Aux"
    std::string DecalPath() const;
};

const char* LayerTypeName(LayerType t);
const char* BlendName(Blend b);
const char* CapName(Cap c);
const char* WarpName(Warp w);
// "#rrggbb" <-> sRGB 0..1, quantised to 8 bits (Piece colours are always on that grid).
std::string ToHex(const glm::vec3& srgb);
bool FromHex(const std::string& hex, glm::vec3& out);
glm::vec3 Quantise(const glm::vec3& srgb);

} // namespace Graffiti
