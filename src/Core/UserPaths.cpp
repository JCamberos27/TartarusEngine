#include "UserPaths.h"
#include "Core/Env.h"

#include <filesystem>

namespace UserPaths {
namespace {

std::string& AppName() { static std::string s = "TartarusEngine"; return s; }

std::string FindRoot() {
    namespace fs = std::filesystem;

    std::string base = Env::Str(Env::Var::LocalAppData);

    // LOCALAPPDATA is set for every normal Windows user session; this fallback only matters for
    // an unusual service/sandbox context with no user profile — keep the historical
    // ProjectPaths-style behavior (working directory) rather than inventing a location or
    // failing outright.
    if (base.empty()) {
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        return ec ? std::string(".") : cwd.string();
    }

    fs::path dir = fs::path(base) / AppName();
    std::error_code ec;
    fs::create_directories(dir, ec); // best-effort; Save()/Load() below tolerate it not existing
    return dir.lexically_normal().string();
}

} // namespace

void SetAppName(const std::string& name) { if (!name.empty()) AppName() = name; }

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
