#pragma once

#include <string>

// `TartarusEngine.exe --audio-test [scene]`: headless, offline.
int RunAudioTest(const std::string& scene, const std::string& reportPath);
