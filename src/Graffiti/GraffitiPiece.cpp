#include "GraffitiPiece.h"
#include "AtomicFile.h"
#include "Log.h"

#include <json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace Graffiti {
namespace {

// Type-checked reads over one JSON object, as DecalAsset's: a missing key or a wrong type keeps the
// default (a wrong type with a warning), so a hand-edited .graffiti never throws out of Load.
struct Reader {
    const json& obj;
    const std::string& path;

    const json* Find(const char* k) const {
        if (!obj.is_object()) return nullptr;
        auto it = obj.find(k);
        return it == obj.end() || it->is_null() ? nullptr : &*it;
    }
    void Warn(const char* k, const char* want) const {
        Log::Warn("Graffiti: '" + path + "': \"" + k + "\" should be " + want + " - using the default.",
                  LogContext::Asset(path));
    }
    Reader Sub(const char* k) const {
        const json* v = Find(k);
        static const json kEmpty = json::object();
        if (v && !v->is_object()) Warn(k, "an object");
        return {v && v->is_object() ? *v : kEmpty, path};
    }
    float Num(const char* k, float def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_number()) return v->get<float>();
        Warn(k, "a number"); return def;
    }
    int Int(const char* k, int def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_number()) return (int)v->get<double>();
        Warn(k, "a number"); return def;
    }
    bool Bool(const char* k, bool def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_boolean()) return v->get<bool>();
        if (v->is_number()) return v->get<double>() != 0.0;
        Warn(k, "true/false"); return def;
    }
    std::string Str(const char* k, const std::string& def = {}) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_string()) return v->get<std::string>();
        Warn(k, "a string"); return def;
    }
    glm::vec3 Color(const char* k, const glm::vec3& def) const {
        const json* v = Find(k);
        if (!v) return def;
        glm::vec3 c(0.0f);
        if (v->is_string() && FromHex(v->get<std::string>(), c)) return c;
        Warn(k, "a colour like \"#ff8800\""); return def;
    }
    glm::vec2 Vec2(const char* k, const glm::vec2& def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_array() && v->size() >= 2 && (*v)[0].is_number() && (*v)[1].is_number())
            return {(*v)[0].get<float>(), (*v)[1].get<float>()};
        Warn(k, "an array of 2 numbers"); return def;
    }
    // One of `names` (index = the enum value); an unknown name keeps the default.
    template <class E, size_t N> E Enum(const char* k, E def, const char* const (&names)[N]) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_string()) {
            const std::string s = v->get<std::string>();
            for (size_t i = 0; i < N; ++i)
                if (s == names[i]) return (E)i;
        }
        Warn(k, "one of the listed names"); return def;
    }
};

constexpr const char* kTypeNames[] = {"fill", "stamp", "text", "spray"};
constexpr const char* kBlendNames[] = {"normal", "multiply", "screen", "add", "erase"};
constexpr const char* kCapNames[] = {"fat", "skinny", "calligraphy"};
constexpr const char* kWarpNames[] = {"none", "arc", "wave"};

float Unit(float v) { return std::clamp(v, 0.0f, 1.0f); }

std::string StemName(const std::string& path) {
    std::string n = fs::u8path(path).stem().u8string();
    if (n.rfind("GP_", 0) == 0 && n.size() > 3) n = n.substr(3);
    return n;
}

