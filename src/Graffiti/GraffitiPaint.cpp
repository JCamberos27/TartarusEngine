#include "GraffitiPaint.h"
#include "ProjectPaths.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace Graffiti {
namespace {

constexpr float kPi = 3.14159265358979f;
// A held can still lays paint down: a segment emits at least one splat per this much time, however
// little the cursor moved, so dwelling builds a pool (and, past the threshold, drips).
constexpr float kTimeStep = 1.0f / 90.0f;
// Splats sit this fraction of their radius apart along the stroke.
constexpr float kSpacing = 0.22f;
// Coverage is per area: a footprint this wide (metres) puts `amount` down as it is; a wider one,
// further from the wall, spreads the same paint thinner (inverse square).
constexpr float kRefRadius = 0.02f;

const CapModel kCaps[] = {
    //  cone   minR    hard  over  ring  rate  aspect
    {0.33f, 0.012f, 0.15f, 0.55f, 1.9f, 30.0f, 1.0f},  // Fat: a wide, soft cone that throws overspray
    {0.12f, 0.004f, 0.45f, 0.20f, 1.5f, 16.0f, 1.0f},  // Skinny: tight lines, little overspray
    {0.20f, 0.008f, 0.60f, 0.25f, 1.4f, 22.0f, 0.22f}, // Calligraphy: a flattened nib at a fixed angle
};

const char* const kSymbolNames[][2] = {
    {"#", "Hash"},   {"%", "Percent"}, {"+", "Plus"},  {"=", "Equals"},   {"-", "Minus"},  {"\"", "Quote"},
    {"?", "Question"}, {"!", "Exclaim"}, {"[", "BracketL"}, {"(", "ParenL"}, {":", "Colon"},
};

uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
template <class T> uint64_t FnvValue(uint64_t h, const T& v) { return Fnv(h, &v, sizeof(T)); }

glm::vec2 Rotate(const glm::vec2& v, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

uint32_t TextSeed(uint32_t pieceSeed, const std::string& text) {
    uint32_t h = Hash(pieceSeed ^ 0x7E47u);
    for (unsigned char c : text) h = Hash(h, c);
    return h;
}

std::string ResolveAsset(const std::string& p) {
    if (p.empty()) return {};
    return std::filesystem::u8path(p).is_absolute() ? p : ProjectPaths::Resolve(p);
}

int TextureIndex(std::vector<std::string>& textures, const std::string& path) {
    for (size_t i = 0; i < textures.size(); ++i)
        if (textures[i] == path) return (int)i;
    textures.push_back(path);
    return (int)textures.size() - 1;
}

} // namespace

uint32_t Hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

const CapModel& CapFor(Cap c) { return kCaps[std::min((int)c, 2)]; }

uint32_t StrokeSeed(uint32_t pieceSeed, const Stroke& s, uint32_t strokeIndex) {
    const SpraySample f = s.Samples.empty() ? SpraySample{} : s.Samples.front();
    return Hash(Hash(pieceSeed, (uint32_t)f.X, (uint32_t)f.Y), (uint32_t)f.TimeMs ^ ((uint32_t)s.Tip << 24), strokeIndex);
}

const char* AlphabetCharacters(const std::string& alphabet) {
    if (alphabet == "Block") return "A-Z 0-9 # % + = - \" ? ! [ ( :";
    if (alphabet == "Brush") return "A-Z ? +";
    return "A-Z";
}

std::string GlyphPath(const std::string& alphabet, char c) {
    const char u = (char)std::toupper((unsigned char)c);
    std::string suffix;
    if (u >= 'A' && u <= 'Z') {
        suffix = std::string(1, u);
    } else if (u >= '0' && u <= '9') {
        if (alphabet != "Block") return {};
        suffix = std::string(1, u);
    } else {
        for (const auto& s : kSymbolNames)
            if (s[0][0] == u) suffix = s[1];
        if (suffix.empty()) return {};
        const bool brush = alphabet == "Brush" && (u == '?' || u == '+');
        if (alphabet != "Block" && !brush) return {};
    }
    // Forward slashes whatever Resolve gives back: the program's texture table (hashed) is the same on every machine.
    return std::filesystem::u8path(ProjectPaths::Resolve("assets/Decals/Graffiti/Alphabets/" + alphabet + "/T_Decal_Alpha" + alphabet + "_" + suffix + ".png"))
        .generic_u8string();
}

// --- Spray ---------------------------------------------------------------------------------------

void StrokeBuilder::Begin(const Stroke& stroke, int width, int height, float pxPerMetre, uint32_t seed, bool drips) {
    m_Stroke = &stroke;
    m_Cap = CapFor(stroke.Tip);
    m_W = width;
    m_H = height;
    m_PxPerMetre = pxPerMetre;
    m_Seed = seed;
    m_Drips = drips && !stroke.Erase;
    m_HasLast = false;
    m_Carry = m_Pending = 0.0f;
    m_LastTime = 0.0f;
    m_Linear = SrgbToLinear(stroke.Color);
    m_Count = m_DripCount = 0;
    m_GridW = (width + kCell - 1) / kCell;
    m_GridH = (height + kCell - 1) / kCell;
    // assign() keeps the capacity: a builder reused stroke after stroke allocates once.
    m_Wet.assign((size_t)m_GridW * m_GridH, 0.0f);
    m_WetTime.assign(m_Wet.size(), 0.0f);
    m_Dripped.assign(m_Wet.size(), 0);
}

Splat StrokeBuilder::Make(const glm::vec2& at, float radiusAlong, float radiusAcross, float angle, float flow,
                          float time) const {
    Splat s;
    s.Shape = glm::vec4(at, radiusAlong, radiusAcross);
    s.Params = glm::vec4(angle, flow, m_Cap.Overspray, m_Cap.Hardness);
    s.Color = glm::vec4(m_Linear, 1.0f); // w: paint thickness per unit of coverage
    s.Seed = Hash(m_Seed, m_Count);
    s.Flags = m_Stroke->Erase ? kSplatErase : 0u;
    s.Time = time;
    s.Ring = m_Cap.Ring;
    return s;
}

void StrokeBuilder::Add(const SpraySample& sample, std::vector<Splat>& out) {
    const glm::vec2 pos = sample.Pos() * glm::vec2((float)m_W, (float)m_H);
    const float p = sample.Pressure01(), d = sample.Distance(), t = sample.Time();
    if (!m_HasLast) {
        m_HasLast = true;
        m_LastPos = pos;
        m_LastPressure = p;
        m_LastDistance = d;
        m_LastTime = t;
        return;
    }
    const float dt = std::max(t - m_LastTime, 0.0f);
    const float len = glm::length(pos - m_LastPos);
    auto radiusAt = [&](float dist, float pressure) {
        return std::max(m_Cap.MinRadius, dist * m_Cap.ConeTan) * m_PxPerMetre * (0.85f + 0.15f * pressure);
    };
    const float r0 = radiusAt(m_LastDistance, m_LastPressure), r1 = radiusAt(d, p);
    const float spacing = std::max(0.75f, 0.5f * (r0 + r1) * kSpacing);
    // Paint leaves the can by time (the trigger held), not by distance: a fast pass is light, a slow
    // one heavy, a still can pools.
    m_Pending += m_Cap.Rate * m_Stroke->Flow * 0.5f * (m_LastPressure + p) * dt;
    const float steps = len / spacing + dt / kTimeStep;
    const float progress = m_Carry + steps;
    const int n = (int)std::floor(progress);
    if (n > 0 && steps > 0.0f) {
        const float each = m_Pending / (float)n;
        m_Pending = 0.0f;
        for (int k = 0; k < n; ++k) {
            const float f = std::clamp(((float)(k + 1) - m_Carry) / steps, 0.0f, 1.0f);
            const glm::vec2 at = glm::mix(m_LastPos, pos, f);
            Deposit(at, glm::mix(r0, r1, f), each, glm::mix(m_LastTime, t, f), out);
        }
    }
    m_Carry = progress - (float)n;
    m_LastPos = pos;
    m_LastPressure = p;
    m_LastDistance = d;
    m_LastTime = std::max(m_LastTime, t);
}

void StrokeBuilder::Deposit(const glm::vec2& at, float radius, float amount, float time, std::vector<Splat>& out) {
    // Closer to the wall the same paint lands on less of it: coverage goes with the inverse area.
    const float ref = kRefRadius * m_PxPerMetre;
    const float density = amount * (ref * ref) / std::max(radius * radius * m_Cap.Aspect, 1e-4f);
    const float flow = 1.0f - std::exp(-density);
    const float angle = m_Cap.Aspect < 1.0f ? m_Stroke->Angle * (kPi / 180.0f) : 0.0f;
    out.push_back(Make(at, radius, radius * m_Cap.Aspect, angle, flow, time));
    m_Count++;
    if (!m_Drips) return;

    // Pool the paint into the grid under the core (Gaussian weights), drying since the cell was last
    // touched; a cell past the threshold drips, once per stroke.
    const float rc = std::max(radius * std::sqrt(m_Cap.Aspect), 1.0f); // a nib pools over its area, not its length
    const int x0 = std::max(0, (int)((at.x - rc) / kCell)), x1 = std::min(m_GridW - 1, (int)((at.x + rc) / kCell));
    const int y0 = std::max(0, (int)((at.y - rc) / kCell)), y1 = std::min(m_GridH - 1, (int)((at.y + rc) / kCell));
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx) {
            const glm::vec2 c((cx + 0.5f) * kCell, (cy + 0.5f) * kCell);
            const float q = glm::dot(c - at, c - at) / (rc * rc);
            if (q > 1.0f) continue;
            const size_t i = (size_t)cy * m_GridW + cx;
            float w = m_Wet[i] * std::exp(-(time - m_WetTime[i]) / kDryingSeconds);
            w += density * std::exp(-3.0f * q);
            m_WetTime[i] = time;
            if (w > kDripThreshold && !m_Dripped[i]) {
                m_Dripped[i] = 1;
                Drip(c, w - kDripThreshold * 0.5f, time, m_DripCount++, out);
                w = kDripThreshold * 0.35f;
            }
            m_Wet[i] = w;
        }
}

