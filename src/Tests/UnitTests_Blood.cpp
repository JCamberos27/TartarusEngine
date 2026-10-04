#include "UnitTestSupport.h"

#include "BloodFxImport.h"
#include "Combat/BloodFx.h"
#include "Combat/BloodFxPresets.h"
#include "Combat/FxSprites.h"
#include "Combat/ImpactFx.h"
#include "BloodRenderer.h"
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
    fx.Update(0.2f);
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
    fx.Update(0.5f);
    CHECK(fx.Decals().size() == thrown);
    for (int i = 0; i < 6; ++i) fx.Update(0.1f); // a body at rest: looked at twice, 0.2 s apart, then its pool (by ~0.8 s)
    CHECK(fx.Decals().size() == thrown + 1);
    const BloodFx::Decal pool = fx.Decals().back();
    CHECK(pool.Reveal == nullptr && pool.PoolGrow > 15.0f && pool.Spread);
    {   // It starts as a small patch and widens slowly, fast at first: never a full pool popping in.
        BloodFx::Decal p = pool;
        p.Age = 0.0f;
        const float s0 = BloodFx::PoolScale(p);
        p.Age = 2.0f;
        const float s2 = BloodFx::PoolScale(p);
        p.Age = p.PoolGrow * 0.5f;
        const float sHalf = BloodFx::PoolScale(p);
        p.Age = p.PoolGrow + 1.0f;
        const float sEnd = BloodFx::PoolScale(p);
        CHECK(s0 < 0.1f && s2 < 0.4f && s2 > s0 && sHalf > 0.7f && sHalf < 1.0f && std::abs(sEnd - 1.0f) < 1e-5f);
        CHECK(sHalf - s0 > sEnd - sHalf); // the spread slows
    }
    CHECK(std::abs(glm::vec3(pool.Model[3]).x - corpse.x) < 1e-4f && std::abs(glm::vec3(pool.Model[3]).y - 0.01f) < 1e-3f);
    BloodFx::Decal grow = pool;
    grow.Age = 0.0f;
    const float c0 = BloodFx::DecalCutout(grow);
    grow.Age = grow.PoolGrow * 0.5f;
    const float c1 = BloodFx::DecalCutout(grow);
    grow.Age = grow.PoolGrow + 1.0f;
    CHECK(c0 > c1 && c1 > BloodFx::DecalCutout(grow) && BloodFx::DecalCutout(grow) < 0.01f);
    grow.Age = 5000.0f;
    CHECK(BloodFx::DecalCutout(grow) < 0.01f); // and stays: stains don't shrink away
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
    CHECK(BloodFx::DecalCutout(r) < 0.01f); // held, past its old lifetime
    // Stains stay for good by default; the cap still holds the count.
    fx.Update(1000.0f);
    CHECK(!fx.Decals().empty());
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


