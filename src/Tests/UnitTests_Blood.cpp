#include "UnitTestSupport.h"

#include "BloodFxImport.h"
#include "Combat/BloodFx.h"
#include "Combat/BloodFxPresets.h"
#include "Combat/FxSprites.h"
#include "GameModuleAPI.h"
#include "KnifeFxImport.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Unit tests for the volumetric blood (docs/BLOOD_FX.md): the importer's EXR / VAT maths, the presets
// and the game-side spray logic. No GL: the renderer's sim lookup and the physics ray are stubbed.

namespace {
using namespace BloodFxImport;

void Put32(std::vector<std::uint8_t>& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((std::uint8_t)(v >> (8 * i))); }
void Put64(std::vector<std::uint8_t>& b, std::uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((std::uint8_t)(v >> (8 * i))); }
void PutStr(std::vector<std::uint8_t>& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); b.push_back(0); }

// A zlib stream of one stored (uncompressed) deflate block - valid input for any inflater.
std::vector<std::uint8_t> ZlibStored(const std::vector<std::uint8_t>& d) {
    std::vector<std::uint8_t> z = {0x78, 0x01, 0x01};
    const std::uint16_t len = (std::uint16_t)d.size(), nlen = (std::uint16_t)~len;
    z.push_back((std::uint8_t)(len & 0xFF)); z.push_back((std::uint8_t)(len >> 8));
    z.push_back((std::uint8_t)(nlen & 0xFF)); z.push_back((std::uint8_t)(nlen >> 8));
    z.insert(z.end(), d.begin(), d.end());
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t c : d) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    const std::uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back((std::uint8_t)(adler >> (8 * i)));
    return z;
}

// A scanline EXR with HALF B, G, R channels, ZIPS (one line per block, zlib + predictor + interleave).
std::vector<std::uint8_t> MakeExr(int w, int h, const std::vector<float>& bgr /* per line: B[w] G[w] R[w] */) {
    std::vector<std::uint8_t> b;
    Put32(b, 20000630u);
    Put32(b, 2u);
    PutStr(b, "channels"); PutStr(b, "chlist");
    std::vector<std::uint8_t> ch;
    for (const char* n : {"B", "G", "R"}) { PutStr(ch, n); Put32(ch, 1); Put32(ch, 0); Put32(ch, 1); Put32(ch, 1); }
    ch.push_back(0);
    Put32(b, (std::uint32_t)ch.size()); b.insert(b.end(), ch.begin(), ch.end());
    PutStr(b, "compression"); PutStr(b, "compression"); Put32(b, 1); b.push_back(2);
    PutStr(b, "dataWindow"); PutStr(b, "box2i"); Put32(b, 16); Put32(b, 0); Put32(b, 0); Put32(b, (std::uint32_t)(w - 1)); Put32(b, (std::uint32_t)(h - 1));
    b.push_back(0);
    const size_t table = b.size();
    for (int y = 0; y < h; ++y) Put64(b, 0);
    for (int y = 0; y < h; ++y) {
        std::vector<std::uint8_t> raw;
        for (int i = 0; i < 3 * w; ++i) {
            const std::uint16_t hv = FloatToHalf(bgr[(size_t)y * 3 * w + i]);
            raw.push_back((std::uint8_t)(hv & 0xFF));
            raw.push_back((std::uint8_t)(hv >> 8));
        }
        // OpenEXR's ZIP pre-pass: split even / odd bytes, then delta-encode.
        std::vector<std::uint8_t> t(raw.size());
        const size_t half = (raw.size() + 1) / 2;
        for (size_t i = 0; i < raw.size(); ++i) (i % 2 == 0 ? t[i / 2] : t[half + i / 2]) = raw[i];
        for (size_t i = t.size() - 1; i > 0; --i) t[i] = (std::uint8_t)(t[i] - t[i - 1] + 128);
        // A block that doesn't shrink is stored raw (the spec's rule, which the reader follows) - so line 1
        // goes in uncompressed and the others through zlib with the reader made to inflate them.
        const std::vector<std::uint8_t> z = y == 1 ? raw : ZlibStored(t);
        const std::uint64_t at = b.size();
        std::memcpy(&b[table + (size_t)y * 8], &at, 8);
        Put32(b, (std::uint32_t)y);
        Put32(b, (std::uint32_t)z.size());
        b.insert(b.end(), z.begin(), z.end());
    }
    return b;
}

void Test_Blood_HalfAndOct() {
    for (float f : {0.0f, 1.0f, -2.5f, 0.1696f, 65504.0f, 6.1e-5f, 3.0e-6f})
        CHECK(std::abs(HalfToFloat(FloatToHalf(f)) - f) <= std::abs(f) * 1e-3f + 1e-7f);
    const glm::vec3 dirs[] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0.3f, -0.8f, 0.52f}, {-0.6f, 0.1f, -0.79f}};
    for (glm::vec3 d : dirs) {
        d = glm::normalize(d);
        float x, y, z;
        UnpackOctNormal(PackOctNormal(d.x, d.y, d.z), x, y, z);
        CHECK(glm::dot(glm::vec3(x, y, z), d) > 0.9995f); // within ~1.8 degrees at 8 bits per axis
    }
    CHECK(std::abs(LinearToGamma(0.0f)) < 1e-6f);
    CHECK(std::abs(LinearToGamma(1.0f) - 1.0f) < 1e-4f);
    CHECK(std::abs(LinearToGamma(0.1696f) - 0.4497f) < 2e-3f); // a dead vertex's stored value, decoded
}

