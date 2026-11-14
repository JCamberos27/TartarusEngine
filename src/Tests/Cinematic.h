#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

class World;

// TARTARUS_CINEMATIC=<path.json> with --smoke-test <scene> --smoke-shots <dir>: a fly-through video of the scene. The
// Scene view follows a camera path (a smooth spline through keyframes), the clock steps exactly 1/60 s a frame, and
// every frame of the Scene view (no editor UI) is piped to ffmpeg as H.264 at 60 fps.
//
//   {
//     "out": "trailer.mp4",            // relative to the --smoke-shots dir
//     "ffmpeg": "C:/.../ffmpeg.exe",   // or TARTARUS_FFMPEG
//     "warmup": 240,                   // frames held on the first key before recording (streaming, TAA, probes)
//     "crf": 14,
//     "keys": [ {"t": 0.0, "pos": [x, y, z], "yaw": 0, "pitch": 0, "fov": 50}, ...  // "cut": true starts a new shot
//     ],
//     "lights": [ {"t": 12.5, "circuit": "Kitchen", "on": true}, ... ]          // "circuit": "*" = every circuit
//   }
//
// The render size comes from TARTARUS_SHOT_SIZE (default 1920x1080).
namespace Cinematic {

struct Key {
    float T = 0.0f;
    glm::vec3 Pos{0.0f};
    float Yaw = 0.0f, Pitch = 0.0f, Fov = 50.0f;
    bool Cut = false;      // a hard cut: the spline doesn't run through the previous key
};

struct LightCue {
    float T = 0.0f;
    std::string Circuit;
    bool On = true;
    bool Done = false;
};

struct Pose {
    glm::vec3 Pos{0.0f};
    float Yaw = 0.0f, Pitch = 0.0f, Fov = 50.0f;
};

// Whether TARTARUS_CINEMATIC is set (read once).
bool Active();

// Loads the path; false (with the reason in `error`) when it can't be used.
bool Load(const std::string& path, std::string& error);

// Pure: the pose at time t on keys (sorted by T). Catmull-Rom through positions and angles (yaw unwrapped), each
// shot (from a cut to the next) on its own, eased in and out at its ends.
Pose Evaluate(const std::vector<Key>& keys, float t);

const std::vector<Key>& Keys();
std::vector<LightCue>& Lights();
float Duration();
int Warmup();

// The video: starts ffmpeg (w x h RGBA, top row first) into the --smoke-shots dir; Write sends one frame; Finish
// closes it and reports. Start returns false if ffmpeg won't start.
bool Start(const std::string& shotsDir, int w, int h);
void Write(const std::vector<unsigned char>& rgbaTopDown);
void Finish();
bool Recording();

} // namespace Cinematic
