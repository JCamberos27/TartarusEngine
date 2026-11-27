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

#include <json.hpp>

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

// A harness run draws Shaded whatever view mode the user left the editor in: an Unlit or Wireframe pref turned every
// smoke, perf-bench and editor-shot capture (the Game view too, which shares the pref) into a picture without lighting
// or shadows. Only the scratch copy changes; the user's own prefs keep their mode. One line says what the run uses.
void ForceShadedPrefs(const char* what, const std::filesystem::path& prefs) {
    int had = 0; // no prefs, or no mode in them: the editor's default, Shaded
    nlohmann::json j;
    if (std::ifstream in(prefs); in) j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_object() && j.contains("shadingMode") && j["shadingMode"].is_number_integer()) had = j["shadingMode"].get<int>();
    if (had != 0) {
        j["shadingMode"] = 0; // EditorLayer::ShadingMode::Shaded
        std::ofstream out(prefs, std::ios::trunc);
        out << j.dump(2);
        out.close();
        if (!out) {
            std::cerr << what << ": view mode " << had << " (could not write Shaded into this run's prefs copy)" << std::endl;
            return;
        }
    }
    std::cerr << what << ": view mode Shaded"
              << (had != 0 ? " (the user's prefs have mode " + std::to_string(had) + "; this run's copy set to Shaded)" : std::string())
              << std::endl;
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
    ForceShadedPrefs(what, scratch / "editor_prefs.json");
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

bool PrepareRestoreCheckProfile() {
    // Plain editor mode writes prefs, layout, log and the Script IDE session on exit: all of it goes to a fresh
    // scratch folder, and only for a named instance, so the check can never write the user's own files.
    namespace fs = std::filesystem;
    if (!InstanceNameOk("--restore-check")) return false;
    if (ScratchInstanceName().empty()) {
        std::cerr << "--restore-check: set TARTARUS_EDITOR_TEST_INSTANCE (it never runs on the real user folder); refusing to run."
                  << std::endl;
        return false;
    }
    if (!UseScratchFolder("--restore-check", "TartarusEngine-Restart")) return false;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(UserPaths::Root(), ec)) fs::remove_all(entry.path(), ec);
    ScriptIDE::SetSessionDirectory(UserPaths::Resolve("IDE"));
    return true;
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
    std::string Kept;   // Library/EditorTestsOriginal/<name>: the bytes, on disk, until the run puts the file back
    std::string Absent; // ...or this marker, when the run started without the file
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
    namespace fs = std::filesystem;
    const fs::path keep = fs::u8path(ProjectPaths::Resolve("Library/EditorTestsOriginal"));
    std::error_code ec;
    for (const char* name : {"settings.json", "layers.json"}) {
        File file;
        file.Path = ProjectPaths::Resolve(name);
        file.Kept = (keep / name).u8string();
        file.Absent = (keep / (std::string(name) + ".absent")).u8string();
        // An earlier run that never put the file back (killed, or a hard crash): back as it was before that run first.
        std::string kept;
        if (ReadBytes(file.Kept, kept)) {
            std::ofstream out(fs::u8path(file.Path), std::ios::binary | std::ios::trunc);
            out.write(kept.data(), (std::streamsize)kept.size());
            std::cerr << "--editor-tests: an earlier run ended without putting " << file.Path << " back; put back from its copy" << std::endl;
        } else if (fs::exists(fs::u8path(file.Absent), ec) && fs::remove(fs::u8path(file.Path), ec)) {
            std::cerr << "--editor-tests: removed " << file.Path << " (made by an earlier run that never removed it)" << std::endl;
        }
        file.Existed = ReadBytes(file.Path, file.Bytes);
        fs::create_directories(keep, ec);
        fs::remove(fs::u8path(file.Kept), ec);
        fs::remove(fs::u8path(file.Absent), ec);
        std::ofstream copy(fs::u8path(file.Existed ? file.Kept : file.Absent), std::ios::binary | std::ios::trunc);
        if (file.Existed) copy.write(file.Bytes.data(), (std::streamsize)file.Bytes.size());
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
    // Each file back as it was: its on-disk copy has done its job (a later call still has the bytes in memory). One that
    // couldn't be put back keeps it, for the next run.
    for (const File& file : m_Files) {
        std::error_code ec;
        std::string now;
        const bool back = file.Existed ? ReadBytes(file.Path, now) && now == file.Bytes : !fs::exists(fs::u8path(file.Path), ec);
        if (!back) continue;
        fs::remove(fs::u8path(file.Kept), ec);
        fs::remove(fs::u8path(file.Absent), ec);
    }
}