void Test_Blood_ExrZipsRoundTrip() {
    const int w = 5, h = 3;
    std::vector<float> px;
    for (int y = 0; y < h; ++y)
        for (int c = 0; c < 3; ++c)
            for (int x = 0; x < w; ++x) px.push_back(0.125f * (float)x - 0.5f * (float)c + (float)y);
    const std::vector<std::uint8_t> file = MakeExr(w, h, px);
    ExrImage img;
    std::string err;
    CHECK(ReadExr(file, img, &err));
    CHECK(img.Width == w && img.Height == h);
    CHECK(img.ChannelIndex("R") == 2 && img.ChannelIndex("B") == 0);
    bool same = img.Pixels.size() == px.size();
    for (size_t i = 0; same && i < px.size(); ++i) same = std::abs(img.Pixels[i] - px[i]) < 1e-3f;
    CHECK(same);
    std::vector<std::uint8_t> bad = file;
    bad[0] = 0;
    CHECK(!ReadExr(bad, img, &err));
}

void Test_Blood_VatBuild() {
    // A 6-wide texture holds 2 triangles per row; 3 triangles over 2 rows per frame, 2 frames.
    const int W = 6, frames = 2, rpf = 2, H = frames * rpf;
    ExrImage pos, nrm;
    for (ExrImage* im : {&pos, &nrm}) {
        im->Width = W;
        im->Height = H;
        im->Channels = {"B", "G", "R"};
        im->Pixels.assign((size_t)W * H * 3, 0.0f);
    }
    std::vector<float> uvs;
    const size_t pv = 9;
    // Each vertex's raw texel (gamma-encoded like the asset), and the UV the FBX gives it.
    const float bmin = -10.0f, bmax = 10.0f, ho[3] = {1.0f, 2.0f, 3.0f};
    auto store = [&](ExrImage& im, int x, int y, float r, float g, float b) {
        const auto lin = [](float v) { return std::pow((v + 0.055f) / 1.055f, 2.4f); }; // gamma -> linear (inverse)
        im.Pixels[((size_t)y * 3 + 2) * W + x] = lin(r);
        im.Pixels[((size_t)y * 3 + 1) * W + x] = lin(g);
        im.Pixels[((size_t)y * 3 + 0) * W + x] = lin(b);
    };
    for (std::uint32_t v = 0; v < pv; ++v) {
        std::uint32_t x, row;
        VatTexel(v, 2, x, row);
        uvs.push_back(((float)x + 0.5f) / W);
        uvs.push_back(1.0f - ((float)row + 0.5f) / H);
        for (int f = 0; f < frames; ++f) {
            // Triangle 2 is dead in frame 1 (all corners equal).
            const bool dead = f == 1 && v / 3 == 2;
            const float px = dead ? 0.5f : 0.3f + 0.05f * (float)(v % 3) + 0.1f * (float)f;
            const float py = dead ? 0.5f : 0.4f + 0.05f * (float)((v + 1) % 3);
            const float pz = dead ? 0.5f : 0.45f + 0.01f * (float)v;
            store(pos, (int)x, f * rpf + (int)row, px, py, pz);
            store(nrm, (int)x, f * rpf + (int)row, 0.5f, 0.5f, 1.0f); // +Unity Z in the texture's G after the swizzle
        }
    }
    VatData vat;
    std::string err;
    CHECK(BuildVat(pos, nrm, uvs, pv, frames, bmin, bmax, ho, vat, &err));
    CHECK(vat.Header.TrisPerRow == 2 && vat.Header.RowsPerFrame == 2 && vat.Header.VertexCount == 9);
    CHECK(vat.Frames[0].LiveTris == 3 && vat.Frames[1].LiveTris == 2);
    // Vertex 4 in frame 0, decoded as the shader does, is Unity's (-px, pz, py) + offset with z mirrored.
    std::uint32_t x, row;
    VatTexel(4, 2, x, row);
    const std::uint16_t* t = &vat.Texels[((size_t)row * W + x) * 4];
    const VatFrame& fr = vat.Frames[0];
    glm::vec3 p;
    for (int i = 0; i < 3; ++i) p[i] = fr.Min[i] + (float)t[i] / 65535.0f * (fr.Max[i] - fr.Min[i]);
    const float ux = (0.3f + 0.05f * 1.0f) * 20.0f - 10.0f, uy = (0.4f + 0.05f * 2.0f) * 20.0f - 10.0f, uz = (0.45f + 0.04f) * 20.0f - 10.0f;
    CHECK(glm::length(p - glm::vec3(-ux + ho[0], uz + ho[1], -(uy + ho[2]))) < 2e-3f);
    // A UV that doesn't match the gl_VertexID layout is refused.
    std::vector<float> wrong = uvs;
    wrong[0] = 0.99f;
    CHECK(!BuildVat(pos, nrm, wrong, pv, frames, bmin, bmax, ho, vat, &err));
    CHECK(!BuildVat(pos, nrm, uvs, pv, 3, bmin, bmax, ho, vat, &err)); // 4 rows aren't 3 frames
}

