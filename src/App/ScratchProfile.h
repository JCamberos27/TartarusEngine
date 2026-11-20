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

// --editor-shot: the prefs and layout the user sees, copied into a scratch folder this run may freely rewrite (window
// placement, layout, last scene).
bool PrepareEditorShotProfile(const LaunchOptions& opt);

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
