#include "Tests/GiTour.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace GiTour {

namespace {

using nlohmann::json;

bool Finite(float v) { return std::isfinite(v); }

bool ReadNumber(const json& o, const char* key, float& out, std::string& error, const std::string& where) {
    if (!o.contains(key)) return true; // keep the default
    if (!o[key].is_number()) { error = where + ": '" + key + "' is not a number"; return false; }
    out = o[key].get<float>();
    if (!Finite(out)) { error = where + ": '" + key + "' is not finite"; return false; }
    return true;
}

std::string ReadString(const json& o, const char* key) {
    return o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string();
}

float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

} // namespace

float LerpAngle(float a, float b, float t) {
    float d = std::fmod(b - a, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return a + d * t;
}

bool Parse(const std::string& text, Tour& out, std::string& error, std::vector<std::string>* warnings) {
    out = Tour{};
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (doc.is_discarded() || !doc.is_object()) { error = "not a JSON object"; return false; }
    if (doc.contains("version") && (!doc["version"].is_number_integer() || doc["version"].get<int>() != 1)) {
        error = "unsupported 'version' (this tool reads version 1)";
        return false;
    }
    out.Scene = ReadString(doc, "scene");
    if (out.Scene.empty()) { error = "no 'scene'"; return false; }
    if (doc.contains("player")) {
        const json& p = doc["player"];
        if (!p.is_object()) { error = "'player' is not an object"; return false; }
        if (!ReadNumber(p, "eyeHeight", out.EyeHeight, error, "player") ||
            !ReadNumber(p, "verticalFovDeg", out.VerticalFovDeg, error, "player") ||
            !ReadNumber(p, "walkSpeed", out.WalkSpeed, error, "player"))
            return false;
    }
    if (out.WalkSpeed <= 0.0f) { error = "player.walkSpeed must be above 0"; return false; }
    if (out.VerticalFovDeg <= 1.0f || out.VerticalFovDeg >= 179.0f) { error = "player.verticalFovDeg out of range"; return false; }
    if (doc.value("snapToFloor", false) && warnings)
        warnings->push_back("snapToFloor is ignored: the camera sits at feet.y + eyeHeight as given");

    if (!doc.contains("waypoints") || !doc["waypoints"].is_array() || doc["waypoints"].empty()) {
        error = "no 'waypoints'";
        return false;
    }
    std::unordered_map<std::string, int> byId;
    for (const json& w : doc["waypoints"]) {
        const std::string where = "waypoint " + std::to_string(out.Waypoints.size());
        if (!w.is_object()) { error = where + " is not an object"; return false; }
        Waypoint wp;
        wp.Id = ReadString(w, "id");
        if (wp.Id.empty()) { error = where + " has no 'id'"; return false; }
        if (byId.count(wp.Id)) { error = "waypoint id '" + wp.Id + "' is used twice"; return false; }
        wp.Room = ReadString(w, "room");
        wp.GiVolume = ReadString(w, "giVolume");
        wp.Kind = ReadString(w, "kind");
        const json* feet = w.contains("feet") ? &w["feet"] : nullptr;
        if (!feet || !feet->is_array() || feet->size() != 3 || !(*feet)[0].is_number() || !(*feet)[1].is_number() ||
            !(*feet)[2].is_number()) {
            error = "waypoint '" + wp.Id + "': 'feet' must be [x, y, z]";
            return false;
        }
        const glm::vec3 f((*feet)[0].get<float>(), (*feet)[1].get<float>(), (*feet)[2].get<float>());
        if (!Finite(f.x) || !Finite(f.y) || !Finite(f.z)) { error = "waypoint '" + wp.Id + "': 'feet' is not finite"; return false; }
        wp.Eye = f + glm::vec3(0.0f, out.EyeHeight, 0.0f);
        if (!ReadNumber(w, "yawDeg", wp.Yaw, error, "waypoint '" + wp.Id + "'") ||
            !ReadNumber(w, "pitchDeg", wp.Pitch, error, "waypoint '" + wp.Id + "'"))
            return false;
        wp.Pitch = std::clamp(wp.Pitch, -89.0f, 89.0f);
        wp.Cut = w.value("cut", false);
        byId[wp.Id] = (int)out.Waypoints.size();
        out.Waypoints.push_back(std::move(wp));
    }

    if (doc.contains("routes")) {
        if (!doc["routes"].is_array()) { error = "'routes' is not an array"; return false; }
        for (const json& r : doc["routes"]) {
            Route route;
            route.Id = ReadString(r, "id");
            const std::string where = "route '" + route.Id + "'";
            if (route.Id.empty()) { error = "a route has no 'id'"; return false; }
            route.Name = ReadString(r, "name");
            if (!ReadNumber(r, "speed", route.Speed, error, where)) return false;
            if (route.Speed < 0.0f) { error = where + ": 'speed' is negative"; return false; }
            route.FaceWaypoints = r.value("faceWaypoints", false);
            if (!r.contains("points") || !r["points"].is_array() || r["points"].size() < 2) {
                error = where + " needs at least 2 'points'";
                return false;
            }
            for (const json& p : r["points"]) {
                const auto it = p.is_string() ? byId.find(p.get<std::string>()) : byId.end();
                if (it == byId.end()) { error = where + " names an unknown waypoint " + p.dump(); return false; }
                route.Points.push_back(it->second);
            }
            out.Routes.push_back(std::move(route));
        }
    }
    return true;
}

bool Load(const std::string& path, Tour& out, std::string& error, std::vector<std::string>* warnings) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "can't open " + path; return false; }
    std::stringstream ss;
    ss << in.rdbuf();
    if (!Parse(ss.str(), out, error, warnings)) { error = path + ": " + error; return false; }
    return true;
}

