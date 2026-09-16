#pragma once
#include <string>
#include <vector>

// The Capture tool's file + pixel plumbing. Images always land in project/screenshots/ so the
// Asset Browser's "Screenshots" folder can list them. The editor (toolbar / Preferences / the
// PrintScreen key) raises a request; main.cpp does the actual grab from whichever framebuffer
// the chosen mode needs, then calls Save().
namespace Screenshot {
    // Absolute path of the screenshots folder (created on demand).
    std::string Dir();

    // Read `w`x`h` RGBA8 pixels out of the currently-bound READ framebuffer starting at
    // (x, y) in GL's bottom-left-origin convention.
    std::vector<unsigned char> GrabRegion(int x, int y, int w, int h);

    // Write RGBA8 `pixels` (w*h*4 bytes) to project/screenshots/. `flipY` true when the data is
    // GL bottom-left origin (it usually is). `format`: 0 = PNG, 1 = JPG. `sceneName` is folded
    // into the filename ("" -> "untitled"). Returns the written path, or "" on failure.
    std::string Save(const unsigned char* pixels, int w, int h, bool flipY,
                     int format, const std::string& sceneName);

    // Grab the whole default back buffer and Save() it (the "full editor window" mode).
    std::string SaveBackbuffer(int width, int height, int format, const std::string& sceneName);

    // Open the OS file browser with `path` selected (no-op / best effort off Windows).
    void ShowInFolder(const std::string& path);

    // Open `path` in whatever the OS has associated with its extension — the notification card's
    // "Open" action (Phase 3 item 9, audit #5). No-op / best effort off Windows, same as
    // ShowInFolder above.
    void OpenFile(const std::string& path);

    // Path to a small procedurally-generated camera-shutter click WAV (written once, into the
    // screenshots folder as ".shutter.wav"). "" if it couldn't be written.
    std::string ShutterClipPath();

    // Decodes the image at `path` and places it on the OS clipboard as a real image (Phase 6
    // item 7's lightbox "Copy image" action) - Windows CF_DIB, so a paste into another app lands
    // pixels, not a file reference. Returns false on any decode/clipboard failure (logs why);
    // no-op / false off Windows.
    bool CopyImageToClipboard(const std::string& path);
}
