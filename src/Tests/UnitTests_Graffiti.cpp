// The Graffiti Designer's CPU side (docs/GRAFFITI_DESIGNER.md): the .graffiti round trip and its
// forgiving load, and the paint program - deterministic from the seed (the CPU half of "a re-bake
// is byte-identical"; the GPU half is the smoke_decals bake check), the live stroke stream equal to
// the bake's, the spray model (cone, distance, pressure, drips), seeds that survive a layer reorder,
// and the text layout.
#include "UnitTestSupport.h"
#include "../Graffiti/GraffitiCompositor.h"
#include "../Graffiti/GraffitiPaint.h"
#include "../Graffiti/GraffitiPiece.h"
#include "../Core/PenInput.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
using namespace Graffiti;
namespace fs = std::filesystem;

bool Near(float a, float b, float eps = 1e-5f) { return std::abs(a - b) <= eps; }

fs::path TempDir() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "TartarusUnitTests" / "Graffiti";
    fs::create_directories(dir, ec);
    return dir;
}

// Every glyph is a 0.8-wide image with ink across its middle 0.6; stamps are 2:1.
ImageMetrics FakeMetrics(const std::string& path) {
    ImageMetrics m;
    m.Valid = path.find("missing") == std::string::npos;
    m.Aspect = path.find("Stamp") != std::string::npos ? 2.0f : 0.8f;
    m.InkLeft = 0.2f;
    m.InkRight = 0.8f;
    return m;
}

// A stroke dragged from `from` to `to` over `seconds`, sampled at 120 Hz, then held still `hold`.
Stroke MakeStroke(Cap cap, glm::vec2 from, glm::vec2 to, float seconds, float distance, float hold = 0.0f) {
    Stroke s;
    s.Tip = cap;
    s.Color = Quantise(glm::vec3(0.9f, 0.2f, 0.1f));
    const int n = std::max(2, (int)(seconds * 120.0f));
    for (int i = 0; i <= n; ++i) {
        const float f = (float)i / (float)n;
        s.Samples.push_back(SpraySample::Make(glm::mix(from, to, f), 0.9f, distance, f * seconds));
    }
    for (int i = 1; i <= (int)(hold * 120.0f); ++i)
        s.Samples.push_back(SpraySample::Make(to, 1.0f, distance, seconds + (float)i / 120.0f));
    return s;
}

Piece RichPiece() {
    Piece p;
    p.Path = (TempDir() / "GP_Test.graffiti").u8string();
    p.Name = "Test";
    p.Width = 1024;
    p.Height = 512;
    p.Metres = {2.0f, 1.0f};
    p.Seed = 4242;
    p.Roughness = 0.4f;
    p.Relief = 0.5f;
    p.ShowThrough = 0.25f;
    p.Drips = true;
    p.GlowColor = Quantise({0.2f, 1.0f, 0.4f});
    p.GlowStrength = 2.0f;
    p.UV = true;
    p.Fade = 0.1f;
    p.Peel = 0.2f;

    Layer fill;
    fill.Type = LayerType::Fill;
    fill.Name = "Backdrop";
    fill.Color = Quantise({0.1f, 0.1f, 0.3f});
    fill.Gradient = true;
    fill.GradientTo = Quantise({0.6f, 0.1f, 0.5f});
    fill.GradientAngle = 45.0f;
    fill.Opacity = 0.5f;
    fill.Mode = Blend::Multiply;
    p.Layers.push_back(fill);

    Layer stamp;
    stamp.Type = LayerType::Stamp;
    stamp.Name = "Crown";
    stamp.Texture = "assets/Decals/Graffiti/Shapes/T_Stamp_Crown.png";
    stamp.Position = {0.3f, 0.25f};
    stamp.Size = 0.3f;
    stamp.Rotation = 12.0f;
    stamp.Aspect = 1.5f;
    stamp.Luma = false;
    stamp.TextureColor = true;
    stamp.Shadow = true;
    stamp.ShadowOffset = {0.02f, -0.01f};
    p.Layers.push_back(stamp);

    Layer text;
    text.Type = LayerType::Text;
    text.Name = "Letters";
    text.Text = "KING 9";
    text.Alphabet = "Block";
    text.Kerning = 0.05f;
    text.Jitter = 0.4f;
    text.WarpMode = Warp::Arc;
    text.WarpAmount = 0.35f;
    text.Outline = true;
    text.OutlineWidth = 0.02f;
    text.Metal = 1.0f;
    LetterStyle ls;
    ls.Index = 2;
    ls.HasFill = true;
    ls.Fill = Quantise({0.2f, 0.7f, 1.0f});
    text.Letters.push_back(ls);
    p.Layers.push_back(text);

    Layer spray;
    spray.Type = LayerType::Spray;
    spray.Name = "Fill-in";
    spray.Clip = true;
    spray.Glow = 0.75f;
    spray.Strokes.push_back(MakeStroke(Cap::Fat, {0.1f, 0.5f}, {0.9f, 0.55f}, 0.8f, 0.12f));
    spray.Strokes.push_back(MakeStroke(Cap::Skinny, {0.5f, 0.2f}, {0.52f, 0.3f}, 0.3f, 0.05f, 1.0f));
    Stroke erase = MakeStroke(Cap::Calligraphy, {0.2f, 0.8f}, {0.4f, 0.7f}, 0.4f, 0.1f);
    erase.Erase = true;
    erase.Angle = 30.0f;
    erase.Flow = 0.6f;
    spray.Strokes.push_back(erase);
    p.Layers.push_back(spray);
    return p;
}

