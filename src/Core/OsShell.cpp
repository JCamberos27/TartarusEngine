#include "OsShell.h"

#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace OsShell {

void ShowInFolder(const std::string& path) {
#if defined(_WIN32)
    std::string arg = "/select,\"" + std::filesystem::path(path).make_preferred().string() + "\"";
    ShellExecuteA(nullptr, "open", "explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
#else
    (void)path;
#endif
}

void OpenFile(const std::string& path) {
#if defined(_WIN32)
    const std::string native = std::filesystem::path(path).make_preferred().string();
    ShellExecuteA(nullptr, "open", native.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)path;
#endif
}

} // namespace OsShell