std::vector<Frame> BuildTimeline(const Tour& tour, const TimelineOptions& opt) {
    std::vector<Frame> frames;
    const int fps = std::max(opt.Fps, 1);
    auto hold = [&](Step kind, const Waypoint& w, int index, int count, const std::string& segment) {
        for (int i = 0; i < count; ++i) frames.push_back({kind, w.Eye, w.Yaw, w.Pitch, index, segment});
    };
    // Walks a -> b at `speed`, one frame per 1/fps s, ending on b's position. lookAlong: face the direction of travel
    // (pitch 0); else blend a's view into b's.
    auto walk = [&](const Waypoint& a, const Waypoint& b, int toIndex, float speed, bool lookAlong, const std::string& segment) {
        const glm::vec3 d = b.Eye - a.Eye;
        const float len = glm::length(d);
        const int n = std::max(1, (int)std::ceil(len / speed * (float)fps));
        float travelYaw = a.Yaw;
        if (len > 1e-4f && (std::abs(d.x) > 1e-4f || std::abs(d.z) > 1e-4f))
            travelYaw = glm::degrees(std::atan2(d.z, d.x)); // front = (cos yaw, ., sin yaw)
        for (int i = 1; i <= n; ++i) {
            const float t = (float)i / (float)n;
            Frame f{Step::Walk, a.Eye + d * t, 0.0f, 0.0f, toIndex, segment};
            if (lookAlong) { f.Yaw = travelYaw; f.Pitch = 0.0f; }
            else { const float s = Smooth(t); f.Yaw = LerpAngle(a.Yaw, b.Yaw, s); f.Pitch = a.Pitch + (b.Pitch - a.Pitch) * s; }
            frames.push_back(f);
        }
    };

    for (int i = 0; i < (int)tour.Waypoints.size(); ++i) {
        const Waypoint& w = tour.Waypoints[i];
        if (i > 0 && !w.Cut) {
            const Waypoint& prev = tour.Waypoints[i - 1];
            walk(prev, w, i, tour.WalkSpeed, /*lookAlong=*/false, "walk:" + prev.Id + "->" + w.Id);
        }
        hold(Step::Settle, w, i, opt.ArriveSettle, "settle:" + w.Id);
        hold(Step::ShotFull, w, i, 1, "shot:" + w.Id);
        hold(Step::Settle, w, i, opt.ViewSettle, "settle:" + w.Id);
        hold(Step::ShotGi, w, i, 1, "shot:" + w.Id);
        hold(Step::Settle, w, i, opt.ViewSettle, "settle:" + w.Id);
        hold(Step::Still, w, i, opt.StillFrames, "still:" + w.Id);
    }
    for (const Route& r : tour.Routes) {
        const float speed = r.Speed > 0.0f ? r.Speed : tour.WalkSpeed;
        const Waypoint& first = tour.Waypoints[r.Points.front()];
        hold(Step::Settle, first, r.Points.front(), opt.ArriveSettle, "settle:route:" + r.Id);
        for (size_t k = 1; k < r.Points.size(); ++k) {
            const Waypoint& a = tour.Waypoints[r.Points[k - 1]];
            const Waypoint& b = tour.Waypoints[r.Points[k]];
            walk(a, b, r.Points[k], speed, !r.FaceWaypoints, "route:" + r.Id + ":" + a.Id + "->" + b.Id);
        }
    }
    return frames;
}

double MaskedMeanLuminance(const unsigned char* rgba, const float* depth, int w, int h, int stride, float farDepth,
                           int* maskCount) {
    static float s_Linear[256];
    static const bool s_Init = [] {
        for (int i = 0; i < 256; ++i) {
            const float c = (float)i / 255.0f;
            s_Linear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return true;
    }();
    (void)s_Init;
    stride = std::max(stride, 1);
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < h; y += stride)
        for (int x = 0; x < w; x += stride) {
            const size_t i = (size_t)y * (size_t)w + (size_t)x;
            if (!(depth[i] < farDepth)) continue;
            const unsigned char* p = rgba + i * 4;
            sum += 0.2126 * s_Linear[p[0]] + 0.7152 * s_Linear[p[1]] + 0.0722 * s_Linear[p[2]];
            ++n;
        }
    if (maskCount) *maskCount = n;
    return n > 0 ? sum / n : -1.0;
}

std::uint64_t SlotSetHash(std::vector<std::uint32_t> spots, std::vector<std::uint32_t> points) {
    std::sort(spots.begin(), spots.end());
    std::sort(points.begin(), points.end());
    std::uint64_t h = 1469598103934665603ull; // FNV-1a
    auto mix = [&](std::uint32_t v) {
        for (int b = 0; b < 4; ++b) { h ^= (v >> (8 * b)) & 0xFFu; h *= 1099511628211ull; }
    };
    for (std::uint32_t v : spots) mix(v);
    mix(0xFFFFFFFFu); // the separator: a spot and a point holder never hash alike
    for (std::uint32_t v : points) mix(v);
    return h;
}

std::string CsvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string q = "\"";
    for (char c : s) { if (c == '"') q += '"'; q += c; }
    return q + "\"";
}

std::string CsvHeader() { return "frame,mode,segment,waypoint,room,luminance,mask_pixels,slot_hash,slots_changed"; }

std::string CsvLine(const CsvRow& r) {
    char lum[32], hash[24];
    std::snprintf(lum, sizeof(lum), "%.6f", r.Luminance);
    std::snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)r.SlotHash);
    return std::to_string(r.Frame) + "," + CsvField(r.Mode) + "," + CsvField(r.Segment) + "," + CsvField(r.Waypoint) + "," +
           CsvField(r.Room) + "," + lum + "," + std::to_string(r.MaskPixels) + "," + hash + "," + (r.SlotsChanged ? "1" : "0");
}

} // namespace GiTour