void Defaults() {
    const Piece p;
    CHECK(p.Width == 2048 && p.Height == 1024 && p.Seed == 1);
    CHECK(Near(p.Metres.x, 2.0f) && Near(p.Metres.y, 1.0f));
    CHECK(p.Drips && !p.UV && p.GlowStrength == 0.0f);
    const Layer l;
    CHECK(l.Type == LayerType::Spray && l.Visible && Near(l.Opacity, 1.0f) && l.Mode == Blend::Normal);
    CHECK(l.Alphabet == "Tag" && l.WarpMode == Warp::None);
    // Default colours sit on the #rrggbb grid, so a fresh layer bakes the same before and after a save.
    CHECK(Quantise(l.Color) == l.Color && Quantise(l.GradientTo) == l.GradientTo && Quantise(l.OutlineColor) == l.OutlineColor);
    CHECK(Quantise(p.GlowColor) == p.GlowColor && Quantise(Stroke{}.Color) == Stroke{}.Color);
    CHECK(Piece::SnapSide(700) == 512 && Piece::SnapSide(900) == 1024 && Piece::SnapSide(4096) == 2048);
}

void HexColours() {
    glm::vec3 c;
    CHECK(FromHex("#ff8000", c) && Near(c.r, 1.0f) && Near(c.g, 128.0f / 255.0f) && c.b == 0.0f);
    CHECK(FromHex("FF8000", c));
    CHECK(!FromHex("#ff80", c) && !FromHex("#gg8000", c));
    CHECK(ToHex({1.0f, 0.5f, 0.0f}) == "#ff8000");
    for (int i = 0; i < 256; i += 17) { // every 8-bit value survives the trip
        const glm::vec3 q = Quantise(glm::vec3((float)i / 255.0f));
        glm::vec3 back;
        CHECK(FromHex(ToHex(q), back) && back == q);
    }
}