void StrokeBuilder::Drip(const glm::vec2& at, float mass, float time, uint32_t id, std::vector<Splat>& out) {
    // A bead that runs down the wall (canvas +y), thinning as it lays its trail, ending in a bulb.
    const uint32_t h = Hash(m_Seed ^ 0xD819u, id);
    const float m = std::min(mass, 4.0f);
    const float widthPx = std::max(1.2f, (0.0035f + 0.0012f * m) * m_PxPerMetre * (0.8f + 0.4f * Rand01(Hash(h, 1))));
    const float lengthPx = (0.025f + 0.045f * m) * m_PxPerMetre * (0.55f + 0.9f * Rand01(Hash(h, 2)));
    const float speed = (0.08f + 0.08f * Rand01(Hash(h, 3))) * m_PxPerMetre; // texels per second
    const float phase = Rand01(Hash(h, 4)) * 2.0f * kPi, wobble = widthPx * (0.15f + 0.35f * Rand01(Hash(h, 5)));
    const float start = time + 0.12f + 0.25f * Rand01(Hash(h, 6)); // the bead gathers before it runs
    const glm::vec2 origin = at + glm::vec2(RandSigned(Hash(h, 7)), RandSigned(Hash(h, 8))) * (0.4f * kCell);
    const float step = std::max(0.6f, widthPx * 0.4f);
    const int n = std::max(2, (int)(lengthPx / step));
    const CapModel saved = m_Cap;
    m_Cap.Overspray = 0.0f;
    m_Cap.Hardness = 0.85f;
    m_Cap.Ring = 1.0f;
    for (int k = 0; k <= n; ++k) {
        const float y = (float)k * step;
        const glm::vec2 p = origin + glm::vec2(std::sin(y / (widthPx * 6.0f) + phase) * wobble, y);
        if (p.y > (float)m_H + widthPx) break;
        const float taper = 1.0f - 0.35f * (float)k / (float)n;
        const float w = widthPx * (k == n ? 1.45f : taper) * 0.5f;
        Splat s = Make(p, w, w, 0.0f, 0.9f, start + y / speed);
        s.Flags |= kSplatDrip;
        s.Color.w = 1.4f; // a drip is thick paint: it shows in the relief
        out.push_back(s);
        m_Count++;
    }
    m_Cap = saved;
}

