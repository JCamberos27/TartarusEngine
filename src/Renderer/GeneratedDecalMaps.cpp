#include "GeneratedDecalMaps.h"

#include "AtomicFile.h"
#include "Log.h"
#include "ProjectPaths.h"

#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

GeneratedDecalMaps& GeneratedDecalMaps::Get() {
    static GeneratedDecalMaps* g = new GeneratedDecalMaps();
    return *g;
}

std::string GeneratedDecalMaps::Dir() { return ProjectPaths::Resolve("Library/DecalMaps"); }

std::string GeneratedDecalMaps::MapPath(const std::string& key, const char* map) {
    return (fs::u8path(Dir()) / (key + "_" + map + ".png")).u8string();
}

namespace {
bool WritePng(const std::string& path, int w, int h, int comp, const std::vector<uint8_t>& px) {
    std::string bytes;
    stbi_write_png_to_func([](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append((const char*)data, (size_t)size); },
                           &bytes, w, h, comp, px.data(), w * comp);
    return !bytes.empty() && AtomicFile::WriteBytes(fs::u8path(path), bytes, true);
}
bool Exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(fs::u8path(p), ec);
}
} // namespace

std::string GeneratedDecalMaps::DonePath(const std::string& key) { return (fs::u8path(Dir()) / (key + ".done")).u8string(); }

bool GeneratedDecalMaps::Write(const std::string& key, const Images& im) {
    const size_t px = (size_t)im.Width * (size_t)im.Height;
    const bool albedo = im.Albedo.size() == px * 4, aux = im.Aux.size() == px;
    if (im.Width <= 0 || im.Height <= 0 || (!albedo && !aux)) return false;
    if (albedo && !WritePng(MapPath(key, "Albedo"), im.Width, im.Height, 4, im.Albedo)) return false;
    if (im.Normal.size() == px * 4 && !WritePng(MapPath(key, "Normal"), im.Width, im.Height, 4, im.Normal)) return false;
    if (aux && !WritePng(MapPath(key, "Aux"), im.Width, im.Height, 1, im.Aux)) return false;
    // Last: Find takes this marker as "the set is complete" (a set cut short by a crash is made again).
    return AtomicFile::WriteBytes(fs::u8path(DonePath(key)), "1", true);
}

const DecalTextureArrays::Source* GeneratedDecalMaps::Find(const std::string& key, int tier, const Prepare& prepare) {
    auto it = m_Entries.find(key);
    if (it == m_Entries.end()) {
        auto e = std::make_unique<Entry>();
        e->Src.Tier = tier;
        auto fill = [key](DecalTextureArrays::Source& s) {
            s.Albedo = Exists(MapPath(key, "Albedo")) ? MapPath(key, "Albedo") : std::string();
            s.Normal = Exists(MapPath(key, "Normal")) ? MapPath(key, "Normal") : std::string();
            s.Aux = Exists(MapPath(key, "Aux")) ? MapPath(key, "Aux") : std::string();
        };
        if (Exists(DonePath(key))) {
            fill(e->Src);
            e->Ready = true;
        } else if (prepare) {
            Job job = prepare();
            if (!job) {
                e->Failed = true;
            } else {
                e->Pending = std::async(std::launch::async, [key, job]() { return Write(key, job()); });
            }
        } else {
            e->Failed = true;
        }
        it = m_Entries.emplace(key, std::move(e)).first;
    }
    Entry& e = *it->second;
    if (!e.Ready && !e.Failed && e.Pending.valid() && e.Pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        bool ok = false;
        try { ok = e.Pending.get(); } catch (...) {}
        if (ok) {
            e.Src.Albedo = Exists(MapPath(key, "Albedo")) ? MapPath(key, "Albedo") : std::string();
            e.Src.Normal = Exists(MapPath(key, "Normal")) ? MapPath(key, "Normal") : std::string();
            e.Src.Aux = Exists(MapPath(key, "Aux")) ? MapPath(key, "Aux") : std::string();
            e.Ready = true;
        } else {
            e.Failed = true;
            Log::Warn("Decals: could not make the runtime decal maps '" + key + "' in " + Dir());
        }
    }
    // Sealed (its slot key hashed) once ready, and again only if a caller asks for another tier.
    if (e.Ready && (e.Src.Tier != tier || e.Src.Key == 0)) {
        e.Src.Tier = tier;
        e.Src.Seal();
    }
    e.Src.Tier = tier;
    return e.Ready ? &e.Src : nullptr;
}

