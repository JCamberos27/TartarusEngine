#include "ResolutionManager.h"

namespace ResolutionManager {

const std::vector<ResolutionPreset>& BuiltInPresets() {
    static const std::vector<ResolutionPreset> presets = {
        { "Free Aspect",      AspectRatioMode::FreeAspect,      0,    0,    0.0f },
        { "16:9 Aspect",      AspectRatioMode::FixedAspect,      0,    0,    16.0f / 9.0f },
        { "16:10 Aspect",     AspectRatioMode::FixedAspect,      0,    0,    16.0f / 10.0f },
        { "4:3 Aspect",       AspectRatioMode::FixedAspect,      0,    0,    4.0f / 3.0f },
        { "21:9 Aspect",      AspectRatioMode::FixedAspect,      0,    0,    21.0f / 9.0f },
        { "1920x1080 FHD",    AspectRatioMode::FixedResolution,  1920, 1080, 1920.0f / 1080.0f },
        { "1280x720 HD",      AspectRatioMode::FixedResolution,  1280, 720,  1280.0f / 720.0f },
        { "2560x1440 QHD",    AspectRatioMode::FixedResolution,  2560, 1440, 2560.0f / 1440.0f },
        { "3840x2160 4K",     AspectRatioMode::FixedResolution,  3840, 2160, 3840.0f / 2160.0f },
    };
    return presets;
}

LetterboxRect CalculateLetterboxRect(ImVec2 containerSize, float targetAspect) {
    if (targetAspect <= 0.0f || containerSize.x <= 0.0f || containerSize.y <= 0.0f) {
        return { ImVec2(0.0f, 0.0f), containerSize };
    }

    float containerAspect = containerSize.x / containerSize.y;
    ImVec2 size;
    if (containerAspect > targetAspect) {
        // Container is relatively wider than the target — pillarbox (bars on left/right).
        size.y = containerSize.y;
        size.x = size.y * targetAspect;
    } else {
        // Container is relatively taller than the target — letterbox (bars on top/bottom).
        size.x = containerSize.x;
        size.y = size.x / targetAspect;
    }
    ImVec2 offset((containerSize.x - size.x) * 0.5f, (containerSize.y - size.y) * 0.5f);
    return { offset, size };
}

bool MapContainerPosToFramebufferUV(ImVec2 containerLocalPos, const LetterboxRect& rect, ImVec2& outUV) {
    if (rect.Size.x <= 0.0f || rect.Size.y <= 0.0f) return false;
    ImVec2 local(containerLocalPos.x - rect.Offset.x, containerLocalPos.y - rect.Offset.y);
    if (local.x < 0.0f || local.y < 0.0f || local.x > rect.Size.x || local.y > rect.Size.y) {
        return false; // over a letterbox/pillarbox bar, not the actual rendered image
    }
    outUV = ImVec2(local.x / rect.Size.x, local.y / rect.Size.y);
    return true;
}

} // namespace ResolutionManager
