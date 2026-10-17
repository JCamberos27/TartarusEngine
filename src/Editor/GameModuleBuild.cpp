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
std::atomic<bool> g_Running{false}, g_Finished{false}, g_Queued{false};
bool g_Succeeded = true;
std::string g_Log;

fs::path BinaryRoot() { return fs::u8path(TARTARUS_BINARY_DIR); }

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
    const fs::path log = BinaryRoot() / "TartarusModules.build.log";
    std::wstring command = L"\"" + fs::u8path(TARTARUS_CMAKE_COMMAND).wstring() + L"\" --build \"" + BinaryRoot().wstring() +
                           L"\" --config " + fs::u8path(TARTARUS_BUILD_CONFIG).wstring() + L" --target TartarusGame TartarusEditor";
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    std::ifstream in(log, std::ios::binary);
    std::ostringstream text; text << in.rdbuf();
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Log = text.str();
    if (g_Log.empty() && !ok) g_Log = "Couldn't start CMake for the module rebuild.";
    g_Succeeded = ok;
    g_Finished = true;
}
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

bool Start() {
    if (!Available() || g_Running) return false;
    if (g_Worker.joinable()) g_Worker.join();
    g_Queued = false; g_Finished = false; g_Running = true;
    Log::Info("Rebuilding the game and editor modules; they reload when the build finishes.");
    EditorSound::Play(EditorSound::Event::BuildStart);
    g_Worker = std::thread(Run);
    return true;
}
bool Running() { return g_Running; }
void Request() { if (!Start()) g_Queued = Available(); }
void Poll() {
    if (g_Running && g_Finished) {
        g_Worker.join(); g_Running = false;
        EditorSound::Play(LastSucceeded() ? EditorSound::Event::BuildSuccess : EditorSound::Event::BuildFail);
        if (LastSucceeded()) Log::Info("Module rebuild finished; hot reload picks the new code up.");
        else Log::Error("Module rebuild failed. See the Script IDE's Build output.");
    }
    if (!g_Running && g_Queued) Start();
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
