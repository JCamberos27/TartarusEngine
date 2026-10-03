#include "UnitTestSupport.h"

#include "BloodFxImport.h"
#include "Combat/BloodFx.h"
#include "Combat/BloodFxPresets.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
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
    }
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
    CHECK(rays == 1);
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
} // namespace

void RegisterBloodTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Blood::HalfAndOct", Test_Blood_HalfAndOct});
    tests.push_back({"Blood::ExrZipsRoundTrip", Test_Blood_ExrZipsRoundTrip});
    tests.push_back({"Blood::VatBuild", Test_Blood_VatBuild});
    tests.push_back({"Blood::FbxUVs", Test_Blood_FbxUVs});
    tests.push_back({"Blood::Presets", Test_Blood_Presets});
    tests.push_back({"Blood::SprayTiming", Test_Blood_SprayTiming});
    tests.push_back({"Blood::FleshHits", Test_Blood_FleshHits});
}
