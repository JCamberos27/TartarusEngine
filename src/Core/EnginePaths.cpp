#include "EnginePaths.h"

#include <cstdlib>
#include <filesystem>

namespace EnginePaths {
namespace {

namespace fs = std::filesystem;

std::string s_ExeDir;

fs::path EnvRoot() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996) // getenv is fine for a one-shot read-only override check
#endif
    const char* env = std::getenv("TARTARUS_ASSET_ROOT");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (env && env[0] != '\0') return fs::path(env);
    return {};
}

// The first component of `rel` ("assets" for "assets/shaders/..."), used to decide whether an
// ancestor directory is an install root.
fs::path FirstComponent(const fs::path& rel) {
    for (const auto& part : rel) return part;
    return {};
}

} // namespace

void Init(const char* argv0) {
    std::error_code ec;
    fs::path exe = argv0 ? fs::absolute(argv0, ec) : fs::path();
    if (ec || exe.empty()) {
        fs::path cwd = fs::current_path(ec);
        s_ExeDir = ec ? std::string(".") : cwd.string();
        return;
    }
    s_ExeDir = exe.parent_path().string();
}

const std::string& ExeDir() {
    return s_ExeDir;
}

std::string Resolve(const std::string& relativePath) {
    std::error_code ec;
    const fs::path rel(relativePath);

    // 1. Explicit override.
    if (fs::path root = EnvRoot(); !root.empty()) {
        fs::path candidate = (root / rel).lexically_normal();
        if (fs::exists(candidate, ec) && !ec) return candidate.string();
    }

    // 2. Next to the executable — the layout CMake's POST_BUILD steps produce.
    if (!s_ExeDir.empty()) {
        fs::path candidate = (fs::path(s_ExeDir) / rel).lexically_normal();
        if (fs::exists(candidate, ec) && !ec) return candidate.string();
    }

    // 3. An ancestor of the exe dir that directly contains the install root ("assets/").
    const fs::path marker = FirstComponent(rel);
    if (!s_ExeDir.empty() && !marker.empty()) {
        fs::path dir(s_ExeDir);
        for (int level = 0; level < 6; ++level) {
            if (fs::is_directory(dir / marker, ec) && !ec) {
                fs::path candidate = (dir / rel).lexically_normal();
                if (fs::exists(candidate, ec) && !ec) return candidate.string();
            }
            fs::path parent = dir.parent_path();
            if (parent.empty() || parent == dir) break;
            dir = parent;
        }
    }

    // 4. Historical fallback: resolve against the working directory. Returned even when it does
    // not exist, so a "file not found" diagnostic downstream can print a concrete path.
    fs::path cwd = fs::current_path(ec);
    if (ec) return relativePath;
    return (cwd / rel).lexically_normal().string();
}

bool Exists(const std::string& relativePath) {
    std::error_code ec;
    bool ok = fs::exists(fs::path(Resolve(relativePath)), ec);
    return ok && !ec;
}

} // namespace EnginePaths
