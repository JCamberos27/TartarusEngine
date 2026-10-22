#pragma once
#include "GraffitiPiece.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// The Graffiti Designer's paint program (docs/GRAFFITI_DESIGNER.md): a piece's layers expanded
// into the primitives the compositor draws, on the CPU, with no GL. Every random choice (the
// overspray seed of a splat, a drip's wobble, a letter's baseline jitter) comes from the piece
// seed through a counter-based hash, and every brush step from the recorded samples, so the same
// recipe always gives the same primitive stream in the same order - the half of bake determinism
// that is unit-tested without a GPU (the compositor draws the stream in that order, no atomics).
namespace Graffiti {

// A counter-based hash (lowbias32): Rand(seed, stream, i) is a pure function, so nothing depends
// on how many numbers anything else drew before it.
uint32_t Hash(uint32_t x);
inline uint32_t Hash(uint32_t a, uint32_t b) { return Hash(a ^ Hash(b + 0x9E3779B9u)); }
inline uint32_t Hash(uint32_t a, uint32_t b, uint32_t c) { return Hash(Hash(a, b), c); }
inline float Rand01(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }
inline float RandSigned(uint32_t h) { return Rand01(h) * 2.0f - 1.0f; }

// The cap model: how wide the spray cone opens, how hard its core is, how much speckled overspray
// it throws past the cone, its paint rate, and the calligraphy nib's flattening.
struct CapModel {
    float ConeTan;   // footprint radius = distance x ConeTan (metres)
    float MinRadius; // metres: even pressed to the wall a cap leaves a dot this wide
    float Hardness;  // 0 soft Gaussian .. 1 near-flat core
    float Overspray; // speckle density past the core
    float Ring;      // the overspray reaches this many core radii
    float Rate;      // paint per second at full pressure (coverage units over the footprint)
    float Aspect;    // across / along for the nib: 1 round
};
const CapModel& CapFor(Cap c);

// The speckle seed of stroke `strokeIndex` of a layer. It comes from what the stroke holds (its first
// sample and cap) and its place in its own layer, never the layer's index, so reordering layers keeps
// every stroke's speckle; and it is known from the stroke's first sample, so the live canvas streams
// a stroke with exactly the seed the bake will use.
uint32_t StrokeSeed(uint32_t pieceSeed, const Stroke& s, uint32_t strokeIndex);

// One spray footprint, laid out as the compositor's per-instance vertex data (64 bytes).
struct Splat {
    glm::vec4 Shape;  // centre xy (canvas texels, y down), radius along / across the angle
    glm::vec4 Params; // angle (radians), flow (core opacity 0..1), overspray density, hardness
    glm::vec4 Color;  // linear rgb, paint thickness this splat lays down
    uint32_t Seed;    // the overspray speckle's hash stream
    uint32_t Flags;   // kSplatErase, kSplatDrip
    float Time;       // seconds into the stroke: the live canvas draws a splat once it is due
    float Ring;       // overspray reach in core radii
};
static_assert(sizeof(Splat) == 64, "Splat is the compositor's 64-byte instance record");
constexpr uint32_t kSplatErase = 1u << 0;
constexpr uint32_t kSplatDrip = 1u << 1;

// A textured quad: a stamp or one letter of a Text layer. Drawn one by one (each binds its image).
struct Quad {
    glm::vec2 Centre;    // canvas texels
    glm::vec2 Half;      // half size, texels
    float Angle;         // radians
    int Texture;         // index into Program::Textures
    glm::vec3 Fill;      // linear
    glm::vec3 OutlineColor; // linear
    float OutlinePx;     // 0: none (the glyph's own outline, drawn with its fill)
    bool Luma;           // opacity from luminance
    bool TextureColor;   // the image's colours instead of Fill
    bool OwnColor;       // Fill overrides the layer gradient (a per-letter colour)
    float Delay;         // seconds: the Designer cascades new letters in (not part of the bake)
    float Reveal = 1.0f; // 0..1 of that cascade: scale and opacity (always 1 in a bake)
};

// What a layer draws, in order. Splats are grouped into runs of one blend state (paint / erase).
struct LayerProgram {
    int Layer = -1;
    std::vector<Splat> Splats;
    struct Run { uint32_t Begin, End; bool Erase; };
    std::vector<Run> Runs;
    std::vector<Quad> Quads;
    float Duration = 0.0f; // the last splat's time: drips still running after the last sample
};

struct Program {
    int Width = 0, Height = 0;
    std::vector<std::string> Textures; // absolute image paths the quads index
    std::vector<LayerProgram> Layers;  // visible layers only, bottom to top
    // FNV-1a over every primitive's bytes and the texture list: equal programs hash equal.
    uint64_t Hash() const;
};

// An image's ink box and aspect, for laying out letters and sizing stamps. The compositor reads it
// from the image's alpha (GraffitiGlyphs); the tests pass a table.
struct ImageMetrics {
    bool Valid = false;
    float Aspect = 1.0f;          // width / height
    float InkLeft = 0.0f, InkRight = 1.0f; // 0..1 across the image: where the letter's ink is
};
using MetricsFn = std::function<ImageMetrics(const std::string& absolutePath)>;

// The image a character of `alphabet` is (absolute; empty when the alphabet has no such glyph).
// Tag / Brush / Bubble: A-Z (Brush also ? and +); Block: A-Z, 0-9 and # % + = - " ? ! [ ( :.
// Letters are case-insensitive.
std::string GlyphPath(const std::string& alphabet, char c);
// The characters `alphabet` can set, for the Text tool's hint.
const char* AlphabetCharacters(const std::string& alphabet);

// Streams one stroke's samples into splats, exactly as Build does for the whole stroke: the live
// canvas feeds samples as the mouse moves and draws what comes out, so the live picture and the
// bake are the same stream. Positions are canvas uv; output is canvas texels.
class StrokeBuilder {
public:
    // `pxPerMetre` from the canvas size and its metres; `seed` the piece seed mixed with the layer
    // and stroke index. `drips`: let pooled paint run.
    void Begin(const Stroke& stroke, int width, int height, float pxPerMetre, uint32_t seed, bool drips);
    // One more sample. Appends this step's splats (and the trails of any drips it set off) to `out`.
    void Add(const SpraySample& s, std::vector<Splat>& out);
    float LastTime() const { return m_LastTime; }

