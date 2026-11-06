#include "App/Application.h"
#include "App/CliTools.h"
#include "App/LaunchOptions.h"
#include "CrashHandler.h"

#include <iostream>
#include <string>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // DWORD

// Ask the vendor drivers to run this process on the discrete GPU. On a multi-adapter box
// (laptop Optimus, or a desktop with the monitor cabled to the motherboard) the GL context
// otherwise lands on the integrated GPU — observed here as Intel UHD 630 instead of an RTX
// 4060, which turned this trivial scene into a 50-100 ms frame: the CPU raced ahead and then
// blocked in SwapBuffers waiting for the iGPU to drain the HDR + MSAA + multi-light + CSM
// pipeline. These must be *exported* symbols in the .exe for the NVIDIA / AMD drivers to see them.
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 1;
    __declspec(dllexport) int   AmdPowerXpressRequestHighPerformance = 1;
}

int main(int argc, char** argv) {
    // Before anything else: on a crash, write a minidump to %LOCALAPPDATA%\TartarusEngine\Crashes
    // and tell the user, instead of vanishing with a bare exit 139 (#148).
    CrashHandler::Install();

    // What the command line asks for (App/LaunchOptions.h); --project applies when the parse succeeds.
    LaunchOptions opt;
    if (std::string argError; !ParseLaunchOptions(argc, argv, opt, argError)) {
        std::cerr << "TartarusEngine: " << argError << "\nRun TartarusEngine --help for the options." << std::endl;
        return 2;
    }
    if (opt.mode == LaunchMode::Help) {
        std::cout << LaunchHelpText() << std::flush;
        return 0;
    }
    // The tools that run and exit before anything is set up (App/CliTools.h): --unit-tests, --model-report, ...
    if (const int code = RunEarlyCliTool(opt, argv[0]); code >= 0) return code;
    // The run itself: scratch profiles, crash tests, paths, the window, the frame loop and shutdown (App/Application.h).
    Application app(opt, argv[0]);
    return app.Run();
}
