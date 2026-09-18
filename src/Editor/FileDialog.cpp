#include "FileDialog.h"
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace {
// Where the next dialog opens. Seeded once with the project folder (SetDefaultDirectory), then
// updated to the folder of whatever file was last picked — so Import doesn't keep dumping the
// user in build/Release/ and instead remembers where they were last working (#15 P4). Wide, so
// a non-ASCII folder round-trips exactly.
std::wstring s_lastDir;

// #139 — the dialogs are UTF-16 COM (IFileOpenDialog / IFileSaveDialog): no MAX_PATH buffer, no
// ANSI code page. Paths cross the API boundary as UTF-8, which is also the process code page
// (TartarusEngine.manifest), so every narrow std::string / std::filesystem call agrees.
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

void RememberDirOf(const std::wstring& pickedPath) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(pickedPath).parent_path();
    if (!dir.empty() && std::filesystem::is_directory(dir, ec) && !ec) s_lastDir = dir.wstring();
}

// Win32 "Name\0spec\0Name\0spec\0\0" filter -> COMDLG_FILTERSPEC pairs. `storage` owns the text.
std::vector<COMDLG_FILTERSPEC> ParseFilter(const char* filter, std::vector<std::wstring>& storage) {
    storage.clear();
    for (const char* p = filter; p && *p; p += std::strlen(p) + 1) storage.push_back(Widen(p));
    if (storage.size() % 2) storage.pop_back();
    std::vector<COMDLG_FILTERSPEC> specs;
    for (size_t i = 0; i + 1 < storage.size(); i += 2)
        specs.push_back({ storage[i].c_str(), storage[i + 1].c_str() });
    return specs;
}

std::wstring ItemPath(IShellItem* item) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}

// The shell dialogs need a single-threaded COM apartment, but miniaudio initialises the main
// thread as multi-threaded (MA_COINIT_VALUE), and IFileDialog::Show() hung there. So each dialog
// runs on its own short-lived STA thread while this thread keeps pumping window messages (as
// the old GetOpenFileName modal loop did), so the editor window never goes "Not Responding".
template <class Fn>
void RunOnStaThread(Fn&& fn) {
    std::thread worker([&fn] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (SUCCEEDED(hr)) {
            fn();
            CoUninitialize();
        }
    });
    HANDLE h = (HANDLE)worker.native_handle();
    for (;;) {
        const DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT);
        if (r != WAIT_OBJECT_0 + 1) break; // worker finished (or the wait failed)
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    worker.join();
}

// Shared setup for both dialog kinds. Returns false if the user cancelled or anything failed.
bool Show(IFileDialog* dlg, const char* filter, GLFWwindow* owner, FILEOPENDIALOGOPTIONS extra) {
    std::vector<std::wstring> storage;
    std::vector<COMDLG_FILTERSPEC> specs = ParseFilter(filter, storage);
    if (!specs.empty()) dlg->SetFileTypes((UINT)specs.size(), specs.data());

    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | extra);

    if (!s_lastDir.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(s_lastDir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    return SUCCEEDED(dlg->Show(owner ? glfwGetWin32Window(owner) : nullptr));
}

std::vector<std::string> Open(const char* filter, GLFWwindow* owner, bool multi) {
    std::vector<std::string> out;
    RunOnStaThread([&] {
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
            return;
        const FILEOPENDIALOGOPTIONS extra = FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | (multi ? (DWORD)FOS_ALLOWMULTISELECT : 0u);
        IShellItemArray* results = nullptr;
        if (Show(dlg, filter, owner, extra) && SUCCEEDED(dlg->GetResults(&results))) {
            DWORD count = 0;
            results->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (FAILED(results->GetItemAt(i, &item))) continue;
                const std::wstring w = ItemPath(item);
                item->Release();
                if (w.empty()) continue;
                RememberDirOf(w);
                out.push_back(Narrow(w));
            }
            results->Release();
        }
        dlg->Release();
    });
    return out;
}
}

void FileDialog::SetDefaultDirectory(const std::string& dir) {
    std::error_code ec;
    const std::wstring w = Widen(dir);
    if (s_lastDir.empty() && !w.empty() && std::filesystem::is_directory(w, ec) && !ec) {
        s_lastDir = w;
    }
}

std::string FileDialog::OpenFile(const char* filter, GLFWwindow* owner) {
    std::vector<std::string> picked = Open(filter, owner, false);
    return picked.empty() ? std::string() : picked.front();
}

std::vector<std::string> FileDialog::OpenFiles(const char* filter, GLFWwindow* owner) {
    return Open(filter, owner, true);
}

std::string FileDialog::SaveFile(const char* filter, const char* defaultExt, GLFWwindow* owner) {
    std::string path;
    RunOnStaThread([&] {
        IFileSaveDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
            return;
        if (defaultExt && *defaultExt) dlg->SetDefaultExtension(Widen(defaultExt).c_str());
        IShellItem* item = nullptr;
        if (Show(dlg, filter, owner, FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST) && SUCCEEDED(dlg->GetResult(&item))) {
            const std::wstring w = ItemPath(item);
            item->Release();
            if (!w.empty()) {
                RememberDirOf(w);
                path = Narrow(w);
            }
        }
        dlg->Release();
    });
    return path;
}

bool FileDialog::RecycleFile(const std::string& path, std::string& errorOut) {
    // SHFileOperationW's pFrom wants an absolute, backslash-separated, DOUBLE-null-terminated
    // string (a single trailing '\0' is not enough — it's a list format even for one entry).
    std::error_code ec;
    std::wstring wpath = std::filesystem::absolute(Widen(path), ec).wstring();
    if (ec) { errorOut = ec.message(); return false; }
    wpath.push_back(L'\0');

    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = wpath.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

    int result = SHFileOperationW(&op);
    if (result != 0 || op.fAnyOperationsAborted) {
        errorOut = "the file may be open in another program or write-protected";
        return false;
    }
    return true;
}
