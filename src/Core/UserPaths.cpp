#include "UserPaths.h"

#include <filesystem>
#include <cstdlib>

namespace UserPaths {
namespace {

std::string FindRoot() {
    namespace fs = std::filesystem;

    // _dupenv_s rather than getenv: MSVC's secure-CRT form, thread-safe and doesn't trip
    // C4996 (see GLFramebufferCheck.cpp's own getenv warning for the alternative).
    char* buf = nullptr;
    std::size_t len = 0;
    std::string base;
    if (_dupenv_s(&buf, &len, "LOCALAPPDATA") == 0 && buf) {
        base = buf;
        std::free(buf);
    }

    // LOCALAPPDATA is set for every normal Windows user session; this fallback only matters for
    // an unusual service/sandbox context with no user profile — keep the historical
    // ProjectPaths-style behavior (working directory) rather than inventing a location or
    // failing outright.
    if (base.empty()) {
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        return ec ? std::string(".") : cwd.string();
    }

    fs::path dir = fs::path(base) / "TartarusEngine";
    std::error_code ec;
    fs::create_directories(dir, ec); // best-effort; Save()/Load() below tolerate it not existing
    return dir.lexically_normal().string();
}

} // namespace

const std::string& Root() {
    // Resolved once: the environment doesn't change during a run, and every call site
    // (preferences, shortcuts) would otherwise redo the same lookup.
    static const std::string root = FindRoot();
    return root;
}

std::string Resolve(const std::string& name) {
    return (std::filesystem::path(Root()) / name).lexically_normal().string();
}

} // namespace UserPaths