    // The pooled-paint grid: one cell per kCell texels. Exposed for the tests.
    static constexpr int kCell = 8;
    // Paint in a cell past this sets a drip off (coverage units, decaying as it dries).
    static constexpr float kDripThreshold = 6.0f;
    static constexpr float kDryingSeconds = 1.1f;

private:
    void Deposit(const glm::vec2& at, float radius, float amount, float time, std::vector<Splat>& out);
    void Drip(const glm::vec2& at, float mass, float time, uint32_t id, std::vector<Splat>& out);
    Splat Make(const glm::vec2& at, float radiusAlong, float radiusAcross, float angle, float flow, float time) const;

    const Stroke* m_Stroke = nullptr;
    CapModel m_Cap{};
    int m_W = 0, m_H = 0;
    float m_PxPerMetre = 1.0f;
    uint32_t m_Seed = 0;
    bool m_Drips = true;
    bool m_HasLast = false;
    glm::vec2 m_LastPos{0.0f};
    float m_LastPressure = 0.0f, m_LastDistance = 0.0f, m_LastTime = 0.0f;
    float m_Carry = 0.0f;   // emission progress carried to the next segment
    float m_Pending = 0.0f; // paint accumulated since the last splat
    glm::vec3 m_Linear{0.0f};
    uint32_t m_Count = 0, m_DripCount = 0;
    int m_GridW = 0, m_GridH = 0;
    std::vector<float> m_Wet, m_WetTime; // lazily dried pooled paint per cell
    std::vector<uint8_t> m_Dripped;      // a cell drips once per stroke
};

// The whole piece. `metrics` sizes letters and stamps. Splats come out sorted by (time, emission
// order) per stroke, strokes in recorded order.
Program Build(const Piece& piece, const MetricsFn& metrics);
// One layer (the live canvas rebuilds only the layer being edited).
LayerProgram BuildLayer(const Piece& piece, int layerIndex, const MetricsFn& metrics, std::vector<std::string>& textures);
// Sorts a stroke's splats by (time, emission order) and appends them to `layer` as runs.
void AppendStroke(LayerProgram& layer, std::vector<Splat>& strokeSplats, bool erase);

// Each layer's built output, kept between builds and looked up by a hash of everything the layer's
// build reads (the layer's type-specific fields and strokes, the piece's canvas, seed, metres and
// drips, and `epoch`: the image metrics' epoch). Opacity, blend, clip, gradient, shadow, metal, glow,
// names and visibility are applied at draw time, so changing them never rebuilds a layer; editing
// one layer rebuilds that layer only. Texture indices are stored per layer and remapped into the
// program's shared table on assembly, so a cached build is byte-identical to a fresh one.
struct BuildCache {
    struct Entry {
        LayerProgram Layer;                 // Quad::Texture indexes Textures below
        std::vector<std::string> Textures;
        uint32_t Used = 0;
    };
    std::unordered_map<uint64_t, Entry> Layers;
    uint32_t Generation = 0;
    int Built = 0, Reused = 0; // the last Build's counts (tests, Statistics)
};
Program Build(const Piece& piece, const MetricsFn& metrics, BuildCache& cache, uint32_t epoch);

inline glm::vec3 SrgbToLinear(const glm::vec3& c) {
    return glm::mix(c / 12.92f, glm::pow((c + 0.055f) / 1.055f, glm::vec3(2.4f)), glm::step(glm::vec3(0.04045f), c));
}

} // namespace Graffiti
