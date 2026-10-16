#include "MotionDemo.h"

#include "gl.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace MotionDemo {

namespace {
constexpr double kFrame = 1.0 / 60.0;

struct State {
    bool   read = false;
    bool   active = false;
    std::string out, ffmpeg;
    long long frame = 0;
    FILE*  pipe = nullptr;
    int    w = 0, h = 0;
    std::vector<unsigned char> pixels;
    long long written = 0;
};
State s_State;

std::string Env(const char* name) {
    char* v = nullptr;
    size_t n = 0;
    std::string out;
    if (_dupenv_s(&v, &n, name) == 0 && v) { out = v; std::free(v); }
    return out;
}
} // namespace

bool Active() {
    State& s = s_State;
    if (!s.read) {
        s.read = true;
        s.out = Env("TARTARUS_MOTION_DEMO");
        s.ffmpeg = Env("TARTARUS_FFMPEG");
        s.active = !s.out.empty() && !s.ffmpeg.empty();
    }
    return s.active;
}

void PinClock() {
    if (!Active()) return;
    glfwSetTime(1.0 + (double)s_State.frame * kFrame);
    ++s_State.frame;
}

void PinImGuiDelta() {
    if (Active()) ImGui::GetIO().DeltaTime = (float)kFrame;
}

void StartRecording(int width, int height) {
    State& s = s_State;
    if (!Active() || s.pipe || width <= 0 || height <= 0) return;
    s.w = width;
    s.h = height;
    // Raw RGBA in, bottom-up (GL's order, flipped by ffmpeg), H.264 out at exactly 60 fps.
    char size[32];
    std::snprintf(size, sizeof(size), "%dx%d", width, height);
    const std::string cmd = "\"\"" + s.ffmpeg + "\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s " + size +
                            " -framerate 60 -i - -vf vflip -c:v libx264 -preset slow -crf 15 -pix_fmt yuv420p"
                            " -r 60 -movflags +faststart \"" + s.out + "\"\"";
    s.pipe = _popen(cmd.c_str(), "wb");
    if (!s.pipe) std::cerr << "[MotionDemo] could not start ffmpeg" << std::endl;
    else std::cout << "[MotionDemo] recording " << size << " @ 60 fps -> " << s.out << std::endl;
}

void StopRecording() {
    State& s = s_State;
    if (!s.pipe) return;
    const int rc = _pclose(s.pipe);
    s.pipe = nullptr;
    std::cout << "[MotionDemo] " << s.written << " frames (" << (double)s.written / 60.0 << " s), ffmpeg exit " << rc << std::endl;
}

bool Recording() { return s_State.pipe != nullptr; }

void DrawCursor() {
    if (!Recording()) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMousePosValid(&io.MousePos)) return;
    // A standard arrow, white with a dark outline, its tip on the pointer. Slightly smaller while
    // the button is held, so presses read on video.
    const float k = (io.MouseDown[0] ? 0.88f : 1.0f) * 1.15f;
    const ImVec2 p = io.MousePos;
    const ImVec2 pts[7] = {
        ImVec2(p.x, p.y), ImVec2(p.x, p.y + 17 * k), ImVec2(p.x + 4.2f * k, p.y + 13.2f * k), ImVec2(p.x + 7.4f * k, p.y + 20 * k),
        ImVec2(p.x + 10 * k, p.y + 18.8f * k), ImVec2(p.x + 6.9f * k, p.y + 12.2f * k), ImVec2(p.x + 12 * k, p.y + 12 * k),
    };
    ImDrawList* dl = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    dl->AddConcavePolyFilled(pts, 7, IM_COL32(255, 255, 255, 255));
    dl->AddPolyline(pts, 7, IM_COL32(0, 0, 0, 230), ImDrawFlags_Closed, 1.3f);
}

void CaptureFrame(int width, int height) {
    State& s = s_State;
    if (!s.pipe) return;
    if (width != s.w || height != s.h) return; // the window can't change size mid-video
    s.pixels.resize((size_t)width * (size_t)height * 4);
    // The default framebuffer reads from its back buffer; RGBA rows are 4-byte aligned already.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, s.pixels.data());
    if (std::fwrite(s.pixels.data(), 1, s.pixels.size(), s.pipe) == s.pixels.size()) ++s.written;
}

} // namespace MotionDemo