void Test_Blood_FbxUVs() {
    const std::string fbx =
        "; FBX 7.3.0 project file\nGeometry: 1, \"Geometry::X\", \"Mesh\" {\n Vertices: *9 {\n a: 0,0,0,1,0,0,0,1,0\n }\n"
        " PolygonVertexIndex: *3 {\n a: 0,1,-3\n }\n LayerElementUV: 0 {\n  MappingInformationType: \"ByPolygonVertex\"\n"
        "  ReferenceInformationType: \"IndexToDirect\"\n  UV: *4 {\n   a: 0.25,0.75,0.5,0.125\n  }\n  UVIndex: *3 {\n   a: 1,0,1\n  }\n }\n}\n";
    std::vector<float> uvs;
    size_t pv = 0;
    std::string err;
    CHECK(ReadFbxAsciiUVs(fbx, uvs, pv, &err));
    CHECK(pv == 3 && uvs.size() == 6);
    CHECK(uvs[0] == 0.5f && uvs[1] == 0.125f && uvs[2] == 0.25f && uvs[3] == 0.75f);
    CHECK(!ReadFbxAsciiUVs("Kaydara FBX Binary", uvs, pv, &err));
}

void Test_Blood_Presets() {
    const auto& presets = BloodPresets();
    CHECK(presets.size() == 17);
    static const char* kSims[] = {"blood1", "blood2_left", "blood2_right", "blood2_vertical", "blood3", "blood4",
                                  "blood5", "blood6", "blood7", "blood8", "blood9"};
    int sprays = 0;
    bool simsKnown = true, curvesSane = true;
    for (const BloodPresetDef& p : presets) {
        for (const BloodSprayDef& s : p.Sprays) {
            ++sprays;
            bool known = false;
            for (const char* k : kSims) known |= std::strcmp(k, s.Sim) == 0;
            simsKnown &= known && s.TimeLimit > 0.5f && s.FramesCount > 10.0f;
        }
        for (const BloodDecalDef& d : p.Decals)
            curvesSane &= d.Reveal.Eval(0.0f) > 0.99f && d.Reveal.Eval(0.27f) < 0.05f && d.RevealSeconds > 1.0f;
    }
    CHECK(sprays >= 20);
    CHECK(simsKnown);
    CHECK(curvesSane);
    // Every preset BloodFx::Choose can return exists.
    BloodFx::Hit h;
    for (int kind = 0; kind < 7; ++kind)
        for (float roll : {0.0f, 0.3f, 0.6f, 0.99f}) {
            h = BloodFx::Hit{};
            h.Player = kind == 0; h.Corpse = kind == 1; h.Killed = kind == 2 || kind == 3; h.Head = kind == 3 || kind == 4;
            if (kind == 5) h.Direction = glm::vec3(0.0f, -1.0f, 0.0f);
            const BloodFx::Choice c = BloodFx::Choose(h, roll);
            CHECK(FindBloodPreset(c.Preset) != nullptr && c.Size > 0.2f && c.Size < 1.5f);
        }
    // A linear curve and a stepped one.
    BloodCurve lin{2, {{0, 0, 1, 1}, {1, 1, 1, 1}}};
    CHECK(std::abs(lin.Eval(0.25f) - 0.25f) < 1e-5f && lin.Eval(-1.0f) == 0.0f && lin.Eval(2.0f) == 1.0f);
    BloodCurve step{2, {{0, 3, 0, 1e30f}, {1, 5, 0, 0}}};
    CHECK(step.Eval(0.5f) == 3.0f);
}

void Test_Blood_SprayTiming() {
    CHECK(BloodFx::FrameAt(0.0f, 80.0f) == 0);
    CHECK(BloodFx::FrameAt(0.5f, 80.0f) == 40);
    CHECK(BloodFx::FrameAt(1.0f, 80.0f) == 80);
    CHECK(BloodFx::FrameAt(7.0f, 80.0f) == 80);
    const BloodSprayDef def{"blood1", {}, 2.3f, 80.0f};
    CHECK(std::abs(BloodFx::PlaybackSeconds(def, 2.0f, 1.0f) - 1.15f) < 1e-4f);
    CHECK(std::abs(BloodFx::PlaybackSeconds(def, 2.0f, 0.25f) - 0.575f) < 1e-4f); // a quarter the size falls in half the time
    // The prefab's +X goes along the round's flattened line.
    for (glm::vec3 dir : {glm::vec3(0, 0, -1), glm::vec3(1, -0.3f, 1), glm::vec3(-1, 0.2f, 0)}) {
        const glm::mat4 m = BloodFx::PrefabToWorld(glm::vec3(1, 2, 3), dir, 0.5f, 0.0f);
        const glm::vec3 x = glm::normalize(glm::vec3(m * glm::vec4(1, 0, 0, 0)));
        CHECK(glm::dot(x, glm::normalize(glm::vec3(dir.x, 0, dir.z))) > 0.9999f);
        CHECK(glm::length(glm::vec3(m * glm::vec4(0, 1, 0, 0)) - glm::vec3(0, 0.5f, 0)) < 1e-5f); // gravity stays down
        CHECK(glm::length(glm::vec3(m[3]) - glm::vec3(1, 2, 3)) < 1e-6f);
        // A prefab that throws along its -X (blood7/8) turns that onto the line instead.
        const glm::mat4 r = BloodFx::PrefabToWorld(glm::vec3(0.0f), dir, 1.0f, 0.0f, glm::vec3(-1, 0, 0));
        CHECK(glm::dot(glm::normalize(glm::vec3(r * glm::vec4(-1, 0, 0, 0))), glm::normalize(glm::vec3(dir.x, 0, dir.z))) > 0.9999f);
    }
    // The axis comes from where the sims' fluid ends up (+X when unknown).
    BloodPresetDef p{"t", 0.75f, 2.0f, {{"a", {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0}, 2.0f, 10.0f}}, {}};
    BloodFxImport::VatFrame last{};
    last.Centroid[0] = -5.0f; // the sim's -X, mirrored by the spray's matrix onto the prefab's +X
    auto lookup = [&](const char*, glm::vec3& o) -> const BloodFxImport::VatFrame* { o = glm::vec3(0.0f); return &last; };
    CHECK(glm::dot(BloodFx::PrefabAxis(p, lookup), glm::vec3(1, 0, 0)) > 0.999f);
    p.Sprays[0].M[0] = 1.0f;
    CHECK(glm::dot(BloodFx::PrefabAxis(p, lookup), glm::vec3(-1, 0, 0)) > 0.999f);
    CHECK(glm::dot(BloodFx::PrefabAxis(p, nullptr), glm::vec3(1, 0, 0)) > 0.999f);
}

