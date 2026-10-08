#include "StyleWidgets.h"

#include "EnhancerCore.h"
#include "Palette.h"

#include "EditorTheme.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace Enhancers {

namespace {
void Tip(const char* s) { EditorUI::SetTooltip("%s", s); }

// Case-insensitive substring match where ' ' in the query also matches the '-' FA uses between
// words, so "door open" finds "door-open". `q` is already lower-cased.
bool NameMatches(const char* name, const char* q) {
    if (!*q) return true;
    const std::size_t qn = std::strlen(q);
    for (const char* s = name; *s; ++s) {
        std::size_t k = 0;
        for (; k < qn && s[k]; ++k) {
            const char a = (char)std::tolower((unsigned char)s[k]);
            const char b = q[k] == ' ' ? '-' : q[k];
            if (a != b) break;
        }
        if (k == qn) return true;
    }
    return false;
}

// Swatch outline: the accent for "current", a hairline otherwise so a near-black colour still
// reads as a swatch against the panel.
void DrawSwatch(ImDrawList* dl, ImVec2 mn, ImVec2 mx, std::uint32_t col, bool selected, bool hovered) {
    const float r = EditorTheme::Px(3.0f);
    if (col) {
        dl->AddRectFilled(mn, mx, col | 0xFF000000u, r);
    } else {
        // "None": an empty box with a diagonal slash.
        dl->AddRect(mn, mx, EditorTheme::U32(EditorTheme::Dim), r);
        dl->AddLine(ImVec2(mn.x + 3, mx.y - 3), ImVec2(mx.x - 3, mn.y + 3), EditorTheme::U32(EditorTheme::Danger), 1.5f);
    }
    if (selected)
        dl->AddRect(ImVec2(mn.x - 2, mn.y - 2), ImVec2(mx.x + 2, mx.y + 2), EditorTheme::U32(EditorTheme::AccentBright), r + 1, 0, 2.0f);
    else if (hovered)
        dl->AddRect(ImVec2(mn.x - 1, mn.y - 1), ImVec2(mx.x + 1, mx.y + 1), EditorTheme::U32(EditorTheme::Text), r, 0, 1.0f);
}
} // namespace

bool PaletteColorRow(const char* id, std::uint32_t& color) {
    ImGui::PushID(id);
    const float sw = EditorTheme::Px(18.0f);
    const float gap = EditorTheme::Px(6.0f);
    const float rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool picked = false;
    bool first = true;

    auto swatch = [&](int idx, std::uint32_t col, const char* tip) {
        if (!first) {
            ImGui::SameLine(0.0f, gap);
            if (ImGui::GetCursorScreenPos().x + sw > rightEdge) ImGui::NewLine();
        }
        first = false;
        ImGui::PushID(idx);
        const bool clicked = ImGui::InvisibleButton("##sw", ImVec2(sw, sw));
        const bool hov = ImGui::IsItemHovered();
        DrawSwatch(dl, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), col, (color | 0xFF000000u) == (col | 0xFF000000u) && (col != 0) == (color != 0), hov);
        if (hov) Tip(tip);
        ImGui::PopID();
        return clicked;
    };

    if (swatch(-1, 0, "None")) { color = 0; picked = true; }
    const auto& cols = Palette::Get().Colors;
    for (int i = 0; i < (int)cols.size(); ++i) {
        const std::string hex = ToHexColor(cols[(std::size_t)i]);
        if (swatch(i, cols[(std::size_t)i], hex.c_str())) { color = cols[(std::size_t)i]; picked = true; }
    }

    // Custom colour: a rainbow-ish chip opening a full picker for anything off-palette.
    ImGui::SameLine(0.0f, gap);
    if (ImGui::GetCursorScreenPos().x + sw > rightEdge) ImGui::NewLine();
    if (ImGui::InvisibleButton("##custom", ImVec2(sw, sw))) ImGui::OpenPopup("##customColor");
    {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        dl->AddRectFilledMultiColor(mn, mx, IM_COL32(229, 72, 77, 255), IM_COL32(242, 201, 76, 255),
                                    IM_COL32(76, 141, 246, 255), IM_COL32(155, 108, 242, 255));
        if (ImGui::IsItemHovered()) {
            dl->AddRect(ImVec2(mn.x - 1, mn.y - 1), ImVec2(mx.x + 1, mx.y + 1), EditorTheme::U32(EditorTheme::Text), EditorTheme::Px(3.0f));
            Tip("Custom colour...");
        }
    }
    if (ImGui::BeginPopup("##customColor")) {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(color ? color : Palette::Get().Colors.front());
        if (ImGui::ColorPicker4("##picker", &c.x, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoSidePreview)) {
            color = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 1.0f));
            picked = true;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return picked;
}

