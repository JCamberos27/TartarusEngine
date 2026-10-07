#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Editor Enhancers - the two pickers every Style menu shares (Hierarchy rows, folders, favorites):
// a palette swatch row and a searchable Font Awesome icon grid. Host-side ImGui widgets drawn
// inside a menu/popup the caller already opened; they hold no state beyond the search text the
// caller owns.
namespace Enhancers {

// One row of palette swatches (Palette::Get().Colors), wrapping to the available width, plus a
// trailing "custom" swatch that opens a colour picker. `color` 0 means "none"; the current colour
// is outlined. Returns true on the frame the user picks (or edits the custom colour).
bool PaletteColorRow(const char* id, std::uint32_t& color);

// Searchable icon grid over every Font Awesome icon (EnhancerCore FAIconTable), the palette's
// quick-pick icons first while the search is empty. `name` is the selected icon's FA name
// ("" = none). `search`/`searchSize` is caller-owned so the query survives the popup closing.
// `gridHeight` <= 0 picks a default. Draws one virtualized grid (ImGuiListClipper), so the
// 1400-icon table costs only the visible rows. Returns true on the frame an icon is picked.
bool IconPickerGrid(const char* id, std::string& name, char* search, std::size_t searchSize, float gridHeight = 0.0f);

} // namespace Enhancers
