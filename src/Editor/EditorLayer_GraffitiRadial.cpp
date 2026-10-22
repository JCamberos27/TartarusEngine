// The viewport radial menu's Graffiti mode (decals Phase 3): with the Designer's Spray in Scene view on,
// holding Space shows its tools, its recent colours and three can distances. The ring itself - bloom,
// hover wash, labels on the rim, the full name in the hub - is the shared one in
// EditorLayer_ViewportMotion.cpp; this file only supplies the slots and what picking one does.
#include "EditorLayer.h"
#include "EditorGraffitiDesigner.h"
#include "EditorSound.h"

#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
using GTool = GraffitiDesigner::Tool;

constexpr int kRecentColours = 3;
struct SizeDef { int Cm; const char* Label; const char* Full; };
constexpr SizeDef kSizes[] = {{8, "Near", "Near (8 cm)"}, {15, "Mid", "Mid (15 cm)"}, {30, "Far", "Far (30 cm)"}};
// Spray first: it is what the mode is for.
constexpr GTool kTools[] = {GTool::Spray, GTool::Text, GTool::Stamp, GTool::Eyedropper, GTool::Erase, GTool::Move};

const char* ToolLabel(GTool t) {
    switch (t) {
        case GTool::Spray: return "Spray";
        case GTool::Text: return "Text";
        case GTool::Stamp: return "Stamp";
        case GTool::Eyedropper: return "Pick";
        case GTool::Erase: return "Erase";
        default: return "Move";
    }
}
const char* ToolFull(GTool t) {
    switch (t) {
        case GTool::Spray: return "Spray";
        case GTool::Text: return "Text";
        case GTool::Stamp: return "Stamp";
        case GTool::Eyedropper: return "Eyedropper";
        case GTool::Erase: return "Erase";
        default: return "Move";
    }
}
const char* ToolIconOf(GTool t) {
    switch (t) {
        case GTool::Spray: return ICON_FA_SPRAY_CAN;
        case GTool::Text: return ICON_FA_FONT;
        case GTool::Stamp: return ICON_FA_STAMP;
        case GTool::Eyedropper: return ICON_FA_EYE_DROPPER;
        case GTool::Erase: return ICON_FA_ERASER;
        default: return ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT;
    }
}
int Byte(float v) { return (int)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); }
} // namespace

int EditorLayer::GraffitiRadialBuild() {
    m_GraffitiRadial.clear();
    if (!m_Graffiti) return 0;
    const GraffitiDesigner& g = *m_Graffiti;
    for (GTool t : kTools) {
        GraffitiRadialSlot s;
        s.What = GraffitiRadialSlot::Tool;
        s.Index = (int)t;
        s.Label = ToolLabel(t);
        s.FullName = ToolFull(t);
        s.Icon = ToolIconOf(t);
        m_GraffitiRadial.push_back(std::move(s));
    }
    for (int i = 0; i < (int)g.Recent.size() && i < kRecentColours; ++i) {
        const glm::vec3& c = g.Recent[(size_t)i];
        char hex[16];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", Byte(c.x), Byte(c.y), Byte(c.z));
        GraffitiRadialSlot s;
        s.What = GraffitiRadialSlot::Colour;
        s.Index = i;
        s.Label = "Col " + std::to_string(i + 1);
        s.FullName = std::string("Colour ") + hex;
        s.Icon = ICON_FA_PALETTE;
        s.Dot = IM_COL32(Byte(c.x), Byte(c.y), Byte(c.z), 255);
        m_GraffitiRadial.push_back(std::move(s));
    }
    for (const SizeDef& d : kSizes) {
        GraffitiRadialSlot s;
        s.What = GraffitiRadialSlot::Size;
        s.Index = d.Cm;
        s.Label = d.Label;
        s.FullName = std::string("Can distance: ") + d.Full;
        s.Icon = ICON_FA_CIRCLE;
        m_GraffitiRadial.push_back(std::move(s));
    }
    return (int)m_GraffitiRadial.size();
}

bool EditorLayer::GraffitiRadialItem(int i, const char*& icon, const char*& label, ImU32& dot) const {
    if (!m_Graffiti || i < 0 || i >= (int)m_GraffitiRadial.size()) return false;
    const GraffitiRadialSlot& s = m_GraffitiRadial[(size_t)i];
    icon = s.Icon;
    label = s.Label.c_str();
    dot = s.Dot;
    const GraffitiDesigner& g = *m_Graffiti;
    switch (s.What) {
        case GraffitiRadialSlot::Tool: return (int)g.Active == s.Index;
        case GraffitiRadialSlot::Colour:
            return s.Index < (int)g.Recent.size() && g.Recent[(size_t)s.Index] == g.Color;
        case GraffitiRadialSlot::Size: return std::fabs(g.DistanceCm - (float)s.Index) < 1.0f;
    }
    return false;
}

const char* EditorLayer::GraffitiRadialFullName(int i) const {
    if (i < 0 || i >= (int)m_GraffitiRadial.size()) return "";
    return m_GraffitiRadial[(size_t)i].FullName.c_str();
}

void EditorLayer::RunGraffitiRadialItem(int i) {
    if (!m_Graffiti || i < 0 || i >= (int)m_GraffitiRadial.size()) return;
    GraffitiDesigner& g = *m_Graffiti;
    const GraffitiRadialSlot s = m_GraffitiRadial[(size_t)i];
    switch (s.What) {
        case GraffitiRadialSlot::Tool: g.Active = (GTool)s.Index; break;
        case GraffitiRadialSlot::Colour:
            if (s.Index < (int)g.Recent.size()) g.Color = g.Recent[(size_t)s.Index];
            break;
        case GraffitiRadialSlot::Size: g.DistanceCm = (float)s.Index; break;
    }
}
