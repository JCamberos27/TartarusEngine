#include "App/ScratchProfile.h"

#include "App/LaunchOptions.h"
#include "Core/Env.h"
#include "Core/Paths.h"
#include "ScriptIDE.h"
#include "Tests/EditorUiTests.h" // --editor-tests

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
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
