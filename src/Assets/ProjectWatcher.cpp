#include "ProjectWatcher.h"
#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace ProjectWatcher {
namespace {

struct Pending {
    Change C;
    Clock::time_point Last;
};

std::mutex g_Mutex;
std::unordered_map<std::string, Pending> g_Pending; // keyed by Key(Change::Path)
bool g_Overflowed = false;

std::thread g_Thread;
std::atomic<bool> g_Running{false};
std::string g_Root;
#ifdef _WIN32
HANDLE g_StopEvent = nullptr;
#endif

std::string Key(const std::string& p) {
    std::string k = fs::path(p).lexically_normal().generic_string();
#ifdef _WIN32
    for (char& c : k) c = (char)std::tolower((unsigned char)c);
#endif
    return k;
}

// Paths the editor itself churns (caches, captures, layouts) and transient files are never news.
bool Ignored(const fs::path& rel) {
    if (rel.empty()) return true;
    std::string first = rel.begin()->string();
    std::transform(first.begin(), first.end(), first.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (first == "library" || first == "screenshots" || first == "layouts") return true;
    const std::string name = rel.filename().string();
    if (name.find(".tmp-") != std::string::npos) return true;        // AtomicFile's temp files
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) return true;
    if (name.rfind("~$", 0) == 0) return true;                         // Office-style lock files
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".meta") == 0) return true; // follow their asset
    return false;
}

// Folds a new event for one path into whatever is already pending for it (caller holds g_Mutex).
void Record(Change c) {
    const auto now = Clock::now();
    const std::string key = Key(c.Path);
    if (c.Type == Change::Kind::Renamed) {
        // A rename of something already pending chains onto it (a -> b -> c is a -> c).
        auto prev = g_Pending.find(Key(c.OldPath));
        if (prev != g_Pending.end()) {
            Change merged = c;
            if (prev->second.C.Type == Change::Kind::Added) { merged.Type = Change::Kind::Added; merged.OldPath.clear(); }
            else if (prev->second.C.Type == Change::Kind::Renamed) merged.OldPath = prev->second.C.OldPath;
            g_Pending.erase(prev);
            c = merged;
        }
        g_Pending[key] = {c, now};
        return;
    }
    auto it = g_Pending.find(key);
    if (it == g_Pending.end()) { g_Pending[key] = {c, now}; return; }
    Change& p = it->second.C;
    using K = Change::Kind;
    if (p.Type == K::Added && c.Type == K::Removed) { g_Pending.erase(it); return; }   // a temp file
    if (p.Type == K::Removed && c.Type == K::Added) p.Type = K::Modified;              // replaced in place
    else if (c.Type == K::Removed) { p.Type = K::Removed; p.OldPath.clear(); }
    // Added/Renamed followed by Modified stays Added/Renamed.
    it->second.Last = now;
}

#ifdef _WIN32
// A folder added or renamed arrives as one event; report each file inside it.
void RecordFolder(const fs::path& dir, const fs::path& oldDir, Change::Kind kind) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path rel = fs::relative(it->path(), dir, ec);
        if (Ignored(fs::relative(it->path(), g_Root, ec))) continue;
        Change c;
        c.Type = kind;
        c.Path = it->path().string();
        if (kind == Change::Kind::Renamed) c.OldPath = (oldDir / rel).string();
        Record(c);
    }
}

