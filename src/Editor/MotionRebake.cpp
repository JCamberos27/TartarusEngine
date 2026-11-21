#include "MotionRebake.h"
#include "EditorSound.h"
#include "FirstPersonBodyMotion.h"
#include "Log.h"
#include "Core/Paths.h"
#include "MotionMatching/MotionBake.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace MotionRebake {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::mutex g_Mutex;
std::thread g_Worker;
std::atomic<bool> g_Running{false}, g_Finished{false};
bool g_Succeeded = false;
std::string g_Config, g_Output, g_Log; // the running bake's config, its database base, its output
std::string g_QueuedConfig;
// A database rewritten outside a bake of ours: invalidated once its files have been quiet a moment (a bake writes
// .mmanim and .mmdb one after the other; a load between the two would pair a new stream with old features).
std::string g_ChangedBase;
Clock::time_point g_ChangedAt;
constexpr auto kQuiet = std::chrono::milliseconds(750);
// Our last finished bake: its file writes reach the project watcher after Poll invalidated it, so they're not news.
std::string g_LastOutput;
Clock::time_point g_LastDoneAt;
constexpr auto kOwnWrites = std::chrono::seconds(5);

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// One spelling per file: absolute, normalized, forward slashes, lower case.
std::string Spelling(const std::string& path) {
    std::error_code ec;
    return Lower(fs::absolute(fs::u8path(path), ec).lexically_normal().generic_u8string());
}

fs::path LogPath() {
    std::error_code ec;
    return fs::temp_directory_path(ec) / "TartarusMotionRebake.log";
}

void Run(std::string config) {
    wchar_t exe[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    std::wstring command = L"\"" + std::wstring(exe, n) + L"\" --bake-motion \"" + fs::u8path(config).wstring() + L"\"";
    const fs::path log = LogPath();
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (n > 0 && out != INVALID_HANDLE_VALUE) {
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdOutput = out; startup.hStdError = out;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process)) {
            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); ok = code == 0;
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    std::ifstream in(log, std::ios::binary);
    std::ostringstream text; text << in.rdbuf();
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Log = text.str();
    if (g_Log.empty() && !ok) g_Log = "Couldn't start the motion bake.";
    g_Succeeded = ok;
    g_Finished = true;
}

bool Start(const std::string& config) {
    MotionBakeConfig cfg;
    std::string error;
    if (!cfg.Load(config, error)) {
        Log::Error("Motion rebake: '" + ProjectPaths::Relativize(config) + "' doesn't load (" + error + ").");
        return false;
    }
    if (g_Worker.joinable()) g_Worker.join();
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Config = config;
        g_Output = cfg.Output;
    }
    g_Finished = false;
    g_Running = true;
    Log::Info("Rebaking the motion matching database (" + ProjectPaths::Relativize(config) +
              "); a body in Play switches to it when the bake finishes.");
    EditorSound::Play(EditorSound::Event::BuildStart);
    g_Worker = std::thread(Run, config);
    return true;
}
}

bool IsConfig(const std::string& path) { return Lower(fs::u8path(path).extension().u8string()) == ".mmconfig"; }

bool IsBakedDatabase(const std::string& path, std::string* base) {
    const fs::path p = fs::u8path(path);
    const std::string ext = Lower(p.extension().u8string());
    if (ext != ".mmanim" && ext != ".mmdb") return false;
    if (base) *base = fs::path(p).replace_extension().u8string();
    return true;
}

bool Request(const std::string& configPath) {
    if (g_Running) {
        MotionBakeConfig cfg;
        std::string error;
        if (!cfg.Load(configPath, error)) {
            Log::Error("Motion rebake: '" + ProjectPaths::Relativize(configPath) + "' doesn't load (" + error + ").");
            return false;
        }
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_QueuedConfig = configPath; // the newest save wins; several saves in a row become one more bake
        return true;
    }
    return Start(configPath);
}

void NotifyDatabaseChanged(const std::string& base) {
    if (g_Running) return; // our own bake writing: Poll invalidates once it has finished
    if (!g_LastOutput.empty() && Spelling(base) == Spelling(g_LastOutput) && Clock::now() - g_LastDoneAt < kOwnWrites) return;
    g_ChangedBase = base;
    g_ChangedAt = Clock::now();
}

bool Running() { return g_Running; }

void Poll() {
    if (g_Running && g_Finished) {
        g_Worker.join();
        g_Running = false;
        std::string output, config, log;
        bool ok;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            output = g_Output; config = g_Config; log = g_Log; ok = g_Succeeded;
        }
        EditorSound::Play(ok ? EditorSound::Event::BuildSuccess : EditorSound::Event::BuildFail);
        if (ok) {
            SharedMotionDatabase::Invalidate(output);
            g_LastOutput = output;
            g_LastDoneAt = Clock::now();
            g_ChangedBase.clear(); // the bake's own file writes
            Log::Info("Motion rebake finished: '" + ProjectPaths::Relativize(output) + "' reloads.");
        } else {
            // The last lines say why (the bake's own report).
            const size_t from = log.size() > 2000 ? log.rfind('\n', log.size() - 2000) : std::string::npos;
            Log::Error("Motion rebake of '" + ProjectPaths::Relativize(config) + "' failed; the old database stays.\n" +
                       (from == std::string::npos ? log : log.substr(from + 1)));
        }
    }
    if (!g_Running) {
        std::string queued;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            queued.swap(g_QueuedConfig);
        }
        if (!queued.empty()) Start(queued);
    }
    if (!g_ChangedBase.empty() && !g_Running && Clock::now() - g_ChangedAt >= kQuiet) {
        SharedMotionDatabase::Invalidate(g_ChangedBase);
        Log::Info("Reloading the motion matching database '" + ProjectPaths::Relativize(g_ChangedBase) +
                  "' (rebaked outside the editor).");
        g_ChangedBase.clear();
    }
}

void Shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_QueuedConfig.clear();
    }
    if (g_Worker.joinable()) g_Worker.join();
    g_Running = false;
}
}
