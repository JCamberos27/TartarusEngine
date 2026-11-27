#include "MotionDataReport.h"

#include "MotionSearch.h"

#include <algorithm>
#include <cmath>

namespace MotionDataReport {

const char* DirName(int d) {
    static const char* const kNames[DirCount] = {"-", "forward", "back", "left", "right"};
    return d >= 0 && d < DirCount ? kNames[d] : "?";
}

int DirOf(const MotionDatabase& db, std::uint32_t frame) {
    if (db.Gait(frame) == MotionLabel::Idle) return DirNone;
    const auto raw = db.Raw(frame);
    const float x = raw[19], z = raw[20]; // the root at +TrajFrames[2], in the frame's root space (+z faces, +x is the body's left)
    if (std::hypot(x, z) < 1e-4f) return DirNone;
    const float deg = std::atan2(x, z) * 57.29578f;
    if (std::fabs(deg) <= 45.0f) return DirForward;
    if (std::fabs(deg) >= 135.0f) return DirBack;
    return deg > 0.0f ? DirLeft : DirRight;
}

Report Build(const MotionDatabase& db, int minRunway) {
    Report r;
    r.Fps = db.Fps();
    r.MinRunway = minRunway;
    MotionSearchParams p; // the defaults: Forbid Airborne, Excluded and NoSearch always
    p.MinRunway = minRunway;
    for (const auto& clip : db.Stream().Clips()) {
        Cell* seen[MotionLabel::StyleCount * 4 * DirCount] = {};
        int nSeen = 0;
        Cell* runCell = nullptr; // the cell the current run of searchable frames is in (reset with every take)
        std::uint32_t run = 0;
        for (std::uint32_t k = 0; k < clip.Count; ++k) {
            const std::uint32_t f = clip.First + k;
            const int style = std::min<int>(db.Style(f), MotionLabel::StyleCount - 1), gait = std::min<int>(db.Gait(f), 3);
            Cell& c = r.Cells[style][gait][DirOf(db, f)];
            const std::uint8_t fl = db.Flags(f);
            ++c.Frames, ++r.Total.Frames;
            const auto charge = [&](std::uint32_t Cell::*cause) { ++(c.*cause), ++(r.Total.*cause); };
            bool ok = false;
            if (fl & MotionLabel::Excluded) charge(&Cell::Excluded);
            else if (fl & MotionLabel::NoSearch) charge(&Cell::NoSearch);
            else if (fl & p.Forbid) charge(&Cell::Airborne);
            else if (db.Runway(f) < (std::uint16_t)std::max(0, minRunway)) charge(&Cell::ShortRunway);
            else ok = true;
            if (!ok) {
                run = 0;
                runCell = nullptr;
                continue;
            }
            charge(&Cell::Searchable);
            if (runCell != &c) {
                runCell = &c;
                run = 0;
            }
            c.Longest = std::max(c.Longest, ++run);
            r.Total.Longest = std::max(r.Total.Longest, c.Longest);
            if (std::find(seen, seen + nSeen, &c) == seen + nSeen) {
                seen[nSeen++] = &c;
                ++c.Takes;
            }
        }
    }
    return r;
}

void Print(const Report& r, std::FILE* out) {
    const float s = 1.0f / std::max(1.0f, r.Fps);
    std::fprintf(out, "[MmData] searchable seconds per style x gait x direction after the filters (Excluded, NoSearch, Airborne, runway >= %d)\n",
                 r.MinRunway);
    std::fprintf(out, "[MmData] %-7s %-5s %-8s %8s | %8s %8s %8s %8s | %10s %8s %5s\n", "style", "gait", "dir", "total s", "excluded", "nosearch",
                 "airborne", "runway", "SEARCHABLE", "longest", "takes");
    const auto row = [&](const char* style, const char* gait, const char* dir, const Cell& c) {
        std::fprintf(out, "[MmData] %-7s %-5s %-8s %8.1f | %8.1f %8.1f %8.1f %8.1f | %10.1f %7.2fs %5d\n", style, gait, dir, c.Frames * s,
                     c.Excluded * s, c.NoSearch * s, c.Airborne * s, c.ShortRunway * s, c.Searchable * s, c.Longest * s, c.Takes);
    };
    for (int st = 0; st < MotionLabel::StyleCount; ++st)
        for (int g = 0; g < 4; ++g)
            for (int d = 0; d < DirCount; ++d)
                if (const Cell& c = r.Cells[st][g][d]; c.Frames) row(MotionLabel::StyleName(st), MotionLabel::GaitName(g), DirName(d), c);
    row("all", "", "", r.Total);
}

IdleReport BuildIdle(const MotionDatabase& db, int minRunway, int idleRunway) {
    IdleReport r;
    MotionSearchParams p;
    p.Require = MotionLabel::IdleTake;
    const std::uint16_t need = (std::uint16_t)std::max(0, std::max(minRunway, idleRunway));
    for (const auto& clip : db.Stream().Clips()) {
        bool take = false;
        for (std::uint32_t k = 0; k < clip.Count; ++k) {
            const std::uint32_t f = clip.First + k;
            if (!(db.Flags(f) & MotionLabel::IdleTake)) continue;
            take = true;
            ++r.TakeFrames;
            if (db.Gait(f) != MotionLabel::Idle) continue;
            ++r.Idle;
            if (!MotionSearch::Passes(db.Flags(f), p)) continue;
            ++r.Searchable;
            if (db.Runway(f) >= need) ++r.WithRunway;
        }
        if (take) ++r.Takes;
    }
    return r;
}

} // namespace MotionDataReport