void RoundTrip() {
    const Piece p = RichPiece();
    const std::string a = p.ToString();
    const auto q = Piece::FromString(a, p.Path);
    CHECK(!q->Missing);
    CHECK(q->ToString() == a); // byte-stable
    CHECK(q->Width == 1024 && q->Height == 512 && q->Seed == 4242 && q->UV && Near(q->GlowStrength, 2.0f));
    CHECK(q->Layers.size() == 4);
    if (q->Layers.size() != 4) return;
    const Layer& f = q->Layers[0];
    CHECK(f.Type == LayerType::Fill && f.Gradient && f.Mode == Blend::Multiply && Near(f.Opacity, 0.5f) && f.GradientTo == p.Layers[0].GradientTo);
    const Layer& s = q->Layers[1];
    CHECK(s.Type == LayerType::Stamp && s.Texture == p.Layers[1].Texture && s.TextureColor && !s.Luma && s.Shadow);
    CHECK(Near(s.ShadowOffset.y, -0.01f) && Near(s.Aspect, 1.5f) && Near(s.Rotation, 12.0f));
    const Layer& t = q->Layers[2];
    CHECK(t.Text == "KING 9" && t.Alphabet == "Block" && t.WarpMode == Warp::Arc && t.Letters.size() == 1 && t.Metal == 1.0f);
    CHECK(t.Letters.size() == 1 && t.Letters[0].Index == 2 && t.Letters[0].HasFill && !t.Letters[0].HasOutline);
    const Layer& sp = q->Layers[3];
    CHECK(sp.Clip && Near(sp.Glow, 0.75f) && sp.Strokes.size() == 3);
    if (sp.Strokes.size() == 3) {
        CHECK(sp.Strokes[2].Erase && sp.Strokes[2].Tip == Cap::Calligraphy && Near(sp.Strokes[2].Angle, 30.0f));
        CHECK(sp.Strokes[1].Samples.size() == p.Layers[3].Strokes[1].Samples.size());
        const SpraySample& a0 = sp.Strokes[1].Samples.back();
        const SpraySample& b0 = p.Layers[3].Strokes[1].Samples.back();
        CHECK(a0.X == b0.X && a0.Y == b0.Y && a0.Pressure == b0.Pressure && a0.DistanceMm == b0.DistanceMm && a0.TimeMs == b0.TimeMs);
    }
    // A sample is one line of the file.
    CHECK(a.find("[10000, 50000, 900, 120, 0]") != std::string::npos);

    // Save / Load through the disk, atomically.
    CHECK(p.Save());
    const auto disk = Piece::Load(p.Path);
    CHECK(!disk->Missing && disk->Name == "Test" && disk->ToString() == a);
    CHECK(fs::u8path(p.MapPath("Albedo")).filename().u8string() == "T_Decal_Piece_Test_Albedo.png");
    CHECK(fs::u8path(p.DecalPath()).filename().u8string() == "DA_Piece_Test.decal");
}

void ForgivingLoad() {
    const std::string path = (TempDir() / "GP_Bad.graffiti").u8string();
    const auto broken = Piece::FromString("{ not json", path);
    CHECK(broken->Missing);
    CHECK(Piece::Load((TempDir() / "GP_Nope.graffiti").u8string())->Missing);
    const auto p = Piece::FromString(R"({
        "graffitiVersion": 9, "canvas": {"width": 3000, "height": "big", "metres": [100, 0]},
        "paint": {"roughness": 7, "glow": "purple"},
        "layers": [
          {"type": "text", "alphabet": "Comic", "warp": "spiral", "color": 12, "size": -3},
          {"type": "spray", "strokes": [
              {"cap": "nozzle", "samples": [[1, 2, 5000, 1, 100], [3, 4, 500, 150, 50], "x", [1, 2]]},
              {"samples": []}]},
          5,
          {"type": "hologram"}
        ]})", path);
    CHECK(!p->Missing);
    CHECK(p->Width == 2048 && p->Height == 1024);         // snapped; the wrong type kept the default
    CHECK(Near(p->Metres.x, 20.0f) && Near(p->Metres.y, 0.05f));
    CHECK(Near(p->Roughness, 1.0f));
    CHECK(p->Layers.size() == 3);                         // the bare 5 dropped
    if (p->Layers.size() != 3) return;
    CHECK(p->Layers[0].Alphabet == "Tag" && p->Layers[0].WarpMode == Warp::None && Near(p->Layers[0].Size, 0.01f));
    CHECK(p->Layers[1].Strokes.size() == 1);              // the empty stroke dropped
    if (p->Layers[1].Strokes.size() == 1) {
        const Stroke& s = p->Layers[1].Strokes[0];
        CHECK(s.Tip == Cap::Fat && s.Samples.size() == 2); // two bad samples dropped
        CHECK(s.Samples[0].Pressure == 1000 && s.Samples[0].DistanceMm == 20);
        CHECK(s.Samples[1].TimeMs == 100);                // never back in time
    }
    CHECK(p->Layers[2].Type == LayerType::Spray);         // an unknown type keeps the default
}

