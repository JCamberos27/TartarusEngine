#include "App/ScratchProfile.h"

#include "App/LaunchOptions.h"
#include "Core/Env.h"
#include "Core/Paths.h"
#include "ScriptIDE.h"
#include "Tests/EditorUiTests.h" // --editor-tests

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

bool PrepareEditorTestsProfile(const LaunchOptions& opt) {
    // A fresh scratch user folder every run: default prefs and layout, no bookmarks, tabs or
    // favorites - the tests start from a known state and never touch the user's own files.
    namespace fs = std::filesystem;
    // TARTARUS_EDITOR_TEST_INSTANCE=<name> gives this run its own folder
    // (TartarusEngine-Test-<name>), so runs from different checkouts at the same time don't
    // overwrite each other's prefs. Letters, digits, '-' and '_' only.
    std::string scratchName = "TartarusEngine-Test";
    if (const std::string s = Env::Str(Env::Var::EditorTestInstance);
        !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '-' || ch == '_'; }))
        scratchName += "-" + s;
    UserPaths::SetAppName(scratchName);
    const fs::path scratch = UserPaths::Root();
    if (scratch.filename() != scratchName) {
        std::cerr << "--editor-tests: could not set up a scratch user folder; refusing to run." << std::endl;
        return false;
    }
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(scratch, ec)) fs::remove_all(entry.path(), ec);
    // The Script IDE's session (open tabs, unsaved buffers) lives in the project; this run's goes
    // in the scratch folder instead, so it starts with no tabs and leaves the user's alone.
    ScriptIDE::SetSessionDirectory(UserPaths::Resolve("IDE"));
    EditorUiTests::Configure(opt.editorTestsFilter);
    return true;
}

bool PrepareEditorShotProfile(const LaunchOptions& opt) {
    // The prefs and layout the user sees, copied into a scratch user folder that this run may
    // freely rewrite (window placement, layout, last scene).
    // UserPaths::Root() pins on first use, so the real folder comes from BaseDir() instead.
    namespace fs = std::filesystem;
    fs::path real;
    if (const std::string appData = UserPaths::BaseDir(); !appData.empty())
        real = fs::path(appData) / "TartarusEngine";
    UserPaths::SetAppName("TartarusEngine-Shot");
    const fs::path scratch = UserPaths::Root();
    std::error_code eqEc;
    if (real.empty() || fs::equivalent(real, scratch, eqEc) || scratch.filename() != "TartarusEngine-Shot") {
        std::cerr << "--editor-shot: could not set up a scratch user folder; refusing to run." << std::endl;
        return false;
    }
    std::error_code ec;
    for (const char* f : {"imgui.ini", "editor_prefs.json", "shortcuts.json"})
        fs::copy_file(real / f, scratch / f, fs::copy_options::overwrite_existing, ec);
    if (opt.editorShotDefaultLayout) fs::remove(scratch / "imgui.ini", ec);
    return true;
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