void Test_Blood_Footprints() {
    BloodFx fx;
    fx.SetSimLookup([](const char*) { return 0; });
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetKnifeLookup([](const char* e, int& cells) {
        const std::string n(e);
        cells = n == "footprint" ? 8 : 4;
        return n == "pool_smooth" || n == "pool_big" ? 10 : n == "footprint" ? 30 : -1;
    });
    fx.SetBodyRay([](const glm::vec3&, const glm::vec3&, float, unsigned&, int&, glm::vec3&) { return false; });
    const glm::vec3 corpse(0.0f, 0.3f, 0.0f);
    fx.SetBodyLookup([&](unsigned, glm::vec3& c) { c = corpse; return true; });
    fx.SetRaycast([](const glm::vec3& o, const glm::vec3& d, float maxD, glm::vec3& p, glm::vec3& n) {
        if (d.y > -0.5f || o.y > maxD) return false;
        p = glm::vec3(o.x, 0.0f, o.z);
        n = glm::vec3(0, 1, 0);
        return true;
    });
    // A corpse's pool, grown in.
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.3f, 0);
    h.Direction = glm::vec3(1, 0, 0);
    h.Entity = 3;
    h.Damage = 40.0f;
    h.Killed = true;
    fx.OnFleshHit(h);
    for (int i = 0; i < 300; ++i) fx.Update(0.1f);
    CHECK(fx.PoolsSpawned() == 1);
    // Walking clean: no prints.
    fx.OnFootstep(-1, glm::vec3(5, 0, 5), glm::vec3(0, 0, -1.5f), 0);
    CHECK(fx.PrintsSpawned() == 0 && fx.BloodySteps(-1) == 0);
    // Through the pool, then away: the next steps print, fainter each, then stop.
    fx.OnFootstep(-1, glm::vec3(0, 0, 0), glm::vec3(0, 0, -1.5f), 1);
    CHECK(fx.BloodySteps(-1) == BloodFx::kPrintSteps);
    float lastOpacity = 2.0f;
    bool fading = true;
    for (int i = 0; i < BloodFx::kPrintSteps + 4; ++i) {
        fx.OnFootstep(-1, glm::vec3(0, 0, -2.0f - 0.7f * (float)i), glm::vec3(0, 0, -1.5f), i % 2);
        const BloodFx::Decal& d = fx.Decals().back();
        if (d.Knife == 30 && i < BloodFx::kPrintSteps) { fading &= d.Opacity < lastOpacity; lastOpacity = d.Opacity; }
    }
    CHECK(fx.PrintsSpawned() == BloodFx::kPrintSteps && fading && fx.BloodySteps(-1) == 0);
    // The prints point the way they walked (toe = the image's top = -z of the box, so the box's z runs back).
    const BloodFx::Decal& print = fx.Decals().back();
    CHECK(print.Knife == 30 && glm::normalize(glm::vec3(print.Model[2])).z > 0.99f);
    // Facing wins over the way they move: stepping in it again and turning to face +x while moving -z, the print's
    // toe points +x.
    fx.Clear();
    fx.OnFleshHit(h);
    for (int i = 0; i < 300; ++i) fx.Update(0.1f);
    fx.OnFootstep(-1, glm::vec3(0, 0, 0), glm::vec3(0, 0, -1.5f), 1);
    fx.OnFootstep(-1, glm::vec3(0, 0, -2.0f), glm::vec3(0, 0, -1.5f), 0, glm::vec3(1, 0, 0));
    CHECK(fx.Decals().back().Knife == 30 && glm::normalize(glm::vec3(fx.Decals().back().Model[2])).x < -0.99f);
    // Old blood doesn't mark a sole.
    for (int i = 0; i < 700; ++i) fx.Update(0.1f);
    fx.OnFootstep(7, glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), -1);
    CHECK(fx.BloodySteps(7) == 0);
}






void Test_ImpactFx_Surfaces() {
    // Substring words: none may hide inside another surface's name ("pane" in "Panel" did).
    CHECK(ImpactFx::SurfaceFromName("Metal Plate") == "metal");
    CHECK(ImpactFx::SurfaceFromName("Wood Panel") == "wood");
    CHECK(ImpactFx::SurfaceFromName("Brick Panel") == "brick");
    CHECK(ImpactFx::SurfaceFromName("Glass Pane") == "glass");
    CHECK(ImpactFx::SurfaceFromName("Mud Bank") == "mud");
    CHECK(ImpactFx::SurfaceFromName("Tile Panel") == "tile");
    CHECK(ImpactFx::SurfaceFromName("Concrete Block") == "concrete");
    CHECK(ImpactFx::SurfaceFromName("Back Wall") == "concrete");
    CHECK(ImpactFx::SurfaceFromName("Loose Crate 1") == "wood");
    CHECK(ImpactFx::SurfaceFromName("something else") == "concrete");
    CHECK(std::string(ImpactFx::HoleEntry("brick")) == "hole_brick" && std::string(ImpactFx::HoleEntry("anything")) == "hole_concrete");
    // The hole a little bigger than the round: the cell x the hole's share of it = kHoleScale x the calibre.
    for (const char* s : {"concrete", "brick", "wood", "metal", "glass", "mud", "sand", "tile", "asphalt", "rock"}) {
        const float hole = ImpactFx::HoleSize(s, 0.0027f) * ImpactFx::HoleFraction(s);
        CHECK(std::abs(hole - 0.0027f * 2.0f * ImpactFx::kHoleScale) < 1e-4f && ImpactFx::HoleRim(s) > ImpactFx::HoleFraction(s));
    }
    CHECK(ImpactFx::HoleSize("concrete", 0.0027f) < 0.1f); // not a 15 cm crater
}