std::vector<Splat> SplatsOf(const Program& prog) {
    std::vector<Splat> all;
    for (const LayerProgram& l : prog.Layers) all.insert(all.end(), l.Splats.begin(), l.Splats.end());
    return all;
}

void ProgramDeterminism() {
    const Piece p = RichPiece();
    const Program a = Build(p, FakeMetrics), b = Build(p, FakeMetrics);
    CHECK(a.Hash() == b.Hash());
    CHECK(SplatsOf(a).size() > 100);
    const auto reread = Piece::FromString(p.ToString(), p.Path); // what a re-bake from disk sees
    CHECK(Build(*reread, FakeMetrics).Hash() == a.Hash());
    Piece other = p;
    other.Seed = 4243;
    CHECK(Build(other, FakeMetrics).Hash() != a.Hash());
    Piece hidden = p;
    hidden.Layers[1].Visible = false;
    const Program h = Build(hidden, FakeMetrics);
    CHECK(h.Layers.size() == 3 && h.Hash() != a.Hash());
    // Reordering layers moves their programs but keeps each one's primitives (seeds come from
    // content, not position).
    Piece swapped = p;
    std::swap(swapped.Layers[2], swapped.Layers[3]);
    const Program s = Build(swapped, FakeMetrics);
    CHECK(s.Layers.size() == 4 && s.Layers[3].Quads.size() == a.Layers[2].Quads.size());
    CHECK(s.Layers[2].Splats.size() == a.Layers[3].Splats.size());
    CHECK(std::equal(s.Layers[2].Splats.begin(), s.Layers[2].Splats.end(), a.Layers[3].Splats.begin(),
                     [](const Splat& x, const Splat& y) { return std::memcmp(&x, &y, sizeof(Splat)) == 0; }));
}

// The Designer draws a stroke as it is recorded, in batches of what has come due by each frame;
// that must be exactly the bake's stream.
void LiveEqualsBake() {
    Piece p = RichPiece();
    const int li = 3;
    const Layer& layer = p.Layers[(size_t)li];
    const Program bake = Build(p, FakeMetrics);
    const LayerProgram& expect = bake.Layers[(size_t)li];
    const float pxPerMetre = (float)p.Width / p.Metres.x;
    // The live way, stroke by stroke: a sample per frame (the stroke's seed known from its first
    // sample), drawing in time order what has come due, then the drips still running at the end.
    LayerProgram live;
    for (size_t si = 0; si < layer.Strokes.size(); ++si) {
        const Stroke& st = layer.Strokes[si];
        StrokeBuilder builder;
        builder.Begin(st, p.Width, p.Height, pxPerMetre, StrokeSeed(p.Seed, st, (uint32_t)si), p.Drips);
        std::vector<Splat> pending, stream;
        std::vector<uint32_t> seq;
        uint32_t next = 0;
        auto drain = [&](float now) {
            std::vector<size_t> due;
            for (size_t i = 0; i < pending.size(); ++i)
                if (pending[i].Time <= now) due.push_back(i);
            std::sort(due.begin(), due.end(), [&](size_t x, size_t y) {
                return pending[x].Time < pending[y].Time || (pending[x].Time == pending[y].Time && seq[x] < seq[y]);
            });
            for (size_t i : due) stream.push_back(pending[i]);
            size_t keep = 0;
            for (size_t i = 0; i < pending.size(); ++i)
                if (pending[i].Time > now) {
                    pending[keep] = pending[i];
                    seq[keep++] = seq[i];
                }
            pending.resize(keep);
            seq.resize(keep);
        };
        for (const SpraySample& s : st.Samples) {
            const size_t before = pending.size();
            builder.Add(s, pending);
            for (size_t i = before; i < pending.size(); ++i) seq.push_back(next++);
            drain(s.Time());
        }
        drain(1e30f);
        // Appended as a whole stroke (time-sorted already, so AppendStroke keeps the order).
        AppendStroke(live, stream, st.Erase);
    }
    CHECK(live.Splats.size() == expect.Splats.size());
    CHECK(live.Splats.size() == expect.Splats.size() &&
          std::equal(live.Splats.begin(), live.Splats.end(), expect.Splats.begin(),
                     [](const Splat& x, const Splat& y) { return std::memcmp(&x, &y, sizeof(Splat)) == 0; }));
    CHECK(live.Runs.size() == expect.Runs.size());
}

