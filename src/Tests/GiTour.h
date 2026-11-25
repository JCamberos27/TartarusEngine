#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// --gi-tour <tour.json> --out <dir> (GI Phase 0, measure only): a fixed camera tour of a scene through the Scene view,
// for the "the shading changes as I walk" diagnosis. The pure part lives here (the tour file, the frame timeline, the
// luminance over the static-surface mask, the shadow-slot set hash and the CSV), so it is unit-tested without a GL
// context (UnitTests_GiTour.cpp); SmokeHarness::GiTourFrame drives it frame by frame.
//
// The tour file (tools/gi/forest_house_tour.json, plain JSON):
//   { "version": 1, "scene": "scenes/Forest.json",
//     "player": { "eyeHeight": 1.7, "verticalFovDeg": 58.72, "walkSpeed": 4.0 }, "doorState": "allOpen",
//     "waypoints": [ { "id": "hall", "room": "Entrance hall", "giVolume": "...", "kind": "room",
//                      "feet": [x, y, z], "yawDeg": 90, "pitchDeg": 0, "cut": false } ],
//     "routes": [ { "id": "front_to_kitchen", "name": "...", "speed": 4.0, "points": ["hall", ...],
//                   "faceWaypoints": false } ] }
// The camera sits at feet + (0, eyeHeight, 0) with the engine Camera's yaw / pitch (yaw -90 looks down -Z). The
// waypoints are walked in file order; "cut": true on a waypoint teleports to it instead of walking from the last one.
namespace GiTour {

struct Waypoint {
    std::string Id, Room, GiVolume, Kind;
    glm::vec3 Eye{0.0f};
    float Yaw = -90.0f, Pitch = 0.0f;
    bool Cut = false;
};

struct Route {
    std::string Id, Name;
    float Speed = 0.0f;           // m/s; 0 = the player's walk speed
    std::vector<int> Points;      // indices into Tour::Waypoints
    bool FaceWaypoints = false;
};

struct Tour {
    std::string Scene;            // project-relative scene path
    float EyeHeight = 1.7f;
    float VerticalFovDeg = 58.72f;
    float WalkSpeed = 1.4f;       // m/s
    // "doorState": "allOpen" (the default: every Openable fully open for the whole tour, deterministic and how the
    // house is walked) or "authored" (left as the scene saved them, shut).
    std::string DoorState = "allOpen";
    // "stillFrames": the stationary control held after each waypoint's shots (default -1 = TimelineOptions::StillFrames, 2 s).
    // 0 skips it: a stills-only sweep (many waypoints, PNGs wanted, no still CSV rows) saves 44% of its frames.
    int StillFrames = -1;
    std::vector<Waypoint> Waypoints;
    std::vector<Route> Routes;
};

// Parses the tour file's text. False with `error` set on a file the tour can't run (bad JSON, no waypoints, a route
// naming an unknown waypoint, a non-finite number). `warnings` gets what is ignored (snapToFloor).
bool Parse(const std::string& text, Tour& out, std::string& error, std::vector<std::string>* warnings = nullptr);
bool Load(const std::string& path, Tour& out, std::string& error, std::vector<std::string>* warnings = nullptr);

// What the harness does on one frame of the tour (60 frames a second, a fixed step).
enum class Step {
    Settle,      // hold the pose (TAA, streaming and the shadow caches catch up); nothing recorded
    ShotFull,    // capture <waypoint>_full.png, then switch to the GI-only view
    ShotGi,      // capture <waypoint>_gi.png, then back to full lighting
    Still,       // the stationary control: a CSV row each frame, the camera held
    Walk,        // a CSV row each frame, the camera moving
};

struct Frame {
    Step Kind = Step::Settle;
    glm::vec3 Eye{0.0f};
    float Yaw = 0.0f, Pitch = 0.0f;
    int Waypoint = -1;            // the waypoint held (Settle / Shot / Still), or walked to (Walk)
    std::string Segment;          // "still:<id>", "walk:<from>-><to>", "route:<route>:<from>-><to>"
};

struct TimelineOptions {
    int Fps = 60;
    int ArriveSettle = 90;        // frames held on arriving (TAA, streaming, shadow caches)
    int ViewSettle = 30;          // frames held after switching the debug view, before its shot
    int StillFrames = 120;        // the stationary control: 2 s
};

// The whole tour as frames: per waypoint (walked to from the last unless it cuts) the settle, the two shots, the 2 s
// hold; then each route from its first point, walked through the rest looking along the way.
std::vector<Frame> BuildTimeline(const Tour& tour, const TimelineOptions& opt = {});

// The shortest-arc blend of two angles in degrees.
float LerpAngle(float a, float b, float t);

// Mean relative luminance (Rec. 709, of the sRGB-decoded colour) over the static-surface mask: the pixels whose depth
// is below `farDepth` (drawn geometry, not the sky). rgba is w*h RGBA8 and depth w*h floats, in the same row order.
// `stride` samples every stride-th pixel both ways. Returns -1 when no pixel is in the mask; `maskCount` gets its size.
double MaskedMeanLuminance(const unsigned char* rgba, const float* depth, int w, int h, int stride = 1,
                           float farDepth = 0.999999f, int* maskCount = nullptr);

// Where the frame's drawn pixels are (all of them, sky excluded): the median view-space distance in metres, and the
// share nearer than `closeM`. Tells a camera pressed against geometry (nearly all pixels within a few cm) from a dark
// room (depth spread over metres). depth is the [0, 1] window depth of a glm::perspective(near, far) projection.
struct DepthStats { double MedianM = -1.0; double CloseFraction = 0.0; };
DepthStats MaskedDepthStats(const float* depth, int w, int h, float nearPlane, float farPlane, int stride = 1,
                            float closeM = 0.3f, float farDepth = 0.999999f);

// An order-independent hash of the frame's shadow-slot holders (entity ids), spots and points kept apart.
std::uint64_t SlotSetHash(std::vector<std::uint32_t> spots, std::vector<std::uint32_t> points);

// The walk / still CSV.
struct CsvRow {
    int Frame = 0;
    std::string Mode;             // "walk" | "still"
    std::string Segment;
    std::string Waypoint;         // id
    std::string Room;
    double Luminance = 0.0;
    int MaskPixels = 0;
    double MedianDepthM = -1.0;   // MaskedDepthStats
    double CloseFraction = 0.0;
    std::uint64_t SlotHash = 0;
    bool SlotsChanged = false;    // differs from the previous row in the same segment
};
std::string CsvHeader();
std::string CsvLine(const CsvRow& r);
// A CSV field, quoted when it holds a comma, quote or line break.
std::string CsvField(const std::string& s);

} // namespace GiTour
