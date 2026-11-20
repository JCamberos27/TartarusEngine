#include "Tests/FfmpegPipe.h"

#include "Log.h"

#include <iostream>

bool FfmpegPipe::Open(const Options& o) {
    if (m_Pipe) return true;
    if (o.W <= 0 || o.H <= 0) return false;
    m_Opt = o;
    m_Written = m_Dropped = 0;
    const std::string size = std::to_string(o.W) + "x" + std::to_string(o.H);
    // yuv420p needs even sizes: an odd width or height is cropped by one pixel, or libx264 quits after a few frames.
    const std::string vf = std::string(o.FlipY ? "vflip," : "") + "crop=trunc(iw/2)*2:trunc(ih/2)*2";
    // The outer quotes are for cmd.exe, which _popen runs the line through.
    const std::string cmd = "\"\"" + o.Ffmpeg + "\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s " + size +
                            " -framerate 60 -i - -vf " + vf + " -c:v libx264 " + o.Encode +
                            " -pix_fmt yuv420p -r 60 -movflags +faststart \"" + o.Out + "\"\"";
    m_Pipe = _popen(cmd.c_str(), "wb");
    if (!m_Pipe) {
        Log::Error(o.Tag + ": could not start ffmpeg");
        return false;
    }
    std::cout << "[" << o.Tag << "] recording " << size << " @ 60 fps -> " << o.Out << std::endl;
    return true;
}

void FfmpegPipe::Write(const void* rgba, std::size_t bytes) {
    if (!m_Pipe) return;
    if (bytes != (std::size_t)m_Opt.W * (std::size_t)m_Opt.H * 4) { ++m_Dropped; return; } // the size changed mid-clip
    if (std::fwrite(rgba, 1, bytes, m_Pipe) == bytes) ++m_Written;
    else ++m_Dropped;
}

int FfmpegPipe::Close() {
    if (!m_Pipe) return -1;
    const int rc = _pclose(m_Pipe);
    m_Pipe = nullptr;
    std::cout << "[" << m_Opt.Tag << "] " << m_Written << " frames (" << (double)m_Written / 60.0 << " s), ffmpeg exit " << rc
              << std::endl;
    if (rc != 0 || m_Dropped > 0)
        Log::Error(m_Opt.Tag + ": ffmpeg exit " + std::to_string(rc) + ", " + std::to_string(m_Dropped) +
                   " frame(s) not written - the clip is incomplete");
    else if (m_Written == 0)
        Log::Error(m_Opt.Tag + ": no frame was recorded");
    return rc;
}