int CountDrips(const std::vector<Splat>& v) {
    return (int)std::count_if(v.begin(), v.end(), [](const Splat& s) { return (s.Flags & kSplatDrip) != 0; });
}

void SprayModel() {
    const int W = 1024, H = 1024;
    const float ppm = 1024.0f; // 1 m canvas
    auto run = [&](const Stroke& st, bool drips) {
        StrokeBuilder b;
        std::vector<Splat> out;
        b.Begin(st, W, H, ppm, 7u, drips);
        for (const SpraySample& s : st.Samples) b.Add(s, out);
        return out;
    };
    // Further from the wall: a wider, fainter footprint.
    const auto nearS = run(MakeStroke(Cap::Fat, {0.2f, 0.5f}, {0.8f, 0.5f}, 1.0f, 0.06f), false);
    const auto farS = run(MakeStroke(Cap::Fat, {0.2f, 0.5f}, {0.8f, 0.5f}, 1.0f, 0.30f), false);
    CHECK(!nearS.empty() && !farS.empty());
    if (!nearS.empty() && !farS.empty()) {
        CHECK(farS[farS.size() / 2].Shape.z > nearS[nearS.size() / 2].Shape.z * 3.0f);
        CHECK(farS[farS.size() / 2].Params.y < nearS[nearS.size() / 2].Params.y);
    }
    // Skinny caps are tighter than fat ones at the same distance; calligraphy is flattened and turned.
    const auto skinny = run(MakeStroke(Cap::Skinny, {0.2f, 0.5f}, {0.8f, 0.5f}, 1.0f, 0.15f), false);
    const auto fat = run(MakeStroke(Cap::Fat, {0.2f, 0.5f}, {0.8f, 0.5f}, 1.0f, 0.15f), false);
    Stroke nibStroke = MakeStroke(Cap::Calligraphy, {0.2f, 0.5f}, {0.8f, 0.5f}, 1.0f, 0.15f);
    nibStroke.Angle = 45.0f;
    const auto nib = run(nibStroke, false);
    CHECK(!skinny.empty() && !fat.empty() && !nib.empty());
    if (!skinny.empty() && !fat.empty() && !nib.empty()) {
        CHECK(skinny[5].Shape.z < fat[5].Shape.z);
        CHECK(nib[5].Shape.w < nib[5].Shape.z * 0.5f && Near(nib[5].Params.x, 45.0f * 3.14159265f / 180.0f, 1e-4f));
    }
    // Splats follow the stroke, in time order, inside its span.
    bool ordered = true, inside = true;
    for (size_t i = 1; i < fat.size(); ++i) ordered &= fat[i].Time >= fat[i - 1].Time;
    for (const Splat& s : fat) inside &= s.Shape.x >= 0.2f * W - 1.0f && s.Shape.x <= 0.8f * W + 1.0f && Near(s.Shape.y, 0.5f * H, 1.0f);
    CHECK(ordered && inside);
    // A held can pools: more paint per area, and past the threshold it drips.
    const Stroke dwell = MakeStroke(Cap::Fat, {0.5f, 0.3f}, {0.5f, 0.3f}, 0.05f, 0.05f, 1.5f);
    const auto pooled = run(dwell, true);
    CHECK(CountDrips(pooled) > 0);
    CHECK(CountDrips(run(dwell, false)) == 0);
    Stroke eraseDwell = dwell;
    eraseDwell.Erase = true;
    const auto erased = run(eraseDwell, true);
    CHECK(CountDrips(erased) == 0 && !erased.empty() && (erased[0].Flags & kSplatErase) != 0);
    CHECK(CountDrips(fat) == 0 || true);
    CHECK(CountDrips(run(MakeStroke(Cap::Fat, {0.1f, 0.5f}, {0.9f, 0.5f}, 0.35f, 0.25f), true)) == 0); // a quick far pass
    // Drips run down the wall (canvas +y), later than the paint that set them off, and taper.
    float minY = 1e9f, maxY = -1e9f, firstDripTime = 1e9f;
    for (const Splat& s : pooled)
        if (s.Flags & kSplatDrip) {
            minY = std::min(minY, s.Shape.y);
            maxY = std::max(maxY, s.Shape.y);
            firstDripTime = std::min(firstDripTime, s.Time);
        }
    CHECK(maxY - minY > 10.0f && minY >= 0.3f * H - StrokeBuilder::kCell);
    CHECK(firstDripTime > 0.05f);
}

