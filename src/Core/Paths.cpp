#include "Paths.h"
#include "Core/Env.h"
#include "Core/Log.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string CwdOrDot() {
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    return ec ? std::string(".") : cwd.string();
}

std::string Join(const std::string& root, const std::string& name) {
    return (fs::path(root) / name).lexically_normal().string();
}

// Walks up from `dir` (at most `levels` folders) for the first one that holds a directory named
// `child`. Returns that folder, or empty.
fs::path FindAncestorWith(fs::path dir, const fs::path& child, int levels) {
    std::error_code ec;
    for (int level = 0; level < levels && !dir.empty(); ++level) {
        if (fs::is_directory(dir / child, ec) && !ec) return dir;
        fs::path parent = dir.parent_path();
        if (parent.empty() || parent == dir) break; // reached the root
        dir = parent;
    }
    return {};
}

// A root that is chosen once and then never moves. The override must land before the first Get();
// one that comes later is refused and logged, because callers already hold the old root.
struct PinnedRoot {
    const char* What;
    std::once_flag Once;
    std::string Value;
    std::string Override;
    std::atomic<bool> Pinned{false};

    template <class Find>
    const std::string& Get(Find find) {
        std::call_once(Once, [&] {
            Value = Override.empty() ? find() : Override;
            Pinned = true;
        });
        return Value;
    }
    void SetOverride(const std::string& v) {
        if (Pinned) {
            if (v != Override) Log::Error(std::string(What) + ": '" + v + "' came after the folder was already in use ('" + Value + "'); ignored.");
            return;
        }
        Override = v;
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------
namespace EnginePaths {
namespace {
std::string s_ExeDir;

// The exe's own path from the OS. argv[0] can be relative to the working directory or a bare name
// found through PATH, so fs::absolute(argv0) pointed somewhere else (S01-04).
fs::path ModulePath() {
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); return fs::path(buf); }
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);
    }
#else
    return {};
#endif
}
} // namespace

void Init(const char* argv0) {
    std::error_code ec;
    fs::path exe = ModulePath();
    if (exe.empty() && argv0) exe = fs::absolute(argv0, ec);
    s_ExeDir = (ec || exe.empty()) ? CwdOrDot() : exe.parent_path().lexically_normal().string();
}

const std::string& ExeDir() { return s_ExeDir; }

std::string Resolve(const std::string& relativePath) {
    std::error_code ec;
    const fs::path rel(relativePath);
    auto existing = [&](const fs::path& root) -> std::string {
        const fs::path candidate = (root / rel).lexically_normal();
        return fs::exists(candidate, ec) && !ec ? candidate.string() : std::string();
    };

    // 1. Explicit override.
    if (const std::string env = Env::Str(Env::Var::AssetRoot); !env.empty())
        if (std::string hit = existing(env); !hit.empty()) return hit;

    if (!s_ExeDir.empty()) {
        // 2. Next to the executable: the layout CMake's POST_BUILD steps produce.
        if (std::string hit = existing(s_ExeDir); !hit.empty()) return hit;
        // 3. An ancestor of the exe dir that directly holds the install root ("assets/").
        //    begin() rather than a range-for: MSVC flags its unreachable increment as C4702 (#173).
        if (const auto first = rel.begin(); first != rel.end())
            if (const fs::path dir = FindAncestorWith(s_ExeDir, *first, 6); !dir.empty())
                if (std::string hit = existing(dir); !hit.empty()) return hit;
    }

    // 4. The working directory, even when missing, so a "not found" downstream prints a real path.
    const fs::path cwd = fs::current_path(ec);
    return ec ? relativePath : (cwd / rel).lexically_normal().string();
}

} // namespace EnginePaths

// ---------------------------------------------------------------------------------------------
namespace ProjectPaths {
namespace {
PinnedRoot& Pin() { static PinnedRoot p{"--project"}; return p; }

std::string FindRoot() {
    // #151: the exe's location first. A launch from a shortcut, IDE or terminal with another
    // working directory used to adopt whatever "project" folder sat above THAT directory.
    for (const std::string& from : {EnginePaths::ExeDir(), CwdOrDot()}) {
        if (from.empty()) continue;
        if (const fs::path dir = FindAncestorWith(from, "project", 8); !dir.empty())
            return (dir / "project").lexically_normal().string();
    }
    // No project folder anywhere: keep the historical behavior (the working directory).
    return CwdOrDot();
}

std::string RelativizeUncached(const std::string& absolutePath) {
    std::error_code ec;
    const std::string rel = fs::relative(absolutePath, Root(), ec).generic_string();
    // relative() fails, or walks up and out with "..", for a path outside Root(): keep the original.
    if (ec || rel.empty() || rel.rfind("..", 0) == 0) return absolutePath;
    return rel;
}
} // namespace

void SetRootOverride(const std::string& root) { Pin().SetOverride(root); }
const std::string& Root() { return Pin().Get(FindRoot); }
std::string Resolve(const std::string& name) { return Join(Root(), name); }

// relative() canonicalizes both paths against the disk (a CreateFile each), and the Asset Browser
// asks for every project file each time its index lands. Root() never moves, so an absolute path's
// answer is memoized; a relative one depends on the working directory and isn't.
std::string Relativize(const std::string& absolutePath) {
    if (!fs::path(absolutePath).is_absolute()) return RelativizeUncached(absolutePath);
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

// ---------------------------------------------------------------------------------------------
namespace UserPaths {
namespace {
PinnedRoot& Pin() { static PinnedRoot p{"User folder name"}; return p; }

std::string FindRoot() {
    const std::string base = BaseDir();
    // LOCALAPPDATA is set in every normal user session; without one (a service or sandbox), use
    // the working directory rather than inventing a location.
    if (base.empty()) return CwdOrDot();
    const std::string& name = Pin().Override;
    const fs::path dir = fs::path(base) / (name.empty() ? std::string("TartarusEngine") : name);
    std::error_code ec;
    fs::create_directories(dir, ec); // best-effort; Save()/Load() tolerate a missing folder
    return dir.lexically_normal().string();
}
} // namespace

std::string BaseDir() { return Env::Str(Env::Var::LocalAppData); }

// The override here is the folder *name*; Get() would otherwise use it as the whole root, so the
// root is always found through FindRoot and the name passed through Override.
void SetAppName(const std::string& name) { if (!name.empty()) Pin().SetOverride(name); }
const std::string& Root() {
    PinnedRoot& p = Pin();
    std::call_once(p.Once, [&] { p.Value = FindRoot(); p.Pinned = true; });
    return p.Value;
}
std::string Resolve(const std::string& name) { return Join(Root(), name); }

} // namespace UserPaths
