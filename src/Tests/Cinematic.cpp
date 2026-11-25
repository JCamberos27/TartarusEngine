#include "Tests/Cinematic.h"

#include "Core/Env.h"
#include "Tests/FfmpegPipe.h"
#include "Log.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace Cinematic {

namespace {
struct State {
    bool read = false, active = false;
    std::vector<Key> keys;
    std::vector<LightCue> lights;
    std::vector<SunKey> sun;
    std::vector<MoverCue> movers;
    std::string out = "cinematic.mp4", ffmpeg;
    int warmup = 240, crf = 14;
    FfmpegPipe pipe;
    std::string path;
    int rc = -1; // ffmpeg's exit code once Finish closed the pipe; -1 before
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
        s.sun.clear();
        s.movers.clear();
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
        if (j.contains("sun"))
            for (const auto& k : j.at("sun")) s.sun.push_back({k.at("t").get<float>(), k.at("hours").get<float>()});
        std::stable_sort(s.sun.begin(), s.sun.end(), [](const SunKey& a, const SunKey& b) { return a.T < b.T; });
        if (j.contains("movers"))
            for (const auto& m : j.at("movers")) {
                MoverCue c;
                c.Name = m.at("name").get<std::string>();
                const auto& n = m.at("near");
                c.Near = glm::vec3(n[0].get<float>(), n[1].get<float>(), n[2].get<float>());
                const auto& o = m.at("offset");
                c.Offset = glm::vec3(o[0].get<float>(), o[1].get<float>(), o[2].get<float>());
                c.Period = std::max(m.value("period", c.Period), 0.05f);
                c.T0 = m.value("t0", 0.0f);
                c.T1 = m.value("t1", 1e9f);
                s.movers.push_back(c);
            }
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
bool SunHours(float t, float& hours) {
    const std::vector<SunKey>& k = s_State.sun;
    if (k.empty()) return false;
    if (t <= k.front().T) { hours = k.front().Hours; return true; }
    if (t >= k.back().T) { hours = k.back().Hours; return true; }
    size_t i = 0;
    while (i + 1 < k.size() && k[i + 1].T <= t) ++i;
    const float u = (t - k[i].T) / std::max(k[i + 1].T - k[i].T, 1e-4f);
    hours = k[i].Hours + (k[i + 1].Hours - k[i].Hours) * u;
    return true;
}
const std::vector<MoverCue>& Movers() { return s_State.movers; }
glm::vec3 MoverOffset(const MoverCue& m, float t) {
    if (t < m.T0 || t > m.T1) return glm::vec3(0.0f);
    return m.Offset * std::sin(6.2831853f * (t - m.T0) / m.Period);
}
float Duration() { return s_State.keys.empty() ? 0.0f : s_State.keys.back().T; }
int Warmup() { return s_State.warmup; }

bool Start(const std::string& shotsDir, int w, int h) {
    State& s = s_State;
    if (s.pipe.IsOpen()) return true;
    std::error_code ec;
    std::filesystem::create_directories(shotsDir, ec);
    const std::string out = (std::filesystem::path(shotsDir) / s.out).string();
    // Top row first; quality-first encode (a trailer, not a test artifact).
    if (!s.pipe.Open({s.ffmpeg, out, w, h, /*FlipY*/ false, "-preset slow -tune film -crf " + std::to_string(s.crf), "Cinematic"}))
        return false;
    std::cout << "[Cinematic] " << Duration() << " s of keys" << std::endl;
    return true;
}

void Write(const std::vector<unsigned char>& rgba) {
    State& s = s_State;
    s.pipe.Write(rgba.data(), rgba.size()); // a frame of another size (the framebuffer changed mid-clip) counts as dropped
}

bool Finish() {
    State& s = s_State;
    if (s.pipe.IsOpen()) s.rc = s.pipe.Close(); // logs a broken clip
    return s.rc == 0 && s.pipe.Dropped() == 0 && s.pipe.Written() > 0;
}


} // namespace Cinematic