void GeneratedDecalMaps::Flush() {
    for (auto& [key, e] : m_Entries)
        if (e->Pending.valid()) e->Pending.wait();
}

// --- the image makers ----------------------------------------------------------------------------

namespace DecalBake {
namespace {
float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float Mix(float a, float b, float t) { return a + (b - a) * t; }
uint8_t U8(float v) { return (uint8_t)std::lround(Clamp01(v) * 255.0f); }
void PutNormal(std::vector<uint8_t>& out, size_t i, uint8_t r, uint8_t g) {
    const float x = r / 127.5f - 1.0f, y = g / 127.5f - 1.0f;
    out[i * 4 + 0] = r;
    out[i * 4 + 1] = g;
    out[i * 4 + 2] = U8(std::sqrt(std::max(1.0f - x * x - y * y, 0.0f)) * 0.5f + 0.5f);
    out[i * 4 + 3] = 255;
}
} // namespace

uint8_t LinearToSrgb8(float l) {
    l = Clamp01(l);
    const float s = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    return U8(s);
}

float Srgb8ToLinear(uint8_t v) {
    const float s = v / 255.0f;
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

GeneratedDecalMaps::Images BloodSet(const uint8_t* norm, const uint8_t* mask, int w, int h) {
    GeneratedDecalMaps::Images im;
    im.Width = w;
    im.Height = h;
    const size_t n = (size_t)w * (size_t)h;
    im.Albedo.resize(n * 4);
    im.Normal.resize(n * 4);
    im.Aux.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const float coverage = Clamp01(norm[i * 4 + 3] / 255.0f * 2.0f);
        const float reveal = mask[i * 4 + 0] / 255.0f, thick = mask[i * 4 + 2] / 255.0f;
        // At no cutout: the pooled core, as the old blood decal pass shaded it.
        const float core = Clamp01(reveal * 5.0f) * coverage * thick;
        const uint8_t shade = LinearToSrgb8(Mix(1.0f, 0.6f, core));
        im.Albedo[i * 4 + 0] = im.Albedo[i * 4 + 1] = im.Albedo[i * 4 + 2] = shade;
        // The pack's own alpha, not the coverage: the x2 gain is applied after filtering (RuntimeDecal::CoverGain),
        // as the old pass did, or thin streaks thin out further at every mip.
        im.Albedo[i * 4 + 3] = norm[i * 4 + 3];
        PutNormal(im.Normal, i, norm[i * 4 + 0], norm[i * 4 + 1]);
        im.Aux[i] = mask[i * 4 + 0];
    }
    return im;
}

GeneratedDecalMaps::Images KnifeLayer(const uint8_t* rgba, const uint8_t* rg, int size, int cols, int rows, KnifeMode mode) {
    GeneratedDecalMaps::Images im;
    im.Width = im.Height = size;
    const size_t n = (size_t)size * (size_t)size;
    im.Albedo.resize(n * 4);
    im.Aux.resize(n);
    if (rg) im.Normal.resize(n * 4);
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);
    const float cw = (float)size / (float)cols, chh = (float)size / (float)rows;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const size_t i = (size_t)y * size + x;
            // Distance out from the cell's centre: 0 there, 1 at the middle of its edge.
            const float tx = std::fmod((float)x + 0.5f, cw) / cw, ty = std::fmod((float)y + 0.5f, chh) / chh;
            const float r = std::sqrt((tx - 0.5f) * (tx - 0.5f) + (ty - 0.5f) * (ty - 0.5f)) * 2.0f;
            const float a = rgba[i * 4 + 3] / 255.0f;
            const float alpha = Clamp01(a * 6.0f);
            if (mode == KnifeMode::Hole) {
                im.Albedo[i * 4 + 0] = rgba[i * 4 + 0];
                im.Albedo[i * 4 + 1] = rgba[i * 4 + 1];
                im.Albedo[i * 4 + 2] = rgba[i * 4 + 2];
                im.Albedo[i * 4 + 3] = U8(alpha);
                im.Aux[i] = U8(HoleAux(r));
            } else {
                float shade;
                if (mode == KnifeMode::BloodAlbedo) {
                    // Only how light or dark the pack's stain is; the hue is the palette's (the tint).
                    const float v = Clamp01(std::max({Srgb8ToLinear(rgba[i * 4 + 0]), Srgb8ToLinear(rgba[i * 4 + 1]),
                                                      Srgb8ToLinear(rgba[i * 4 + 2])}) / 0.25f);
                    shade = Mix(0.8f, 1.0f, v) * 0.65f;
                } else {
                    shade = Mix(1.0f, 0.6f, Clamp01((a - 0.6f) * 2.5f) * alpha); // the thick core darker
                }
                const uint8_t s = LinearToSrgb8(shade);
                im.Albedo[i * 4 + 0] = im.Albedo[i * 4 + 1] = im.Albedo[i * 4 + 2] = s;
                im.Albedo[i * 4 + 3] = rgba[i * 4 + 3]; // the raw alpha: its x6 gain comes after filtering (CoverGain)
                // The cutout eats the thin, outer coverage first: reveal = a - cutout (0.6 + 0.6 r) > 0.
                im.Aux[i] = U8(a / (0.6f + 0.6f * r));
            }
            if (rg) PutNormal(im.Normal, i, rg[i * 2 + 0], rg[i * 2 + 1]);
        }
    }
    return im;
}