void Test_Blood_Perf() {
    // Culling: far or tiny on screen isn't drawn; near, or with the eye inside it, is.
    BloodRenderer& r = BloodRenderer::Get();
    const glm::vec3 eye(0.0f);
    CHECK(r.WorthDrawing(glm::vec3(0, 0, -10), 0.5f, eye, 80.0f));
    CHECK(!r.WorthDrawing(glm::vec3(0, 0, -100), 0.5f, eye, 80.0f));
    CHECK(!r.WorthDrawing(glm::vec3(0, 0, -50), 0.05f, eye, 80.0f));   // 1 mm-ish on screen
    CHECK(r.WorthDrawing(glm::vec3(0, 0, -0.1f), 0.5f, eye, 80.0f));   // inside it
    // Stain dedupe: a stain landing right on a fresh one like it is the same stain.
    BloodFx fx;
    fx.SetSimLookup([](const char*) { return 0; });
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetBodyRay([](const glm::vec3&, const glm::vec3&, float, unsigned&, int&, glm::vec3&) { return false; });
    fx.SetRaycast([](const glm::vec3& o, const glm::vec3& d, float maxD, glm::vec3& p, glm::vec3& n) {
        if (d.y > -0.5f || o.y > maxD) return false;
        p = glm::vec3(o.x, 0.0f, o.z);
        n = glm::vec3(0, 1, 0);
        return true;
    });
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.3f, 0);
    h.Direction = glm::vec3(1, 0, 0);
    h.Entity = 1;
    h.Damage = 40.0f;
    fx.OnFleshHit(h);
    for (int i = 0; i < 20; ++i) fx.Update(0.05f);
    CHECK(!fx.Decals().empty());
    if (!fx.Decals().empty()) {
        const BloodFx::Decal& d = fx.Decals().front();
        CHECK(fx.DuplicateOf(d.Set, d.Knife, d.Model) == 0);
        glm::mat4 moved = d.Model;
        moved[3] += glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        CHECK(fx.DuplicateOf(d.Set, d.Knife, moved) == -1);
        CHECK(fx.DuplicateOf(d.Set, 99, d.Model) == -1);
    }
}


void Test_Blood_LabActions() {
    BloodFx fx;
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetKnifeLookup([](const char* e, int& cells) {
        const std::string n(e);
        cells = n.rfind("leak", 0) == 0 ? 30 : 4;
        return n == "pool_smooth" || n == "pool_big" ? 10 : n.rfind("leak", 0) == 0 ? 20 : -1;
    });
    fx.SpawnPoolAt(glm::vec3(1, 0, 1), glm::vec3(0, 1, 0), 1.2f);
    CHECK(fx.PoolsSpawned() == 1 && !fx.Decals().empty() && (fx.Decals().back().Knife == 10));
    const size_t before = fx.Decals().size();
    fx.SpatterWallAt(glm::vec3(0, 1.4f, -1), glm::vec3(0, 0, 1), 0.6f);
    CHECK(fx.Decals().size() >= before + 3 && fx.DripsSpawned() >= 1); // the blot, the streaks, the drips
    fx.Config.Gore = 0; // the lab's pool / spatter ignore Gore (they're asked for); hits don't
    BloodFx::Hit h;
    h.Entity = 3;
    h.Damage = 40.0f;
    const int sprays = fx.SpraysSpawned();
    fx.OnFleshHit(h);
    CHECK(fx.SpraysSpawned() == sprays);
}