void ThreadMain(std::wstring rootW) {
    HANDLE dir = CreateFileW(rootW.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        Log::Warn("Project watcher: can't watch '" + g_Root + "' - changes made outside the editor are picked up on restart.");
        g_Running = false;
        return;
    }
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    alignas(DWORD) static BYTE buffer[64 * 1024];
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
    const fs::path root(g_Root);

    while (g_Running) {
        ResetEvent(ov.hEvent);
        if (!ReadDirectoryChangesW(dir, buffer, sizeof(buffer), TRUE, filter, nullptr, &ov, nullptr)) {
            Log::Warn("Project watcher: stopped (ReadDirectoryChangesW failed).");
            break;
        }
        HANDLE waits[2] = {ov.hEvent, g_StopEvent};
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) { CancelIoEx(dir, &ov); break; } // stop requested
        DWORD bytes = 0;
        if (!GetOverlappedResult(dir, &ov, &bytes, FALSE)) continue;
        std::lock_guard<std::mutex> lk(g_Mutex);
        if (bytes == 0) { g_Overflowed = true; continue; } // too many changes at once: rescan

        std::wstring renamedFrom;
        for (auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);;) {
            const std::wstring relW(info->FileName, info->FileNameLength / sizeof(WCHAR));
            const fs::path rel(relW);
            const fs::path abs = (root / rel).lexically_normal();
            if (!Ignored(rel)) {
                std::error_code ec;
                const bool isDir = fs::is_directory(abs, ec);
                Change c;
                c.Path = abs.string();
                switch (info->Action) {
                    case FILE_ACTION_ADDED:
                        if (isDir) RecordFolder(abs, {}, Change::Kind::Added);
                        else { c.Type = Change::Kind::Added; Record(c); }
                        break;
                    case FILE_ACTION_REMOVED:
                        c.Type = Change::Kind::Removed; // a file or a whole folder; the consumer matches prefixes
                        Record(c);
                        break;
                    case FILE_ACTION_MODIFIED:
                        if (!isDir) { c.Type = Change::Kind::Modified; Record(c); }
                        break;
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        renamedFrom = relW;
                        break;
                    case FILE_ACTION_RENAMED_NEW_NAME: {
                        const fs::path oldAbs = (root / fs::path(renamedFrom)).lexically_normal();
                        if (renamedFrom.empty() || Ignored(fs::path(renamedFrom))) {
                            // Renamed from a temp/ignored name (e.g. an atomic save): it's a write.
                            if (isDir) RecordFolder(abs, {}, Change::Kind::Added);
                            else { c.Type = Change::Kind::Modified; Record(c); }
                        } else if (isDir) {
                            RecordFolder(abs, oldAbs, Change::Kind::Renamed);
                        } else {
                            c.Type = Change::Kind::Renamed;
                            c.OldPath = oldAbs.string();
                            Record(c);
                        }
                        renamedFrom.clear();
                        break;
                    }
                }
            } else if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
                renamedFrom = relW; // an ignored name renamed to a real one (atomic save) - handled above
            }
            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(info) + info->NextEntryOffset);
        }
    }
    CloseHandle(ov.hEvent);
    CloseHandle(dir);
}
#endif

} // namespace

bool Start(const std::string& root) {
#ifdef _WIN32
    if (g_Running && Key(root) == Key(g_Root)) return true;
    Stop();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return false;
    g_Root = fs::absolute(root, ec).lexically_normal().string();
    g_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_Running = true;
    g_Thread = std::thread(ThreadMain, fs::path(g_Root).wstring());
    return true;
#else
    (void)root;
    return false;
#endif
}

void Stop() {
#ifdef _WIN32
    if (!g_Thread.joinable()) return;
    g_Running = false;
    SetEvent(g_StopEvent);
    g_Thread.join();
    CloseHandle(g_StopEvent);
    g_StopEvent = nullptr;
    std::lock_guard<std::mutex> lk(g_Mutex);
    g_Pending.clear();
    g_Overflowed = false;
#endif
}

bool IsRunning() { return g_Running; }

std::vector<Change> Drain(int settleMs, bool& overflowed) {
    std::vector<Change> out;
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lk(g_Mutex);
    overflowed = g_Overflowed;
    g_Overflowed = false;
    // Hold everything back while anything is still settling: a move across folders arrives as a
    // Removed + an Added, and they must be handed out together to be recognised as a move.
    for (const auto& [key, p] : g_Pending)
        if (now - p.Last < std::chrono::milliseconds(settleMs)) return out;
    for (auto& [key, p] : g_Pending) out.push_back(std::move(p.C));
    g_Pending.clear();

    // A Removed + an Added with the same file name in one batch is a move (Explorer moves across
    // folders that way): report it as a rename so the asset keeps its identity.
    for (auto& removed : out) {
        if (removed.Type != Change::Kind::Removed) continue;
        const std::string name = Key(fs::path(removed.Path).filename().string());
        for (auto& added : out) {
            if (added.Type != Change::Kind::Added || Key(fs::path(added.Path).filename().string()) != name) continue;
            added.Type = Change::Kind::Renamed;
            added.OldPath = removed.Path;
            removed.Path.clear(); // consumed
            break;
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const Change& c) { return c.Path.empty(); }), out.end());
    return out;
}

} // namespace ProjectWatcher
