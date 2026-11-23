#include "UnitTestSupport.h"
#include "GiTour.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

const char* kTour = R"({
  "version": 1,
  "scene": "scenes/Forest.json",
  "player": { "eyeHeight": 1.5, "horizontalFovDeg": 90.0, "verticalFovDeg": 60.0, "walkSpeed": 2.0, "source": "x" },
  "snapToFloor": true,
  "waypoints": [
    { "id": "a", "room": "Hall, front", "giVolume": "GI hall", "kind": "room", "feet": [0, 0, 0], "yawDeg": 170, "pitchDeg": 0 },
    { "id": "b", "room": "Hall", "kind": "door_in", "feet": [2, 0, 0], "yawDeg": -170, "pitchDeg": 10 },
    { "id": "c", "room": "Upstairs", "kind": "room", "feet": [0, 3, 0], "yawDeg": 0, "cut": true }
  ],
  "routes": [ { "id": "r1", "name": "a to b", "points": ["a", "b"] } ]
})";

// The tour file: the camera at feet + eye height, the player block, the cut flag, the route's waypoints by id, and
// snapToFloor ignored with a warning.
void GiTourParse() {
    GiTour::Tour t;
    std::string err;
    std::vector<std::string> warn;
    CHECK(GiTour::Parse(kTour, t, err, &warn));
    CHECK(t.Scene == "scenes/Forest.json" && Near(t.VerticalFovDeg, 60.0f) && Near(t.WalkSpeed, 2.0f));
    CHECK(t.Waypoints.size() == 3 && t.Routes.size() == 1);
    CHECK(Near(t.Waypoints[0].Eye.y, 1.5f) && Near(t.Waypoints[2].Eye.y, 4.5f) && Near(t.Waypoints[1].Eye.x, 2.0f));
    CHECK(t.Waypoints[0].Room == "Hall, front" && t.Waypoints[0].GiVolume == "GI hall" && t.Waypoints[1].Kind == "door_in");
    CHECK(!t.Waypoints[1].Cut && t.Waypoints[2].Cut && Near(t.Waypoints[1].Pitch, 10.0f));
    CHECK(t.Routes[0].Points.size() == 2 && t.Routes[0].Points[1] == 1 && t.Routes[0].Speed == 0.0f);
    CHECK(warn.size() == 1);
    CHECK(t.DoorState == "allOpen"); // the default
    CHECK(GiTour::Parse(R"({"scene":"s","doorState":"authored","waypoints":[{"id":"a","feet":[0,0,0]}]})", t, err) &&
          t.DoorState == "authored");
    CHECK(!GiTour::Parse(R"({"scene":"s","doorState":"ajar","waypoints":[{"id":"a","feet":[0,0,0]}]})", t, err) &&
          err.find("doorState") != std::string::npos);
}

// A tour that can't run is refused with a reason, not run half-way.
void GiTourParseErrors() {
    GiTour::Tour t;
    std::string err;
    CHECK(!GiTour::Parse("not json", t, err));
    CHECK(!GiTour::Parse(R"({"scene":"s.json","waypoints":[]})", t, err) && err.find("waypoints") != std::string::npos);
    CHECK(!GiTour::Parse(R"({"waypoints":[{"id":"a","feet":[0,0,0]}]})", t, err) && err.find("scene") != std::string::npos);
    CHECK(!GiTour::Parse(R"({"scene":"s","waypoints":[{"id":"a","feet":[0,0]}]})", t, err) && err.find("feet") != std::string::npos);
    CHECK(!GiTour::Parse(R"({"scene":"s","waypoints":[{"id":"a","feet":[0,0,0]},{"id":"a","feet":[1,0,0]}]})", t, err) &&
          err.find("twice") != std::string::npos);
    CHECK(!GiTour::Parse(R"({"scene":"s","waypoints":[{"id":"a","feet":[0,0,0]}],"routes":[{"id":"r","points":["a","zz"]}]})", t, err) &&
          err.find("zz") != std::string::npos);
    CHECK(!GiTour::Parse(R"({"version":2,"scene":"s","waypoints":[{"id":"a","feet":[0,0,0]}]})", t, err));
    CHECK(!GiTour::Parse(R"({"scene":"s","player":{"walkSpeed":0},"waypoints":[{"id":"a","feet":[0,0,0]}]})", t, err));
}

