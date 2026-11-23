#include "GameModuleBuild.h"
#include "EditorSound.h"
#include "Log.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifndef TARTARUS_SOURCE_DIR
#define TARTARUS_SOURCE_DIR ""
#endif
#ifndef TARTARUS_BINARY_DIR
#define TARTARUS_BINARY_DIR ""
#endif
#ifndef TARTARUS_CMAKE_COMMAND
#define TARTARUS_CMAKE_COMMAND "cmake"
#endif
#ifndef TARTARUS_BUILD_CONFIG
#define TARTARUS_BUILD_CONFIG "Release"
#endif

namespace GameModuleBuild {
namespace {
namespace fs = std::filesystem;
std::mutex g_Mutex;
std::thread g_Worker;
std::atomic<bool> g_Running{false}, g_Finished{false}, g_Queued{false}, g_Held{false};
bool g_Succeeded = true;
std::string g_Log;

// The sources the reloadable targets compile, as CMake lists them (TartarusModules.sources in the
// build folder, one source-relative path per line). Read once.
const std::vector<std::string>& ReloadableSources() {
    static const std::vector<std::string> sources = [] {
        std::vector<std::string> list;
        std::ifstream in(BinaryRoot() / "TartarusModules.sources");
        for (std::string line; std::getline(in, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) list.push_back(line);
        }
        return list;
    }();
    return sources;
}

void Run() {
    std::string text;
    const bool ok = RunCMakeBuild(L"TartarusGame TartarusEditor", BinaryRoot() / "TartarusModules.build.log", text);
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Log = std::move(text);
    g_Succeeded = ok;
    g_Finished = true;
}
}

bool RunCMakeBuild(const std::wstring& targets, const fs::path& logPath, std::string& log) {
    std::wstring command = L"\"" + fs::u8path(TARTARUS_CMAKE_COMMAND).wstring() + L"\" --build \"" + BinaryRoot().wstring() +
                           L"\" --config " + fs::u8path(TARTARUS_BUILD_CONFIG).wstring() + L" --target " + targets;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (out != INVALID_HANDLE_VALUE) {
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdOutput = out; startup.hStdError = out;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                           BinaryRoot().c_str(), &startup, &process)) {
            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); ok = code == 0;
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
        CloseHandle(out);
    }
    std::ifstream in(logPath, std::ios::binary);
    std::ostringstream text; text << in.rdbuf();
    log = text.str();
    if (log.empty() && !ok) log = "Couldn't start CMake for the build.";
    return ok;
}

bool Available() {
    static const bool available = [] {
        std::error_code ec;
        return TARTARUS_SOURCE_DIR[0] && TARTARUS_BINARY_DIR[0] && fs::is_directory(SourceRoot(), ec) &&
               fs::is_regular_file(BinaryRoot() / "CMakeCache.txt", ec);
    }();
    return available;
}
fs::path SourceRoot() { return fs::u8path(TARTARUS_SOURCE_DIR); }
fs::path BinaryRoot() { return fs::u8path(TARTARUS_BINARY_DIR); }

fs::path ExePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) { buffer.resize(n); return fs::path(buffer); }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

bool InsideBuildTree(const fs::path& exe, const fs::path& buildRoot) {
    if (exe.empty() || buildRoot.empty()) return false;
    std::error_code ec;
    const fs::path rel = fs::relative(exe, buildRoot, ec);
    return !ec && !rel.empty() && *rel.begin() != "..";
}

std::string RebuildUnavailableReason() {
    // Fixed for the process (the exe and its build tree don't move); menus ask every frame they're open.
    static const std::string reason = [] {
        if (!Available())
            return std::string("Only in an editor built from source: no CMake build folder (CMakeCache.txt) for this exe.");
        if (!InsideBuildTree(ExePath(), BinaryRoot()))
            return std::string("This exe isn't inside the CMake build folder it was built in, so a rebuild "
                               "wouldn't update the DLLs next to it. Run the editor from its build folder.");
        return std::string();
    }();
    return reason;
}
bool CanRebuild() { return RebuildUnavailableReason().empty(); }

bool Start() {
    if (!CanRebuild() || g_Running) return false;
    if (g_Held) { g_Queued = true; return false; }
    if (g_Worker.joinable()) g_Worker.join();
    g_Queued = false; g_Finished = false; g_Running = true;
    Log::Info("Rebuilding the game and editor modules; they reload when the build finishes.");
    EditorSound::Play(EditorSound::Event::BuildStart);
    g_Worker = std::thread(Run);
    return true;
}
bool Running() { return g_Running; }
void Request() { if (!Start()) g_Queued = CanRebuild(); }
void Hold(bool held) { g_Held = held; }
void Poll() {
    if (g_Running && g_Finished) {
        g_Worker.join(); g_Running = false;
        EditorSound::Play(LastSucceeded() ? EditorSound::Event::BuildSuccess : EditorSound::Event::BuildFail);
        if (LastSucceeded()) Log::Info("Module rebuild finished; hot reload picks the new code up.");
        else Log::Error("Module rebuild failed. See the Script IDE's Build output.");
    }
    if (!g_Running && g_Queued && !g_Held) Start();
}
bool LastSucceeded() { std::lock_guard<std::mutex> lock(g_Mutex); return g_Succeeded; }
std::string LastLog() { std::lock_guard<std::mutex> lock(g_Mutex); return g_Log; }

void Shutdown() { g_Queued = false; if (g_Worker.joinable()) g_Worker.join(); g_Running = false; }

bool IsReloadableSource(const fs::path& path) {
    if (!Available()) return false;
    std::error_code ec;
    const fs::path relative = fs::relative(fs::absolute(path, ec), SourceRoot(), ec).lexically_normal();
    if (ec) return false;
    // A module source, or the header beside one (WeaponRules.h next to WeaponRules.cpp).
    const std::string file = relative.generic_u8string();
    const fs::path ext = relative.extension();
    const bool header = ext == ".h" || ext == ".hpp" || ext == ".inl";
    const std::string asSource = header ? fs::path(relative).replace_extension(".cpp").generic_u8string() : file;
    for (const std::string& source : ReloadableSources())
        if (_stricmp(asSource.c_str(), source.c_str()) == 0) return true;
    return false;
}
}
