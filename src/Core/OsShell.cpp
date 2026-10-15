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

// stb_image_write's one implementation in the engine (main.cpp's test-harness PNG / JPG writers use it).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996) // stb_image_write.h's own sprintf() use, not this file's
#endif
#include <stb_image_write.h>
#ifdef _MSC_VER
#pragma warning(pop)
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