void Test_Blood_GroundSplatter() {
    // Every hit throws blood out along its line, landing on whatever it meets when it has fallen there; a head throws more.
    BloodFx fx;
    fx.SetSimLookup([](const char*) { return 0; });
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetKnifeLookup([](const char*, int&) { return -1; });
    fx.SetBodyRay([](const glm::vec3&, const glm::vec3&, float, unsigned&, int&, glm::vec3&) { return false; });
    float wallX = 100.0f; // a wall facing -x at x = wallX, and the floor at y = 0
    fx.SetRaycast([&](const glm::vec3& o, const glm::vec3& d, float maxD, glm::vec3& p, glm::vec3& n) {
        float best = maxD;
        bool hit = false;
        if (d.y < -1e-4f && -o.y / d.y <= best) { best = -o.y / d.y; n = glm::vec3(0, 1, 0); hit = true; }
        if (d.x > 1e-4f && (wallX - o.x) / d.x >= 0.0f && (wallX - o.x) / d.x <= best) { best = (wallX - o.x) / d.x; n = glm::vec3(-1, 0, 0); hit = true; }
        if (hit) p = o + d * best;
        return hit;
    });
    glm::vec3 body(0.0f, 0.0f, 0.0f);
    fx.SetBodyLookup([&](unsigned, glm::vec3& c) { c = body + glm::vec3(0, 1.0f, 0); return true; });
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.3f, 0);
    h.Direction = glm::vec3(1, 0, 0);
    h.Entity = 3;
    h.Damage = 40.0f;
    fx.OnFleshHit(h);
    CHECK(fx.GroundSplatters() == 1);
    bool beyond = false, timed = false;
    for (const BloodFx::Decal& d : fx.Decals()) {
        const glm::vec3 at(d.Model[3]);
        beyond |= at.x > 0.3f && std::abs(at.y - 0.01f) < 0.02f;
        timed |= d.Age < -0.2f && d.Age > -0.8f; // ~ the fall from chest height
    }
    CHECK(beyond && timed);
    // A wall in the way: it lands on the wall, facing out of it.
    fx.Clear();
    wallX = 1.2f;
    h.Entity = 9;
    fx.OnFleshHit(h);
    bool onWall = false;
    for (const BloodFx::Decal& d : fx.Decals()) onWall |= std::abs(glm::vec3(d.Model[3]).x - 1.2f) < 0.02f && glm::normalize(glm::vec3(d.Model[1])).x < -0.9f;
    CHECK(onWall && fx.GroundSplatters() >= 1);
    wallX = 100.0f;
    fx.Clear();
    h.Entity = 3;
    fx.OnFleshHit(h);
    // Head bigger than body, a head kill bigger again.
    BloodFx::Hit head = h;
    head.Head = true;
    const float headWound = BloodFx::GroundSplatterSize(head, 1.0f);
    CHECK(headWound > BloodFx::GroundSplatterSize(h, 1.0f));
    head.Killed = true;
    CHECK(BloodFx::GroundSplatterSize(head, 1.0f) > headWound);
    // The living wound drips as he walks away; a kill doesn't (it pools instead).
    CHECK(fx.Bleeding(3));
    const int before = fx.BleedDrops();
    for (int i = 0; i < 50; ++i) { body.x -= 0.05f; fx.Update(0.1f); }
    CHECK(fx.BleedDrops() >= before + 4);
    for (int i = 0; i < 60; ++i) fx.Update(0.1f);
    CHECK(!fx.Bleeding(3));
    h.Entity = 4;
    h.Killed = true;
    fx.OnFleshHit(h);
    CHECK(!fx.Bleeding(4));
}

void Test_Blood_CorpseSplash() {
    // Shooting a body lying on the floor: blood splashes round it on the floor, and the wound pools.
    BloodFx fx;
    fx.SetSimLookup([](const char*) { return 0; });
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetKnifeLookup([](const char*, int&) { return -1; });
    fx.SetBodyRay([](const glm::vec3&, const glm::vec3&, float, unsigned&, int&, glm::vec3&) { return false; });
    fx.SetRaycast([](const glm::vec3& o, const glm::vec3& d, float maxD, glm::vec3& p, glm::vec3& n) {
        if (d.y > -1e-4f || -o.y / d.y > maxD || o.y < 0.0f) return false; // the floor at y = 0, from above only
        p = o + d * (-o.y / d.y);
        n = glm::vec3(0, 1, 0);
        return true;
    });
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 0.15f, 0);
    h.Direction = glm::normalize(glm::vec3(0, -0.6f, -1));
    h.Entity = 6;
    h.Damage = 40.0f;
    h.Corpse = true;
    fx.OnFleshHit(h);
    int onFloor = 0;
    bool pool = false;
    for (const BloodFx::Decal& d : fx.Decals()) {
        onFloor += std::abs(glm::vec3(d.Model[3]).y - 0.01f) < 0.02f && glm::length(glm::vec2(d.Model[3].x, d.Model[3].z)) < 2.5f;
        pool |= d.Spread;
    }
    CHECK(onFloor >= 3 && !pool && fx.GroundSplatters() == 1); // its splash; the body's one pool comes from its death
}

