#pragma once
#include <string>

// Grab the current back buffer (the fully composited editor frame — scene + panels + overlays)
// and write it to project/screenshots/shot_<timestamp>.png. Returns the file path, or "" on
// failure. Call between the last draw of a frame and SwapBuffers().
namespace Screenshot {
    std::string SaveBackbuffer(int width, int height);
}
