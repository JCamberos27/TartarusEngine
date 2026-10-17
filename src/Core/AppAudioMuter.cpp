#include "AppAudioMuter.h"
#include "UserPaths.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#endif

namespace AppAudioMuter {

namespace {
std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
} // namespace

bool NameMatches(const std::string& imagePath, const std::vector<std::string>& exeNames) {
    const size_t slash = imagePath.find_last_of("\\/");
    const std::string file = Lower(slash == std::string::npos ? imagePath : imagePath.substr(slash + 1));
    if (file.empty()) return false;
    for (const std::string& n : exeNames) {
        const std::string want = Lower(Trim(n));
        if (want.empty()) continue;
        // "Mechvibes" matches Mechvibes.exe as well as "Mechvibes.exe" does.
        if (file == want || file == want + ".exe") return true;
    }
    return false;
}

std::vector<std::string> ParseNameList(const std::string& commaSeparated) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= commaSeparated.size()) {
        size_t end = commaSeparated.find_first_of(",;", start);
        if (end == std::string::npos) end = commaSeparated.size();
        const std::string n = Trim(commaSeparated.substr(start, end - start));
        if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
        start = end + 1;
    }
    return out;
}

std::string JoinNameList(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out;
}

#if defined(_WIN32)
namespace {

std::string MarkerPath() { return UserPaths::Resolve("muted_apps.txt"); }

std::string Utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string ProcessImage(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD len = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    std::string out;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) out = Utf8(buf);
    CloseHandle(h);
    return out;
}

template <class T> void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// Calls fn(instanceId, volume) for every audio session, on every active output device, whose process matches `names`.
// `volume` is AddRef'd for the callee's keeping (fn returns true to keep it; false releases it). COM must be initialised.
template <class Fn> void ForEachMatchingSession(const std::vector<std::string>& names, Fn&& fn) {
    if (names.empty()) return;
    IMMDeviceEnumerator* devices = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&devices)))
        return;
    IMMDeviceCollection* list = nullptr;
    UINT deviceCount = 0;
    if (SUCCEEDED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list)) && list) list->GetCount(&deviceCount);
    const DWORD self = GetCurrentProcessId();
    for (UINT d = 0; d < deviceCount; ++d) {
        IMMDevice* device = nullptr;
        IAudioSessionManager2* manager = nullptr;
        IAudioSessionEnumerator* sessions = nullptr;
        int sessionCount = 0;
        if (SUCCEEDED(list->Item(d, &device)) &&
            SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&manager)) &&
            SUCCEEDED(manager->GetSessionEnumerator(&sessions)))
            sessions->GetCount(&sessionCount);
        for (int i = 0; i < sessionCount; ++i) {
            IAudioSessionControl* control = nullptr;
            IAudioSessionControl2* control2 = nullptr;
            ISimpleAudioVolume* volume = nullptr;
            DWORD pid = 0;
            if (SUCCEEDED(sessions->GetSession(i, &control)) &&
                SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&control2)) &&
                SUCCEEDED(control2->GetProcessId(&pid)) && pid != 0 && pid != self &&
                NameMatches(ProcessImage(pid), names) &&
                SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&volume))) {
                LPWSTR id = nullptr;
                std::string instance;
                if (SUCCEEDED(control2->GetSessionInstanceIdentifier(&id)) && id) {
                    instance = Utf8(id);
                    CoTaskMemFree(id);
                }
                if (fn(instance, volume)) volume = nullptr; // kept
            }
            SafeRelease(volume);
            SafeRelease(control2);
            SafeRelease(control);
        }
        SafeRelease(sessions);
        SafeRelease(manager);
        SafeRelease(device);
    }
    SafeRelease(list);
    SafeRelease(devices);
}

struct ComScope {
    bool ok;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ok = hr == S_OK || hr == S_FALSE; // RPC_E_CHANGED_MODE: usable, but not ours to uninitialise
    }
    ~ComScope() {
        if (ok) CoUninitialize();
    }
};

struct State {
    std::mutex M;
    std::condition_variable Cv;
    std::thread Worker;
    std::vector<std::string> Names;
    bool Desired = false, Stop = false, Started = false;
} g;