void Test_Blood_SplashVariety() {
    // The thrown blood draws on all the shapes there are: the KriptoFX stains and Real Blood's splatters, mirrored.
    BloodFx fx;
    std::vector<std::string> asked;
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetKnifeLookup([&](const char* e, int& cells) {
        asked.push_back(e);
        cells = std::string(e) == "drops" ? 16 : 4;
        return 40;
    });
    int mirrored = 0;
    std::vector<int> cells;
    for (int i = 0; i < 200; ++i) {
        const auto kind = (BloodFx::Splash)(i % 3);
        BloodFx::Decal* d = fx.AddSplash(kind, glm::vec3((float)i, 0, 0), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0), glm::vec3(0.3f, 0.3f, 0.3f), 0.0f);
        CHECK(d != nullptr);
        if (!d) continue;
        mirrored += d->Mirror;
        if (kind == BloodFx::Splash::Drop && d->Knife == 40) cells.push_back(d->Cell);
    }
    const auto has = [&](const char* n) { return std::find(asked.begin(), asked.end(), std::string(n)) != asked.end(); };
    CHECK(has("splat_small") && has("splat_wide") && has("splat_medium") && has("drops"));
    CHECK(mirrored > 30 && mirrored < 110);
    // On a floor only the round drops of the sheet (not the runs).
    for (int c : cells) CHECK(c == 3 || c == 4 || c == 6 || c == 8 || c == 9 || c == 10 || c == 11 || c == 13 || c == 15);
}

void Test_Blood_OnePool() {
    // One pool per body, from under its middle once it has stopped moving - not where it was mid-fall.
    BloodFx fx;
    fx.SetSimLookup([](const char*) { return 0; });
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetKnifeLookup([](const char*, int&) { return -1; });
    fx.SetBodyRay([](const glm::vec3&, const glm::vec3&, float, unsigned&, int&, glm::vec3&) { return false; });
    fx.SetRaycast([](const glm::vec3& o, const glm::vec3& d, float maxD, glm::vec3& p, glm::vec3& n) {
        if (d.y > -1e-4f || -o.y / d.y > maxD || o.y < 0.0f) return false;
        p = o + d * (-o.y / d.y);
        n = glm::vec3(0, 1, 0);
        return true;
    });
    glm::vec3 body(0.0f, 1.0f, 0.0f);
    fx.SetBodyLookup([&](unsigned, glm::vec3& c) { c = body; return true; });
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.3f, 0);
    h.Direction = glm::vec3(1, 0, 0);
    h.Entity = 5;
    h.Damage = 60.0f;
    h.Killed = true;
    fx.OnFleshHit(h);
    fx.OnFleshHit(h); // a second kill hit (a pellet) queues nothing more
    // Falling for 1.5 s (sliding 1 m along x), then still.
    for (int i = 0; i < 15; ++i) { body.x += 0.07f; body.y = std::max(0.2f, body.y - 0.06f); fx.Update(0.1f); }
    CHECK(fx.PoolsSpawned() == 0 && fx.Pooled(5));
    for (int i = 0; i < 10; ++i) fx.Update(0.1f);
    CHECK(fx.PoolsSpawned() == 1);
    for (const BloodFx::Decal& d : fx.Decals())
        if (d.Spread) CHECK(std::abs(d.Model[3].x - body.x) < 1e-3f && std::abs(d.Model[3].z) < 1e-3f);
    // Shot again later, killed again: still the one pool.
    h.Corpse = true;
    fx.OnFleshHit(h);
    for (int i = 0; i < 60; ++i) fx.Update(0.1f);
    CHECK(fx.PoolsSpawned() == 1);
}

