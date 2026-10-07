#pragma once
#include <json.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Editor Enhancers - the shared style palette (vHierarchy / vFolders "Palette"): the swatches and
// quick-pick icons offered by every Style menu. Per-user (UserPaths "editor_palette.json"), with
// Export/Import so a team can pass one around; the *styles applied* to rows/folders are project
// data and live elsewhere - only the menu of choices is personal.
//
// Colours are packed exactly like ImGui's IM_COL32 (A<<24 | B<<16 | G<<8 | R) so a palette entry
// goes straight into an ImDrawList call, while this header stays ImGui-free for the tests. On
// disk they're "#RRGGBB" / "#RRGGBBAA" strings - readable and hand-editable.
namespace Enhancers {

constexpr std::uint32_t PackRGBA(unsigned r, unsigned g, unsigned b, unsigned a = 255) {
    return ((std::uint32_t)(a & 255) << 24) | ((std::uint32_t)(b & 255) << 16) |
           ((std::uint32_t)(g & 255) << 8) | (std::uint32_t)(r & 255);
}
// "#RRGGBB" (alpha 255) or "#RRGGBBAA"; the '#' is optional. False on anything else.
bool ParseHexColor(const std::string& s, std::uint32_t& out);
std::string ToHexColor(std::uint32_t c); // "#RRGGBB" when opaque, else "#RRGGBBAA"

struct Palette {
    std::vector<std::uint32_t> Colors;
    std::vector<std::string>   Icons; // Font Awesome names (see FAIconGlyph)

    static Palette Defaults();
    static Palette& Get();     // the live per-user palette

    nlohmann::json ToJson() const;
    // Lenient: bad entries are skipped; a missing/empty list falls back to that list's defaults.
    static Palette FromJson(const nlohmann::json& j);
    bool operator==(const Palette& o) const { return Colors == o.Colors && Icons == o.Icons; }

    // Per-user file. Load() leaves Defaults() on a missing/bad file.
    static void Load();
    static void MarkDirty();
    static void Flush();
    static std::string Path();

    bool ExportTo(const std::string& path) const;
    bool ImportFrom(const std::string& path); // replaces this palette on success

    static constexpr std::size_t kMaxColors = 32;
    static constexpr std::size_t kMaxIcons  = 64;
};

} // namespace Enhancers