// The frames: walk a -> b at walk speed (2 m in 1 s = 60 frames), the settle / shots / 2 s hold at each waypoint, no
// walk into a cut, and the route looking along its way.
void GiTourTimeline() {
    GiTour::Tour t;
    std::string err;
    CHECK(GiTour::Parse(kTour, t, err));
    GiTour::TimelineOptions o;
    const std::vector<GiTour::Frame> f = GiTour::BuildTimeline(t, o);
    int walkAB = 0, walkIntoC = 0, fullShots = 0, giShots = 0, still = 0, route = 0;
    for (const GiTour::Frame& x : f) {
        if (x.Segment == "walk:a->b") ++walkAB;
        if (x.Segment.find("->c") != std::string::npos) ++walkIntoC;
        if (x.Kind == GiTour::Step::ShotFull) ++fullShots;
        if (x.Kind == GiTour::Step::ShotGi) ++giShots;
        if (x.Kind == GiTour::Step::Still) ++still;
        if (x.Segment.rfind("route:r1:", 0) == 0) { ++route; CHECK(Near(x.Yaw, 0.0f) && Near(x.Pitch, 0.0f)); }
    }
    CHECK(walkAB == 60 && walkIntoC == 0 && fullShots == 3 && giShots == 3 && still == 3 * o.StillFrames && route == 60);
    const int per = o.ArriveSettle + 1 + o.ViewSettle + 1 + o.ViewSettle + o.StillFrames;
    CHECK((int)f.size() == 3 * per + 60 + o.ArriveSettle + 60);
    // the walk ends on b, its view blended the short way (170 -> -170 passes 180)
    const GiTour::Frame* last = nullptr;
    const GiTour::Frame* mid = nullptr;
    int k = 0;
    for (const GiTour::Frame& x : f)
        if (x.Segment == "walk:a->b") { if (++k == 30) mid = &x; last = &x; }
    CHECK(last && Near(last->Eye.x, 2.0f) && Near(last->Pitch, 10.0f));
    CHECK(mid && std::fabs(std::fmod(mid->Yaw + 360.0f, 360.0f) - 180.0f) < 10.0f);
    CHECK(Near(GiTour::LerpAngle(350.0f, 10.0f, 0.5f), 360.0f) && Near(GiTour::LerpAngle(10.0f, 350.0f, 0.5f), 0.0f));
}

// The static-surface mask (depth below far), the sRGB decode, and the slot set hash's order independence.
void GiTourLuminanceAndHash() {
    const unsigned char rgba[4 * 4] = {255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255, 128, 128, 128, 255};
    const float depth[4] = {0.5f, 0.5f, 1.0f, 1.0f}; // the last two are sky
    int n = 0;
    CHECK(Near((float)GiTour::MaskedMeanLuminance(rgba, depth, 2, 2, 1, 0.999999f, &n), 0.5f) && n == 2);
    const float allSky[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    CHECK(GiTour::MaskedMeanLuminance(rgba, allSky, 2, 2, 1, 0.999999f, &n) < 0.0 && n == 0);
    const float grey[1] = {0.1f};
    CHECK(Near((float)GiTour::MaskedMeanLuminance(rgba + 12, grey, 1, 1), 0.2158f)); // sRGB 128 -> linear 0.2158
    CHECK(GiTour::SlotSetHash({3, 1, 2}, {9}) == GiTour::SlotSetHash({1, 2, 3}, {9}));
    CHECK(GiTour::SlotSetHash({1, 2}, {9}) != GiTour::SlotSetHash({1, 9}, {2}));
    CHECK(GiTour::SlotSetHash({}, {}) != GiTour::SlotSetHash({0}, {}));
    // Depth: window depth back to metres (near 0.1, far 100), the median over drawn pixels, the share nearer than 0.3 m.
    auto win = [](float z) { const float n = 0.1f, f = 100.0f; return (float)(0.5 * ((f + n) / (f - n) - 2.0 * f * n / ((f - n) * z)) + 0.5); };
    const float d4[4] = {win(0.2f), win(2.0f), win(2.0f), 1.0f}; // one pixel 20 cm away, two at 2 m, one sky
    const GiTour::DepthStats ds = GiTour::MaskedDepthStats(d4, 2, 2, 0.1f, 100.0f);
    CHECK(Near((float)ds.MedianM, 2.0f, 0.01f) && Near((float)ds.CloseFraction, 1.0f / 3.0f));
    const float sky[2] = {1.0f, 1.0f};
    CHECK(GiTour::MaskedDepthStats(sky, 2, 1, 0.1f, 100.0f).MedianM < 0.0);
}

// The CSV: the header's columns, quoted fields, fixed number formats.
void GiTourCsv() {
    GiTour::CsvRow r;
    r.Frame = 12; r.Mode = "walk"; r.Segment = "walk:a->b"; r.Waypoint = "b"; r.Room = "Hall, \"front\"";
    r.Luminance = 0.25; r.MaskPixels = 100; r.SlotHash = 0xABCull; r.SlotsChanged = true;
    r.MedianDepthM = 2.5; r.CloseFraction = 0.125;
    CHECK(GiTour::CsvLine(r) == "12,walk,walk:a->b,b,\"Hall, \"\"front\"\"\",0.250000,100,0000000000000abc,1,2.500,0.1250");
    int commas = 0;
    for (char c : GiTour::CsvHeader()) commas += c == ',';
    CHECK(commas == 10);
    CHECK(GiTour::CsvField("plain") == "plain" && GiTour::CsvField("a\nb") == "\"a\nb\"");
}

} // namespace

void RegisterGiTourTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"GI tour: the tour file (eye height, cuts, routes, snapToFloor ignored)", GiTourParse});
    tests.push_back({"GI tour: a tour that can't run is refused with the reason", GiTourParseErrors});
    tests.push_back({"GI tour: the frame timeline (walk speed, shots, 2 s holds, cuts, routes)", GiTourTimeline});
    tests.push_back({"GI tour: masked luminance and the shadow-slot set hash", GiTourLuminanceAndHash});
    tests.push_back({"GI tour: the walk / still CSV", GiTourCsv});
}