void Test_Blood_FleshHits() {
    BloodFx fx;
    fx.SetSimLookup([](const char*) { return 0; });
    int rays = 0;
    // A wall 1 m along -Z, facing the spray.
    fx.SetRaycast([&](const glm::vec3& o, const glm::vec3& d, float maxD, glm::vec3& p, glm::vec3& n) {
        ++rays;
        if (d.z > -0.5f || maxD < 1.0f) return false;
        p = o + glm::vec3(0, 0, -1);
        n = glm::vec3(0, 0, 1);
        return true;
    });
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.3f, 0);
    h.Direction = glm::vec3(0, 0, -1);
    h.Entity = 7;
    h.Damage = 40.0f;
    h.Killed = true;
    fx.OnFleshHit(h);
    CHECK(fx.SpraysSpawned() >= 1 && !fx.Sprays().empty());
    CHECK(rays >= 1);
    CHECK(fx.LastSprayClipped());
    // The fluid in front of the wall is kept, behind it clipped.
    const glm::vec4 plane = fx.Sprays()[0].ClipPlane;
    CHECK(glm::dot(plane, glm::vec4(0, 1, -0.5f, 1)) > 0.0f && glm::dot(plane, glm::vec4(0, 1, -1.3f, 1)) < 0.0f);
    // A shotgun's other pellets this moment add no spray; a hit on someone else does.
    const int after = fx.SpraysSpawned();
    fx.OnFleshHit(h);
    CHECK(fx.SpraysSpawned() == after);
    h.Entity = 8;
    h.Direction = glm::vec3(1, 0, 0); // no wall that way
    fx.OnFleshHit(h);
    CHECK(fx.SpraysSpawned() > after && !fx.LastSprayClipped());
    // Each spray plays out and is dropped; MaxSprays caps the live ones.
    fx.Update(0.5f);
    CHECK(!fx.Sprays().empty());
    fx.Update(5.0f);
    CHECK(fx.Sprays().empty());
    fx.Config.MaxSprays = 2;
    for (unsigned e = 100; e < 110; ++e) { h.Entity = e; fx.OnFleshHit(h); }
    CHECK((int)fx.Sprays().size() <= 2);
    fx.Config.Enabled = false;
    const int before = fx.SpraysSpawned();
    h.Entity = 200;
    fx.OnFleshHit(h);
    CHECK(fx.SpraysSpawned() == before);
}
void Test_Blood_Decals() {
    BloodFx fx;
    fx.SetSimLookup([](const char*) { return 0; });
    fx.SetDecalSetLookup([](const char*) { return 0; });
    glm::vec3 corpse(0.0f, 0.3f, -0.5f);
    bool corpseThere = true;
    fx.SetBodyLookup([&](unsigned, glm::vec3& c) { c = corpse; return corpseThere; });
    // A floor at y = 0 everywhere, a wall 1 m along -Z, a ceiling at 2.6 m.
    fx.SetRaycast([](const glm::vec3& o, const glm::vec3& d, float maxD, glm::vec3& p, glm::vec3& n) {
        if (d.y < -0.5f) { if (o.y > maxD) return false; p = glm::vec3(o.x, 0.0f, o.z); n = glm::vec3(0, 1, 0); return true; }
        if (d.y > 0.5f) { if (2.6f - o.y > maxD) return false; p = glm::vec3(o.x, 2.6f, o.z); n = glm::vec3(0, -1, 0); return true; }
        if (d.z < -0.5f) { if (o.z + 1.0f > maxD) return false; p = glm::vec3(o.x, o.y, -1.0f); n = glm::vec3(0, 0, 1); return true; }
        return false;
    });
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.6f, 0);
    h.Direction = glm::vec3(0, 0, -1);
    h.Entity = 3;
    h.Damage = 100.0f;
    h.Killed = true;
    h.Head = true;
    fx.OnFleshHit(h);
    const auto& ds = fx.Decals();
    bool floor = false, wall = false, ceiling = false, boxesRight = true;
    for (const BloodFx::Decal& d : ds) {
        const glm::vec3 up = glm::normalize(glm::vec3(d.Model[1])), at(d.Model[3]);
        floor |= up.y > 0.9f && std::abs(at.y - 0.01f) < 0.02f;
        wall |= up.z > 0.9f && std::abs(at.z + 1.0f) < 0.02f;
        ceiling |= up.y < -0.9f && std::abs(at.y - 2.6f) < 0.02f;
        boxesRight &= glm::determinant(d.Model) > 0.0f; // the box shader draws its far faces: no mirrors
        boxesRight &= d.Age <= 0.0f;                    // nothing lands the moment it's thrown
    }
    CHECK(floor);
    CHECK(wall);
    CHECK(ceiling);
    CHECK(boxesRight);
    // Stains land, spread in and hold; a dead body's pool appears once it has come to rest, then spreads.
    const size_t thrown = ds.size();
    fx.Update(1.5f);
    CHECK(fx.Decals().size() == thrown);
    fx.Update(2.0f);
    CHECK(fx.Decals().size() == thrown + 1);
    const BloodFx::Decal pool = fx.Decals().back();
    CHECK(pool.Reveal == nullptr && pool.PoolGrow > 5.0f);
    CHECK(std::abs(glm::vec3(pool.Model[3]).x - corpse.x) < 1e-4f && std::abs(glm::vec3(pool.Model[3]).y - 0.01f) < 1e-3f);
    BloodFx::Decal grow = pool;
    grow.Age = 0.0f;
    const float c0 = BloodFx::DecalCutout(grow);
    grow.Age = grow.PoolGrow * 0.5f;
    const float c1 = BloodFx::DecalCutout(grow);
    grow.Age = grow.PoolGrow + 1.0f;
    CHECK(c0 > c1 && c1 > BloodFx::DecalCutout(grow) && BloodFx::DecalCutout(grow) < 0.01f);
    grow.Age = grow.Life - 0.01f;
    CHECK(BloodFx::DecalCutout(grow) > 0.95f); // shrinks away at the end
    // The reveal-curve stains: hidden, spread by 0.3 of the curve, held, then the curve's tail.
    BloodCurve curve{3, {{0.0f, 1.0f, 0, 0}, {0.3f, 0.0f, 0, 0}, {1.0f, 1.0f, 0, 0}}};
    BloodFx::Decal r;
    r.Reveal = &curve;
    r.RevealSeconds = 10.0f;
    r.Life = 100.0f;
    r.Age = 0.0f;
    CHECK(BloodFx::DecalCutout(r) > 0.99f);
    r.Age = 50.0f;
    CHECK(BloodFx::DecalCutout(r) < 0.01f);
    r.Age = 99.9f;
    CHECK(BloodFx::DecalCutout(r) > 0.95f);
    // Stains go at the end of their life; the cap keeps the oldest out.
    fx.Update(fx.Config.DecalLifetime * 1.2f);
    CHECK(fx.Decals().empty());
    fx.Config.MaxDecals = 3;
    for (unsigned e = 50; e < 60; ++e) { h.Entity = e; h.Point.x = (float)e; fx.OnFleshHit(h); }
    CHECK((int)fx.Decals().size() <= 3);
    // No pool for a body that's gone by then.
    fx.Clear();
    corpseThere = false;
    h.Entity = 99;
    fx.OnFleshHit(h);
    const size_t before = fx.Decals().size();
    fx.Update(5.0f);
    CHECK(fx.Decals().size() == before);
}
void Test_Blood_Splats() {
    BloodFx fx;
    fx.SetSimLookup([](const char*) { return 0; });
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetRaycast([](const glm::vec3&, const glm::vec3&, float, glm::vec3&, glm::vec3&) { return false; });
    // Soldier 5: two meshes, each in its own bind space (one shifted 1 m up, one at 100x scale);
    // soldier 6 stands 1.5 m behind him, in the spray's path.
    std::vector<unsigned> alive = {5, 6};
    fx.SetSplatSpace([&](unsigned entity, int, bool, const glm::vec3&, BloodFx::SplatSpace& out) {
        if (std::find(alive.begin(), alive.end(), entity) == alive.end()) return false;
        out.Group = entity;
        out.Members.emplace_back(entity * 10 + 1, glm::translate(glm::mat4(1.0f), glm::vec3(0, 1, 0)));
        out.Members.emplace_back(entity * 10 + 2, glm::scale(glm::mat4(1.0f), glm::vec3(100.0f)));
        return true;
    });
    fx.SetSplatMembers([&](unsigned group, std::vector<unsigned>&) {
        return std::find(alive.begin(), alive.end(), group) != alive.end();
    });
    fx.SetBodyRay([](const glm::vec3& o, const glm::vec3& d, float maxD, unsigned& e, int& part, glm::vec3& p) {
        if (d.z > -0.5f || maxD < 1.5f) return false;
        e = 6;
        part = 1;
        p = o + d * 1.5f;
        return true;
    });
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.3f, 0);
    h.Direction = glm::vec3(0, 0, -1);
    h.Entity = 5;
    h.Part = 1;
    h.Damage = 30.0f;
    fx.OnFleshHit(h);
    bool entry = false, exit = false, onOther = false, scaled = false;
    for (const BloodFx::Splat& s : fx.Splats()) {
        if (s.Member == 51 && std::abs(s.Center.y - 2.3f) < 1e-4f && s.Normal.z > 0.99f) entry = true;     // facing the shooter
        if (s.Member == 51 && s.Center.z < -0.2f && s.Normal.z < -0.99f) exit = true;                     // out of the back
        if (s.Member == 52 && std::abs(s.Center.y - 130.0f) < 1e-2f && s.Radius > 5.0f) scaled = true;   // its own units
        onOther |= s.Group == 6;
    }
    CHECK(entry);
    CHECK(exit);
    CHECK(scaled);
    CHECK(onOther);
    CHECK(fx.SplatsSpawned() >= 4);
    // A mesh holds 24 at most.
    for (int i = 0; i < 40; ++i) { h.Entity = 5; fx.Update(0.3f); fx.OnFleshHit(h); }
    int on51 = 0;
    for (const BloodFx::Splat& s : fx.Splats()) on51 += s.Member == 51;
    CHECK(on51 <= 24 && on51 > 0);
    // When the body goes, its blood goes with it.
    alive = {6};
    fx.Update(0.1f);
    bool any5 = false;
    for (const BloodFx::Splat& s : fx.Splats()) any5 |= s.Group == 5;
    CHECK(!any5);
    CHECK(!fx.Splats().empty());
}

