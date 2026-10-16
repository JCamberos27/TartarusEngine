#pragma once
// The Glass motion demo recorder (--editor-tests "motion demo"). Off unless TARTARUS_MOTION_DEMO
// names an output .mp4 and TARTARUS_FFMPEG an ffmpeg executable.
//
// While active, the run steps on a virtual clock: every frame is exactly 1/60 s after the last,
// however long it took to render and capture, so the video plays back at a true 60 fps with no
// dropped or stretched frames. While recording, each finished frame is read back and piped to
// ffmpeg as raw RGBA, and a drawn cursor shows where the harness's synthetic mouse is.

namespace MotionDemo {

// Reads the environment once. True when a demo run was asked for.
bool Active();

// main.cpp, at the top of every frame (before Time::BeginFrame): pins the clock to the next 1/60 s.
void PinClock();
// The harness's input hook: ImGui's delta for this frame, exactly 1/60 s.
void PinImGuiDelta();

void StartRecording(int width, int height);
void StopRecording();
bool Recording();

// main.cpp, right before the editor's ImGui frame is rendered: the demo cursor.
void DrawCursor();
// main.cpp, after the frame is drawn and before the swap: appends it to the video.
void CaptureFrame(int width, int height);

} // namespace MotionDemo
