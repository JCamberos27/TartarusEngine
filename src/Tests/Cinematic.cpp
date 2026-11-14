#include "Tests/Cinematic.h"

#include "Core/Env.h"
#include "Log.h"

#include <json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace Cinematic {

namespace {
struct State {
    bool read = false, active = false;
    std::vector<Key> keys;
    std::vector<LightCue> lights;
    std::string out = "cinematic.mp4", ffmpeg;
    int warmup = 240, crf = 14;
    FILE* pipe = nullptr;
    int w = 0, h = 0;
    long long written = 0;
    std::string path;
};
State s_State;

float CatmullRom(float p0, float p1, float p2, float p3, float u) {
    const float u2 = u * u, u3 = u2 * u;
    return 0.5f * (2.0f * p1 + (-p0 + p2) * u + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * u3);
}

// The unwrapped yaw of every key: each within half a turn of the one before, so the camera turns the short way.
std::vector<float> UnwrappedYaws(const std::vector<Key>& keys) {
    std::vector<float> y(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        y[i] = keys[i].Yaw;
        if (i > 0 && !keys[i].Cut) {
            while (y[i] - y[i - 1] > 180.0f) y[i] -= 360.0f;
            while (y[i] - y[i - 1] < -180.0f) y[i] += 360.0f;
        }
    }
    return y;
}
} // namespace

bool Active() {
    State& s = s_State;
    if (!s.read) {
        s.read = true;
        s.path = Env::Str(Env::Var::Cinematic);
        s.active = !s.path.empty();
    }
    return s.active;
}

bool Load(const std::string& path, std::string& error) {
    State& s = s_State;
    std::ifstream f(path);
    if (!f) { error = "can't open " + path; return false; }
    nlohmann::json j;
    try {
        f >> j;
        s.keys.clear();
        s.lights.clear();
        s.out = j.value("out", s.out);
        s.ffmpeg = j.value("ffmpeg", Env::Str(Env::Var::Ffmpeg));
        s.warmup = std::max(1, j.value("warmup", s.warmup));
        s.crf = j.value("crf", s.crf);
        float fov = 50.0f;
        for (const auto& k : j.at("keys")) {
            Key key;
            key.T = k.at("t").get<float>();
            const auto& p = k.at("pos");
            key.Pos = glm::vec3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
            key.Yaw = k.value("yaw", 0.0f);
            key.Pitch = k.value("pitch", 0.0f);
            fov = key.Fov = k.value("fov", fov);   // a key without one keeps the last
            key.Cut = k.value("cut", false);
            s.keys.push_back(key);
        }
        if (j.contains("lights"))
            for (const auto& l : j.at("lights"))
                s.lights.push_back({l.at("t").get<float>(), l.at("circuit").get<std::string>(), l.value("on", true), false});
    } catch (const std::exception& e) {
        error = std::string("bad cinematic json: ") + e.what();
        return false;
    }
    std::stable_sort(s.keys.begin(), s.keys.end(), [](const Key& a, const Key& b) { return a.T < b.T; });
    if (s.keys.size() < 2) { error = "a cinematic needs at least two keys"; return false; }
    if (s.ffmpeg.empty()) { error = "no ffmpeg (\"ffmpeg\" in the json or TARTARUS_FFMPEG)"; return false; }
    return true;
}