// --- v2: the Knife packs, energy, exits, flipbook puffs ---------------------------------------------------------
void Test_Blood_Energy() {
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.3f, 0);
    h.Origin = glm::vec3(0, 1.5f, 15.0f);
    h.Damage = 38.0f;
    const float rifle = BloodFx::Energy(h);
    CHECK(rifle > 0.85f && rifle < 1.2f);
    BloodFx::Hit far = h;
    far.Origin = glm::vec3(0, 1.5f, 120.0f);
    CHECK(BloodFx::Energy(far) < rifle);
    BloodFx::Hit head = h;
    head.Head = true;
    CHECK(BloodFx::Energy(head) > rifle);
    BloodFx::Hit shotgun = h;     // one pellet of a 9-pellet load, carrying the load
    shotgun.Damage = 14.0f;
    shotgun.Pellets = 9;
    CHECK(BloodFx::Energy(shotgun) > BloodFx::Energy(BloodFx::Hit{h.Point, h.Direction, 0, -1, 14.0f}));
    BloodFx::Hit graze = h;
    graze.Damage = 8.0f;
    CHECK(BloodFx::Energy(graze) < BloodFx::kExitEnergy);
    CHECK(BloodFx::Energy(BloodFx::Hit{h.Point, h.Direction, 0, -1, 10000.0f}) <= 3.0f);
    // The spray's stretch: along the line of flight longer, across it unchanged.
    const glm::mat4 m = BloodFx::PrefabToWorld(glm::vec3(0.0f), glm::vec3(1, 0, 0), 1.0f, 0.0f, glm::vec3(1, 0, 0), 1.4f);
    CHECK(std::abs(glm::length(glm::vec3(m * glm::vec4(1, 0, 0, 0))) - 1.4f) < 1e-4f);
    CHECK(std::abs(glm::length(glm::vec3(m * glm::vec4(0, 0, 1, 0))) - 1.0f) < 1e-4f);
    CHECK(std::abs(glm::length(glm::vec3(m * glm::vec4(0, 1, 0, 0))) - 1.0f) < 1e-4f);
}

