#include "BuildPipeline.h"
#include "EnginePaths.h"
#include "Log.h"
#include "PlayerConfig.h"
#include "ProjectPaths.h"
#include "ProjectSettings.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace BuildPipeline {
namespace {

// Folder/file names safe on Windows: the product name becomes both.
std::string SanitizeFileName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const bool bad = c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '|' ||
                         c == '?' || c == '*' || c == '\\' || (unsigned char)c < 32;
        out += bad ? '_' : c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? std::string("Game") : out;
}

bool IsInside(const fs::path& child, const fs::path& parent) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(child, ec);
    const fs::path p = fs::weakly_canonical(parent, ec);
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
        if (pi->empty()) continue; // trailing separator
        if (ci == c.end() || *ci != *pi) return false;
    }
    return true;
}

// Project folders that only matter to the editor.
bool IsEditorOnlyProjectDir(const std::string& name) {
    return name == "Library" || name == "screenshots" || name == "layouts" || name == "scenes";
}

struct Copier {
    Report& R;
    fs::path OutRoot;
    std::string Error;

    bool File(const fs::path& from, const fs::path& to, const char* group) {
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec) || ec) {
            Error = "couldn't copy '" + from.string() + "': " + ec.message();
            return false;
        }
        const std::uint64_t size = fs::file_size(to, ec);
        ++R.FileCount;
        R.TotalBytes += size;
        auto g = std::find_if(R.BytesByGroup.begin(), R.BytesByGroup.end(),
                              [&](const auto& p) { return p.first == group; });
        if (g == R.BytesByGroup.end()) R.BytesByGroup.push_back({group, size});
        else g->second += size;
        R.LargestFiles.push_back({fs::relative(to, OutRoot, ec).generic_string(), size});
        return true;
    }

    // Every regular file under `from`, mirrored under `to`. `skip` filters top-level entries.
    template <typename Skip>
    bool Tree(const fs::path& from, const fs::path& to, const char* group, Skip skip) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const fs::path rel = fs::relative(it->path(), from, ec);
            if (it.depth() == 0 && skip(*it)) {
                if (it->is_directory()) it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file() && !File(it->path(), to / rel, group)) return false;
        }
        if (ec) { Error = "couldn't read '" + from.string() + "': " + ec.message(); return false; }
        return true;
    }
};

} // namespace

std::string ResolveOutputDir(const ProjectSettings::BuildSettings& s) {
    if (!s.OutputDir.empty()) return fs::path(s.OutputDir).lexically_normal().string();
    const fs::path project(ProjectPaths::Root());
    return (project.parent_path() / "Builds" / SanitizeFileName(s.ProductName)).lexically_normal().string();
}