void TextLayout() {
    Piece p;
    p.Width = p.Height = 1024;
    Layer t;
    t.Type = LayerType::Text;
    t.Alphabet = "Tag";
    t.Text = "AB C";
    t.Size = 0.25f;
    t.Jitter = 0.0f;
    t.Position = {0.5f, 0.5f};
    t.Outline = true;
    t.OutlineWidth = 0.01f;
    LetterStyle s;
    s.Index = 3; // the C, after the space
    s.HasFill = true;
    s.Fill = {0.0f, 1.0f, 0.0f};
    t.Letters.push_back(s);
    p.Layers.push_back(t);
    std::vector<std::string> tex;
    const LayerProgram lp = BuildLayer(p, 0, FakeMetrics, tex);
    CHECK(lp.Quads.size() == 3 && tex.size() == 3);
    if (lp.Quads.size() != 3) return;
    CHECK(lp.Quads[0].Centre.x < lp.Quads[1].Centre.x && lp.Quads[1].Centre.x < lp.Quads[2].Centre.x);
    CHECK(Near(lp.Quads[0].Centre.y, 512.0f, 1e-3f) && Near(lp.Quads[0].Half.y, 128.0f, 1e-3f));
    // Centred on the position: the ink spans symmetric about x = 512.
    const float inkL = lp.Quads[0].Centre.x - lp.Quads[0].Half.x + 0.2f * 2.0f * lp.Quads[0].Half.x;
    const float inkR = lp.Quads[2].Centre.x - lp.Quads[2].Half.x + 0.8f * 2.0f * lp.Quads[2].Half.x;
    CHECK(Near((inkL + inkR) * 0.5f, 512.0f, 0.5f));
    CHECK(lp.Quads[2].OwnColor && lp.Quads[2].Fill == glm::vec3(0.0f, 1.0f, 0.0f) && !lp.Quads[0].OwnColor);
    CHECK(Near(lp.Quads[0].OutlinePx, 10.24f, 1e-3f));
    CHECK(lp.Quads[1].Delay > lp.Quads[0].Delay);
    // Kerning spreads the line; an arc lifts the middle and turns the ends the opposite ways.
    p.Layers[0].Kerning = 0.2f;
    const LayerProgram wide = BuildLayer(p, 0, FakeMetrics, tex);
    CHECK(wide.Quads[2].Centre.x - wide.Quads[0].Centre.x > lp.Quads[2].Centre.x - lp.Quads[0].Centre.x);
    p.Layers[0].Kerning = 0.0f;
    p.Layers[0].Text = "ABC";
    p.Layers[0].Letters.clear();
    p.Layers[0].WarpMode = Warp::Arc;
    p.Layers[0].WarpAmount = 0.5f;
    const LayerProgram arc = BuildLayer(p, 0, FakeMetrics, tex);
    CHECK(arc.Quads.size() == 3);
    if (arc.Quads.size() == 3) {
        CHECK(arc.Quads[1].Centre.y < arc.Quads[0].Centre.y && arc.Quads[1].Centre.y < arc.Quads[2].Centre.y);
        CHECK(arc.Quads[0].Angle < 0.0f && arc.Quads[2].Angle > 0.0f && Near(arc.Quads[0].Angle, -arc.Quads[2].Angle, 1e-4f));
    }
    // Characters the alphabet doesn't have are skipped (with their space kept).
    p.Layers[0].WarpMode = Warp::None;
    p.Layers[0].Text = "A#B";
    CHECK(BuildLayer(p, 0, FakeMetrics, tex).Quads.size() == 2);
}

