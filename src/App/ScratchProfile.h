#pragma once

#include <mutex>
#include <string>
#include <vector>

struct LaunchOptions;

// The scratch user folders the unattended runs use, so they never touch the user's own files. Each returns false
// (after saying why on stderr) when the folder could not be set up and the run must not go on.

// --editor-tests: a fresh scratch folder every run (default prefs and layout, no bookmarks, tabs or favorites); the
// Script IDE session and the tests are configured to match.
bool PrepareEditorTestsProfile(const LaunchOptions& opt);

// --editor-shot / --bake-gi: the prefs and layout the user sees, copied into a scratch folder this run may freely rewrite
// (window placement, layout, last scene): TartarusEngine-Shot, or TartarusEngine-Shot-<instance>.
bool PrepareEditorShotProfile(const LaunchOptions& opt);

// --smoke-test / --perf-bench / --npc-test: with TARTARUS_EDITOR_TEST_INSTANCE set, a copy of the user's prefs and
// layout in its own scratch folder (TartarusEngine-Smoke-<name>), so parallel runs from different checkouts never write
// the same prefs; unset, the run uses the real user folder as before.
bool PrepareSmokeProfile();

// TARTARUS_EDITOR_TEST_INSTANCE (shared by every harness): "" when unset or not a valid name. Valid: at most 32
// letters, digits, '-' or '_' (empty counts as valid). ScratchFolderName adds "-<instance>" to a folder name.
std::string ScratchInstanceName();
bool ScratchInstanceNameValid(const std::string& name);
std::string ScratchFolderName(const std::string& base, const std::string& instance);

// With TARTARUS_EDITOR_TEST_INSTANCE set, ImGui copy/paste goes to a buffer inside this process instead of the system
// clipboard, which every process shares (the editor tests copy a path and read it back). Call after ImGui is set up.
void UsePrivateClipboardForInstance();

// --editor-tests drives the real project, and the editor saves two tracked files as it goes: project/settings.json
// (the Asset Browser's folder list on any folder create/rename/delete or undo, the Project Settings widgets) and
// project/layers.json. This keeps their bytes from the start of the run and writes them back: when the summary prints,
// before the editor-test watchdog ends the process (it calls Restore through EditorUiTests::SetRunEndHook), and when
// the guard is destroyed. A real crash (CrashHandler) is not covered. Construct it after EnginePaths::Init (the
// project root is found from the exe's folder).
class ProjectFilesGuard {
public:
    ProjectFilesGuard();
    ~ProjectFilesGuard();
    ProjectFilesGuard(const ProjectFilesGuard&) = delete;
    ProjectFilesGuard& operator=(const ProjectFilesGuard&) = delete;

    // Writes back each file that differs from the start of the run (removes one the run created). Safe to repeat
    // and to call from another thread.
    void Restore();

private:
    struct File;
    std::vector<File> m_Files;
    std::mutex m_Mutex;
};