std::vector<std::string> FindProjectScenes() {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir = ProjectPaths::Resolve("scenes");
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".json") continue;
        out.push_back(fs::relative(it->path(), ProjectPaths::Root(), ec).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

Report Build(const ProjectSettings::BuildSettings& s) {
    const auto t0 = std::chrono::steady_clock::now();
    Report r;
    auto fail = [&r](const std::string& why) {
        r.Ok = false;
        r.Message = "Build failed: " + why;
        Log::Error(r.Message);
        return r;
    };

    if (s.Scenes.empty()) return fail("no scenes in the build. Add at least one in Build Settings.");
    const fs::path projectRoot(ProjectPaths::Root());
    for (const std::string& sc : s.Scenes) {
        std::error_code ec;
        if (!fs::is_regular_file(projectRoot / sc, ec)) return fail("scene '" + sc + "' doesn't exist.");
    }

    const fs::path exeDir(EnginePaths::ExeDir());
    fs::path exe;
#ifdef _WIN32
    {
        wchar_t buf[MAX_PATH * 4];
        const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
        if (n > 0) exe = fs::path(std::wstring(buf, n));
    }
#endif
    if (exe.empty()) return fail("couldn't locate the engine executable.");

    const fs::path out(ResolveOutputDir(s));
    r.OutputDir = out.string();
    std::error_code ec;
    if (IsInside(out, projectRoot)) return fail("the output folder can't be inside the project folder.");
    if (IsInside(out, exeDir) || IsInside(exeDir, out)) return fail("the output folder can't contain, or be inside, the engine's own folder.");
    if (fs::exists(out, ec)) {
        // Only ever clear a folder that is recognisably a previous build.
        if (!fs::is_directory(out, ec)) return fail("'" + out.string() + "' is a file.");
        const bool empty = fs::directory_iterator(out, ec) == fs::directory_iterator();
        if (!empty && !fs::exists(out / PlayerConfig::kFileName, ec))
            return fail("'" + out.string() + "' isn't empty and isn't a previous build. Pick an empty folder.");
        fs::remove_all(out, ec);
        if (ec) return fail("couldn't clear the previous build in '" + out.string() + "' (is the game still running?): " + ec.message());
    }
    fs::create_directories(out, ec);
    if (ec) return fail("couldn't create '" + out.string() + "': " + ec.message());

    Copier copy{r, out, {}};
    const std::string product = SanitizeFileName(s.ProductName);
    r.ExePath = (out / (product + ".exe")).string();
    if (!copy.File(exe, r.ExePath, "Runtime")) return fail(copy.Error);
    for (const auto& e : fs::directory_iterator(exeDir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".dll" && !copy.File(e.path(), out / e.path().filename(), "Runtime"))
            return fail(copy.Error);
    }
    const fs::path engineAssets = exeDir / "assets";
    if (fs::is_directory(engineAssets, ec) &&
        !copy.Tree(engineAssets, out / "assets", "Engine",
                   [](const fs::directory_entry& e) { return e.path().filename() == "test-scenes"; }))
        return fail(copy.Error);
    if (!copy.Tree(projectRoot, out / "project", "Project assets", [](const fs::directory_entry& e) {
            const std::string name = e.path().filename().string();
            if (e.is_directory()) return IsEditorOnlyProjectDir(name);
            return name.size() > 12 && name.compare(name.size() - 12, 12, ".backup.json") == 0;
        }))
        return fail(copy.Error);
    for (const std::string& sc : s.Scenes) {
        if (!copy.File(projectRoot / sc, out / "project" / sc, "Scenes")) return fail(copy.Error);
        const fs::path meta = projectRoot / (sc + ".meta");
        if (fs::exists(meta, ec) && !copy.File(meta, out / "project" / (sc + ".meta"), "Scenes")) return fail(copy.Error);
    }

    PlayerConfig pc;
    pc.ProductName = s.ProductName;
    pc.CompanyName = s.CompanyName;
    pc.Version = s.Version;
    pc.Scenes = s.Scenes;
    pc.Width = s.Width;
    pc.Height = s.Height;
    pc.Fullscreen = s.Fullscreen;
    pc.VSync = s.VSync;
    pc.DevelopmentBuild = s.DevelopmentBuild;
    if (!pc.Save((out / PlayerConfig::kFileName).string())) return fail("couldn't write player.json.");

    std::sort(r.LargestFiles.begin(), r.LargestFiles.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (r.LargestFiles.size() > 10) r.LargestFiles.resize(10);
    std::sort(r.BytesByGroup.begin(), r.BytesByGroup.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    r.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.Ok = true;
    char msg[160];
    std::snprintf(msg, sizeof(msg), "Built %d files (%.1f MB) in %.1f s.", r.FileCount,
                  (double)r.TotalBytes / (1024.0 * 1024.0), r.Seconds);
    r.Message = msg;
    Log::Info(std::string("Build: ") + msg + " -> " + r.OutputDir);
    return r;
}

bool Run(const std::string& exePath) {
#ifdef _WIN32
    const fs::path exe(exePath);
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    const std::wstring dir = exe.parent_path().wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        dir.c_str(), &si, &pi)) {
        Log::Error("Build and Run: couldn't start '" + exePath + "' (error " + std::to_string(GetLastError()) + ").");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)exePath;
    return false;
#endif
}

} // namespace BuildPipeline