Pose Evaluate(const std::vector<Key>& keys, float t) {
    Pose out;
    if (keys.empty()) return out;
    const auto pose = [](const Key& k, float yaw) { Pose p; p.Pos = k.Pos; p.Yaw = yaw; p.Pitch = k.Pitch; p.Fov = k.Fov; return p; };
    const std::vector<float> yaw = UnwrappedYaws(keys);
    if (t <= keys.front().T) return pose(keys.front(), yaw.front());
    if (t >= keys.back().T) return pose(keys.back(), yaw.back());
    size_t i = 0;                                        // the segment keys[i] -> keys[i + 1]
    while (i + 1 < keys.size() && keys[i + 1].T <= t) ++i;
    if (keys[i + 1].Cut) return pose(keys[i], yaw[i]);   // holding the shot's last key until the cut
    // the shot this segment belongs to: from its cut (or the first key) to the key before the next cut
    size_t a = i, b = i + 1;
    while (a > 0 && !keys[a].Cut) --a;
    while (b + 1 < keys.size() && !keys[b + 1].Cut) ++b;
    const float span = std::max(keys[i + 1].T - keys[i].T, 1e-4f);
    float u = std::clamp((t - keys[i].T) / span, 0.0f, 1.0f);
    const bool first = i == a, last = i + 1 == b;
    if (first && last) u = u * u * (3.0f - 2.0f * u);   // one segment: ease in and out
    else if (first) u = 2.0f * u * u - u * u * u;        // ease in: starts still, leaves at the spline's speed
    else if (last) u = u + u * u - u * u * u;            // ease out: arrives at the spline's speed, ends still
    const size_t i0 = i > a ? i - 1 : i, i3 = i + 2 <= b ? i + 2 : i + 1;
    const Key &k0 = keys[i0], &k1 = keys[i], &k2 = keys[i + 1], &k3 = keys[i3];
    for (int c = 0; c < 3; ++c) out.Pos[c] = CatmullRom(k0.Pos[c], k1.Pos[c], k2.Pos[c], k3.Pos[c], u);
    out.Yaw = CatmullRom(yaw[i0], yaw[i], yaw[i + 1], yaw[i3], u);
    out.Pitch = CatmullRom(k0.Pitch, k1.Pitch, k2.Pitch, k3.Pitch, u);
    out.Fov = CatmullRom(k0.Fov, k1.Fov, k2.Fov, k3.Fov, u);
    return out;
}

const std::vector<Key>& Keys() { return s_State.keys; }
std::vector<LightCue>& Lights() { return s_State.lights; }
float Duration() { return s_State.keys.empty() ? 0.0f : s_State.keys.back().T; }
int Warmup() { return s_State.warmup; }

bool Start(const std::string& shotsDir, int w, int h) {
    State& s = s_State;
    if (s.pipe) return true;
    std::error_code ec;
    std::filesystem::create_directories(shotsDir, ec);
    const std::string out = (std::filesystem::path(shotsDir) / s.out).string();
    char size[32];
    std::snprintf(size, sizeof(size), "%dx%d", w, h);
    // Raw RGBA in, top row first; H.264 out at exactly 60 fps, quality-first (a trailer, not a test artifact).
    const std::string cmd = "\"\"" + s.ffmpeg + "\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s " + size +
                            " -framerate 60 -i - -c:v libx264 -preset slow -tune film -crf " + std::to_string(s.crf) +
                            " -pix_fmt yuv420p -r 60 -movflags +faststart \"" + out + "\"\"";
    s.pipe = _popen(cmd.c_str(), "wb");
    if (!s.pipe) { Log::Error("Cinematic: could not start ffmpeg"); return false; }
    s.w = w;
    s.h = h;
    std::cout << "[Cinematic] recording " << size << " @ 60 fps, " << Duration() << " s -> " << out << std::endl;
    return true;
}

void Write(const std::vector<unsigned char>& rgba) {
    State& s = s_State;
    if (!s.pipe || rgba.size() != (size_t)s.w * (size_t)s.h * 4) return;
    if (std::fwrite(rgba.data(), 1, rgba.size(), s.pipe) == rgba.size()) ++s.written;
}

void Finish() {
    State& s = s_State;
    if (!s.pipe) return;
    const int rc = _pclose(s.pipe);
    s.pipe = nullptr;
    std::cout << "[Cinematic] " << s.written << " frames (" << (double)s.written / 60.0 << " s), ffmpeg exit " << rc
              << std::endl;
}

bool Recording() { return s_State.pipe != nullptr; }

} // namespace Cinematic
