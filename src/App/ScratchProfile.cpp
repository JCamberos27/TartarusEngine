#include "App/ScratchProfile.h"

#include "App/LaunchOptions.h"
#include "Core/Env.h"
#include "Core/Paths.h"
#include "ScriptIDE.h"
#include "imgui.h"
#include "Tests/EditorUiTests.h" // --editor-tests

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

bool ScratchInstanceNameValid(const std::string& name) {
    return name.size() <= 32 &&
           std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '-' || ch == '_'; });
}

std::string ScratchFolderName(const std::string& base, const std::string& instance) {
    return instance.empty() ? base : base + "-" + instance;
}

std::string ScratchInstanceName() {
    const std::string s = Env::Str(Env::Var::EditorTestInstance);
    return ScratchInstanceNameValid(s) ? s : std::string();
}

namespace {

// TARTARUS_EDITOR_TEST_INSTANCE=<name> gives a harness run its own scratch user folder (<base>-<name>), so runs from
// different checkouts at the same time never write the same prefs, layout or log. A name with other characters
// refuses the run rather than falling back to the shared folder.
bool InstanceNameOk(const char* what) {
    const std::string s = Env::Str(Env::Var::EditorTestInstance);
    if (ScratchInstanceNameValid(s)) return true;
    std::cerr << what << ": TARTARUS_EDITOR_TEST_INSTANCE='" << s
              << "' must be at most 32 letters, digits, '-' or '_'; refusing to run." << std::endl;
    return false;
}

// Points UserPaths at <base>[-<instance>] and checks it took (Root() pins on first use).
bool UseScratchFolder(const char* what, const std::string& base) {
    const std::string name = ScratchFolderName(base, ScratchInstanceName());
    UserPaths::SetAppName(name);
    if (std::filesystem::path(UserPaths::Root()).filename() == name) return true;
    std::cerr << what << ": could not set up a scratch user folder; refusing to run." << std::endl;
    return false;
}

// Copies the prefs and layout the user sees into the scratch folder this run may freely rewrite (window placement,
// layout, last scene). UserPaths::Root() pins on first use, so the real folder comes from BaseDir() instead.
bool CopyUserPrefsToScratch(const char* what, const std::string& base, bool dropLayout) {
    namespace fs = std::filesystem;
    fs::path real;
    if (const std::string appData = UserPaths::BaseDir(); !appData.empty()) real = fs::path(appData) / "TartarusEngine";
    if (!UseScratchFolder(what, base)) return false;
    const fs::path scratch = UserPaths::Root();
    std::error_code eqEc;
    if (real.empty() || fs::equivalent(real, scratch, eqEc)) {
        std::cerr << what << ": could not set up a scratch user folder; refusing to run." << std::endl;
        return false;
    }
    std::error_code ec;
    for (const char* f : {"imgui.ini", "editor_prefs.json", "shortcuts.json"})
        fs::copy_file(real / f, scratch / f, fs::copy_options::overwrite_existing, ec);
    if (dropLayout) fs::remove(scratch / "imgui.ini", ec);
    return true;
}

} // namespace

bool PrepareEditorTestsProfile(const LaunchOptions& opt) {
    // A fresh scratch user folder every run: default prefs and layout, no bookmarks, tabs or
    // favorites - the tests start from a known state and never touch the user's own files.
    namespace fs = std::filesystem;
    if (!InstanceNameOk("--editor-tests") || !UseScratchFolder("--editor-tests", "TartarusEngine-Test")) return false;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(UserPaths::Root(), ec)) fs::remove_all(entry.path(), ec);
    // The Script IDE's session (open tabs, unsaved buffers) lives in the project; this run's goes
    // in the scratch folder instead, so it starts with no tabs and leaves the user's alone.
    ScriptIDE::SetSessionDirectory(UserPaths::Resolve("IDE"));
    EditorUiTests::Configure(opt.editorTestsFilter);
    return true;
}

bool PrepareEditorShotProfile(const LaunchOptions& opt) {
    return InstanceNameOk("--editor-shot") &&
           CopyUserPrefsToScratch("--editor-shot", "TartarusEngine-Shot", opt.editorShotDefaultLayout);
}

bool PrepareSmokeProfile() {
    // Without an instance name the run keeps using the real user folder, as before.
    if (!InstanceNameOk("--smoke-test")) return false;
    if (ScratchInstanceName().empty()) return true;
    return CopyUserPrefsToScratch("--smoke-test", "TartarusEngine-Smoke", /*dropLayout=*/false);
}

void UsePrivateClipboardForInstance() {
    if (ScratchInstanceName().empty() || !ImGui::GetCurrentContext()) return;
    static std::string clip;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) { clip = text ? text : ""; };
    pio.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* { return clip.c_str(); };
}

struct ProjectFilesGuard::File {
    std::string Path;
    bool Existed = false;
    std::string Bytes;
};

namespace {
bool ReadBytes(const std::string& path, std::string& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
} // namespace

ProjectFilesGuard::ProjectFilesGuard() {
    for (const char* name : {"settings.json", "layers.json"}) {
        File file;
        file.Path = ProjectPaths::Resolve(name);
        file.Existed = ReadBytes(file.Path, file.Bytes);
        m_Files.push_back(std::move(file));
    }
    EditorUiTests::SetRunEndHook([this] { Restore(); });
}

ProjectFilesGuard::~ProjectFilesGuard() {
    EditorUiTests::SetRunEndHook({}); // waits for a watchdog call in progress
    Restore();
}

void ProjectFilesGuard::Restore() {
    namespace fs = std::filesystem;
    std::lock_guard<std::mutex> lk(m_Mutex);
    for (const File& file : m_Files) {
        std::error_code ec;
        const fs::path path = fs::u8path(file.Path);
        if (!file.Existed) {
            if (fs::remove(path, ec)) std::cerr << "--editor-tests: removed " << file.Path << " (made by the run)" << std::endl;
            continue;
        }
        std::string now;
        if (ReadBytes(file.Path, now) && now == file.Bytes) continue; // untouched: leave its timestamp alone
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(file.Bytes.data(), (std::streamsize)file.Bytes.size());
        out.close();
        if (out) std::cerr << "--editor-tests: put " << file.Path << " back as it was before the run" << std::endl;
        else std::cerr << "--editor-tests: could not put " << file.Path << " back; restore it from git" << std::endl;
    }
}
