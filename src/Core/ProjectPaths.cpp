#include "ProjectPaths.h"

#include <filesystem>

namespace ProjectPaths {
namespace {

std::string FindRoot() {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path dir = fs::current_path(ec);
    if (ec) return ".";

    // Walk up looking for a "project" folder. From the usual build/Release/ working directory
    // that's two levels up; the bound just stops this from wandering to the filesystem root on
    // a machine where no such folder exists.
    const int kMaxLevels = 8;
    for (int level = 0; level < kMaxLevels; ++level) {
        fs::path candidate = dir / "project";
        if (fs::is_directory(candidate, ec) && !ec) {
            return candidate.lexically_normal().string();
        }
        fs::path parent = dir.parent_path();
        if (parent.empty() || parent == dir) break; // reached the root
        dir = parent;
    }

    // No project folder anywhere above us — keep the historical behavior (working directory)
    // rather than inventing a location or failing.
    fs::path cwd = fs::current_path(ec);
    return ec ? std::string(".") : cwd.string();
}

} // namespace

const std::string& Root() {
    // Resolved once: the working directory doesn't change during a run, and every call site
    // (scene load/save, preferences) would otherwise redo the same directory walk.
    static const std::string root = FindRoot();
    return root;
}

std::string Resolve(const std::string& name) {
    return (std::filesystem::path(Root()) / name).lexically_normal().string();
}

} // namespace ProjectPaths
