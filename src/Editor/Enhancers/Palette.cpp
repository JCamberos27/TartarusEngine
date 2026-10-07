#include "Palette.h"
#include "EnhancerCore.h"

#include "AtomicFile.h"
#include "Log.h"
#include "UserPaths.h"

#include <cctype>
#include <cstdio>
#include <fstream>

using json = nlohmann::json;

namespace Enhancers {

namespace {
bool g_PaletteDirty = false;

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)std::tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}
} // namespace

bool ParseHexColor(const std::string& s, std::uint32_t& out) {
    std::size_t i = (!s.empty() && s[0] == '#') ? 1 : 0;
    const std::size_t n = s.size() - i;
    if (n != 6 && n != 8) return false;
    unsigned v[4] = {0, 0, 0, 255};
    for (std::size_t k = 0; k < n / 2; ++k) {
        const int hi = HexNibble(s[i + k * 2]), lo = HexNibble(s[i + k * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        v[k] = (unsigned)(hi * 16 + lo);
    }
    out = PackRGBA(v[0], v[1], v[2], v[3]);
    return true;
}

std::string ToHexColor(std::uint32_t c) {
    const unsigned r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = (c >> 24) & 255;
    char buf[10];
    if (a == 255) std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
    else          std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", r, g, b, a);
    return buf;
}

Palette Palette::Defaults() {
    // Ten hues tuned to stay readable as a ~25% row wash and as a full-strength glyph tint on the
    // editor's near-black "phosphor" panels (EditorTheme::Panel), plus two neutrals. Saturation
    // is held back on purpose - the theme is monochrome, so the palette is the only colour on
    // screen and fully saturated primaries read as alarms.
    Palette p;
    p.Colors = {
        PackRGBA(0xE5, 0x48, 0x4D), // red
        PackRGBA(0xF2, 0x99, 0x4A), // orange
        PackRGBA(0xF2, 0xC9, 0x4C), // yellow
        PackRGBA(0x8B, 0xC3, 0x4A), // lime
        PackRGBA(0x4C, 0xC3, 0x8A), // green
        PackRGBA(0x3F, 0xB8, 0xAF), // teal
        PackRGBA(0x4C, 0x8D, 0xF6), // blue
        PackRGBA(0x6E, 0x6C, 0xF2), // indigo
        PackRGBA(0x9B, 0x6C, 0xF2), // purple
        PackRGBA(0xE2, 0x6C, 0xB2), // pink
        PackRGBA(0x8A, 0x8F, 0x98), // grey
        PackRGBA(0xE6, 0xE6, 0xE6), // white
    };
    p.Icons = {
        "folder", "cube", "cubes", "camera", "lightbulb", "sun", "person", "user-group", "gamepad",
        "music", "volume-high", "star", "flag", "bolt", "fire", "tree", "mountain", "road", "house",
        "door-open", "box", "shield", "gear", "code", "palette", "image", "film", "ghost", "skull",
        "crosshairs", "map", "car", "heart", "diamond", "circle", "square", "bookmark", "tag",
        "wand-magic-sparkles", "puzzle-piece",
    };
    return p;
}

Palette& Palette::Get() {
    static Palette p = Defaults();
    return p;
}

json Palette::ToJson() const {
    json j;
    j["version"] = 1;
    json cols = json::array();
    for (auto c : Colors) cols.push_back(ToHexColor(c));
    j["colors"] = cols;
    j["icons"] = Icons;
    return j;
}

Palette Palette::FromJson(const json& j) {
    const Palette defaults = Defaults();
    Palette p;
    if (j.is_object()) {
        if (auto it = j.find("colors"); it != j.end() && it->is_array())
            for (const auto& c : *it) {
                std::uint32_t v;
                if (c.is_string() && ParseHexColor(c.get<std::string>(), v) && p.Colors.size() < kMaxColors) p.Colors.push_back(v);
            }
        if (auto it = j.find("icons"); it != j.end() && it->is_array())
            for (const auto& ic : *it) {
                // Unknown names (an FA rename, a typo) are dropped rather than kept as blanks.
                if (ic.is_string() && FAIconGlyph(ic.get<std::string>().c_str()) && p.Icons.size() < kMaxIcons)
                    p.Icons.push_back(ic.get<std::string>());
            }
    }
    if (p.Colors.empty()) p.Colors = defaults.Colors;
    if (p.Icons.empty()) p.Icons = defaults.Icons;
    return p;
}

std::string Palette::Path() { return UserPaths::Resolve("editor_palette.json"); }

void Palette::Load() {
    std::ifstream in(Path());
    if (!in.is_open()) { Get() = Defaults(); return; }
    try {
        json j;
        in >> j;
        Get() = FromJson(j);
    } catch (const std::exception& e) {
        Log::Warn(std::string("Editor Enhancers: failed to parse '") + Path() + "': " + e.what() + " - using the default palette.");
        Get() = Defaults();
    }
}

void Palette::MarkDirty() { g_PaletteDirty = true; }

void Palette::Flush() {
    if (!g_PaletteDirty) return;
    g_PaletteDirty = false;
    if (!AtomicFile::WriteJson(Path(), Get().ToJson()))
        Log::Warn(std::string("Editor Enhancers: could not write '") + Path() + "'.");
}

bool Palette::ExportTo(const std::string& path) const {
    return !path.empty() && AtomicFile::WriteJson(path, ToJson());
}

bool Palette::ImportFrom(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    try {
        json j;
        in >> j;
        // An object with neither list isn't a palette file - refuse it rather than silently
        // "importing" the defaults over the user's own palette.
        if (!j.is_object() || (!j.contains("colors") && !j.contains("icons"))) return false;
        *this = FromJson(j);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace Enhancers