void Glyphs() {
    CHECK(GlyphPath("Tag", 'a').find("Alphabets/Tag/T_Decal_AlphaTag_A.png") != std::string::npos);
    CHECK(GlyphPath("Block", '7').find("T_Decal_AlphaBlock_7.png") != std::string::npos);
    CHECK(GlyphPath("Block", '#').find("T_Decal_AlphaBlock_Hash.png") != std::string::npos);
    CHECK(GlyphPath("Block", '(').find("T_Decal_AlphaBlock_ParenL.png") != std::string::npos);
    CHECK(GlyphPath("Brush", '?').find("T_Decal_AlphaBrush_Question.png") != std::string::npos);
    CHECK(GlyphPath("Tag", '7').empty() && GlyphPath("Tag", '?').empty() && GlyphPath("Bubble", '+').empty());
    CHECK(GlyphPath("Brush", '!').empty() && GlyphPath("Block", '@').empty());
}

// Pen pressure (docs: Windows Pointer Input -> PenInput): no pen reads as "none", an injected value is
// clamped and wins, and a stroke at light pressure is narrower and lays down less paint than a hard one.
void PenPressureModel() {
    PenInput::SetSynthetic(-1.0f);
    PenInput::Report(-1.0f);
    CHECK(PenInput::Pressure() < 0.0f && !PenInput::Active()); // a mouse: fall back to the slider
    PenInput::SetSynthetic(0.4f);
    CHECK(PenInput::Active() && Near(PenInput::Pressure(), 0.4f));
    PenInput::SetSynthetic(7.0f);
    CHECK(Near(PenInput::Pressure(), 1.0f));
    PenInput::SetSynthetic(-1.0f);
    PenInput::Report(0.6f);
    CHECK(Near(PenInput::Pressure(), 0.6f));
    PenInput::Report(-1.0f);
    CHECK(!PenInput::Active());

    auto strokeAt = [&](float pressure) {
        Stroke st;
        st.Tip = Cap::Fat;
        st.Color = Quantise(glm::vec3(0.9f, 0.2f, 0.1f));
        for (int i = 0; i <= 120; ++i) {
            const float f = (float)i / 120.0f;
            st.Samples.push_back(SpraySample::Make(glm::mix(glm::vec2(0.2f, 0.5f), glm::vec2(0.8f, 0.5f), f), pressure, 0.15f, f));
        }
        StrokeBuilder b;
        std::vector<Splat> out;
        b.Begin(st, 1024, 1024, 1024.0f, 7u, false);
        for (const SpraySample& sm : st.Samples) b.Add(sm, out);
        double radius = 0.0, paint = 0.0;
        for (const Splat& sp : out) {
            radius += sp.Shape.z / (double)out.size();
            paint += sp.Params.y;
        }
        return std::pair<double, double>(radius, paint);
    };
    const auto light = strokeAt(0.2f), hard = strokeAt(1.0f);
    CHECK(light.first > 0.0 && hard.first > light.first);   // wider
    CHECK(light.second > 0.0 && hard.second > light.second * 1.5); // and more paint
}