void Test_Blood_ExitWound() {
    // Body 7 is a slab 0.3 m thick behind the entry (z 0 .. -0.3); body 9 stands behind it.
    BloodFx::BodyRayFn ray = [](const glm::vec3& o, const glm::vec3& d, float maxD, unsigned& entity, int& part, glm::vec3& point) {
        if (d.z <= 0.5f) return false; // only rays coming back along +Z
        part = 1;
        const float tExit = (-0.3f - o.z) / d.z, tOther = (-0.5f - o.z) / d.z;
        if (tOther >= 0.0f && tOther <= maxD && tOther < tExit) { entity = 9; point = o + d * tOther; return true; }
        if (tExit < 0.0f || tExit > maxD) return false;
        entity = 7;
        point = o + d * tExit;
        return true;
    };
    glm::vec3 exit;
    CHECK(BloodFx::FindExit(glm::vec3(0, 1.3f, 0), glm::vec3(0, 0, -1), 7, false, ray, exit));
    CHECK(std::abs(exit.z + 0.3f) < 1e-3f && std::abs(exit.y - 1.3f) < 1e-3f);
    // (Body 9 behind was looked past.) A body the line never leaves: the guess just past the entry.
    CHECK(!BloodFx::FindExit(glm::vec3(0, 1.3f, 0), glm::vec3(0, 0, -1), 8, false, ray, exit));
    CHECK(std::abs(exit.z + 0.13f) < 1e-3f);

    // Through OnFleshHit: a rifle exits where the body really ends; a graze stays in and sprays back out of the entry.
    BloodFx fx;
    fx.SetSimLookup([](const char*) { return 0; });
    fx.SetRaycast([](const glm::vec3&, const glm::vec3&, float, glm::vec3&, glm::vec3&) { return false; });
    fx.SetBodyRay(ray);
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.3f, 0);
    h.Direction = glm::vec3(0, 0, -1);
    h.Entity = 7;
    h.Damage = 40.0f;
    fx.OnFleshHit(h);
    CHECK(fx.LastExited() && fx.LastExitFound() && std::abs(fx.LastExitPoint().z + 0.3f) < 1e-3f);
    CHECK(!fx.Sprays().empty() && fx.Sprays().back().Model[3].z < -0.25f); // thrown from the far side
    h.Entity = 11;
    h.Damage = 6.0f;
    fx.OnFleshHit(h);
    CHECK(!fx.LastExited());
    CHECK(fx.Sprays().back().Model[3].z > -0.01f); // out of the entry, toward the shooter
}

