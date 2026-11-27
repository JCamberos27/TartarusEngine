#pragma once

// MotionDataReport: how much of the motion database a search can actually land on, per style x gait x direction.
// A frame is searchable when it passes MotionSearch::Passes under the player's search filters (Excluded and NoSearch always
// forbidden, Airborne forbidden by default) and has at least `minRunway` frames of runway (MotionDatabase::Runway). The
// frames lost on the way are counted in the order Excluded, NoSearch, Airborne, short runway, so each frame is charged to
// one cause. Excluded is every cause together (hands up, bent over, gestures, glitches, the prone approach, prone: the
// database keeps one flag); the totals per cause are the database's and the rig load's own counters.
// Not counted: the matcher's reselect-avoid windows (they move with what just played), the style filter (a search asks for
// one style, so a style's row is its pool) and the stance filter (a standing search forbids Crouch, a crouched one requires
// it: Crouch-flagged frames in a non-crouch style row, mixed takes and transitions, count as searchable here although a
// standing search can't land on them).
// Direction: the root's travel over the last trajectory sample (+1 s) against its facing, for Walk, Jog and Run frames.

#include "MotionDatabase.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace MotionDataReport {

enum Dir : int { DirNone = 0, DirForward, DirBack, DirLeft, DirRight, DirCount };
const char* DirName(int d);
// The direction of a frame's travel (DirNone: it isn't travelling - an Idle gait frame).
int DirOf(const MotionDatabase& db, std::uint32_t frame);

struct Cell {
    std::uint32_t Frames = 0, Excluded = 0, NoSearch = 0, Airborne = 0, ShortRunway = 0, Searchable = 0;
    std::uint32_t Longest = 0; // the longest unbroken run of searchable frames inside this cell, in one take (Total: the longest cell's)
    int Takes = 0;             // takes with at least one searchable frame in the cell
};

struct Report {
    Cell Cells[MotionLabel::StyleCount][4][DirCount]; // [style][gait][dir]
    Cell Total;
    float Fps = 60.0f;
    int MinRunway = 0;
};

Report Build(const MotionDatabase& db, int minRunway);

// Seconds per cell: frames, lost to each cause, searchable, longest run, takes. Rows with no frames are left out.
void Print(const Report& r, std::FILE* out = stdout);

// The idle takes' frames (MotionLabel::IdleTake) that the standing idle search can land on: Idle gait, searchable, and
// `idleRunway` frames of runway.
struct IdleReport {
    std::uint32_t TakeFrames = 0, Idle = 0, Searchable = 0, WithRunway = 0;
    int Takes = 0;
};
IdleReport BuildIdle(const MotionDatabase& db, int minRunway, int idleRunway);

// The idle-take check: what the standing idle search can pick in each idle take must stay standing. A candidate is an idle-take
// frame the search accepts (Passes, with `idleRunway` frames of runway); it is bad when it is not Idle gait or any of the next
// `idleRunway` frames of its take is not (an idle leading into a step, a turn or a jump stance). A take fails when it has a bad
// candidate or no candidate at all.
struct IdleTakeCheck {
    std::string Name;
    std::uint32_t Frames = 0, Candidates = 0, Bad = 0;
    std::uint32_t FirstBad = 0, LastBad = 0; // take-relative frames of the first and last bad candidate
    int LeadsInto = -1;                       // the gait the first bad candidate runs into (MotionLabel::Gait)
    bool Failed() const { return Candidates == 0 || Bad > 0; }
};
std::vector<IdleTakeCheck> CheckIdleTakes(const MotionDatabase& db, int minRunway, int idleRunway);

} // namespace MotionDataReport