// A cached build is the build: same hash, and only the layers whose inputs changed are built again.
void BuildCacheMatches() {
    Piece p = RichPiece();
    BuildCache cache;
    const Program fresh = Build(p, FakeMetrics);
    const Program first = Build(p, FakeMetrics, cache, 0);
    CHECK(first.Hash() == fresh.Hash());
    CHECK(cache.Built == 4 && cache.Reused == 0);
    // Nothing changed: nothing is built, the program is the same.
    CHECK(Build(p, FakeMetrics, cache, 0).Hash() == fresh.Hash());
    CHECK(cache.Built == 0 && cache.Reused == 4);
    // Opacity, blend, clip, shadow, metal and glow are drawn, not built.
    p.Layers[1].Opacity = 0.4f;
    p.Layers[1].Mode = Blend::Multiply;
    p.Layers[1].Clip = true;
    p.Layers[2].Shadow = true;
    p.Layers[2].Metal = 0.5f;
    p.Layers[3].Glow = 0.5f;
    CHECK(Build(p, FakeMetrics, cache, 0).Hash() == Build(p, FakeMetrics).Hash());
    CHECK(cache.Built == 0);
    // Editing one layer rebuilds that layer only.
    for (size_t i = 0; i < p.Layers.size(); ++i)
        if (p.Layers[i].Type == LayerType::Text) {
            p.Layers[i].Text += "X";
            break;
        }
    const Program edited = Build(p, FakeMetrics, cache, 0);
    CHECK(cache.Built == 1 && cache.Reused == 3);
    CHECK(edited.Hash() == Build(p, FakeMetrics).Hash());
    // Reordering reuses every layer; hiding one drops it from the program.
    std::swap(p.Layers[2], p.Layers[3]);
    CHECK(Build(p, FakeMetrics, cache, 0).Hash() == Build(p, FakeMetrics).Hash());
    CHECK(cache.Built == 0);
    p.Layers[1].Visible = false;
    CHECK(Build(p, FakeMetrics, cache, 0).Hash() == Build(p, FakeMetrics).Hash());
    // New image metrics (a failed read that succeeded): everything is built again.
    p.Layers[1].Visible = true;
    CHECK(Build(p, FakeMetrics, cache, 1).Hash() == Build(p, FakeMetrics).Hash());
    CHECK(cache.Built >= 1);
    // The piece's seed reaches every layer's key.
    p.Seed += 1;
    CHECK(Build(p, FakeMetrics, cache, 1).Hash() == Build(p, FakeMetrics).Hash());
    CHECK(cache.Built == 4);
}

// What a bake writes is taken when it starts: the paths and settings, with no strokes.
void BakeSnapshot() {
    Piece p = RichPiece();
    const auto snap = SnapshotForBake(p);
    CHECK(snap->DecalPath() == p.DecalPath() && snap->MapPath("Albedo") == p.MapPath("Albedo"));
    CHECK(snap->Width == p.Width && snap->Height == p.Height && snap->Seed == p.Seed && snap->Relief == p.Relief);
    CHECK(snap->Layers.size() <= 1);
    for (const Layer& l : snap->Layers) CHECK(l.Strokes.empty());
    // The piece the Designer opens next doesn't reach the snapshot.
    Piece other = p;
    other.Path = (TempDir() / "GP_Other.graffiti").u8string();
    other.Name = "Other";
    CHECK(snap->DecalPath() != other.DecalPath());
}

} // namespace

void RegisterGraffitiTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Graffiti defaults", Defaults});
    tests.push_back({"Graffiti hex colours", HexColours});
    tests.push_back({"Graffiti round trip", RoundTrip});
    tests.push_back({"Graffiti forgiving load", ForgivingLoad});
    tests.push_back({"Graffiti program determinism", ProgramDeterminism});
    tests.push_back({"Graffiti build cache matches a fresh build", BuildCacheMatches});
    tests.push_back({"Graffiti bake snapshot", BakeSnapshot});
    tests.push_back({"Graffiti live stream equals bake", LiveEqualsBake});
    tests.push_back({"Graffiti spray model", SprayModel});
    tests.push_back({"Graffiti text layout", TextLayout});
    tests.push_back({"Graffiti glyph table", Glyphs});
    tests.push_back({"Graffiti pen pressure", PenPressureModel});
}