void AppendStroke(LayerProgram& layer, std::vector<Splat>& strokeSplats, bool erase) {
    // Drip trails are emitted when they start but run later: the stream is drawn in time order, ties
    // in emission order (stable), which is also the order the live canvas meets them.
    std::stable_sort(strokeSplats.begin(), strokeSplats.end(), [](const Splat& a, const Splat& b) { return a.Time < b.Time; });
    if (strokeSplats.empty()) return;
    const uint32_t begin = (uint32_t)layer.Splats.size();
    layer.Splats.insert(layer.Splats.end(), strokeSplats.begin(), strokeSplats.end());
    const uint32_t end = (uint32_t)layer.Splats.size();
    if (!layer.Runs.empty() && layer.Runs.back().Erase == erase && layer.Runs.back().End == begin)
        layer.Runs.back().End = end;
    else
        layer.Runs.push_back({begin, end, erase});
    layer.Duration = std::max(layer.Duration, strokeSplats.back().Time);
}

// --- Layers --------------------------------------------------------------------------------------

LayerProgram BuildLayer(const Piece& piece, int layerIndex, const MetricsFn& metrics, std::vector<std::string>& textures) {
    LayerProgram lp;
    lp.Layer = layerIndex;
    const Layer& l = piece.Layers[(size_t)layerIndex];
    const float W = (float)piece.Width, H = (float)piece.Height;
    const glm::vec3 fill = SrgbToLinear(l.Color);
    const glm::vec3 outline = SrgbToLinear(l.OutlineColor);

    if (l.Type == LayerType::Stamp) {
        const std::string path = ResolveAsset(l.Texture);
        const ImageMetrics m = path.empty() ? ImageMetrics{} : metrics(path);
        if (!m.Valid) return lp;
        Quad q{};
        q.Centre = l.Position * glm::vec2(W, H);
        q.Half = glm::vec2(l.Size * H * m.Aspect * l.Aspect, l.Size * H) * 0.5f;
        q.Angle = l.Rotation * (kPi / 180.0f);
        q.Texture = TextureIndex(textures, path);
        q.Fill = fill;
        q.OutlineColor = outline;
        q.Luma = l.Luma;
        q.TextureColor = l.TextureColor;
        lp.Quads.push_back(q);
    } else if (l.Type == LayerType::Text) {
        // Lay the letters out on a line by their ink (not their image boxes), centred on Position,
        // then warp, jitter and turn the line.
        const float size = l.Size * H;
        const uint32_t seed = TextSeed(piece.Seed, l.Text);
        struct Placed { float X; int Index; int Texture; float Width; };
        Placed placed[256];
        int count = 0;
        float pen = 0.0f;
        for (int i = 0; i < (int)l.Text.size() && count < 256; ++i) {
            const char c = l.Text[(size_t)i];
            const std::string path = c == ' ' ? std::string() : GlyphPath(l.Alphabet, c);
            const ImageMetrics m = path.empty() ? ImageMetrics{} : metrics(path);
            if (!m.Valid) {
                pen += size * 0.45f;
                continue;
            }
            const float w = size * m.Aspect, ink = std::max(m.InkRight - m.InkLeft, 0.05f) * w;
            placed[count++] = {pen - m.InkLeft * w + w * 0.5f, i, TextureIndex(textures, path), w};
            pen += ink + l.Kerning * size;
        }
        const float total = std::max(pen - l.Kerning * size, 1.0f);
        const float arc = l.WarpMode == Warp::Arc ? std::max(l.WarpAmount, 0.01f) * kPi : 0.0f;
        const float radius = arc > 0.0f ? total / arc : 0.0f;
        const float sag = arc > 0.0f ? radius * (1.0f - std::cos(arc * 0.5f)) : 0.0f;
        const float turn = l.Rotation * (kPi / 180.0f);
        for (int k = 0; k < count; ++k) {
            const Placed& g = placed[k];
            float x = g.X - total * 0.5f, y = 0.0f, spin = 0.0f;
            if (l.WarpMode == Warp::Arc) {
                const float phi = x / radius;
                x = radius * std::sin(phi);
                y = radius * (1.0f - std::cos(phi)) - sag * 0.5f;
                spin = phi;
            } else if (l.WarpMode == Warp::Wave) {
                const float ph = x / size * 1.7f + Rand01(seed) * 2.0f * kPi;
                y = std::sin(ph) * l.WarpAmount * 0.28f * size;
                spin = std::atan(std::cos(ph) * l.WarpAmount * 0.28f * 1.7f) * 0.6f;
            }
            const uint32_t h = Hash(seed, (uint32_t)g.Index);
            y += RandSigned(Hash(h, 1)) * l.Jitter * 0.12f * size;
            spin += RandSigned(Hash(h, 2)) * l.Jitter * (7.0f * kPi / 180.0f);
            Quad q{};
            q.Centre = l.Position * glm::vec2(W, H) + Rotate(glm::vec2(x, y), turn);
            q.Half = glm::vec2(g.Width, size) * 0.5f;
            q.Angle = spin + turn;
            q.Texture = g.Texture;
            q.Fill = fill;
            q.OutlineColor = outline;
            q.OutlinePx = l.Outline ? l.OutlineWidth * H : 0.0f;
            for (const LetterStyle& s : l.Letters) {
                if (s.Index != g.Index) continue;
                if (s.HasFill) {
                    q.Fill = SrgbToLinear(s.Fill);
                    q.OwnColor = true;
                }
                if (s.HasOutline) q.OutlineColor = SrgbToLinear(s.Outline);
            }
            q.Luma = false; // the alphabets carry real alpha
            q.Delay = (float)k * 0.035f;
            lp.Quads.push_back(q);
        }
    } else if (l.Type == LayerType::Spray) {
        StrokeBuilder builder;
        std::vector<Splat> stroke;
        const float pxPerMetre = W / std::max(piece.Metres.x, 1e-3f);
        for (size_t s = 0; s < l.Strokes.size(); ++s) {
            const Stroke& st = l.Strokes[s];
            if (st.Samples.empty()) continue;
            stroke.clear();
            builder.Begin(st, piece.Width, piece.Height, pxPerMetre, StrokeSeed(piece.Seed, st, (uint32_t)s), piece.Drips);
            for (const SpraySample& p : st.Samples) builder.Add(p, stroke);
            AppendStroke(lp, stroke, st.Erase);
        }
    }
    return lp;
}

