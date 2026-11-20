#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

// Raw RGBA frames piped into ffmpeg, H.264 out at exactly 60 fps: the shared half of the MotionDemo
// and Cinematic recorders. Each frame must be exactly W x H x 4 bytes; any other size, or a write
// the pipe doesn't take (ffmpeg gone, disk full), counts as dropped. Close() logs an error (which
// fails a harness run) when ffmpeg exits non-zero or a frame was dropped, so a broken clip is never
// a pass.
class FfmpegPipe {
public:
    struct Options {
        std::string Ffmpeg;  // ffmpeg.exe
        std::string Out;     // the .mp4
        int W = 0, H = 0;
        bool FlipY = false;  // frames arrive bottom row first (glReadPixels' order)
        std::string Encode;  // libx264 settings, e.g. "-preset slow -crf 15"
        std::string Tag;     // log prefix, e.g. "MotionDemo"
    };

    FfmpegPipe() = default;
    FfmpegPipe(const FfmpegPipe&) = delete;
    FfmpegPipe& operator=(const FfmpegPipe&) = delete;
    ~FfmpegPipe() { Close(); }

    bool Open(const Options& o);
    bool IsOpen() const { return m_Pipe != nullptr; }
    void Write(const void* rgba, std::size_t bytes);
    void Drop() { if (m_Pipe) ++m_Dropped; } // a frame that couldn't be captured
    // Closes the pipe and returns ffmpeg's exit code (-1 if it was never open).
    int Close();

    long long Written() const { return m_Written; }
    long long Dropped() const { return m_Dropped; }
    int Width() const { return m_Opt.W; }
    int Height() const { return m_Opt.H; }

private:
    Options m_Opt;
    FILE* m_Pipe = nullptr;
    long long m_Written = 0, m_Dropped = 0;
};
