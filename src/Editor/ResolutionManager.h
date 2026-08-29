#pragma once
#include <string>
#include <vector>
#include <imgui.h>

// Mirrors Unity's Game View aspect/resolution bar.
enum class AspectRatioMode {
    FreeAspect,      // fill whatever space is available, no locking
    FixedAspect,     // lock the aspect ratio only — resolved size still fills the container
    FixedResolution, // lock to an exact pixel resolution
};

struct ResolutionPreset {
    std::string Label;
    AspectRatioMode Mode = AspectRatioMode::FreeAspect;
    int Width = 0;   // only meaningful for FixedResolution
    int Height = 0;  // only meaningful for FixedResolution
    float AspectRatio = 16.0f / 9.0f; // meaningful for FixedAspect and FixedResolution
};

// Pure, decoupled from any rendering/windowing state — just the preset list and the
// letterbox/pillarbox math, so GameViewPanel (or a unit test) can use it without a GL context.
namespace ResolutionManager {

// Built-in presets, in display order — Free Aspect first (the default), then aspect-only
// presets, then fixed resolutions. GameViewPanel appends its own "Custom Resolution..." entries
// after these.
const std::vector<ResolutionPreset>& BuiltInPresets();

// Given the space available in the ImGui panel and a target aspect ratio, returns the largest
// centered rect (in the same coordinate space as `containerSize`, i.e. an offset from the
// container's top-left plus a size) that fits `containerSize` while preserving `targetAspect` —
// the classic letterbox/pillarbox fit. Returns `containerSize` itself unchanged (offset {0,0})
// when `targetAspect <= 0` (treat as Free Aspect).
struct LetterboxRect {
    ImVec2 Offset; // top-left, relative to the container's own top-left
    ImVec2 Size;
};
LetterboxRect CalculateLetterboxRect(ImVec2 containerSize, float targetAspect);

// Maps a mouse position given in the ImGui container's local coordinates (e.g.
// `ImGui::GetMousePos() - ImGui::GetItemRectMin()`) into normalized [0,1] UV coordinates of the
// underlying framebuffer, accounting for the letterbox/pillarbox bars. Returns false (and leaves
// `outUV` untouched) when the mouse is over a letterbox bar rather than the actual image.
bool MapContainerPosToFramebufferUV(ImVec2 containerLocalPos, const LetterboxRect& rect, ImVec2& outUV);

} // namespace ResolutionManager
