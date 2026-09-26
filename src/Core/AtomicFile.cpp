#include "AtomicFile.h"
#include <cctype>
#include <unordered_map>
#include <mutex>
#include "Log.h"

#include <algorithm>
#include <fstream>
#include <random>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <thread>
#include <chrono>
#endif

namespace AtomicFile {

namespace {

// A per-write unique temp name in the target's own directory, so the final rename() is a
// same-filesystem move (atomic) rather than a cross-device copy. Concurrent writers to the same
// path (two editor instances) get different temps and the last rename wins — no interleaving.
std::filesystem::path TempPathFor(const std::filesystem::path& target) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::filesystem::path p = target;
    p += ".tmp-" + std::to_string(rng());
    return p;
}

#ifdef _WIN32
std::string LastErrorText(DWORD err) {
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, (LPSTR)&msg, 0, nullptr);
    std::string s = msg ? msg : ("error " + std::to_string(err));
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// Writes the temp file through a raw handle so the bytes can be forced to disk
// (FlushFileBuffers) before the rename makes them the real file — otherwise a power loss right
// after the rename can leave the "new" file zero-filled (#92).
bool WriteTempDurable(const std::filesystem::path& temp, std::string_view bytes, bool binary,
                      const std::filesystem::path& target) {
    std::string text;
    std::string_view out = bytes;
    if (!binary) {
        // Match std::ofstream text mode on Windows: \n -> \r\n.
        text.reserve(bytes.size() + bytes.size() / 16);
        for (char c : bytes) { if (c == '\n') text += '\r'; text += c; }
        out = text;
    }
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log::Error("AtomicFile: cannot open temp file for '" + target.string() + "': " + LastErrorText(GetLastError()));
        return false;
    }
    bool ok = true;
    size_t off = 0;
    while (ok && off < out.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(out.size() - off, 1u << 30);
        DWORD written = 0;
        ok = WriteFile(h, out.data() + off, chunk, &written, nullptr) && written == chunk;
        off += written;
    }
    if (ok) ok = FlushFileBuffers(h) != 0;
    const DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) {
        Log::Error("AtomicFile: write failed for '" + target.string() + "' (disk full?): " + LastErrorText(err));
        std::error_code ec;
        std::filesystem::remove(temp, ec);
    }
    return ok;
}
#endif

} // namespace

namespace {
std::mutex g_SelfWritesMutex;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_SelfWrites;

std::string SelfWriteKey(const std::filesystem::path& p) {
    std::error_code ec;
    std::string k = std::filesystem::absolute(p, ec).lexically_normal().generic_string();
#ifdef _WIN32
    for (char& c : k) c = (char)std::tolower((unsigned char)c);
#endif
    return k;
}
} // namespace

void NoteSelfWrite(const std::filesystem::path& p) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_SelfWritesMutex);
    g_SelfWrites[SelfWriteKey(p)] = now;
    if (g_SelfWrites.size() > 512) // prune old entries now and then
        for (auto it = g_SelfWrites.begin(); it != g_SelfWrites.end();)
            it = now - it->second > std::chrono::seconds(30) ? g_SelfWrites.erase(it) : std::next(it);
}

bool WrittenBySelfRecently(const std::filesystem::path& path, int withinMs) {
    std::lock_guard<std::mutex> lk(g_SelfWritesMutex);
    const auto it = g_SelfWrites.find(SelfWriteKey(path));
    return it != g_SelfWrites.end() &&
           std::chrono::steady_clock::now() - it->second <= std::chrono::milliseconds(withinMs);
}

bool WriteBytes(const std::filesystem::path& path, std::string_view bytes, bool binary) {
    NoteSelfWrite(path); // before the rename, so the watcher's event always finds it
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        ec.clear(); // a pre-existing directory reports an error on some stdlibs; the open below is the real check
    }

    const std::filesystem::path temp = TempPathFor(path);

#ifdef _WIN32
    if (!WriteTempDurable(temp, bytes, binary, path)) return false;

    // #92 — replace in one step and NEVER delete the destination first. The old fallback
    // (remove(target) + rename) assumed the only way rename could fail was "destination
    // exists", but on Windows the usual causes are a sharing violation / AV scanner / cloud-sync
    // lock — in which case remove() could still succeed (or half-succeed) and the second rename
    // would fail too, leaving no file at all. MoveFileExW(REPLACE_EXISTING) replaces atomically;
    // transient locks get a few short retries, and on final failure the temp file is removed and
    // the original is left exactly as it was.
    DWORD err = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25 * (attempt + 1)));
    }
    Log::Error("AtomicFile: couldn't replace '" + path.string() + "' (the existing file was left untouched): " +
               LastErrorText(err));
    std::filesystem::remove(temp, ec);
    return false;
#else
    {
        std::ios::openmode mode = std::ios::out | std::ios::trunc;
        if (binary) mode |= std::ios::binary;
        std::ofstream f(temp, mode);
        if (!f.is_open()) {
            Log::Error("AtomicFile: cannot open temp file for '" + path.string() + "'.");
            return false;
        }
        f.write(bytes.data(), (std::streamsize)bytes.size());
        f.flush();
        if (!f.good()) {
            Log::Error("AtomicFile: write failed for '" + path.string() + "' (disk full?).");
            f.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    // POSIX rename() atomically replaces an existing destination.
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        Log::Error("AtomicFile: rename into place failed for '" + path.string() + "': " + ec.message());
        std::error_code ec2;
        std::filesystem::remove(temp, ec2);
        return false;
    }
    return true;
#endif
}

bool WriteJson(const std::filesystem::path& path, const nlohmann::json& j, int indent) {
    return WriteBytes(path, j.dump(indent) + "\n", /*binary=*/false);
}

} // namespace AtomicFile