bool IconPickerGrid(const char* id, std::string& name, char* search, std::size_t searchSize, float gridHeight) {
    ImGui::PushID(id);
    EditorUIPrimitives::SearchField("##iconSearch", search, searchSize, "Search icons");

    // Filter into a reused index list (no per-frame allocation once warmed). Indices >= 0 are FA
    // table rows; the palette's quick-picks are listed first (and again in the full list - a
    // duplicate costs nothing and keeps the "all icons" order stable).
    static std::vector<int> s_Rows;
    s_Rows.clear();
    std::size_t count = 0;
    const FAIcon* table = FAIconTable(&count);
    char q[64] = {};
    for (std::size_t i = 0; i + 1 < sizeof(q) && search[i]; ++i) q[i] = (char)std::tolower((unsigned char)search[i]);
    if (!q[0]) {
        for (const auto& n : Palette::Get().Icons) {
            const FAIcon* b = table;
            const FAIcon* e = table + count;
            const FAIcon* it = std::lower_bound(b, e, n.c_str(), [](const FAIcon& a, const char* s) { return std::strcmp(a.Name, s) < 0; });
            if (it != e && n == it->Name) s_Rows.push_back((int)(it - b));
        }
    }
    for (std::size_t i = 0; i < count; ++i)
        if (NameMatches(table[i].Name, q)) s_Rows.push_back((int)i);

    const float cell = EditorTheme::Px(30.0f);
    const float h = gridHeight > 0.0f ? gridHeight : EditorTheme::Px(260.0f);
    bool picked = false;

    // "No icon" sits above the grid rather than in it, so it never scrolls away.
    {
        const bool none = name.empty();
        if (EditorUIPrimitives::ActionButton(ICON_FA_BAN "  No icon", "Clear the custom icon", &Tip, none)) {
            name.clear();
            picked = true;
        }
    }

    if (ImGui::BeginChild("##iconGrid", ImVec2(0.0f, h), ImGuiChildFlags_Borders)) {
        const float avail = ImGui::GetContentRegionAvail().x;
        const int cols = std::max(1, (int)(avail / cell));
        const int rows = ((int)s_Rows.size() + cols - 1) / cols;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGuiListClipper clip;
        clip.Begin(rows, cell);
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                for (int c = 0; c < cols; ++c) {
                    const int k = r * cols + c;
                    if (k >= (int)s_Rows.size()) break;
                    const FAIcon& ic = table[(std::size_t)s_Rows[(std::size_t)k]];
                    if (c > 0) ImGui::SameLine();
                    ImGui::PushID(k);
                    if (ImGui::InvisibleButton("##ic", ImVec2(cell, cell))) { name = ic.Name; picked = true; }
                    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                    const bool hov = ImGui::IsItemHovered();
                    const bool sel = name == ic.Name;
                    if (sel || hov)
                        dl->AddRectFilled(mn, mx, EditorTheme::U32(sel ? EditorTheme::WithAlpha(EditorTheme::Accent, 0.22f)
                                                                       : EditorUIPrimitives::FlatHover()), EditorTheme::Px(3.0f));
                    const ImVec2 gs = ImGui::CalcTextSize(ic.Glyph);
                    dl->AddText(ImVec2((mn.x + mx.x - gs.x) * 0.5f, (mn.y + mx.y - gs.y) * 0.5f),
                                EditorTheme::U32(sel ? EditorTheme::AccentBright : (hov ? EditorTheme::Text : EditorTheme::Secondary)), ic.Glyph);
                    if (hov) Tip(ic.Name);
                    ImGui::PopID();
                }
            }
        }
        ImGui::PopStyleVar();
        if (s_Rows.empty()) ImGui::TextDisabled("No icon matches \"%s\"", search);
    }
    ImGui::EndChild();
    ImGui::PopID();
    return picked;
}

} // namespace Enhancers
