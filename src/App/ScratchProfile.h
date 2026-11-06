#pragma once

struct LaunchOptions;

// The scratch user folders the unattended runs use, so they never touch the user's own files. Each returns false
// (after saying why on stderr) when the folder could not be set up and the run must not go on.

// --editor-tests: a fresh scratch folder every run (default prefs and layout, no bookmarks, tabs or favorites); the
// Script IDE session and the tests are configured to match.
bool PrepareEditorTestsProfile(const LaunchOptions& opt);

// --editor-shot: the prefs and layout the user sees, copied into a scratch folder this run may freely rewrite (window
// placement, layout, last scene).
bool PrepareEditorShotProfile(const LaunchOptions& opt);