void Test_Blood_Gear() {
    // The player's gun: speckles that don't stack into a coat, never their own wound, and they stay - across a swap too.
    BloodFx fx;
    fx.SetDecalSetLookup([](const char*) { return 0; });
    unsigned gunEntity = 70;
    const unsigned key = 0xABCDu;
    int bodyOnlyAsks = 0;
    fx.SetSplatSpace([&](unsigned entity, int part, bool, const glm::vec3&, BloodFx::SplatSpace& out) {
        if (entity != kPlayerEntity) return false; // the soldiers' own wounds aren't on the player's gear
        out.Group = kPlayerEntity;
        out.Members.emplace_back(10u, glm::mat4(1.0f)); // a body piece
        out.Keys.push_back(0u);
        if (part == BloodFx::kBodyOnlyPart) { ++bodyOnlyAsks; return true; }
        out.Members.emplace_back(gunEntity, glm::mat4(1.0f));
        out.Keys.push_back(key);
        return true;
    });
    fx.SetSplatMembers([](unsigned, std::vector<unsigned>&) { return true; });
    bool gunOut = true;
    fx.SetMemberResolver([&](unsigned k) { return k == key && gunOut ? gunEntity : 0xFFFFFFFFu; });
    // Being shot: the wound is on the body only.
    BloodFx::Hit hurt;
    hurt.Point = glm::vec3(0, 1.2f, 0);
    hurt.Entity = kPlayerEntity;
    hurt.Player = true;
    hurt.Damage = 30.0f;
    fx.OnFleshHit(hurt);
    int onGun = 0;
    for (const BloodFx::Splat& s : fx.Splats()) onGun += s.Key == key;
    CHECK(bodyOnlyAsks >= 1 && onGun == 0);
    // Speckles from many hits: at most kMaxGearSplats on the gun, never two on one spot, none lost.
    BloodFx::Hit near;
    near.Entity = 3;
    near.Damage = 40.0f;
    fx.SetSimLookup([](const char*) { return -1; });
    fx.SetKnifeLookup([](const char*, int&) { return -1; });
    fx.SetRaycast([](const glm::vec3&, const glm::vec3&, float, glm::vec3&, glm::vec3&) { return false; });
    fx.SetBodyRay([](const glm::vec3&, const glm::vec3&, float, unsigned&, int&, glm::vec3&) { return false; });
    int calls = 0;
    fx.SetPlayerGear([&](glm::vec3* p, glm::vec3* n, int max) {
        const int k = std::min(max, 8);
        for (int i = 0; i < k; ++i) { p[i] = glm::vec3(0.01f * (float)((calls * 7 + i) % 40), 0.0f, 0.0f); n[i] = glm::vec3(0, 1, 0); }
        ++calls;
        return k;
    });
    for (int i = 0; i < 80; ++i) {
        near.Entity = 100u + (unsigned)i;
        near.ByPlayer = true;
        near.Origin = glm::vec3(0, 1.3f, 0.5f);
        near.Point = glm::vec3(0, 1.3f, 0);
        fx.OnFleshHit(near);
        fx.Update(0.3f);
    }
    std::vector<const BloodFx::Splat*> gun;
    for (const BloodFx::Splat& s : fx.Splats())
        if (s.Key == key) gun.push_back(&s);
    CHECK(!gun.empty() && (int)gun.size() <= BloodFx::kMaxGearSplats);
    bool stacked = false;
    for (size_t a = 0; a < gun.size(); ++a)
        for (size_t b = a + 1; b < gun.size(); ++b)
            stacked |= glm::length(gun[a]->Center - gun[b]->Center) < 0.7f * std::max(gun[a]->Radius, gun[b]->Radius);
    CHECK(!stacked);
    for (const BloodFx::Splat* s : gun) CHECK(s->Radius <= 0.031f && s->DryAge == 0.0f); // speckles; gear never dries
    // A swap: the gun rebuilt as entity 71 - its blood is drawn on the new one; put away, it isn't drawn (but kept).
    gunEntity = 71;
    for (const BloodFx::Splat* s : gun) CHECK(fx.DrawnOn(*s) == 71u);
    gunOut = false;
    for (const BloodFx::Splat* s : gun) CHECK(fx.DrawnOn(*s) == 0xFFFFFFFFu);
    CHECK((int)std::count_if(fx.Splats().begin(), fx.Splats().end(), [&](const BloodFx::Splat& s) { return s.Key == key; }) == (int)gun.size());
}

void Test_Blood_DryOutOfView() {
    // Blood doesn't dry while you look at it; turned away, it does - slowly.
    BloodFx fx;
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetKnifeLookup([](const char*, int&) { return -1; });
    fx.SetViewer(glm::vec3(0.0f), glm::vec3(0, 0, -1));
    fx.SpatterWallAt(glm::vec3(0, 1, -5), glm::vec3(0, 0, 1), 0.5f);
    fx.Update(600.0f);
    CHECK(fx.Decals()[0].DryAge == 0.0f);
    fx.SetViewer(glm::vec3(0.0f), glm::vec3(0, 0, 1));
    fx.Update(60.0f);
    CHECK(fx.Decals()[0].DryAge > 59.0f && fx.Decals()[0].DryAge / fx.Decals()[0].DrySeconds < 0.06f);
}

