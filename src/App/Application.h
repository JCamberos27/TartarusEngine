#pragma once

#include <string>

#include "App/LaunchOptions.h"
#include "PlayerConfig.h"

// The engine's run, from the parsed command line to the process exit code: scratch profiles and crash tests, the paths,
// the window and GL context, the scene and editor, the frame loop and shutdown. main() builds one and calls Run().
class Application {
public:
    // `opt` must outlive the Application; `argv0` is argv[0] (where the exe, and so its modules and assets, are).
    Application(const LaunchOptions& opt, const char* argv0);

    // Returns the process exit code.
    int Run();

private:
    // The session proper: the window and engine up, the frame loop, shutdown. Returns the exit code.
    int RunSession();

    const LaunchOptions& m_Opt;
    const char* m_Argv0 = nullptr;
    bool m_Headless = false;     // --smoke-test / --editor-tests: no splash, no modal dialogs
    bool m_PlayerMode = false;   // a player.json next to the exe: this is the built game
    PlayerConfig m_PlayerCfg;
};
