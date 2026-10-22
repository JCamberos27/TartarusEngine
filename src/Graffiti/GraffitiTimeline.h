#pragma once
#include "GraffitiPaint.h"
#include <cstdint>
#include <vector>

// A piece played back over time, the way a spray can puts it on a wall (Decals P4: SprayCanAction,
// docs/GRAFFITI_DESIGNER.md "Spray can"). Layers go on bottom to top. A spray layer follows its
// recorded strokes at their own pace (the splat times), each stroke after the last with a short
// pause; the letters and stamps of a text or stamp layer go on one after another, each swept left to
// right; a fill sweeps down the wall. Pure CPU and deterministic, like the paint program it reads.
namespace Graffiti {

struct Timeline {
    static constexpr float kStrokeGap = 0.3f;  // seconds between two strokes (the can moves to the next)
    static constexpr float kLayerGap = 0.5f;   // and between two layers
    static constexpr float kQuadSeconds = 0.6f; // one letter or stamp
    static constexpr float kFillSeconds = 1.5f; // a fill layer

    struct LayerSpan {
        float Start = 0.0f, Length = 0.0f;
    };
    std::vector<LayerSpan> Layers;              // per program layer
    std::vector<std::vector<float>> SplatStart; // per program layer, per splat: seconds from the piece's start
    std::vector<std::vector<float>> QuadStart;  // per program layer, per quad
    float Duration = 0.0f;                      // the recorded pace's total

    // Where the can is and whether it's spraying at `t` (seconds at the recorded pace): the last splat
    // laid by then within kStrokeGap, its canvas uv (0..1, y down).
    bool SprayingAt(const Program& program, float t, glm::vec2* uv = nullptr) const;
};

Timeline BuildTimeline(const Program& program);

// The reveal order of a piece: per texel of a `w` x `h` map (rows top first), 255 x (1 - when paint first
// lands there / the duration), so a decal that shows texels at or above a falling threshold puts the piece
// on in that order (DecalSystem::RevealOverride). 0 where nothing ever paints. Erasing splats are skipped.
std::vector<uint8_t> RevealMap(const Program& program, const Timeline& timeline, int w, int h);

// The threshold / band for a piece `progress` (0..1) of the way through: band wide, starting at 1 so nothing
// shows at 0, reaching 0 (everything) at 1.
inline float RevealThreshold(float progress, float band) {
    const float p = progress < 0.0f ? 0.0f : progress > 1.0f ? 1.0f : progress;
    const float t = 1.0f - p * (1.0f + band);
    return t > 0.0f ? t : 0.0f;
}

} // namespace Graffiti
