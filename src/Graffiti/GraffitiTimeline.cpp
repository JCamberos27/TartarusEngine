#include "GraffitiTimeline.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Graffiti {

Timeline BuildTimeline(const Program& program) {
    Timeline tl;
    float t = 0.0f;
    const size_t n = program.Layers.size();
    tl.Layers.resize(n);
    tl.SplatStart.resize(n);
    tl.QuadStart.resize(n);
    for (size_t li = 0; li < n; ++li) {
        const LayerProgram& lp = program.Layers[li];
        if (li > 0) t += Timeline::kLayerGap;
        const float start = t;
        // Splats: one stroke's are in time order from 0; a later stroke starts again (its time drops back), and
        // goes on after the one before with a pause.
        std::vector<float>& ss = tl.SplatStart[li];
        ss.resize(lp.Splats.size());
        float strokeBase = start, strokeEnd = start, prev = -1.0f;
        for (size_t i = 0; i < lp.Splats.size(); ++i) {
            const float st = std::max(lp.Splats[i].Time, 0.0f);
            if (i > 0 && (st < prev || (st == 0.0f && prev > 0.0f))) strokeBase = strokeEnd + Timeline::kStrokeGap;
            ss[i] = strokeBase + st;
            strokeEnd = std::max(strokeEnd, ss[i]);
            prev = st;
        }
        float end = lp.Splats.empty() ? start : strokeEnd;
        // Letters and stamps, one after another.
        std::vector<float>& qs = tl.QuadStart[li];
        qs.resize(lp.Quads.size());
        for (size_t i = 0; i < lp.Quads.size(); ++i) {
            qs[i] = end;
            end += Timeline::kQuadSeconds;
        }
        if (lp.Splats.empty() && lp.Quads.empty()) end = start + Timeline::kFillSeconds; // a fill
        tl.Layers[li] = {start, end - start};
        t = end;
    }
    tl.Duration = t;
    return tl;
}

bool Timeline::SprayingAt(const Program& program, float t, glm::vec2* uv) const {
    // The last splat (or quad) laid at or before t, if it was laid less than a stroke gap ago.
    float best = -std::numeric_limits<float>::infinity();
    glm::vec2 at(0.5f);
    const float w = (float)std::max(program.Width, 1), h = (float)std::max(program.Height, 1);
    for (size_t li = 0; li < program.Layers.size() && li < Layers.size(); ++li) {
        const LayerSpan& span = Layers[li];
        if (span.Start > t) break;
        const LayerProgram& lp = program.Layers[li];
        if (lp.Splats.empty() && lp.Quads.empty()) { // a fill: the can sweeps down the canvas
            if (t <= span.Start + span.Length) {
                best = t;
                const float f = (t - span.Start) / std::max(span.Length, 1e-3f);
                at = glm::vec2(0.5f + 0.4f * std::sin(f * 40.0f), f);
            }
            continue;
        }
        // Splats are in time order within a stroke and strokes in order: the last one not after t.
        const std::vector<float>& ss = SplatStart[li];
        const auto it = std::upper_bound(ss.begin(), ss.end(), t);
        if (it != ss.begin()) {
            const size_t i = (size_t)(it - ss.begin()) - 1;
            if (ss[i] > best && !(lp.Splats[i].Flags & kSplatErase)) {
                best = ss[i];
                at = glm::vec2(lp.Splats[i].Shape.x / w, lp.Splats[i].Shape.y / h);
            }
        }
        const std::vector<float>& qs = QuadStart[li];
        for (size_t i = 0; i < qs.size(); ++i) {
            if (qs[i] > t || t > qs[i] + kQuadSeconds) continue;
            best = t;
            const Quad& q = lp.Quads[i];
            const float f = (t - qs[i]) / kQuadSeconds;
            at = glm::vec2((q.Centre.x + (f * 2.0f - 1.0f) * q.Half.x) / w, q.Centre.y / h);
        }
    }
    if (uv) *uv = at;
    return t - best <= kStrokeGap && t <= Duration;
}

std::vector<uint8_t> RevealMap(const Program& program, const Timeline& tl, int w, int h) {
    std::vector<float> first((size_t)w * h, std::numeric_limits<float>::infinity());
    const float sx = (float)w / (float)std::max(program.Width, 1), sy = (float)h / (float)std::max(program.Height, 1);
    auto mark = [&](int x, int y, float t) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        float& f = first[(size_t)y * w + x];
        f = std::min(f, t);
    };
    for (size_t li = 0; li < program.Layers.size() && li < tl.Layers.size(); ++li) {
        const LayerProgram& lp = program.Layers[li];
        const Timeline::LayerSpan& span = tl.Layers[li];
        if (lp.Splats.empty() && lp.Quads.empty()) { // a fill: down the wall
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) mark(x, y, span.Start + span.Length * ((float)y + 0.5f) / (float)h);
            continue;
        }
        for (size_t i = 0; i < lp.Splats.size(); ++i) {
            const Splat& s = lp.Splats[i];
            if (s.Flags & kSplatErase) continue;
            const float t = tl.SplatStart[li][i];
            // The core and the near overspray (the far speckle is sparse: it shows once the core reaches it).
            const float r = std::max(s.Shape.z, s.Shape.w) * std::clamp(s.Ring, 1.0f, 1.5f);
            const float cx = s.Shape.x * sx, cy = s.Shape.y * sy, rx = std::max(r * sx, 0.5f), ry = std::max(r * sy, 0.5f);
            for (int y = (int)std::floor(cy - ry); y <= (int)std::ceil(cy + ry); ++y)
                for (int x = (int)std::floor(cx - rx); x <= (int)std::ceil(cx + rx); ++x) {
                    const float dx = ((float)x + 0.5f - cx) / rx, dy = ((float)y + 0.5f - cy) / ry;
                    if (dx * dx + dy * dy <= 1.0f) mark(x, y, t);
                }
        }
        for (size_t i = 0; i < lp.Quads.size(); ++i) {
            const Quad& q = lp.Quads[i];
            const float t0 = tl.QuadStart[li][i];
            const float c = std::cos(q.Angle), sn = std::sin(q.Angle);
            const float reach = std::sqrt(q.Half.x * q.Half.x + q.Half.y * q.Half.y) + q.OutlinePx;
            const float cx = q.Centre.x * sx, cy = q.Centre.y * sy;
            for (int y = (int)std::floor(cy - reach * sy); y <= (int)std::ceil(cy + reach * sy); ++y)
                for (int x = (int)std::floor(cx - reach * sx); x <= (int)std::ceil(cx + reach * sx); ++x) {
                    // Into the quad's own frame (canvas texels), swept left to right.
                    const float px = ((float)x + 0.5f) / sx - q.Centre.x, py = ((float)y + 0.5f) / sy - q.Centre.y;
                    const float u = px * c + py * sn, v = -px * sn + py * c;
                    const float hx = q.Half.x + q.OutlinePx, hy = q.Half.y + q.OutlinePx;
                    if (std::abs(u) > hx || std::abs(v) > hy) continue;
                    mark(x, y, t0 + Timeline::kQuadSeconds * (u / hx * 0.5f + 0.5f));
                }
        }
    }
    std::vector<uint8_t> out((size_t)w * h, 0);
    const float d = std::max(tl.Duration, 1e-3f);
    for (size_t i = 0; i < out.size(); ++i)
        if (std::isfinite(first[i])) out[i] = (uint8_t)std::clamp((int)std::lround(255.0f * (1.0f - first[i] / d)), 1, 255);
    return out;
}

} // namespace Graffiti
