#pragma once
#include <string>

// Hand a file to the operating system: show it in Explorer, or open it in whatever app is
// associated with its extension. Best effort; a no-op off Windows.
namespace OsShell {
    // Open the OS file browser with `path` selected.
    void ShowInFolder(const std::string& path);
    // Open `path` with the app the OS associates with its extension.
    void OpenFile(const std::string& path);
}