GeneratedDecalMaps::Images ProceduralHole(int cell) {
    GeneratedDecalMaps::Images im;
    im.Width = im.Height = cell * 2;
    const size_t n = (size_t)im.Width * im.Height;
    im.Albedo.resize(n * 4);
    im.Normal.resize(n * 4);
    for (int c = 0; c < 4; ++c) {
        const float seed = (0.1f + 0.25f * (float)c) * 40.0f;
        for (int y = 0; y < cell; ++y) {
            for (int x = 0; x < cell; ++x) {
                const size_t i = (size_t)(y + (c / 2) * cell) * im.Width + (x + (c % 2) * cell);
                // uv 1 = the cell's edge = 2.5 hole radii (WeaponFx.frag.glsl's hole).
                const float u = ((float)x + 0.5f) / cell * 2.0f - 1.0f, v = ((float)y + 0.5f) / cell * 2.0f - 1.0f;
                const float r = std::sqrt(u * u + v * v) * 2.5f, ang = std::atan2(v, u);
                const float ragged = 1.0f + 0.10f * std::sin(ang * 5.0f + seed) + 0.06f * std::sin(ang * 11.0f + seed * 2.3f);
                auto smooth = [](float e0, float e1, float t) {
                    t = Clamp01((t - e0) / (e1 - e0));
                    return t * t * (3.0f - 2.0f * t);
                };
                const float bore = 1.0f - smooth(0.82f, 1.0f, r / ragged);
                const float ring = 1.0f - smooth(1.0f, 2.5f, r / (0.9f + 0.1f * ragged));
                const float a = std::max(bore, ring * ring * 0.55f);
                const uint8_t col = LinearToSrgb8(0.015f * (1.0f - bore * 0.9f));
                im.Albedo[i * 4 + 0] = im.Albedo[i * 4 + 1] = im.Albedo[i * 4 + 2] = col;
                im.Albedo[i * 4 + 3] = U8(a);
                // A lip round the bore, sloping down into it.
                const float len = std::max(std::sqrt(u * u + v * v), 1e-4f);
                const float k = 0.7f * std::exp(-(r - 1.0f) * (r - 1.0f) / 0.08f);
                const float nx = -u / len * k, ny = v / len * k; // image y down, tangent y up
                const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + 1.0f);
                PutNormal(im.Normal, i, U8(nx * inv * 0.5f + 0.5f), U8(ny * inv * 0.5f + 0.5f));
            }
        }
    }
    return im;
}
} // namespace DecalBake
