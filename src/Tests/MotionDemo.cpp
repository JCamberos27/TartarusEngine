#include "MotionDemo.h"

#include "gl.h"
#include "Core/Env.h"
#include "Tests/Cinematic.h"
#include "Tests/FfmpegPipe.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

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
    FfmpegPipe pipe;
    std::vector<unsigned char> pixels;
    bool clockOnly = false;
};
State s_State;

} // namespace

bool Active() {
    State& s = s_State;
    if (!s.read) {
        s.read = true;
        s.out = Env::Str(Env::Var::MotionDemo);
        s.ffmpeg = Env::Str(Env::Var::Ffmpeg);
        s.active = !s.out.empty() && !s.ffmpeg.empty();
        s.clockOnly = !s.active && Cinematic::Active(); // a cinematic steps the same 1/60 s clock
    }
    return s.active;
}

void PinClock() {
    if (!Active() && !s_State.clockOnly) return;
    glfwSetTime(1.0 + (double)s_State.frame * kFrame);
    ++s_State.frame;
}

void PinImGuiDelta() {
    if (Active() || s_State.clockOnly) ImGui::GetIO().DeltaTime = (float)kFrame;
}

void StartRecording(int width, int height) {
    State& s = s_State;
    if (!Active() || s.pipe.IsOpen()) return;
    // Bottom-up rows (GL's order), flipped by ffmpeg.
    s.pipe.Open({s.ffmpeg, s.out, width, height, /*FlipY*/ true, "-preset slow -crf 15", "MotionDemo"});
}

void StopRecording() {
    // A logged error (FfmpegPipe::Close) fails the demo run (the editor-test health check), so a broken video is not a pass.
    s_State.pipe.Close();
}

bool Recording() { return s_State.pipe.IsOpen(); }

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
    if (!s.pipe.IsOpen()) return;
    if (width != s.pipe.Width() || height != s.pipe.Height()) { s.pipe.Drop(); return; } // the window can't change size mid-video
    s.pixels.resize((size_t)width * (size_t)height * 4);
    // The default framebuffer reads from its back buffer; RGBA rows are 4-byte aligned already.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, s.pixels.data());
    s.pipe.Write(s.pixels.data(), s.pixels.size());
}

} // namespace MotionDemo