void Test_Blood_Puffs() {
    FxSprites sprites;
    sprites.SetLookup([](const std::string& name, FxSprites::EntryInfo& info) {
        info.Frames = name == "blood_hit" || name == "blood_burst" ? 16 : 1;
        return name == "blood_hit" ? 0 : name == "blood_burst" ? 1 : name == "blood_cloud" ? 2 : name == "blood_drop" ? 3 : -1;
    });
    BloodFx fx;
    fx.SetSimLookup([](const char*) { return 0; });
    fx.SetRaycast([](const glm::vec3&, const glm::vec3&, float, glm::vec3&, glm::vec3&) { return false; });
    fx.SetBodyRay([](const glm::vec3&, const glm::vec3&, float, unsigned&, int&, glm::vec3&) { return false; });
    fx.SetSprites(&sprites);
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.3f, 0);
    h.Direction = glm::vec3(0, 0, -1);
    h.Entity = 1;
    h.Damage = 40.0f;
    fx.OnFleshHit(h);
    const int body = (int)sprites.Live().size();
    CHECK(body >= 6 && fx.PuffsSpawned() == 1);
    bool blood = false, mist = false;
    for (const auto& p : sprites.Live()) { blood |= p.E.Mode == FxSprites::Shade::Blood; mist |= p.E.Entry == 2; }
    CHECK(blood && mist);
    // A head shot reads denser: more of it.
    sprites.Clear();
    h.Entity = 2;
    h.Head = true;
    fx.OnFleshHit(h);
    CHECK((int)sprites.Live().size() > body);
    // The load's other pellets: a puff each, no spray.
    const int sprays = fx.SpraysSpawned(), puffs = fx.PuffsSpawned();
    fx.OnFleshHit(h);
    CHECK(fx.SpraysSpawned() == sprays && fx.PuffsSpawned() == puffs + 1);
    // Off: none; the player's own wounds never puff in front of their eyes.
    sprites.Clear();
    fx.Config.ImpactPuffs = false;
    h.Entity = 3;
    fx.OnFleshHit(h);
    CHECK(sprites.Live().empty());
    fx.Config.ImpactPuffs = true;
    h.Entity = kPlayerEntity;
    h.Player = true;
    fx.OnFleshHit(h);
    CHECK(sprites.Live().empty());
    // Gore off: nothing at all.
    fx.Config.Gore = 0;
    h.Player = false;
    h.Entity = 4;
    const int before = fx.SpraysSpawned();
    fx.OnFleshHit(h);
    CHECK(fx.SpraysSpawned() == before && sprites.Live().empty());
    // Everything is gone within a second and a half.
    fx.Config.Gore = 2;
    h.Entity = 5;
    fx.OnFleshHit(h);
    for (int i = 0; i < 30; ++i) sprites.Update(0.05f);
    CHECK(sprites.Live().empty());
}


void Test_Blood_KnifeDecals() {
    BloodFx fx;
    fx.SetSimLookup([](const char*) { return 0; });
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetKnifeLookup([](const char* e, int& cells) {
        const std::string n(e);
        cells = n.rfind("leak", 0) == 0 ? 30 : 4;
        return n == "pool_smooth" ? 10 : n == "pool_big" ? 11 : n.rfind("leak", 0) == 0 ? 20 : -1;
    });
    fx.SetBodyRay([](const glm::vec3&, const glm::vec3&, float, unsigned&, int&, glm::vec3&) { return false; });
    glm::vec3 corpse(0.0f, 0.3f, -0.5f);
    fx.SetBodyLookup([&](unsigned, glm::vec3& c) { c = corpse; return true; });
    // A floor at y = 0 and a wall 1 m along -Z.
    fx.SetRaycast([](const glm::vec3& o, const glm::vec3& d, float maxD, glm::vec3& p, glm::vec3& n) {
        if (d.y < -0.5f) { if (o.y > maxD) return false; p = glm::vec3(o.x, 0.0f, o.z); n = glm::vec3(0, 1, 0); return true; }
        if (d.z < -0.5f) { if (o.z + 1.0f > maxD) return false; p = glm::vec3(o.x, o.y, -1.0f); n = glm::vec3(0, 0, 1); return true; }
        return false;
    });
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.3f, 0);
    h.Direction = glm::vec3(0, 0, -1);
    h.Entity = 3;
    h.Damage = 40.0f;
    h.Killed = true;
    fx.OnFleshHit(h);
    // The wall spatter runs down in drips: Knife leak flipbooks hanging under it, image top at the spatter.
    CHECK(fx.DripsSpawned() >= 1);
    const BloodFx::Decal* drip = nullptr;
    for (const BloodFx::Decal& d : fx.Decals())
        if (d.Knife == 20) drip = &d;
    CHECK(drip && drip->Frames == 30 && drip->Cell == 0 && drip->FrameSeconds > 0.0f);
    if (drip) {
        const glm::vec3 x(drip->Model[0]), y(drip->Model[1]), z(drip->Model[2]);
        CHECK(glm::normalize(y).z > 0.99f);              // projects onto the wall
        CHECK(glm::normalize(z).y < -0.99f);             // the image's v runs down it
        CHECK(std::abs(glm::normalize(x).y) < 1e-3f);    // u across, level
    }
    // The corpse's pool is a Knife PBR pool.
    for (int i = 0; i < 40; ++i) fx.Update(0.1f);
    bool pool = false;
    for (const BloodFx::Decal& d : fx.Decals()) pool |= d.Knife == 10 || d.Knife == 11;
    CHECK(pool && fx.PoolsSpawned() == 1);
}