Program Build(const Piece& piece, const MetricsFn& metrics) {
    Program p;
    p.Width = piece.Width;
    p.Height = piece.Height;
    for (int i = 0; i < (int)piece.Layers.size(); ++i)
        if (piece.Layers[(size_t)i].Visible) p.Layers.push_back(BuildLayer(piece, i, metrics, p.Textures));
    return p;
}

namespace {
void KeyString(uint64_t& h, const std::string& s) {
    h = Fnv(h, s.data(), s.size());
    h = FnvValue(h, (uint8_t)0);
}
// Everything BuildLayer reads for this layer (see BuildCache).
uint64_t LayerBuildKey(const Piece& piece, const Layer& l, uint32_t epoch) {
    uint64_t h = 1469598103934665603ull;
    h = FnvValue(h, epoch);
    h = FnvValue(h, piece.Width);
    h = FnvValue(h, piece.Height);
    h = FnvValue(h, piece.Seed);
    h = FnvValue(h, piece.Metres);
    h = FnvValue(h, (uint8_t)piece.Drips);
    h = FnvValue(h, (uint8_t)l.Type);
    if (l.Type == LayerType::Stamp || l.Type == LayerType::Text) {
        h = FnvValue(h, l.Color);
        h = FnvValue(h, l.OutlineColor);
        h = FnvValue(h, (uint8_t)l.Outline);
        h = FnvValue(h, l.OutlineWidth);
        h = FnvValue(h, l.Position);
        h = FnvValue(h, l.Size);
        h = FnvValue(h, l.Rotation);
        KeyString(h, l.Texture);
        h = FnvValue(h, (uint8_t)l.Luma);
        h = FnvValue(h, (uint8_t)l.TextureColor);
        h = FnvValue(h, l.Aspect);
        KeyString(h, l.Text);
        KeyString(h, l.Alphabet);
        h = FnvValue(h, l.Kerning);
        h = FnvValue(h, l.Jitter);
        h = FnvValue(h, (uint8_t)l.WarpMode);
        h = FnvValue(h, l.WarpAmount);
        for (const LetterStyle& s : l.Letters) {
            h = FnvValue(h, s.Index);
            h = FnvValue(h, (uint8_t)s.HasFill);
            h = FnvValue(h, (uint8_t)s.HasOutline);
            h = FnvValue(h, s.Fill);
            h = FnvValue(h, s.Outline);
        }
    } else if (l.Type == LayerType::Spray) {
        for (const Stroke& st : l.Strokes) {
            h = FnvValue(h, (uint8_t)st.Tip);
            h = FnvValue(h, st.Color);
            h = FnvValue(h, (uint8_t)st.Erase);
            h = FnvValue(h, st.Flow);
            h = FnvValue(h, st.Angle);
            h = FnvValue(h, (uint64_t)st.Samples.size());
            for (const SpraySample& s : st.Samples) {
                h = FnvValue(h, s.X);
                h = FnvValue(h, s.Y);
                h = FnvValue(h, s.Pressure);
                h = FnvValue(h, s.DistanceMm);
                h = FnvValue(h, s.TimeMs);
            }
        }
    }
    return h;
}
} // namespace