void Test_Blood_PrintsStay() {
    // Footprints keep through the stain cap; old ones fade out slowly (the oldest first), never popping.
    BloodFx fx;
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetKnifeLookup([](const char* e, int& cells) { cells = 8; return std::string(e) == "footprint" ? 30 : -1; });
    fx.SetRaycast([](const glm::vec3& o, const glm::vec3&, float, glm::vec3& p, glm::vec3& n) { p = glm::vec3(o.x, 0.0f, o.z); n = glm::vec3(0, 1, 0); return true; });
    fx.Config.MaxDecals = 20;
    // A pool to step in, then a trail.
    fx.SpawnPoolAt(glm::vec3(0, 0, 0), glm::vec3(0, 1, 0), 2.0f);
    fx.Update(30.0f);
    fx.OnFootstep(-1, glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), 0);
    for (int i = 0; i < BloodFx::kPrintSteps; ++i) fx.OnFootstep(-1, glm::vec3(0, 0, -2.0f - 0.7f * (float)i), glm::vec3(0, 0, -1), i % 2);
    const int prints = fx.PrintsSpawned();
    CHECK(prints == BloodFx::kPrintSteps);
    // Lots of other blood: the cap takes the others, never the prints.
    for (int i = 0; i < 60; ++i) fx.SpatterWallAt(glm::vec3((float)i, 1, -5), glm::vec3(0, 0, 1), 0.3f);
    int kept = 0;
    for (const BloodFx::Decal& d : fx.Decals()) kept += d.Print;
    CHECK(kept == prints);
    // Still there at 9 minutes; gone after 10 + 3.
    fx.Update(500.0f);
    kept = 0;
    for (const BloodFx::Decal& d : fx.Decals()) kept += d.Print;
    CHECK(kept == prints);
    fx.Update(300.0f); // 830 s on: past 10 min + the 3 min fade
    kept = 0;
    for (const BloodFx::Decal& d : fx.Decals()) kept += d.Print;
    CHECK(kept == 0);
}

void Test_Blood_Persist() {
    // Stains never leave in view: past their lifetime or over the cap, the one that goes is out of view (behind, far).
    BloodFx fx;
    fx.SetDecalSetLookup([](const char*) { return 0; });
    fx.SetKnifeLookup([](const char*, int&) { return -1; });
    fx.SetViewer(glm::vec3(0.0f), glm::vec3(0, 0, -1));
    fx.Config.MaxDecals = 3;
    fx.SpatterWallAt(glm::vec3(0, 1, -5), glm::vec3(0, 0, 1), 0.5f);  // ahead: in view
    const size_t ahead = fx.Decals().size();
    CHECK(ahead >= 1);
    fx.Config.MaxDecals = (int)ahead + 1;
    fx.SpawnPoolAt(glm::vec3(0, 0, 30), glm::vec3(0, 1, 0), 1.0f);  // behind
    fx.SpawnPoolAt(glm::vec3(0, 0, -8), glm::vec3(0, 1, 0), 1.0f);  // ahead again: over the cap, the one behind goes
    bool behind = false;
    for (const BloodFx::Decal& d : fx.Decals()) behind |= glm::vec3(d.Model[3]).z > 20.0f;
    CHECK(!behind && (int)fx.Decals().size() == fx.Config.MaxDecals);
    // A lifetime set: in view it stays past it; turn away and it goes.
    fx.Clear();
    fx.Config.MaxDecals = 64;
    fx.Config.DecalLifetime = 10.0f;
    fx.SpawnPoolAt(glm::vec3(0, 0, -5), glm::vec3(0, 1, 0), 1.0f);
    fx.Update(30.0f);
    CHECK(fx.Decals().size() == 1);
    fx.SetViewer(glm::vec3(0.0f), glm::vec3(0, 0, 1));
    fx.Update(0.1f);
    CHECK(fx.Decals().empty());
    // Far off counts as out of view whichever way you face.
    fx.SpawnPoolAt(glm::vec3(0, 0, 200), glm::vec3(0, 1, 0), 1.0f);
    CHECK(!fx.InView(fx.Decals().back()));
    // Drying is slow: barely started a minute in.
    BloodFx::Settings defaults;
    CHECK(60.0f / (defaults.DrySeconds * 0.8f) < 0.1f && defaults.DecalLifetime == 0.0f);
}