void WriteMarker(const std::vector<std::string>& names) {
    std::ofstream f(std::filesystem::u8path(MarkerPath()), std::ios::trunc);
    for (const std::string& n : names) f << n << "\n";
}
void DeleteMarker() {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(MarkerPath()), ec);
}

void Run() {
    ComScope com;
    std::map<std::string, ISimpleAudioVolume*> muted; // the sessions this muted, by instance id
    std::vector<std::string> lastNames;
    auto unmuteAll = [&] {
        for (auto& [id, v] : muted) {
            v->SetMute(FALSE, nullptr);
            v->Release();
        }
        // The app may have dropped a session we muted and reopened it (it keeps the session's mute): unmute the live one
        // by its id too.
        ForEachMatchingSession(lastNames, [&](const std::string& id, ISimpleAudioVolume* v) {
            BOOL m = FALSE;
            if (muted.count(id) && SUCCEEDED(v->GetMute(&m)) && m) v->SetMute(FALSE, nullptr);
            return false;
        });
        muted.clear();
        DeleteMarker();
    };
    std::unique_lock<std::mutex> lk(g.M);
    for (;;) {
        // While muted, look again every 50 ms: an Electron app (Mechvibes) opens its audio session only when it plays and
        // drops it when idle, so the session to mute appears under the first key press. A few ms of COM work per look.
        if (muted.empty() && !g.Desired) g.Cv.wait(lk, [] { return g.Stop || g.Desired; });
        else g.Cv.wait_for(lk, std::chrono::milliseconds(50));
        const bool want = g.Desired && !g.Stop;
        const std::vector<std::string> names = g.Names;
        if (want) lastNames = names;
        const bool stop = g.Stop;
        lk.unlock();
        if (want) {
            const size_t before = muted.size();
            ForEachMatchingSession(names, [&](const std::string& id, ISimpleAudioVolume* v) {
                BOOL already = FALSE;
                if (FAILED(v->GetMute(&already))) return false;
                const auto it = muted.find(id);
                if (already) return false; // ours and still muted, or the user's own mute: theirs to keep
                if (FAILED(v->SetMute(TRUE, nullptr))) return false;
                // A session we muted that came back unmuted is the app reopening it: keep the fresh handle.
                if (it != muted.end()) it->second->Release();
                muted[id] = v;
                return true;
            });
            if (muted.size() != before && before == 0) WriteMarker(names);
        } else if (!muted.empty()) {
            unmuteAll();
        }
        lk.lock();
        if (stop) break;
    }
    lk.unlock();
    unmuteAll();
}

} // namespace

void Configure(const std::vector<std::string>& exeNames) {
    std::lock_guard<std::mutex> lk(g.M);
    g.Names = exeNames;
}

void SetMuted(bool muted) {
    std::lock_guard<std::mutex> lk(g.M);
    if (g.Stop || g.Desired == muted) return;
    if (muted && g.Names.empty()) return;
    g.Desired = muted;
    if (!g.Started) {
        g.Started = true;
        g.Worker = std::thread(Run);
    }
    g.Cv.notify_all();
}

void Shutdown() {
    {
        std::lock_guard<std::mutex> lk(g.M);
        if (!g.Started) return;
        g.Stop = true;
        g.Desired = false;
        g.Cv.notify_all();
    }
    if (g.Worker.joinable()) g.Worker.join();
}

void RecoverStale() {
    std::vector<std::string> names;
    {
        std::ifstream f(std::filesystem::u8path(MarkerPath()));
        if (!f) return;
        std::string line;
        while (std::getline(f, line))
            if (!Trim(line).empty()) names.push_back(Trim(line));
    }
    // Its own short-lived thread: the caller's COM apartment (miniaudio's) stays as it is.
    std::thread([names] {
        ComScope com;
        ForEachMatchingSession(names, [](const std::string&, ISimpleAudioVolume* v) {
            BOOL m = FALSE;
            if (SUCCEEDED(v->GetMute(&m)) && m) v->SetMute(FALSE, nullptr);
            return false;
        });
    }).join();
    DeleteMarker();
}

#else

void Configure(const std::vector<std::string>&) {}
void SetMuted(bool) {}
void Shutdown() {}
void RecoverStale() {}

#endif

} // namespace AppAudioMuter