// An indented dump puts every number of an array on its own line: a 300-sample stroke would take
// 2100 lines. Arrays holding only numbers go back onto one line ("[12, 340, 1000, 150, 16]"), so a
// sample is a line and a stroke's diff stays readable. Deterministic, like the dump itself.
std::string CompactNumberArrays(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '[') {
            size_t j = i + 1;
            bool numeric = false, ok = true;
            for (; j < in.size() && in[j] != ']'; ++j) {
                const char c = in[j];
                if ((c >= '0' && c <= '9') || c == '-') numeric = true;
                else if (!(c == '.' || c == 'e' || c == 'E' || c == '+' || c == ',' || c == ' ' || c == '\n')) {
                    ok = false;
                    break;
                }
            }
            if (ok && numeric && j < in.size()) {
                out += '[';
                bool gap = false;
                for (size_t k = i + 1; k < j; ++k) {
                    const char c = in[k];
                    if (c == ' ' || c == '\n') {
                        gap = true;
                        continue;
                    }
                    if (gap && out.back() == ',') out += ' ';
                    gap = false;
                    out += c;
                }
                out += ']';
                i = j;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

Stroke ReadStroke(const Reader& r) {
    Stroke s;
    s.Tip = r.Enum("cap", s.Tip, kCapNames);
    s.Color = r.Color("color", s.Color);
    s.Erase = r.Bool("erase", s.Erase);
    s.Flow = Unit(r.Num("flow", s.Flow));
    s.Angle = r.Num("angle", s.Angle);
    if (const json* samples = r.Find("samples")) {
        if (!samples->is_array()) {
            r.Warn("samples", "an array");
            return s;
        }
        s.Samples.reserve(samples->size());
        for (const json& e : *samples) {
            if (!e.is_array() || e.size() < 5 ||
                !std::all_of(e.begin(), e.begin() + 5, [](const json& n) { return n.is_number(); }))
                continue; // one bad sample drops only itself
            SpraySample p;
            p.X = (int32_t)e[0].get<double>();
            p.Y = (int32_t)e[1].get<double>();
            p.Pressure = (uint16_t)std::clamp((int)e[2].get<double>(), 0, 1000);
            p.DistanceMm = (uint16_t)std::clamp((int)e[3].get<double>(), 20, 2000);
            p.TimeMs = std::max(0, (int)e[4].get<double>());
            // Times never run backwards within a stroke (the brush model integrates over them).
            if (!s.Samples.empty()) p.TimeMs = std::max(p.TimeMs, s.Samples.back().TimeMs);
            s.Samples.push_back(p);
        }
    }
    return s;
}

Layer ReadLayer(const Reader& r) {
    Layer l;
    l.Type = r.Enum("type", l.Type, kTypeNames);
    l.Name = r.Str("name", LayerTypeName(l.Type));
    l.Visible = r.Bool("visible", l.Visible);
    l.Opacity = Unit(r.Num("opacity", l.Opacity));
    l.Mode = r.Enum("blend", l.Mode, kBlendNames);
    l.Clip = r.Bool("clip", l.Clip);
    l.Color = r.Color("color", l.Color);
    const Reader g = r.Sub("gradient");
    l.Gradient = g.Bool("on", l.Gradient);
    l.GradientTo = g.Color("to", l.GradientTo);
    l.GradientAngle = g.Num("angle", l.GradientAngle);
    const Reader o = r.Sub("outline");
    l.Outline = o.Bool("on", l.Outline);
    l.OutlineColor = o.Color("color", l.OutlineColor);
    l.OutlineWidth = std::clamp(o.Num("width", l.OutlineWidth), 0.0f, 0.1f);
    const Reader sh = r.Sub("shadow");
    l.Shadow = sh.Bool("on", l.Shadow);
    l.ShadowColor = sh.Color("color", l.ShadowColor);
    l.ShadowOffset = glm::clamp(sh.Vec2("offset", l.ShadowOffset), glm::vec2(-0.25f), glm::vec2(0.25f));
    l.ShadowBlur = std::clamp(sh.Num("blur", l.ShadowBlur), 0.0f, 0.1f);
    l.ShadowOpacity = Unit(sh.Num("opacity", l.ShadowOpacity));
    l.Metal = Unit(r.Num("metal", l.Metal));
    l.Glow = Unit(r.Num("glow", l.Glow));

    if (l.Type == LayerType::Stamp || l.Type == LayerType::Text) {
        l.Position = r.Vec2("position", l.Position);
        l.Size = std::clamp(r.Num("size", l.Size), 0.01f, 4.0f);
        l.Rotation = r.Num("rotation", l.Rotation);
    }
    if (l.Type == LayerType::Stamp) {
        l.Texture = r.Str("texture");
        l.Luma = r.Bool("luma", l.Luma);
        l.TextureColor = r.Bool("textureColor", l.TextureColor);
        l.Aspect = std::clamp(r.Num("aspect", l.Aspect), 0.05f, 20.0f);
    } else if (l.Type == LayerType::Text) {
        l.Text = r.Str("text");
        l.Alphabet = r.Str("alphabet", l.Alphabet);
        if (std::none_of(std::begin(kAlphabets), std::end(kAlphabets), [&](const char* a) { return l.Alphabet == a; })) {
            r.Warn("alphabet", "Tag, Brush, Bubble or Block");
            l.Alphabet = "Tag";
        }
        l.Kerning = std::clamp(r.Num("kerning", l.Kerning), -0.5f, 1.0f);
        l.Jitter = Unit(r.Num("jitter", l.Jitter));
        l.WarpMode = r.Enum("warp", l.WarpMode, kWarpNames);
        l.WarpAmount = Unit(r.Num("warpAmount", l.WarpAmount));
        if (const json* letters = r.Find("letters"); letters && letters->is_array()) {
            for (const json& e : *letters) {
                const Reader lr{e, r.path};
                LetterStyle s;
                s.Index = std::max(0, lr.Int("index", 0));
                s.HasFill = lr.Find("color") != nullptr;
                s.Fill = lr.Color("color", s.Fill);
                s.HasOutline = lr.Find("outline") != nullptr;
                s.Outline = lr.Color("outline", s.Outline);
                if (s.HasFill || s.HasOutline) l.Letters.push_back(s);
            }
            std::stable_sort(l.Letters.begin(), l.Letters.end(),
                             [](const LetterStyle& a, const LetterStyle& b) { return a.Index < b.Index; });
        }
    } else if (l.Type == LayerType::Spray) {
        if (const json* strokes = r.Find("strokes"); strokes && strokes->is_array()) {
            l.Strokes.reserve(strokes->size());
            for (const json& e : *strokes) {
                Stroke s = ReadStroke(Reader{e, r.path});
                if (!s.Samples.empty()) l.Strokes.push_back(std::move(s));
            }
        }
    }
    return l;
}

} // namespace

const char* LayerTypeName(LayerType t) { return kTypeNames[std::min((int)t, 3)]; }
const char* BlendName(Blend b) { return kBlendNames[std::min((int)b, 4)]; }
const char* CapName(Cap c) { return kCapNames[std::min((int)c, 2)]; }
const char* WarpName(Warp w) { return kWarpNames[std::min((int)w, 2)]; }

glm::vec3 Quantise(const glm::vec3& srgb) {
    return glm::round(glm::clamp(srgb, glm::vec3(0.0f), glm::vec3(1.0f)) * 255.0f) / 255.0f;
}

std::string ToHex(const glm::vec3& srgb) {
    const glm::ivec3 c(glm::round(glm::clamp(srgb, glm::vec3(0.0f), glm::vec3(1.0f)) * 255.0f));
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

bool FromHex(const std::string& hex, glm::vec3& out) {
    const size_t at = hex.size() == 7 && hex[0] == '#' ? 1 : hex.size() == 6 ? 0 : std::string::npos;
    if (at == std::string::npos) return false;
    unsigned v = 0;
    for (size_t i = at; i < hex.size(); ++i) {
        const char c = hex[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | (unsigned)d;
    }
    out = glm::vec3((float)(v >> 16 & 0xFF), (float)(v >> 8 & 0xFF), (float)(v & 0xFF)) / 255.0f;
    return true;
}

SpraySample SpraySample::Make(glm::vec2 uv, float pressure, float distanceMetres, float timeSeconds) {
    SpraySample s;
    s.X = (int32_t)std::lround(std::clamp(uv.x, -1.0f, 2.0f) * kPosUnits);
    s.Y = (int32_t)std::lround(std::clamp(uv.y, -1.0f, 2.0f) * kPosUnits);
    s.Pressure = (uint16_t)std::lround(std::clamp(pressure, 0.0f, 1.0f) * 1000.0f);
    s.DistanceMm = (uint16_t)std::lround(std::clamp(distanceMetres, 0.02f, 2.0f) * 1000.0f);
    s.TimeMs = (int32_t)std::lround(std::max(timeSeconds, 0.0f) * 1000.0f);
    return s;
}

int Piece::SnapSide(int s) {
    static constexpr int kSides[] = {512, 1024, 2048};
    int best = kSides[0];
    for (int v : kSides)
        if (std::abs(v - s) < std::abs(best - s)) best = v;
    return best;
}

std::shared_ptr<Piece> Piece::Load(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f.is_open()) {
        auto p = std::make_shared<Piece>();
        p->Path = path;
        p->Name = StemName(path);
        p->Missing = true;
        return p;
    }
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return FromString(bytes, path);
}

std::shared_ptr<Piece> Piece::FromString(const std::string& text, const std::string& path) {
    auto p = std::make_shared<Piece>();
    p->Path = path;
    p->Name = StemName(path);
    json j;
    try { j = json::parse(text); } catch (const std::exception& e) {
        Log::Error("Graffiti: JSON parse error in '" + path + "': " + e.what(), LogContext::Asset(path));
        p->Missing = true;
        return p;
    }
    if (!j.is_object()) {
        Log::Error("Graffiti: '" + path + "' is not a JSON object.", LogContext::Asset(path));
        p->Missing = true;
        return p;
    }
    const Reader r{j, path};
    if (r.Int("graffitiVersion", 1) > 1)
        Log::Warn("Graffiti: '" + path + "' is from a newer engine (graffitiVersion > 1); unknown keys are ignored.",
                  LogContext::Asset(path));

    const Reader canvas = r.Sub("canvas");
    p->Width = SnapSide(canvas.Int("width", p->Width));
    p->Height = SnapSide(canvas.Int("height", p->Height));
    p->Metres = glm::clamp(canvas.Vec2("metres", p->Metres), glm::vec2(0.05f), glm::vec2(20.0f));
    p->Seed = (uint32_t)std::max(0, r.Int("seed", (int)p->Seed));

    const Reader paint = r.Sub("paint");
    p->Roughness = Unit(paint.Num("roughness", p->Roughness));
    p->Relief = Unit(paint.Num("relief", p->Relief));
    p->ShowThrough = Unit(paint.Num("showThrough", p->ShowThrough));
    p->Drips = paint.Bool("drips", p->Drips);
    p->GlowColor = paint.Color("glow", p->GlowColor);
    p->GlowStrength = std::clamp(paint.Num("glowStrength", p->GlowStrength), 0.0f, 16.0f);
    p->UV = paint.Bool("uv", p->UV);
    const Reader wx = r.Sub("weathering");
    p->Fade = Unit(wx.Num("fade", p->Fade));
    p->Bleach = Unit(wx.Num("bleach", p->Bleach));
    p->Peel = Unit(wx.Num("peel", p->Peel));
    p->Buff = Unit(wx.Num("buff", p->Buff));

    if (const json* layers = r.Find("layers")) {
        if (layers->is_array()) {
            p->Layers.reserve(layers->size());
            for (const json& e : *layers) {
                if (!e.is_object()) continue;
                p->Layers.push_back(ReadLayer(Reader{e, path}));
            }
        } else {
            r.Warn("layers", "an array");
        }
    }
    return p;
}

std::string Piece::ToString() const {
    // ordered_json keeps the schema's key order (GraffitiPiece.h): every save of the same piece is
    // byte-identical, and recipes diff cleanly in git.
    using ojson = nlohmann::ordered_json;
    ojson j;
    j["graffitiVersion"] = 1;
    j["canvas"] = {{"width", Width}, {"height", Height}, {"metres", {Metres.x, Metres.y}}};
    j["seed"] = Seed;
    j["paint"] = {{"roughness", Roughness}, {"relief", Relief}, {"showThrough", ShowThrough}, {"drips", Drips},
                  {"glow", ToHex(GlowColor)}, {"glowStrength", GlowStrength}, {"uv", UV}};
    j["weathering"] = {{"fade", Fade}, {"bleach", Bleach}, {"peel", Peel}, {"buff", Buff}};
    ojson layers = ojson::array();
    for (const Layer& l : Layers) {
        ojson o;
        o["type"] = LayerTypeName(l.Type);
        o["name"] = l.Name;
        o["visible"] = l.Visible;
        o["opacity"] = l.Opacity;
        o["blend"] = BlendName(l.Mode);
        o["clip"] = l.Clip;
        o["color"] = ToHex(l.Color);
        o["gradient"] = {{"on", l.Gradient}, {"to", ToHex(l.GradientTo)}, {"angle", l.GradientAngle}};
        o["outline"] = {{"on", l.Outline}, {"color", ToHex(l.OutlineColor)}, {"width", l.OutlineWidth}};
        o["shadow"] = {{"on", l.Shadow},
                       {"color", ToHex(l.ShadowColor)},
                       {"offset", {l.ShadowOffset.x, l.ShadowOffset.y}},
                       {"blur", l.ShadowBlur},
                       {"opacity", l.ShadowOpacity}};
        o["metal"] = l.Metal;
        o["glow"] = l.Glow;
        if (l.Type == LayerType::Stamp || l.Type == LayerType::Text) {
            o["position"] = {l.Position.x, l.Position.y};
            o["size"] = l.Size;
            o["rotation"] = l.Rotation;
        }
        if (l.Type == LayerType::Stamp) {
            o["texture"] = l.Texture;
            o["luma"] = l.Luma;
            o["textureColor"] = l.TextureColor;
            o["aspect"] = l.Aspect;
        } else if (l.Type == LayerType::Text) {
            o["text"] = l.Text;
            o["alphabet"] = l.Alphabet;
            o["kerning"] = l.Kerning;
            o["jitter"] = l.Jitter;
            o["warp"] = WarpName(l.WarpMode);
            o["warpAmount"] = l.WarpAmount;
            ojson letters = ojson::array();
            for (const LetterStyle& s : l.Letters) {
                ojson e;
                e["index"] = s.Index;
                if (s.HasFill) e["color"] = ToHex(s.Fill);
                if (s.HasOutline) e["outline"] = ToHex(s.Outline);
                letters.push_back(std::move(e));
            }
            o["letters"] = std::move(letters);
        } else if (l.Type == LayerType::Spray) {
            ojson strokes = ojson::array();
            for (const Stroke& s : l.Strokes) {
                ojson e;
                e["cap"] = CapName(s.Tip);
                e["color"] = ToHex(s.Color);
                e["erase"] = s.Erase;
                e["flow"] = s.Flow;
                e["angle"] = s.Angle;
                ojson samples = ojson::array();
                for (const SpraySample& p : s.Samples)
                    samples.push_back({p.X, p.Y, (int)p.Pressure, (int)p.DistanceMm, p.TimeMs});
                e["samples"] = std::move(samples);
                strokes.push_back(std::move(e));
            }
            o["strokes"] = std::move(strokes);
        }
        layers.push_back(std::move(o));
    }
    j["layers"] = std::move(layers);
    return CompactNumberArrays(j.dump(2)) + "\n";
}

bool Piece::Save() const { return AtomicFile::WriteBytes(fs::u8path(Path), ToString(), /*binary=*/true); }

std::string Piece::BakeDir() const {
    return (fs::u8path(Path).parent_path() / fs::u8path(Name)).u8string();
}
std::string Piece::MapPath(const char* map) const {
    return (fs::u8path(BakeDir()) / fs::u8path("T_Decal_Piece_" + Name + "_" + map + ".png")).u8string();
}
std::string Piece::DecalPath() const {
    return (fs::u8path(Path).parent_path() / fs::u8path("DA_Piece_" + Name + ".decal")).u8string();
}

} // namespace Graffiti