void Test_Blood_Palette() {
    // One material: the sprays, the stains, the splats and the bursts share the palette's hue, and none is mirror-glossy.
    auto hue = [](const glm::vec3& c) { return glm::vec2(c.g / c.r, c.b / c.r); };
    const glm::vec2 h = hue(BloodPalette::Fresh);
    BloodRenderer& r = BloodRenderer::Get();
    for (const glm::vec3& c : {r.FluidAlbedo, r.FilmFresh, BloodPalette::Thin, BloodPalette::Mist})
        CHECK(glm::length(hue(c) - h) < 0.05f);
    CHECK(r.FluidRoughness >= 0.15f && BloodPalette::RoughPool >= 0.15f && BloodPalette::RoughFresh >= 0.2f && BloodPalette::RoughSprite >= 0.2f);
}

void Test_Blood_Speed() {
    // The spray is out of the wound on the frame of the hit; Speed divides its playback.
    auto spray = [](float speed) {
        BloodFx fx;
        fx.Config.Speed = speed;
        fx.SetSimLookup([](const char*) { return 0; });
        fx.SetDecalSetLookup([](const char*) { return 0; });
        fx.SetKnifeLookup([](const char*, int&) { return -1; });
        fx.SetRaycast([](const glm::vec3&, const glm::vec3&, float, glm::vec3&, glm::vec3&) { return false; });
        fx.SetBodyRay([](const glm::vec3&, const glm::vec3&, float, unsigned&, int&, glm::vec3&) { return false; });
        BloodFx::Hit hit;
        hit.Point = glm::vec3(0, 1.3f, 0);
        hit.Entity = 4;
        hit.Damage = 40.0f;
        fx.OnFleshHit(hit);
        return fx.Sprays().empty() ? BloodFx::Spray{} : fx.Sprays()[0];
    };
    const BloodFx::Spray slow = spray(1.0f), fast = spray(2.0f);
    CHECK(fast.Age > 0.0f && fast.FramesCount > 0.0f && BloodFx::FrameAt(fast.Age / fast.Duration, fast.FramesCount) >= 1);
    CHECK(std::abs(slow.Duration / fast.Duration - 2.0f) < 1e-3f);
    BloodFx::Hit h;
    h.Point = glm::vec3(0, 1.3f, 0);
    // Gore's sound: a big headshot kill only.
    BloodFx::Hit head = h;
    head.Head = head.Killed = true;
    CHECK(BloodFx::BigHeadshot(head, 1.5f, 1) && !BloodFx::BigHeadshot(head, 1.0f, 1) && !BloodFx::BigHeadshot(head, 1.5f, 0));
    head.Player = true;
    CHECK(!BloodFx::BigHeadshot(head, 2.0f, 1));
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
    tests.push_back({"Blood::Footprints", Test_Blood_Footprints});
    tests.push_back({"Blood::Persist", Test_Blood_Persist});
    tests.push_back({"Blood::GroundSplatter", Test_Blood_GroundSplatter});
    tests.push_back({"Blood::OnePool", Test_Blood_OnePool});
    tests.push_back({"Blood::Gear", Test_Blood_Gear});
    tests.push_back({"Blood::DryOutOfView", Test_Blood_DryOutOfView});
    tests.push_back({"Blood::PrintsStay", Test_Blood_PrintsStay});
    tests.push_back({"Blood::CorpseSplash", Test_Blood_CorpseSplash});
    tests.push_back({"Blood::SplashVariety", Test_Blood_SplashVariety});
    tests.push_back({"Blood::Palette", Test_Blood_Palette});
    tests.push_back({"Blood::Speed", Test_Blood_Speed});
    tests.push_back({"ImpactFx::Surfaces", Test_ImpactFx_Surfaces});
    tests.push_back({"Blood::Perf", Test_Blood_Perf});
    tests.push_back({"Blood::LabActions", Test_Blood_LabActions});
    tests.push_back({"FxSprites::Sim", Test_FxSprites_Sim});
    tests.push_back({"KnifeFx::Library", Test_KnifeFx_Library});
}
