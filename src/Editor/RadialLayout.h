#pragma once
// The radial menu's layout math (EditorLayer_ViewportMotion.cpp), pure so a unit test can pin it:
// every wedge holds one block - the icon with a horizontal label under it - centred on the
// wedge's mid-angle, snapped to whole pixels so glyphs stay crisp. Past kMaxLabelled slots the
// wedges hold icons only (the hovered slot's name goes in the hub).

#include <imgui.h>

#include <cmath>

namespace RadialLayout {

constexpr int kMaxLabelled = 8;
constexpr float kPi = 3.14159265358979f;

inline float Snap(float v) { return std::floor(v + 0.5f); }
inline ImVec2 Snap(ImVec2 v) { return ImVec2(Snap(v.x), Snap(v.y)); }

inline bool ShowLabels(int count) { return count <= kMaxLabelled; }

// Wedge angle for slot i: slot 0 at the top, clockwise on screen.
inline float WedgeAngle(int i, int count) { return -kPi * 0.5f + (float)i * 2.0f * kPi / (float)count; }

// The widest a label can be in a block of height `blockH` centred `rBlock` from the hub without
// leaving its wedge: the wedge's width at the block's edge nearest the hub, less `pad`.
inline float MaxLabelWidth(float rBlock, float blockH, int count, float pad) {
    const float d = rBlock - blockH * 0.5f;
    const float w = 2.0f * d * std::tan(kPi / (float)count) - pad;
    return w > 0.0f ? w : 0.0f;
}

struct Block {
    ImVec2 iconPos;    // top-left of the icon's ink box
    ImVec2 labelPos;   // top-left of the label's text box (valid when hasLabel)
    ImVec2 blockMin, blockMax;
};

// Lays the block out about the wedge's mid-angle. `iconInk` is the glyph's ink size and
// `labelSize` the label's text box; the icon sits above the label. All outputs are whole pixels.
inline Block Place(ImVec2 center, float angle, float rBlock, ImVec2 iconInk, ImVec2 labelSize, float gap, bool hasLabel) {
    const float bw = hasLabel && labelSize.x > iconInk.x ? labelSize.x : iconInk.x;
    const float bh = iconInk.y + (hasLabel ? gap + labelSize.y : 0.0f);
    const ImVec2 mid(center.x + std::cos(angle) * rBlock, center.y + std::sin(angle) * rBlock);
    Block b;
    b.blockMin = Snap(ImVec2(mid.x - bw * 0.5f, mid.y - bh * 0.5f));
    b.blockMax = ImVec2(b.blockMin.x + Snap(bw), b.blockMin.y + Snap(bh));
    b.iconPos = Snap(ImVec2(mid.x - iconInk.x * 0.5f, b.blockMin.y));
    b.labelPos = Snap(ImVec2(mid.x - labelSize.x * 0.5f, b.blockMin.y + iconInk.y + gap));
    return b;
}

} // namespace RadialLayout