Program Build(const Piece& piece, const MetricsFn& metrics, BuildCache& cache, uint32_t epoch) {
    Program p;
    p.Width = piece.Width;
    p.Height = piece.Height;
    ++cache.Generation;
    cache.Built = cache.Reused = 0;
    for (int i = 0; i < (int)piece.Layers.size(); ++i) {
        const Layer& l = piece.Layers[(size_t)i];
        if (!l.Visible) continue;
        const uint64_t key = LayerBuildKey(piece, l, epoch);
        auto it = cache.Layers.find(key);
        if (it == cache.Layers.end()) {
            BuildCache::Entry e;
            e.Layer = BuildLayer(piece, i, metrics, e.Textures);
            it = cache.Layers.emplace(key, std::move(e)).first;
            ++cache.Built;
        } else {
            ++cache.Reused;
        }
        it->second.Used = cache.Generation;
        LayerProgram lp = it->second.Layer;
        lp.Layer = i;
        for (Quad& q : lp.Quads)
            if (q.Texture >= 0 && (size_t)q.Texture < it->second.Textures.size()) q.Texture = TextureIndex(p.Textures, it->second.Textures[(size_t)q.Texture]);
        p.Layers.push_back(std::move(lp));
    }
    for (auto it = cache.Layers.begin(); it != cache.Layers.end();)
        it = it->second.Used != cache.Generation ? cache.Layers.erase(it) : std::next(it);
    return p;
}

