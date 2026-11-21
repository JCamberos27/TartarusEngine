#include "EditorRestart.h"
#include "EditorSound.h"
#include "GameModuleBuild.h"
#include "Env.h"
#include "Log.h"
#include "Paths.h"

#include <atomic>
#include <mutex>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

namespace EditorRestart {
namespace {
namespace fs = std::filesystem;
std::mutex g_Mutex;
std::thread g_Worker;
std::atomic<bool> g_Building{false}, g_Finished{false};
bool g_Succeeded = false, g_Ready = false;
std::string g_Log;

fs::path ExePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) { buffer.resize(n); return fs::path(buffer); }
        buffer.resize(buffer.size() * 2);
    }
}
fs::path OldExePath(const fs::path& exe) { fs::path old = exe; old += L".old"; return old; }

// Puts the exe name back when the build left no new exe (failed, or never ran).
void RestoreExeName(const fs::path& exe) {
    std::error_code ec;
    const fs::path old = OldExePath(exe);
    if (!fs::exists(exe, ec) && fs::exists(old, ec)) fs::rename(old, exe, ec);
}

// TARTARUS_RESTART_FAKE_BUILD=ok|fail (the hotreload smoke case): no CMake, the exe is never moved, and
// Relaunch starts nothing (the state file stays for the test to read).
std::string FakeBuild() { return Env::Str(Env::Var::RestartFakeBuild); }

void Run(fs::path exe) {
    std::string log;
    bool ok = false;
    const std::string fake = FakeBuild();
    if (fake.empty()) {
        ok = GameModuleBuild::RunCMakeBuild(L"TartarusEngine TartarusGame TartarusEditor",
                                            GameModuleBuild::BinaryRoot() / "TartarusRestart.build.log", log);
    } else {
        ok = fake == "ok";
        log = ok ? "Fake build: ok." : "Fake build: failed.";
        (void)exe;
    }
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Log = std::move(log);
    g_Succeeded = ok;
    g_Finished = true;
}
} // namespace

bool Available() { return UnavailableReason().empty(); }

std::string UnavailableReason() {
    // Fixed for the process (the exe and its build tree don't move); the File menu asks every frame it's open.
    static const std::string reason = [] {
        if (!GameModuleBuild::Available())
            return std::string("Only in an editor built from source: no CMake build folder (CMakeCache.txt) next to this exe.");
        std::error_code ec;
        const fs::path exe = ExePath();
        // Multi-config generators put the exe in <build>/<Config>: anything under the build tree counts.
        const fs::path rel = exe.empty() ? fs::path() : fs::relative(exe, GameModuleBuild::BinaryRoot(), ec);
        if (ec || rel.empty() || *rel.begin() == "..")
            return std::string("This exe isn't inside the CMake build folder it was built in.");
        return std::string();
    }();
    return reason;
}

bool Request() {
    if (g_Building) return false;
    if (!Available()) { Log::Warn("Rebuild & Restart: " + UnavailableReason()); return false; }
    if (GameModuleBuild::Running()) {
        Log::Warn("Rebuild & Restart: the module build is still running; try again when it finishes.");
        return false;
    }
    if (g_Worker.joinable()) g_Worker.join();
    const fs::path exe = ExePath();
    const fs::path old = OldExePath(exe);
    std::error_code ec;
    fs::remove(old, ec); // a stale one from an earlier restart
    ec.clear();
    if (FakeBuild().empty()) fs::rename(exe, old, ec);
    if (ec) {
        Log::Error("Rebuild & Restart: couldn't move the running exe aside (" + ec.message() + ").");
        EditorSound::Play(EditorSound::Event::BuildFail);
        return false;
    }
    g_Ready = false; g_Finished = false; g_Building = true;
    GameModuleBuild::Hold(true); // one cmake at a time on the build tree
    Log::Info("Rebuild & Restart: building the editor; it restarts on the same scene when the build finishes.");
    EditorSound::Play(EditorSound::Event::BuildStart);
    g_Worker = std::thread(Run, exe);
    return true;
}

bool Building() { return g_Building; }

