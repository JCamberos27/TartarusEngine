#include "Core/ModuleCopies.h"

#include <cwctype>
#include <system_error>
#include <windows.h>

namespace ModuleCopies {

std::optional<std::uint32_t> OwnerPid(const std::wstring& fileName, const std::wstring& prefix) {
    if (fileName.rfind(prefix, 0) != 0) return std::nullopt;
    const size_t start = prefix.size();
    const size_t end = fileName.find(L'_', start);
    if (end == std::wstring::npos || end == start || end - start > 10) return std::nullopt;
    std::uint64_t pid = 0;
    for (size_t i = start; i < end; ++i) {
        if (!std::iswdigit(fileName[i])) return std::nullopt;
        pid = pid * 10 + (fileName[i] - L'0');
    }
    if (pid > 0xFFFFFFFFull) return std::nullopt;
    return static_cast<std::uint32_t>(pid);
}

namespace {
bool Running(std::uint32_t pid) {
    if (pid == ::GetCurrentProcessId()) return true;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return ::GetLastError() == ERROR_ACCESS_DENIED; // exists, just not ours to query
    DWORD code = 0;
    const bool alive = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    ::CloseHandle(h);
    return alive;
}
} // namespace

void SweepOrphans(const std::filesystem::path& dir, const std::wstring& prefix) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(prefix, 0) != 0 || entry.path().extension() != L".dll") continue;
        if (const auto pid = OwnerPid(name, prefix); pid && Running(*pid)) continue;
        std::error_code rmEc;
        std::filesystem::remove(entry.path(), rmEc); // still locked by a live process: fails, left alone
    }
}

} // namespace ModuleCopies