void Test_FxSprites_Sim() {
    int a, b;
    float t;
    FxSprites::FrameAt(0.0f, 16, 0, true, a, b, t);
    CHECK(a == 0 && b == 1 && t == 0.0f);
    FxSprites::FrameAt(0.5f, 16, 0, true, a, b, t);
    CHECK(a == 8 && b == 9);
    FxSprites::FrameAt(1.0f, 16, 0, true, a, b, t);
    CHECK(a == 15 && b == 15);
    FxSprites::FrameAt(0.5f, 4, 2, false, a, b, t);
    CHECK(a == 2 && b == 2 && t == 0.0f);
    FxSprites::FrameAt(0.99f, 4, 9, true, a, b, t); // a start past the end is clamped
    CHECK(a == 3 && b == 3);

    FxSprites fx;
    fx.SetLookup([](const std::string&, FxSprites::EntryInfo& info) { info.Frames = 4; return 0; });
    FxSprites::Emit e;
    e.Entry = fx.Entry("x");
    e.Vel = glm::vec3(1, 0, 0);
    e.Gravity = 1.0f;
    e.Life = 1.0f;
    e.Frame = -1; // a random variant
    fx.Spawn(e);
    CHECK(fx.Live().size() == 1 && fx.Live()[0].Frames == 4 && fx.Live()[0].E.Frame >= 0 && fx.Live()[0].E.Frame < 4);
    for (int i = 0; i < 10; ++i) fx.Update(0.05f);
    const glm::vec3 p = fx.Live()[0].E.Pos;
    CHECK(std::abs(p.x - 0.5f) < 1e-3f && p.y < -1.0f && p.y > -1.6f); // ~ -g t^2 / 2 = -1.23 at 0.5 s
    fx.Update(0.6f);
    CHECK(fx.Live().empty());
    // A floor at y = 0: it bounces and stays above.
    e.Pos = glm::vec3(0, 0.2f, 0);
    e.Vel = glm::vec3(0, -3, 0);
    e.Plane = glm::vec4(0, 1, 0, 0);
    e.Life = 2.0f;
    fx.Spawn(e);
    for (int i = 0; i < 20; ++i) { fx.Update(0.02f); CHECK(fx.Live()[0].E.Pos.y >= -1e-4f); }
    // The pool is capped: the oldest go first.
    fx.Clear();
    fx.MaxLive = 5;
    for (int i = 0; i < 9; ++i) { e.Pos.x = (float)i; fx.Spawn(e); }
    CHECK(fx.Live().size() == 5 && fx.Live()[0].E.Pos.x == 4.0f);
}

void Test_KnifeFx_Library() {
    using namespace KnifeFxImport;
    CHECK(MipCount(1024) == 9 && MipCount(2048) == 10 && MipCount(4) == 1);
    CHECK(LayerBytes(8, 2) == 4 * 16 + 16);
    // Area average: a 4x4 checker of 0 / 255 into one 2x2 cell is mid grey; alpha-weighted colour ignores
    // transparent texels.
    Image src;
    src.W = src.H = 4;
    src.Px.resize(64);
    for (int i = 0; i < 16; ++i) {
        const bool on = ((i % 4) + (i / 4)) % 2 == 0;
        src.Px[i * 4 + 0] = on ? 255 : 0;
        src.Px[i * 4 + 1] = 0;
        src.Px[i * 4 + 2] = 0;
        src.Px[i * 4 + 3] = on ? 255 : 0;
    }
    Image half = HalfSize(src, false);
    CHECK(half.W == 2 && half.H == 2 && std::abs((int)half.Px[0] - 128) <= 1 && std::abs((int)half.Px[3] - 128) <= 1);
    Image weighted = HalfSize(src, true);
    CHECK(weighted.Px[0] >= 254 && std::abs((int)weighted.Px[3] - 128) <= 1);
    // BC3 / BC5 blocks: 16 bytes per 4x4.
    std::vector<std::uint8_t> bc;
    CompressBC3(src, bc);
    CHECK(bc.size() == 16);
    CompressBC5(src, bc);
    CHECK(bc.size() == 32);
    // A library survives the file.
    LibraryData lib;
    lib.Header.Size = 8;
    lib.Header.Mips = 2;
    lib.Header.ColorLayers = 1;
    lib.Header.NormalLayers = 0;
    FileEntry e;
    std::memcpy(e.Name, "blood_hit", 10);
    e.ColorLayer = 0;
    e.Cols = 4;
    e.Rows = 4;
    e.Frames = 16;
    e.Flags = EntryMask;
    lib.Entries.push_back(e);
    lib.Color.assign(LayerBytes(8, 2), 0x5A);
    const std::string path = (std::filesystem::temp_directory_path() / "tartarus_kfx_test.kfx").string();
    CHECK(WriteLibrary(path, lib));
    LibraryData back;
    std::string err;
    CHECK(ReadLibrary(path, back, &err));
    CHECK(back.Entries.size() == 1 && std::string(back.Entries[0].Name) == "blood_hit" && back.Entries[0].Frames == 16);
    CHECK(back.Color.size() == lib.Color.size() && back.Color[5] == 0x5A);
    // A bad entry is refused.
    lib.Entries[0].Frames = 17;
    CHECK(WriteLibrary(path, lib));
    CHECK(!ReadLibrary(path, back, &err));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
} // namespace

void RegisterBloodTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Blood::HalfAndOct", Test_Blood_HalfAndOct});
    tests.push_back({"Blood::ExrZipsRoundTrip", Test_Blood_ExrZipsRoundTrip});
    tests.push_back({"Blood::VatBuild", Test_Blood_VatBuild});
    tests.push_back({"Blood::FbxUVs", Test_Blood_FbxUVs});
    tests.push_back({"Blood::Presets", Test_Blood_Presets});
    tests.push_back({"Blood::SprayTiming", Test_Blood_SprayTiming});
    tests.push_back({"Blood::FleshHits", Test_Blood_FleshHits});
    tests.push_back({"Blood::Decals", Test_Blood_Decals});
    tests.push_back({"Blood::Splats", Test_Blood_Splats});
    tests.push_back({"Blood::Energy", Test_Blood_Energy});
    tests.push_back({"Blood::ExitWound", Test_Blood_ExitWound});
    tests.push_back({"Blood::Puffs", Test_Blood_Puffs});
    tests.push_back({"Blood::KnifeDecals", Test_Blood_KnifeDecals});
    tests.push_back({"FxSprites::Sim", Test_FxSprites_Sim});
    tests.push_back({"KnifeFx::Library", Test_KnifeFx_Library});
}