void Poll() {
    if (!g_Building || !g_Finished) return;
    g_Worker.join();
    g_Building = false;
    const fs::path exe = ExePath();
    std::error_code ec;
    bool ok;
    { std::lock_guard<std::mutex> lock(g_Mutex); ok = g_Succeeded; }
    // An up-to-date build may not relink: the exe is the same code, so the old copy stands in.
    if (ok && !fs::exists(exe, ec)) fs::copy_file(OldExePath(exe), exe, ec);
    if (ok && fs::exists(exe, ec)) {
        g_Ready = true;
        EditorSound::Play(EditorSound::Event::BuildSuccess);
        Log::Info("Rebuild & Restart: build finished, restarting.");
        return;
    }
    RestoreExeName(exe);
    GameModuleBuild::Hold(false);
    EditorSound::Play(EditorSound::Event::BuildFail);
    Log::Error("Rebuild & Restart: the build failed; the editor keeps running. Log: " +
               (GameModuleBuild::BinaryRoot() / "TartarusRestart.build.log").u8string());
}

bool ReadyToRelaunch() { return g_Ready; }
void Cancel() { g_Ready = false; GameModuleBuild::Hold(false); }
std::string LastLog() { std::lock_guard<std::mutex> lock(g_Mutex); return g_Log; }

std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++i; ++slashes; }
        if (i == arg.size()) { out.append(slashes * 2, L'\\'); break; }
        if (arg[i] == L'"') { out.append(slashes * 2 + 1, L'\\'); out += L'"'; }
        else { out.append(slashes, L'\\'); out += arg[i]; }
    }
    return out + L"\"";
}

std::vector<std::wstring> RelaunchArgs(const std::vector<std::wstring>& args, const fs::path& stateFile, unsigned long pid) {
    std::vector<std::wstring> out;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--restore-state" || a == L"--wait-pid") { ++i; continue; } // drop the flag and its value
        if (a.rfind(L"--restore-state=", 0) == 0 || a.rfind(L"--wait-pid=", 0) == 0) continue;
        out.push_back(a);
    }
    out.push_back(L"--restore-state");
    out.push_back(stateFile.wstring());
    out.push_back(L"--wait-pid");
    out.push_back(std::to_wstring(pid));
    return out;
}

bool Relaunch(const fs::path& stateFile) {
    g_Ready = false;
    const fs::path exe = ExePath();
    if (!FakeBuild().empty()) {
        Log::Warn("Rebuild & Restart: fake build, no relaunch; the editor keeps running.");
        GameModuleBuild::Hold(false);
        return false;
    }
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; ++i) args.emplace_back(argv[i]);
    if (argv) LocalFree(argv);
    std::wstring command = QuoteArg(exe.wstring());
    for (const std::wstring& a : RelaunchArgs(args, stateFile, GetCurrentProcessId())) command += L" " + QuoteArg(a);

    wchar_t cwd[MAX_PATH * 4] = {};
    GetCurrentDirectoryW(static_cast<DWORD>(std::size(cwd)), cwd);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd, &startup, &process)) {
        Log::Error("Rebuild & Restart: couldn't start the new editor (error " + std::to_string(GetLastError()) + "); this one keeps running.");
        EditorSound::Play(EditorSound::Event::BuildFail);
        std::error_code ec;
        fs::remove(stateFile, ec);
        GameModuleBuild::Hold(false);
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool CleanupAtStartup(unsigned long waitPid, unsigned long timeoutMs) {
    bool exited = true;
    if (waitPid != 0) {
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, waitPid)) {
            exited = WaitForSingleObject(old, timeoutMs) != WAIT_TIMEOUT;
            CloseHandle(old);
        }
    }
    const fs::path exe = ExePath();
    if (exe.empty()) return exited;
    std::error_code ec;
    fs::remove(OldExePath(exe), ec); // fails harmlessly while another copy still runs from it
    return exited;
}

fs::path StateFile() {
    return fs::u8path(UserPaths::Resolve("restart_state_" + std::to_string(GetCurrentProcessId()) + ".json"));
}

void Shutdown() {
    if (g_Worker.joinable()) g_Worker.join();
    if (g_Building) RestoreExeName(ExePath());
    g_Building = false;
    g_Ready = false;
}

} // namespace EditorRestart