uint64_t Program::Hash() const {
    uint64_t h = 1469598103934665603ull;
    h = FnvValue(h, Width);
    h = FnvValue(h, Height);
    for (const std::string& t : Textures) h = Fnv(h, t.data(), t.size() + 1);
    for (const LayerProgram& l : Layers) {
        h = FnvValue(h, l.Layer);
        if (!l.Splats.empty()) h = Fnv(h, l.Splats.data(), l.Splats.size() * sizeof(Splat)); // no padding
        for (const LayerProgram::Run& r : l.Runs) {
            h = FnvValue(h, r.Begin);
            h = FnvValue(h, r.End);
            h = FnvValue(h, (uint8_t)r.Erase);
        }
        // Field by field: Quad has padding after its bools.
        for (const Quad& q : l.Quads) {
            h = FnvValue(h, q.Centre);
            h = FnvValue(h, q.Half);
            h = FnvValue(h, q.Angle);
            h = FnvValue(h, q.Texture);
            h = FnvValue(h, q.Fill);
            h = FnvValue(h, q.OutlineColor);
            h = FnvValue(h, q.OutlinePx);
            const uint8_t bits = (uint8_t)((q.Luma ? 1 : 0) | (q.TextureColor ? 2 : 0) | (q.OwnColor ? 4 : 0));
            h = FnvValue(h, bits);
        }
        h = FnvValue(h, l.Duration);
    }
    return h;
}

} // namespace Graffiti
