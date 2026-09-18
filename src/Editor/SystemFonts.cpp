#include "SystemFonts.h"

#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#endif

std::string SystemFonts::Path(const char* fileName) {
#ifdef _WIN32
    static const std::filesystem::path fontDir = [] {
        std::filesystem::path dir;
        PWSTR w = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &w)) && w) dir = w;
        CoTaskMemFree(w);
        return dir;
    }();
    if (fontDir.empty() || !fileName) return {};
    const std::filesystem::path full = fontDir / fileName;
    std::error_code ec;
    if (!std::filesystem::exists(full, ec)) return {};
    // UTF-8: ImGui's file loader (ImFileOpen) widens with CP_UTF8.
    const std::wstring ws = full.wstring();
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), out.data(), n, nullptr, nullptr);
    return out;
#else
    (void)fileName;
    return {};
#endif
}
