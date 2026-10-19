#include "ProjectPaths.h"
#include "EnginePaths.h"

#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace ProjectPaths {
namespace {

namespace fs = std::filesystem;

// Walk up from `dir` looking for a "project" folder. From the usual build/Release/ exe
// directory that's two levels up; the bound just stops this from wandering to the filesystem
// root on a machine where no such folder exists. Empty if none.
std::string FindProjectAbove(fs::path dir) {
    std::error_code ec;
    const int kMaxLevels = 8;
    for (int level = 0; level < kMaxLevels && !dir.empty(); ++level) {
        fs::path candidate = dir / "project";
        if (fs::is_directory(candidate, ec) && !ec) return candidate.lexically_normal().string();
        fs::path parent = dir.parent_path();
        if (parent == dir) break; // reached the root
        dir = parent;
    }
    return {};
}

std::string FindRoot() {
    std::error_code ec;
    // #151 - the executable's location first: a launch from a shortcut, IDE or terminal with
    // some other working directory used to adopt whatever "project" folder sat above THAT
    // directory (or the directory itself), and write scenes and settings there.
    if (!EnginePaths::ExeDir().empty())
        if (std::string r = FindProjectAbove(fs::path(EnginePaths::ExeDir())); !r.empty()) return r;
    // Then the working directory (an exe built outside the source tree, run from inside it).
    const fs::path cwd = fs::current_path(ec);
    if (!ec)
        if (std::string r = FindProjectAbove(cwd); !r.empty()) return r;
    // No project folder anywhere — keep the historical behavior (working directory) rather
    // than inventing a location or failing.
    return ec ? std::string(".") : cwd.string();
}

std::string& Override() { static std::string s; return s; }

} // namespace

void SetRootOverride(const std::string& root) { Override() = root; }

const std::string& Root() {
    // Resolved once: the working directory doesn't change during a run, and every call site
    // (scene load/save, preferences) would otherwise redo the same directory walk.
    static const std::string root = Override().empty() ? FindRoot() : Override();
    return root;
}

std::string Resolve(const std::string& name) {
    return (std::filesystem::path(Root()) / name).lexically_normal().string();
}

namespace {
std::string RelativizeUncached(const std::string& absolutePath) {
    std::error_code ec;
    const std::filesystem::path rel =
        std::filesystem::relative(absolutePath, Root(), ec);
    const std::string relStr = rel.generic_string();
    // relative() fails (or walks "up and out" with a leading "..") for a path outside Root() —
    // fall back to the original string rather than showing a confusing "../../.." chain.
    if (ec || relStr.empty() || relStr.rfind("..", 0) == 0) return absolutePath;
    return relStr;
}
} // namespace

// relative() canonicalizes both paths against the disk (a CreateFile per path, two per call), and the
// Asset Browser asks for every file in the project each time its index lands. Root() never changes
// during a run, so an absolute path's answer is memoized. A relative one depends on the working
// directory and isn't.
std::string Relativize(const std::string& absolutePath) {
    if (!std::filesystem::path(absolutePath).is_absolute()) return RelativizeUncached(absolutePath);
    static std::mutex mutex;
    static std::unordered_map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto it = cache.find(absolutePath); it != cache.end()) return it->second;
    }
    std::string rel = RelativizeUncached(absolutePath); // outside the lock: other threads keep going
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() > 65536) cache.clear();
    cache.emplace(absolutePath, rel);
    return rel;
}

} // namespace ProjectPaths
